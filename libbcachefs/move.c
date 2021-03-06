
#include "bcachefs.h"
#include "btree_gc.h"
#include "btree_update.h"
#include "buckets.h"
#include "inode.h"
#include "io.h"
#include "move.h"
#include "super-io.h"
#include "keylist.h"

#include <linux/ioprio.h>
#include <linux/kthread.h>

#include <trace/events/bcachefs.h>

struct moving_io {
	struct list_head	list;
	struct closure		cl;
	bool			read_completed;
	unsigned		sectors;

	struct bch_read_bio	rbio;

	struct migrate_write	write;
	/* Must be last since it is variable size */
	struct bio_vec		bi_inline_vecs[0];
};

struct moving_context {
	/* Closure for waiting on all reads and writes to complete */
	struct closure		cl;

	struct bch_move_stats	*stats;

	struct list_head	reads;
	atomic_t		sectors_in_flight;
	wait_queue_head_t	wait;
};

static int bch2_migrate_index_update(struct bch_write_op *op)
{
	struct bch_fs *c = op->c;
	struct migrate_write *m =
		container_of(op, struct migrate_write, op);
	struct keylist *keys = &op->insert_keys;
	struct btree_iter iter;
	int ret = 0;

	bch2_btree_iter_init(&iter, c, BTREE_ID_EXTENTS,
			     bkey_start_pos(&bch2_keylist_front(keys)->k),
			     BTREE_ITER_SLOTS|BTREE_ITER_INTENT);

	while (1) {
		struct bkey_s_c k = bch2_btree_iter_peek_slot(&iter);
		struct bkey_i_extent *insert, *new =
			bkey_i_to_extent(bch2_keylist_front(keys));
		BKEY_PADDED(k) _new, _insert;
		struct bch_extent_ptr *ptr;
		struct bch_extent_crc_unpacked crc;
		bool did_work = false;

		if (btree_iter_err(k)) {
			ret = bch2_btree_iter_unlock(&iter);
			break;
		}

		if (bversion_cmp(k.k->version, new->k.version) ||
		    !bkey_extent_is_data(k.k) ||
		    !bch2_extent_matches_ptr(c, bkey_s_c_to_extent(k),
					     m->ptr, m->offset))
			goto nomatch;

		bkey_reassemble(&_insert.k, k);
		insert = bkey_i_to_extent(&_insert.k);

		bkey_copy(&_new.k, bch2_keylist_front(keys));
		new = bkey_i_to_extent(&_new.k);

		bch2_cut_front(iter.pos, &insert->k_i);
		bch2_cut_back(new->k.p, &insert->k);
		bch2_cut_back(insert->k.p, &new->k);

		if (m->move_dev >= 0 &&
		    (ptr = (struct bch_extent_ptr *)
		     bch2_extent_has_device(extent_i_to_s_c(insert),
					    m->move_dev)))
			bch2_extent_drop_ptr(extent_i_to_s(insert), ptr);

		extent_for_each_ptr_crc(extent_i_to_s(new), ptr, crc) {
			if (bch2_extent_has_device(extent_i_to_s_c(insert), ptr->dev)) {
				/*
				 * raced with another move op? extent already
				 * has a pointer to the device we just wrote
				 * data to
				 */
				continue;
			}

			bch2_extent_crc_append(insert, crc);
			extent_ptr_append(insert, *ptr);
			did_work = true;
		}

		if (!did_work)
			goto nomatch;

		bch2_extent_narrow_crcs(insert,
				(struct bch_extent_crc_unpacked) { 0 });
		bch2_extent_normalize(c, extent_i_to_s(insert).s);
		bch2_extent_mark_replicas_cached(c, extent_i_to_s(insert));

		ret = bch2_check_mark_super(c, BCH_DATA_USER,
				bch2_extent_devs(extent_i_to_s_c(insert)));
		if (ret)
			break;

		ret = bch2_btree_insert_at(c, &op->res,
				NULL, op_journal_seq(op),
				BTREE_INSERT_ATOMIC|
				BTREE_INSERT_NOFAIL|
				m->btree_insert_flags,
				BTREE_INSERT_ENTRY(&iter, &insert->k_i));
		if (!ret)
			atomic_long_inc(&c->extent_migrate_done);
		if (ret == -EINTR)
			ret = 0;
		if (ret)
			break;
next:
		while (bkey_cmp(iter.pos, bch2_keylist_front(keys)->k.p) >= 0) {
			bch2_keylist_pop_front(keys);
			if (bch2_keylist_empty(keys))
				goto out;
		}

		bch2_cut_front(iter.pos, bch2_keylist_front(keys));
		continue;
nomatch:
		if (m->ctxt)
			atomic64_add(k.k->p.offset - iter.pos.offset,
				     &m->ctxt->stats->sectors_raced);
		atomic_long_inc(&c->extent_migrate_raced);
		trace_move_race(&new->k);
		bch2_btree_iter_next_slot(&iter);
		goto next;
	}
out:
	bch2_btree_iter_unlock(&iter);
	return ret;
}

void bch2_migrate_write_init(struct migrate_write *m,
			     struct bch_read_bio *rbio)
{
	/* write bio must own pages: */
	BUG_ON(!m->op.wbio.bio.bi_vcnt);

	m->ptr		= rbio->pick.ptr;
	m->offset	= rbio->pos.offset - rbio->pick.crc.offset;
	m->op.devs_have	= rbio->devs_have;
	m->op.pos	= rbio->pos;
	m->op.version	= rbio->version;
	m->op.crc	= rbio->pick.crc;

	if (bch2_csum_type_is_encryption(m->op.crc.csum_type)) {
		m->op.nonce	= m->op.crc.nonce + m->op.crc.offset;
		m->op.csum_type = m->op.crc.csum_type;
	}

	if (m->move_dev >= 0)
		bch2_dev_list_drop_dev(&m->op.devs_have, m->move_dev);

	if (m->btree_insert_flags & BTREE_INSERT_USE_RESERVE)
		m->op.alloc_reserve = RESERVE_MOVINGGC;

	m->op.flags |= BCH_WRITE_ONLY_SPECIFIED_DEVS|
		BCH_WRITE_PAGES_STABLE|
		BCH_WRITE_PAGES_OWNED|
		BCH_WRITE_DATA_ENCODED|
		BCH_WRITE_NOMARK_REPLICAS;

	m->op.wbio.bio.bi_iter.bi_size = m->op.crc.compressed_size << 9;
	m->op.nr_replicas	= 1;
	m->op.nr_replicas_required = 1;
	m->op.index_update_fn	= bch2_migrate_index_update;
}

static void move_free(struct closure *cl)
{
	struct moving_io *io = container_of(cl, struct moving_io, cl);
	struct moving_context *ctxt = io->write.ctxt;
	struct bio_vec *bv;
	int i;

	bch2_disk_reservation_put(io->write.op.c, &io->write.op.res);

	bio_for_each_segment_all(bv, &io->write.op.wbio.bio, i)
		if (bv->bv_page)
			__free_page(bv->bv_page);

	atomic_sub(io->sectors, &ctxt->sectors_in_flight);
	wake_up(&ctxt->wait);

	kfree(io);
}

static void move_write(struct closure *cl)
{
	struct moving_io *io = container_of(cl, struct moving_io, cl);

	if (likely(!io->rbio.bio.bi_status)) {
		bch2_migrate_write_init(&io->write, &io->rbio);
		closure_call(&io->write.op.cl, bch2_write, NULL, cl);
	}

	closure_return_with_destructor(cl, move_free);
}

static inline struct moving_io *next_pending_write(struct moving_context *ctxt)
{
	struct moving_io *io =
		list_first_entry_or_null(&ctxt->reads, struct moving_io, list);

	return io && io->read_completed ? io : NULL;
}

static void move_read_endio(struct bio *bio)
{
	struct moving_io *io = container_of(bio, struct moving_io, rbio.bio);
	struct moving_context *ctxt = io->write.ctxt;

	io->read_completed = true;
	if (next_pending_write(ctxt))
		wake_up(&ctxt->wait);

	closure_put(&ctxt->cl);
}

static int bch2_move_extent(struct bch_fs *c,
			  struct moving_context *ctxt,
			  struct bch_devs_mask *devs,
			  struct write_point_specifier wp,
			  int btree_insert_flags,
			  int move_device,
			  struct bch_io_opts opts,
			  struct bkey_s_c_extent e)
{
	struct extent_pick_ptr pick;
	struct moving_io *io;
	const struct bch_extent_ptr *ptr;
	struct bch_extent_crc_unpacked crc;
	unsigned sectors = e.k->size, pages, nr_good;
	int ret = -ENOMEM;

	bch2_extent_pick_ptr(c, e.s_c, NULL, &pick);
	if (IS_ERR_OR_NULL(pick.ca))
		return pick.ca ? PTR_ERR(pick.ca) : 0;

	/* write path might have to decompress data: */
	extent_for_each_ptr_crc(e, ptr, crc)
		sectors = max_t(unsigned, sectors, crc.uncompressed_size);

	pages = DIV_ROUND_UP(sectors, PAGE_SECTORS);
	io = kzalloc(sizeof(struct moving_io) +
		     sizeof(struct bio_vec) * pages, GFP_KERNEL);
	if (!io)
		goto err;

	io->write.ctxt	= ctxt;
	io->sectors	= e.k->size;

	bio_init(&io->write.op.wbio.bio, io->bi_inline_vecs, pages);
	bio_set_prio(&io->write.op.wbio.bio,
		     IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0));
	io->write.op.wbio.bio.bi_iter.bi_size = sectors << 9;

	bch2_bio_map(&io->write.op.wbio.bio, NULL);
	if (bio_alloc_pages(&io->write.op.wbio.bio, GFP_KERNEL))
		goto err_free;

	io->rbio.opts = opts;
	bio_init(&io->rbio.bio, io->bi_inline_vecs, pages);
	bio_set_prio(&io->rbio.bio, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0));
	io->rbio.bio.bi_iter.bi_size = sectors << 9;

	bio_set_op_attrs(&io->rbio.bio, REQ_OP_READ, 0);
	io->rbio.bio.bi_iter.bi_sector	= bkey_start_offset(e.k);
	io->rbio.bio.bi_end_io		= move_read_endio;

	io->write.btree_insert_flags = btree_insert_flags;
	io->write.move_dev	= move_device;

	bch2_write_op_init(&io->write.op, c);
	io->write.op.csum_type = bch2_data_checksum_type(c, opts.data_checksum);
	io->write.op.compression_type =
		bch2_compression_opt_to_type(opts.compression);
	io->write.op.devs	= devs;
	io->write.op.write_point = wp;

	if (move_device < 0 &&
	    ((nr_good = bch2_extent_nr_good_ptrs(c, e)) <
	     c->opts.data_replicas)) {
		io->write.op.nr_replicas = c->opts.data_replicas - nr_good;

		ret = bch2_disk_reservation_get(c, &io->write.op.res,
						e.k->size,
						io->write.op.nr_replicas, 0);
		if (ret)
			goto err_free_pages;
	}

	atomic64_inc(&ctxt->stats->keys_moved);
	atomic64_add(e.k->size, &ctxt->stats->sectors_moved);

	trace_move_extent(e.k);

	atomic_add(io->sectors, &ctxt->sectors_in_flight);
	list_add_tail(&io->list, &ctxt->reads);

	/*
	 * dropped by move_read_endio() - guards against use after free of
	 * ctxt when doing wakeup
	 */
	closure_get(&ctxt->cl);
	bch2_read_extent(c, &io->rbio, e, &pick, BCH_READ_NODECODE);
	return 0;
err_free_pages:
	bio_free_pages(&io->write.op.wbio.bio);
err_free:
	kfree(io);
err:
	percpu_ref_put(&pick.ca->io_ref);
	trace_move_alloc_fail(e.k);
	return ret;
}

static void do_pending_writes(struct moving_context *ctxt)
{
	struct moving_io *io;

	while ((io = next_pending_write(ctxt))) {
		list_del(&io->list);
		closure_call(&io->cl, move_write, NULL, &ctxt->cl);
	}
}

#define move_ctxt_wait_event(_ctxt, _cond)			\
do {								\
	do_pending_writes(_ctxt);				\
								\
	if (_cond)						\
		break;						\
	__wait_event((_ctxt)->wait,				\
		     next_pending_write(_ctxt) || (_cond));	\
} while (1)

static void bch2_move_ctxt_wait_for_io(struct moving_context *ctxt)
{
	unsigned sectors_pending = atomic_read(&ctxt->sectors_in_flight);

	move_ctxt_wait_event(ctxt,
		!atomic_read(&ctxt->sectors_in_flight) ||
		atomic_read(&ctxt->sectors_in_flight) != sectors_pending);
}

int bch2_move_data(struct bch_fs *c,
		   struct bch_ratelimit *rate,
		   unsigned sectors_in_flight,
		   struct bch_devs_mask *devs,
		   struct write_point_specifier wp,
		   int btree_insert_flags,
		   int move_device,
		   struct bpos start,
		   struct bpos end,
		   move_pred_fn pred, void *arg,
		   struct bch_move_stats *stats)
{
	bool kthread = (current->flags & PF_KTHREAD) != 0;
	struct moving_context ctxt = { .stats = stats };
	struct bch_io_opts opts = bch2_opts_to_inode_opts(c->opts);
	BKEY_PADDED(k) tmp;
	struct bkey_s_c k;
	struct bkey_s_c_extent e;
	u64 cur_inum = U64_MAX;
	int ret = 0;

	closure_init_stack(&ctxt.cl);
	INIT_LIST_HEAD(&ctxt.reads);
	init_waitqueue_head(&ctxt.wait);

	stats->data_type = BCH_DATA_USER;
	bch2_btree_iter_init(&stats->iter, c, BTREE_ID_EXTENTS, start,
			     BTREE_ITER_PREFETCH);

	if (rate)
		bch2_ratelimit_reset(rate);

	while (!kthread || !(ret = kthread_should_stop())) {
		if (atomic_read(&ctxt.sectors_in_flight) >= sectors_in_flight) {
			bch2_btree_iter_unlock(&stats->iter);
			move_ctxt_wait_event(&ctxt,
					     atomic_read(&ctxt.sectors_in_flight) <
					     sectors_in_flight);
		}

		if (rate &&
		    bch2_ratelimit_delay(rate) &&
		    (bch2_btree_iter_unlock(&stats->iter),
		     (ret = bch2_ratelimit_wait_freezable_stoppable(rate))))
			break;
peek:
		k = bch2_btree_iter_peek(&stats->iter);
		if (!k.k)
			break;
		ret = btree_iter_err(k);
		if (ret)
			break;
		if (bkey_cmp(bkey_start_pos(k.k), end) >= 0)
			break;

		if (!bkey_extent_is_data(k.k))
			goto next_nondata;

		e = bkey_s_c_to_extent(k);

		if (cur_inum != k.k->p.inode) {
			struct bch_inode_unpacked inode;

			/* don't hold btree locks while looking up inode: */
			bch2_btree_iter_unlock(&stats->iter);

			opts = bch2_opts_to_inode_opts(c->opts);
			if (!bch2_inode_find_by_inum(c, k.k->p.inode, &inode))
				bch2_io_opts_apply(&opts, bch2_inode_opts_get(&inode));
			cur_inum = k.k->p.inode;
			goto peek;
		}

		if (!pred(arg, e))
			goto next;

		/* unlock before doing IO: */
		bkey_reassemble(&tmp.k, k);
		k = bkey_i_to_s_c(&tmp.k);
		bch2_btree_iter_unlock(&stats->iter);

		if (bch2_move_extent(c, &ctxt, devs, wp,
				     btree_insert_flags,
				     move_device, opts,
				     bkey_s_c_to_extent(k))) {
			/* memory allocation failure, wait for some IO to finish */
			bch2_move_ctxt_wait_for_io(&ctxt);
			continue;
		}

		if (rate)
			bch2_ratelimit_increment(rate, k.k->size);
next:
		atomic64_add(k.k->size * bch2_extent_nr_dirty_ptrs(k),
			     &stats->sectors_seen);
next_nondata:
		bch2_btree_iter_next(&stats->iter);
		bch2_btree_iter_cond_resched(&stats->iter);
	}

	bch2_btree_iter_unlock(&stats->iter);

	move_ctxt_wait_event(&ctxt, !atomic_read(&ctxt.sectors_in_flight));
	closure_sync(&ctxt.cl);

	EBUG_ON(!list_empty(&ctxt.reads));
	EBUG_ON(atomic_read(&ctxt.sectors_in_flight));

	trace_move_data(c,
			atomic64_read(&stats->sectors_moved),
			atomic64_read(&stats->keys_moved));

	return ret;
}

static int bch2_gc_data_replicas(struct bch_fs *c)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret;

	mutex_lock(&c->replicas_gc_lock);
	bch2_replicas_gc_start(c, 1 << BCH_DATA_USER);

	for_each_btree_key(&iter, c, BTREE_ID_EXTENTS, POS_MIN,
			   BTREE_ITER_PREFETCH, k) {
		ret = bch2_check_mark_super(c, BCH_DATA_USER, bch2_bkey_devs(k));
		if (ret)
			break;
	}
	ret = bch2_btree_iter_unlock(&iter) ?: ret;

	bch2_replicas_gc_end(c, ret);
	mutex_unlock(&c->replicas_gc_lock);

	return ret;
}

static int bch2_gc_btree_replicas(struct bch_fs *c)
{
	struct btree_iter iter;
	struct btree *b;
	unsigned id;
	int ret = 0;

	mutex_lock(&c->replicas_gc_lock);
	bch2_replicas_gc_start(c, 1 << BCH_DATA_BTREE);

	for (id = 0; id < BTREE_ID_NR; id++) {
		for_each_btree_node(&iter, c, id, POS_MIN, BTREE_ITER_PREFETCH, b) {
			ret = bch2_check_mark_super(c, BCH_DATA_BTREE,
					bch2_bkey_devs(bkey_i_to_s_c(&b->key)));

			bch2_btree_iter_cond_resched(&iter);
		}

		ret = bch2_btree_iter_unlock(&iter) ?: ret;
	}

	bch2_replicas_gc_end(c, ret);
	mutex_unlock(&c->replicas_gc_lock);

	return ret;
}

static int bch2_move_btree(struct bch_fs *c,
			   move_pred_fn pred,
			   void *arg,
			   struct bch_move_stats *stats)
{
	struct btree *b;
	unsigned id;
	int ret = 0;

	stats->data_type = BCH_DATA_BTREE;

	for (id = 0; id < BTREE_ID_NR; id++) {
		for_each_btree_node(&stats->iter, c, id, POS_MIN, BTREE_ITER_PREFETCH, b) {
			if (pred(arg, bkey_i_to_s_c_extent(&b->key)))
				ret = bch2_btree_node_rewrite(c, &stats->iter,
						b->data->keys.seq, 0) ?: ret;

			bch2_btree_iter_cond_resched(&stats->iter);
		}

		ret = bch2_btree_iter_unlock(&stats->iter) ?: ret;
	}

	return ret;
}

#if 0
static bool scrub_data_pred(void *arg, struct bkey_s_c_extent e)
{
}
#endif

static bool rereplicate_metadata_pred(void *arg, struct bkey_s_c_extent e)
{
	struct bch_fs *c = arg;
	unsigned nr_good = bch2_extent_nr_good_ptrs(c, e);

	return nr_good && nr_good < c->opts.metadata_replicas;
}

static bool rereplicate_data_pred(void *arg, struct bkey_s_c_extent e)
{
	struct bch_fs *c = arg;
	unsigned nr_good = bch2_extent_nr_good_ptrs(c, e);

	return nr_good && nr_good < c->opts.data_replicas;
}

static bool migrate_pred(void *arg, struct bkey_s_c_extent e)
{
	struct bch_ioctl_data *op = arg;

	return bch2_extent_has_device(e, op->migrate.dev);
}

int bch2_data_job(struct bch_fs *c,
		  struct bch_move_stats *stats,
		  struct bch_ioctl_data op)
{
	int ret = 0;

	switch (op.op) {
	case BCH_DATA_OP_REREPLICATE:
		stats->data_type = BCH_DATA_JOURNAL;
		ret = bch2_journal_flush_device(&c->journal, -1);

		ret = bch2_move_btree(c, rereplicate_metadata_pred, c, stats) ?: ret;
		ret = bch2_gc_btree_replicas(c) ?: ret;

		ret = bch2_move_data(c, NULL, SECTORS_IN_FLIGHT_PER_DEVICE,
				     NULL,
				     writepoint_hashed((unsigned long) current),
				     0, -1,
				     op.start,
				     op.end,
				     rereplicate_data_pred, c, stats) ?: ret;
		ret = bch2_gc_data_replicas(c) ?: ret;
		break;
	case BCH_DATA_OP_MIGRATE:
		if (op.migrate.dev >= c->sb.nr_devices)
			return -EINVAL;

		stats->data_type = BCH_DATA_JOURNAL;
		ret = bch2_journal_flush_device(&c->journal, op.migrate.dev);

		ret = bch2_move_btree(c, migrate_pred, &op, stats) ?: ret;
		ret = bch2_gc_btree_replicas(c) ?: ret;

		ret = bch2_move_data(c, NULL, SECTORS_IN_FLIGHT_PER_DEVICE,
				     NULL,
				     writepoint_hashed((unsigned long) current),
				     0, -1,
				     op.start,
				     op.end,
				     migrate_pred, &op, stats) ?: ret;
		ret = bch2_gc_data_replicas(c) ?: ret;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}
