
#include "bcachefs.h"
#include "btree_update.h"
#include "btree_update_interior.h"
#include "btree_io.h"
#include "btree_iter.h"
#include "btree_locking.h"
#include "debug.h"
#include "extents.h"
#include "journal.h"
#include "keylist.h"

#include <linux/sort.h>
#include <trace/events/bcachefs.h>

/* Inserting into a given leaf node (last stage of insert): */

/* Handle overwrites and do insert, for non extents: */
bool bch2_btree_bset_insert_key(struct btree_iter *iter,
				struct btree *b,
				struct btree_node_iter *node_iter,
				struct bkey_i *insert)
{
	const struct bkey_format *f = &b->format;
	struct bkey_packed *k;
	struct bset_tree *t;
	unsigned clobber_u64s;

	EBUG_ON(btree_node_just_written(b));
	EBUG_ON(bset_written(b, btree_bset_last(b)));
	EBUG_ON(bkey_deleted(&insert->k) && bkey_val_u64s(&insert->k));
	EBUG_ON(bkey_cmp(bkey_start_pos(&insert->k), b->data->min_key) < 0 ||
		bkey_cmp(insert->k.p, b->data->max_key) > 0);

	k = bch2_btree_node_iter_peek_all(node_iter, b);
	if (k && !bkey_cmp_packed(b, k, &insert->k)) {
		BUG_ON(bkey_whiteout(k));

		t = bch2_bkey_to_bset(b, k);

		if (bset_unwritten(b, bset(b, t)) &&
		    bkey_val_u64s(&insert->k) == bkeyp_val_u64s(f, k) &&
		    !bkey_whiteout(&insert->k)) {
			k->type = insert->k.type;
			memcpy_u64s(bkeyp_val(f, k), &insert->v,
				    bkey_val_u64s(&insert->k));
			return true;
		}

		insert->k.needs_whiteout = k->needs_whiteout;

		btree_keys_account_key_drop(&b->nr, t - b->set, k);

		if (t == bset_tree_last(b)) {
			clobber_u64s = k->u64s;

			/*
			 * If we're deleting, and the key we're deleting doesn't
			 * need a whiteout (it wasn't overwriting a key that had
			 * been written to disk) - just delete it:
			 */
			if (bkey_whiteout(&insert->k) && !k->needs_whiteout) {
				bch2_bset_delete(b, k, clobber_u64s);
				bch2_btree_node_iter_fix(iter, b, node_iter, t,
							k, clobber_u64s, 0);
				return true;
			}

			goto overwrite;
		}

		k->type = KEY_TYPE_DELETED;
		bch2_btree_node_iter_fix(iter, b, node_iter, t, k,
					k->u64s, k->u64s);

		if (bkey_whiteout(&insert->k)) {
			reserve_whiteout(b, t, k);
			return true;
		} else {
			k->needs_whiteout = false;
		}
	} else {
		/*
		 * Deleting, but the key to delete wasn't found - nothing to do:
		 */
		if (bkey_whiteout(&insert->k))
			return false;

		insert->k.needs_whiteout = false;
	}

	t = bset_tree_last(b);
	k = bch2_btree_node_iter_bset_pos(node_iter, b, t);
	clobber_u64s = 0;
overwrite:
	bch2_bset_insert(b, node_iter, k, insert, clobber_u64s);
	if (k->u64s != clobber_u64s || bkey_whiteout(&insert->k))
		bch2_btree_node_iter_fix(iter, b, node_iter, t, k,
					clobber_u64s, k->u64s);
	return true;
}

static void __btree_node_flush(struct journal *j, struct journal_entry_pin *pin,
			       unsigned i, u64 seq)
{
	struct bch_fs *c = container_of(j, struct bch_fs, journal);
	struct btree_write *w = container_of(pin, struct btree_write, journal);
	struct btree *b = container_of(w, struct btree, writes[i]);

	six_lock_read(&b->lock);
	bch2_btree_node_write_cond(c, b,
			(btree_current_write(b) == w &&
			 w->journal.pin_list == journal_seq_pin(j, seq)));
	six_unlock_read(&b->lock);
}

static void btree_node_flush0(struct journal *j, struct journal_entry_pin *pin, u64 seq)
{
	return __btree_node_flush(j, pin, 0, seq);
}

static void btree_node_flush1(struct journal *j, struct journal_entry_pin *pin, u64 seq)
{
	return __btree_node_flush(j, pin, 1, seq);
}

void bch2_btree_journal_key(struct btree_insert *trans,
			   struct btree_iter *iter,
			   struct bkey_i *insert)
{
	struct bch_fs *c = trans->c;
	struct journal *j = &c->journal;
	struct btree *b = iter->l[0].b;
	struct btree_write *w = btree_current_write(b);

	EBUG_ON(iter->level || b->level);
	EBUG_ON(trans->journal_res.ref !=
		!(trans->flags & BTREE_INSERT_JOURNAL_REPLAY));

	if (likely(trans->journal_res.ref)) {
		u64 seq = trans->journal_res.seq;
		bool needs_whiteout = insert->k.needs_whiteout;

		/* ick */
		insert->k.needs_whiteout = false;
		bch2_journal_add_keys(j, &trans->journal_res,
				      iter->btree_id, insert);
		insert->k.needs_whiteout = needs_whiteout;

		bch2_journal_set_has_inode(j, &trans->journal_res,
					   insert->k.p.inode);

		if (trans->journal_seq)
			*trans->journal_seq = seq;
		btree_bset_last(b)->journal_seq = cpu_to_le64(seq);
	}

	if (unlikely(!journal_pin_active(&w->journal)))
		bch2_journal_pin_add(j, &trans->journal_res,
				     &w->journal,
				     btree_node_write_idx(b) == 0
				     ? btree_node_flush0
				     : btree_node_flush1);

	if (unlikely(!btree_node_dirty(b)))
		set_btree_node_dirty(b);
}

static enum btree_insert_ret
bch2_insert_fixup_key(struct btree_insert *trans,
		     struct btree_insert_entry *insert)
{
	struct btree_iter *iter = insert->iter;
	struct btree_iter_level *l = &iter->l[0];

	EBUG_ON(iter->level);
	EBUG_ON(insert->k->k.u64s >
		bch_btree_keys_u64s_remaining(trans->c, l->b));

	if (bch2_btree_bset_insert_key(iter, l->b, &l->iter,
				       insert->k))
		bch2_btree_journal_key(trans, iter, insert->k);

	trans->did_work = true;
	return BTREE_INSERT_OK;
}

/**
 * btree_insert_key - insert a key one key into a leaf node
 */
static enum btree_insert_ret
btree_insert_key_leaf(struct btree_insert *trans,
		      struct btree_insert_entry *insert)
{
	struct bch_fs *c = trans->c;
	struct btree_iter *iter = insert->iter;
	struct btree *b = iter->l[0].b;
	enum btree_insert_ret ret;
	int old_u64s = le16_to_cpu(btree_bset_last(b)->u64s);
	int old_live_u64s = b->nr.live_u64s;
	int live_u64s_added, u64s_added;

	btree_iter_set_dirty(iter, BTREE_ITER_NEED_PEEK);

	ret = !btree_node_is_extents(b)
		? bch2_insert_fixup_key(trans, insert)
		: bch2_insert_fixup_extent(trans, insert);

	live_u64s_added = (int) b->nr.live_u64s - old_live_u64s;
	u64s_added = (int) le16_to_cpu(btree_bset_last(b)->u64s) - old_u64s;

	if (b->sib_u64s[0] != U16_MAX && live_u64s_added < 0)
		b->sib_u64s[0] = max(0, (int) b->sib_u64s[0] + live_u64s_added);
	if (b->sib_u64s[1] != U16_MAX && live_u64s_added < 0)
		b->sib_u64s[1] = max(0, (int) b->sib_u64s[1] + live_u64s_added);

	if (u64s_added > live_u64s_added &&
	    bch2_maybe_compact_whiteouts(c, b))
		bch2_btree_iter_reinit_node(iter, b);

	trace_btree_insert_key(c, b, insert->k);
	return ret;
}

static bool same_leaf_as_prev(struct btree_insert *trans,
			      struct btree_insert_entry *i)
{
	/*
	 * Because we sorted the transaction entries, if multiple iterators
	 * point to the same leaf node they'll always be adjacent now:
	 */
	return i != trans->entries &&
		i[0].iter->l[0].b == i[-1].iter->l[0].b;
}

#define trans_for_each_entry(trans, i)					\
	for ((i) = (trans)->entries; (i) < (trans)->entries + (trans)->nr; (i)++)

inline void bch2_btree_node_lock_for_insert(struct bch_fs *c, struct btree *b,
					    struct btree_iter *iter)
{
	bch2_btree_node_lock_write(b, iter);

	if (btree_node_just_written(b) &&
	    bch2_btree_post_write_cleanup(c, b))
		bch2_btree_iter_reinit_node(iter, b);

	/*
	 * If the last bset has been written, or if it's gotten too big - start
	 * a new bset to insert into:
	 */
	if (want_new_bset(c, b))
		bch2_btree_init_next(c, b, iter);
}

static void multi_lock_write(struct bch_fs *c, struct btree_insert *trans)
{
	struct btree_insert_entry *i;

	trans_for_each_entry(trans, i)
		if (!same_leaf_as_prev(trans, i))
			bch2_btree_node_lock_for_insert(c, i->iter->l[0].b,
							i->iter);
}

static void multi_unlock_write(struct btree_insert *trans)
{
	struct btree_insert_entry *i;

	trans_for_each_entry(trans, i)
		if (!same_leaf_as_prev(trans, i))
			bch2_btree_node_unlock_write(i->iter->l[0].b, i->iter);
}

static inline void btree_trans_sort(struct btree_insert *trans)
{
	int i, end = trans->nr;

	while (--end > 0)
		for (i = 0; i < end; i++)
			if (btree_iter_cmp(trans->entries[i].iter,
					   trans->entries[i + 1].iter) > 0)
				swap(trans->entries[i], trans->entries[i + 1]);
}

/* Normal update interface: */

/**
 * __bch_btree_insert_at - insert keys at given iterator positions
 *
 * This is main entry point for btree updates.
 *
 * Return values:
 * -EINTR: locking changed, this function should be called again. Only returned
 *  if passed BTREE_INSERT_ATOMIC.
 * -EROFS: filesystem read only
 * -EIO: journal or btree node IO error
 */
int __bch2_btree_insert_at(struct btree_insert *trans)
{
	struct bch_fs *c = trans->c;
	struct btree_insert_entry *i;
	struct btree_iter *split = NULL;
	bool cycle_gc_lock = false;
	unsigned u64s;
	int ret;

	trans_for_each_entry(trans, i) {
		BUG_ON(i->iter->level);
		BUG_ON(bkey_cmp(bkey_start_pos(&i->k->k), i->iter->pos));
		BUG_ON(debug_check_bkeys(c) &&
		       bch2_bkey_invalid(c, i->iter->btree_id,
					 bkey_i_to_s_c(i->k)));
	}

	btree_trans_sort(trans);

	if (unlikely(!percpu_ref_tryget(&c->writes)))
		return -EROFS;
retry_locks:
	ret = -EINTR;
	trans_for_each_entry(trans, i) {
		if (!bch2_btree_iter_set_locks_want(i->iter, 1))
			goto err;

		if (i->iter->uptodate == BTREE_ITER_NEED_TRAVERSE) {
			ret = bch2_btree_iter_traverse(i->iter);
			if (ret)
				goto err;
		}
	}
retry:
	trans->did_work = false;
	u64s = 0;
	trans_for_each_entry(trans, i)
		if (!i->done)
			u64s += jset_u64s(i->k->k.u64s + i->extra_res);

	memset(&trans->journal_res, 0, sizeof(trans->journal_res));

	ret = !(trans->flags & BTREE_INSERT_JOURNAL_REPLAY)
		? bch2_journal_res_get(&c->journal,
				      &trans->journal_res,
				      u64s, u64s)
		: 0;
	if (ret)
		goto err;

	multi_lock_write(c, trans);

	if (race_fault()) {
		ret = -EINTR;
		goto unlock;
	}

	u64s = 0;
	trans_for_each_entry(trans, i) {
		/* Multiple inserts might go to same leaf: */
		if (!same_leaf_as_prev(trans, i))
			u64s = 0;

		/*
		 * bch2_btree_node_insert_fits() must be called under write lock:
		 * with only an intent lock, another thread can still call
		 * bch2_btree_node_write(), converting an unwritten bset to a
		 * written one
		 */
		if (!i->done) {
			u64s += i->k->k.u64s + i->extra_res;
			if (!bch2_btree_node_insert_fits(c,
					i->iter->l[0].b, u64s)) {
				split = i->iter;
				goto unlock;
			}
		}
	}

	ret = 0;
	split = NULL;
	cycle_gc_lock = false;

	trans_for_each_entry(trans, i) {
		if (i->done)
			continue;

		switch (btree_insert_key_leaf(trans, i)) {
		case BTREE_INSERT_OK:
			i->done = true;
			break;
		case BTREE_INSERT_JOURNAL_RES_FULL:
		case BTREE_INSERT_NEED_TRAVERSE:
			ret = -EINTR;
			break;
		case BTREE_INSERT_NEED_RESCHED:
			ret = -EAGAIN;
			break;
		case BTREE_INSERT_BTREE_NODE_FULL:
			split = i->iter;
			break;
		case BTREE_INSERT_ENOSPC:
			ret = -ENOSPC;
			break;
		case BTREE_INSERT_NEED_GC_LOCK:
			cycle_gc_lock = true;
			ret = -EINTR;
			break;
		default:
			BUG();
		}

		if (!trans->did_work && (ret || split))
			break;
	}
unlock:
	multi_unlock_write(trans);
	bch2_journal_res_put(&c->journal, &trans->journal_res);

	if (split)
		goto split;
	if (ret)
		goto err;

	trans_for_each_entry(trans, i)
		if (i->iter->flags & BTREE_ITER_AT_END_OF_LEAF)
			goto out;

	trans_for_each_entry(trans, i) {
		/*
		 * iterators are inconsistent when they hit end of leaf, until
		 * traversed again
		 */
		if (i->iter->uptodate < BTREE_ITER_NEED_TRAVERSE &&
		    !same_leaf_as_prev(trans, i))
			bch2_foreground_maybe_merge(c, i->iter, 0);
	}
out:
	/* make sure we didn't lose an error: */
	if (!ret && IS_ENABLED(CONFIG_BCACHEFS_DEBUG))
		trans_for_each_entry(trans, i)
			BUG_ON(!i->done);

	percpu_ref_put(&c->writes);
	return ret;
split:
	/*
	 * have to drop journal res before splitting, because splitting means
	 * allocating new btree nodes, and holding a journal reservation
	 * potentially blocks the allocator:
	 */
	ret = bch2_btree_split_leaf(c, split, trans->flags);
	if (ret)
		goto err;
	/*
	 * if the split didn't have to drop locks the insert will still be
	 * atomic (in the BTREE_INSERT_ATOMIC sense, what the caller peeked()
	 * and is overwriting won't have changed)
	 */
	goto retry_locks;
err:
	if (cycle_gc_lock) {
		down_read(&c->gc_lock);
		up_read(&c->gc_lock);
	}

	if (ret == -EINTR) {
		trans_for_each_entry(trans, i) {
			int ret2 = bch2_btree_iter_traverse(i->iter);
			if (ret2) {
				ret = ret2;
				goto out;
			}
		}

		/*
		 * BTREE_ITER_ATOMIC means we have to return -EINTR if we
		 * dropped locks:
		 */
		if (!(trans->flags & BTREE_INSERT_ATOMIC))
			goto retry;
	}

	goto out;
}

int bch2_btree_delete_at(struct btree_iter *iter, unsigned flags)
{
	struct bkey_i k;

	bkey_init(&k.k);
	k.k.p = iter->pos;

	return bch2_btree_insert_at(iter->c, NULL, NULL, NULL,
				    BTREE_INSERT_NOFAIL|
				    BTREE_INSERT_USE_RESERVE|flags,
				    BTREE_INSERT_ENTRY(iter, &k));
}

int bch2_btree_insert_list_at(struct btree_iter *iter,
			     struct keylist *keys,
			     struct disk_reservation *disk_res,
			     struct extent_insert_hook *hook,
			     u64 *journal_seq, unsigned flags)
{
	BUG_ON(flags & BTREE_INSERT_ATOMIC);
	BUG_ON(bch2_keylist_empty(keys));
	bch2_verify_keylist_sorted(keys);

	while (!bch2_keylist_empty(keys)) {
		int ret = bch2_btree_insert_at(iter->c, disk_res, hook,
				journal_seq, flags,
				BTREE_INSERT_ENTRY(iter, bch2_keylist_front(keys)));
		if (ret)
			return ret;

		bch2_keylist_pop_front(keys);
	}

	return 0;
}

/**
 * bch_btree_insert - insert keys into the extent btree
 * @c:			pointer to struct bch_fs
 * @id:			btree to insert into
 * @insert_keys:	list of keys to insert
 * @hook:		insert callback
 */
int bch2_btree_insert(struct bch_fs *c, enum btree_id id,
		     struct bkey_i *k,
		     struct disk_reservation *disk_res,
		     struct extent_insert_hook *hook,
		     u64 *journal_seq, int flags)
{
	struct btree_iter iter;
	int ret;

	bch2_btree_iter_init(&iter, c, id, bkey_start_pos(&k->k),
			     BTREE_ITER_INTENT);
	ret = bch2_btree_insert_at(c, disk_res, hook, journal_seq, flags,
				  BTREE_INSERT_ENTRY(&iter, k));
	bch2_btree_iter_unlock(&iter);

	return ret;
}

/*
 * bch_btree_delete_range - delete everything within a given range
 *
 * Range is a half open interval - [start, end)
 */
int bch2_btree_delete_range(struct bch_fs *c, enum btree_id id,
			   struct bpos start,
			   struct bpos end,
			   struct bversion version,
			   struct disk_reservation *disk_res,
			   struct extent_insert_hook *hook,
			   u64 *journal_seq)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret = 0;

	bch2_btree_iter_init(&iter, c, id, start,
			     BTREE_ITER_INTENT);

	while ((k = bch2_btree_iter_peek(&iter)).k &&
	       !(ret = btree_iter_err(k))) {
		unsigned max_sectors = KEY_SIZE_MAX & (~0 << c->block_bits);
		/* really shouldn't be using a bare, unpadded bkey_i */
		struct bkey_i delete;

		if (bkey_cmp(iter.pos, end) >= 0)
			break;

		bkey_init(&delete.k);

		/*
		 * For extents, iter.pos won't necessarily be the same as
		 * bkey_start_pos(k.k) (for non extents they always will be the
		 * same). It's important that we delete starting from iter.pos
		 * because the range we want to delete could start in the middle
		 * of k.
		 *
		 * (bch2_btree_iter_peek() does guarantee that iter.pos >=
		 * bkey_start_pos(k.k)).
		 */
		delete.k.p = iter.pos;
		delete.k.version = version;

		if (iter.flags & BTREE_ITER_IS_EXTENTS) {
			/*
			 * The extents btree is special - KEY_TYPE_DISCARD is
			 * used for deletions, not KEY_TYPE_DELETED. This is an
			 * internal implementation detail that probably
			 * shouldn't be exposed (internally, KEY_TYPE_DELETED is
			 * used as a proxy for k->size == 0):
			 */
			delete.k.type = KEY_TYPE_DISCARD;

			/* create the biggest key we can */
			bch2_key_resize(&delete.k, max_sectors);
			bch2_cut_back(end, &delete.k);
		}

		ret = bch2_btree_insert_at(c, disk_res, hook, journal_seq,
					  BTREE_INSERT_NOFAIL,
					  BTREE_INSERT_ENTRY(&iter, &delete));
		if (ret)
			break;

		bch2_btree_iter_cond_resched(&iter);
	}

	bch2_btree_iter_unlock(&iter);
	return ret;
}
