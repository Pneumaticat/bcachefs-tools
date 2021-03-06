/*
 * Some low level IO code, and hacks for various block layer limitations
 *
 * Copyright 2010, 2011 Kent Overstreet <kent.overstreet@gmail.com>
 * Copyright 2012 Google, Inc.
 */

#include "bcachefs.h"
#include "alloc.h"
#include "bset.h"
#include "btree_update.h"
#include "buckets.h"
#include "checksum.h"
#include "compress.h"
#include "clock.h"
#include "debug.h"
#include "error.h"
#include "extents.h"
#include "io.h"
#include "journal.h"
#include "keylist.h"
#include "move.h"
#include "super.h"
#include "super-io.h"

#include <linux/blkdev.h>
#include <linux/random.h>

#include <trace/events/bcachefs.h>

/* Allocate, free from mempool: */

void bch2_latency_acct(struct bch_dev *ca, unsigned submit_time_us, int rw)
{
	u64 now = local_clock();
	unsigned io_latency = (now >> 10) - submit_time_us;
	atomic_t *latency = &ca->latency[rw];
	unsigned old, new, v = atomic_read(latency);

	do {
		old = v;

		/*
		 * If the io latency was reasonably close to the current
		 * latency, skip doing the update and atomic operation - most of
		 * the time:
		 */
		if (abs((int) (old - io_latency)) < (old >> 1) &&
		    now & ~(~0 << 5))
			break;

		new = ewma_add((u64) old, io_latency, 6);
	} while ((v = atomic_cmpxchg(latency, old, new)) != old);
}

void bch2_bio_free_pages_pool(struct bch_fs *c, struct bio *bio)
{
	struct bio_vec *bv;
	unsigned i;

	bio_for_each_segment_all(bv, bio, i)
		if (bv->bv_page != ZERO_PAGE(0))
			mempool_free(bv->bv_page, &c->bio_bounce_pages);
	bio->bi_vcnt = 0;
}

static void bch2_bio_alloc_page_pool(struct bch_fs *c, struct bio *bio,
				    bool *using_mempool)
{
	struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt++];

	if (likely(!*using_mempool)) {
		bv->bv_page = alloc_page(GFP_NOIO);
		if (unlikely(!bv->bv_page)) {
			mutex_lock(&c->bio_bounce_pages_lock);
			*using_mempool = true;
			goto pool_alloc;

		}
	} else {
pool_alloc:
		bv->bv_page = mempool_alloc(&c->bio_bounce_pages, GFP_NOIO);
	}

	bv->bv_len = PAGE_SIZE;
	bv->bv_offset = 0;
}

void bch2_bio_alloc_pages_pool(struct bch_fs *c, struct bio *bio,
			       size_t bytes)
{
	bool using_mempool = false;

	BUG_ON(DIV_ROUND_UP(bytes, PAGE_SIZE) > bio->bi_max_vecs);

	bio->bi_iter.bi_size = bytes;

	while (bio->bi_vcnt < DIV_ROUND_UP(bytes, PAGE_SIZE))
		bch2_bio_alloc_page_pool(c, bio, &using_mempool);

	if (using_mempool)
		mutex_unlock(&c->bio_bounce_pages_lock);
}

void bch2_bio_alloc_more_pages_pool(struct bch_fs *c, struct bio *bio,
				    size_t bytes)
{
	while (bio->bi_vcnt < DIV_ROUND_UP(bytes, PAGE_SIZE)) {
		struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt];

		BUG_ON(bio->bi_vcnt >= bio->bi_max_vecs);

		bv->bv_page = alloc_page(GFP_NOIO);
		if (!bv->bv_page) {
			/*
			 * We already allocated from mempool, we can't allocate from it again
			 * without freeing the pages we already allocated or else we could
			 * deadlock:
			 */
			bch2_bio_free_pages_pool(c, bio);
			bch2_bio_alloc_pages_pool(c, bio, bytes);
			return;
		}

		bv->bv_len = PAGE_SIZE;
		bv->bv_offset = 0;
		bio->bi_vcnt++;
	}

	bio->bi_iter.bi_size = bytes;
}

/* Writes */

void bch2_submit_wbio_replicas(struct bch_write_bio *wbio, struct bch_fs *c,
			       enum bch_data_type type,
			       const struct bkey_i *k)
{
	struct bkey_s_c_extent e = bkey_i_to_s_c_extent(k);
	const struct bch_extent_ptr *ptr;
	struct bch_write_bio *n;
	struct bch_dev *ca;

	BUG_ON(c->opts.nochanges);

	extent_for_each_ptr(e, ptr) {
		BUG_ON(ptr->dev >= BCH_SB_MEMBERS_MAX ||
		       !c->devs[ptr->dev]);

		ca = bch_dev_bkey_exists(c, ptr->dev);

		if (ptr + 1 < &extent_entry_last(e)->ptr) {
			n = to_wbio(bio_clone_fast(&wbio->bio, GFP_NOIO,
						   &ca->replica_set));

			n->bio.bi_end_io	= wbio->bio.bi_end_io;
			n->bio.bi_private	= wbio->bio.bi_private;
			n->parent		= wbio;
			n->split		= true;
			n->bounce		= false;
			n->put_bio		= true;
			n->bio.bi_opf		= wbio->bio.bi_opf;
			bio_inc_remaining(&wbio->bio);
		} else {
			n = wbio;
			n->split		= false;
		}

		n->c			= c;
		n->ca			= ca;
		n->submit_time_us	= local_clock_us();
		n->bio.bi_iter.bi_sector = ptr->offset;

		if (!journal_flushes_device(ca))
			n->bio.bi_opf |= REQ_FUA;

		if (likely(percpu_ref_tryget(&ca->io_ref))) {
			this_cpu_add(ca->io_done->sectors[WRITE][type],
				     bio_sectors(&n->bio));

			n->have_io_ref		= true;
			bio_set_dev(&n->bio, ca->disk_sb.bdev);
			submit_bio(&n->bio);
		} else {
			n->have_io_ref		= false;
			n->bio.bi_status	= BLK_STS_REMOVED;
			bio_endio(&n->bio);
		}
	}
}

static void __bch2_write(struct closure *);

static void bch2_write_done(struct closure *cl)
{
	struct bch_write_op *op = container_of(cl, struct bch_write_op, cl);

	BUG_ON(!(op->flags & BCH_WRITE_DONE));

	if (!op->error && (op->flags & BCH_WRITE_FLUSH))
		op->error = bch2_journal_error(&op->c->journal);

	if (!(op->flags & BCH_WRITE_NOPUT_RESERVATION))
		bch2_disk_reservation_put(op->c, &op->res);
	percpu_ref_put(&op->c->writes);
	bch2_keylist_free(&op->insert_keys, op->inline_keys);
	op->flags &= ~(BCH_WRITE_DONE|BCH_WRITE_LOOPED);

	closure_return(cl);
}

static u64 keylist_sectors(struct keylist *keys)
{
	struct bkey_i *k;
	u64 ret = 0;

	for_each_keylist_key(keys, k)
		ret += k->k.size;

	return ret;
}

int bch2_write_index_default(struct bch_write_op *op)
{
	struct keylist *keys = &op->insert_keys;
	struct btree_iter iter;
	int ret;

	bch2_btree_iter_init(&iter, op->c, BTREE_ID_EXTENTS,
			     bkey_start_pos(&bch2_keylist_front(keys)->k),
			     BTREE_ITER_INTENT);

	ret = bch2_btree_insert_list_at(&iter, keys, &op->res,
				       NULL, op_journal_seq(op),
				       BTREE_INSERT_NOFAIL);
	bch2_btree_iter_unlock(&iter);

	return ret;
}

/**
 * bch_write_index - after a write, update index to point to new data
 */
static void bch2_write_index(struct closure *cl)
{
	struct bch_write_op *op = container_of(cl, struct bch_write_op, cl);
	struct bch_fs *c = op->c;
	struct keylist *keys = &op->insert_keys;
	struct bkey_s_extent e;
	struct bch_extent_ptr *ptr;
	struct bkey_i *src, *dst = keys->keys, *n;
	int ret;

	op->flags |= BCH_WRITE_LOOPED;

	for (src = keys->keys; src != keys->top; src = n) {
		n = bkey_next(src);
		bkey_copy(dst, src);

		e = bkey_i_to_s_extent(dst);
		extent_for_each_ptr_backwards(e, ptr)
			if (test_bit(ptr->dev, op->failed.d))
				bch2_extent_drop_ptr(e, ptr);

		if (!bch2_extent_nr_ptrs(e.c)) {
			ret = -EIO;
			goto err;
		}

		if (!(op->flags & BCH_WRITE_NOMARK_REPLICAS)) {
			ret = bch2_check_mark_super(c, BCH_DATA_USER,
						    bch2_extent_devs(e.c));
			if (ret)
				goto err;
		}

		dst = bkey_next(dst);
	}

	keys->top = dst;

	if (!bch2_keylist_empty(keys)) {
		u64 sectors_start = keylist_sectors(keys);
		int ret = op->index_update_fn(op);

		BUG_ON(keylist_sectors(keys) && !ret);

		op->written += sectors_start - keylist_sectors(keys);

		if (ret) {
			__bcache_io_error(c, "btree IO error %i", ret);
			op->error = ret;
		}
	}
out:
	bch2_open_bucket_put_refs(c, &op->open_buckets_nr, op->open_buckets);

	if (!(op->flags & BCH_WRITE_DONE))
		continue_at(cl, __bch2_write, op->io_wq);

	if (!op->error && (op->flags & BCH_WRITE_FLUSH)) {
		bch2_journal_flush_seq_async(&c->journal,
					     *op_journal_seq(op),
					     cl);
		continue_at(cl, bch2_write_done, index_update_wq(op));
	} else {
		continue_at_nobarrier(cl, bch2_write_done, NULL);
	}
	return;
err:
	keys->top = keys->keys;
	op->error = ret;
	op->flags |= BCH_WRITE_DONE;
	goto out;
}

static void bch2_write_endio(struct bio *bio)
{
	struct closure *cl		= bio->bi_private;
	struct bch_write_op *op		= container_of(cl, struct bch_write_op, cl);
	struct bch_write_bio *wbio	= to_wbio(bio);
	struct bch_write_bio *parent	= wbio->split ? wbio->parent : NULL;
	struct bch_fs *c		= wbio->c;
	struct bch_dev *ca		= wbio->ca;

	bch2_latency_acct(ca, wbio->submit_time_us, WRITE);

	if (bch2_dev_io_err_on(bio->bi_status, ca, "data write"))
		set_bit(ca->dev_idx, op->failed.d);

	if (wbio->have_io_ref)
		percpu_ref_put(&ca->io_ref);

	if (wbio->bounce)
		bch2_bio_free_pages_pool(c, bio);

	if (wbio->put_bio)
		bio_put(bio);

	if (parent)
		bio_endio(&parent->bio);
	else
		closure_put(cl);
}

static void init_append_extent(struct bch_write_op *op,
			       struct write_point *wp,
			       struct bversion version,
			       struct bch_extent_crc_unpacked crc)
{
	struct bkey_i_extent *e = bkey_extent_init(op->insert_keys.top);

	op->pos.offset += crc.uncompressed_size;
	e->k.p = op->pos;
	e->k.size = crc.uncompressed_size;
	e->k.version = version;
	bkey_extent_set_cached(&e->k, op->flags & BCH_WRITE_CACHED);

	bch2_extent_crc_append(e, crc);
	bch2_alloc_sectors_append_ptrs(op->c, wp, e, crc.compressed_size);

	bch2_keylist_push(&op->insert_keys);
}

static struct bio *bch2_write_bio_alloc(struct bch_fs *c,
					struct write_point *wp,
					struct bio *src,
					bool *page_alloc_failed)
{
	struct bch_write_bio *wbio;
	struct bio *bio;
	unsigned output_available =
		min(wp->sectors_free << 9, src->bi_iter.bi_size);
	unsigned pages = DIV_ROUND_UP(output_available, PAGE_SIZE);

	bio = bio_alloc_bioset(GFP_NOIO, pages, &c->bio_write);
	wbio			= wbio_init(bio);
	wbio->bounce		= true;
	wbio->put_bio		= true;
	/* copy WRITE_SYNC flag */
	wbio->bio.bi_opf	= src->bi_opf;

	/*
	 * We can't use mempool for more than c->sb.encoded_extent_max
	 * worth of pages, but we'd like to allocate more if we can:
	 */
	while (bio->bi_iter.bi_size < output_available) {
		unsigned len = min_t(unsigned, PAGE_SIZE,
				     output_available - bio->bi_iter.bi_size);
		struct page *p;

		p = alloc_page(GFP_NOIO);
		if (!p) {
			unsigned pool_max =
				min_t(unsigned, output_available,
				      c->sb.encoded_extent_max << 9);

			if (bio_sectors(bio) < pool_max)
				bch2_bio_alloc_pages_pool(c, bio, pool_max);
			break;
		}

		bio->bi_io_vec[bio->bi_vcnt++] = (struct bio_vec) {
			.bv_page	= p,
			.bv_len		= len,
			.bv_offset	= 0,
		};
		bio->bi_iter.bi_size += len;
	}

	*page_alloc_failed = bio->bi_vcnt < pages;
	return bio;
}

static int bch2_write_rechecksum(struct bch_fs *c,
				 struct bch_write_op *op,
				 unsigned new_csum_type)
{
	struct bio *bio = &op->wbio.bio;
	struct bch_extent_crc_unpacked new_crc;
	int ret;

	/* bch2_rechecksum_bio() can't encrypt or decrypt data: */

	if (bch2_csum_type_is_encryption(op->crc.csum_type) !=
	    bch2_csum_type_is_encryption(new_csum_type))
		new_csum_type = op->crc.csum_type;

	ret = bch2_rechecksum_bio(c, bio, op->version, op->crc,
				  NULL, &new_crc,
				  op->crc.offset, op->crc.live_size,
				  new_csum_type);
	if (ret)
		return ret;

	bio_advance(bio, op->crc.offset << 9);
	bio->bi_iter.bi_size = op->crc.live_size << 9;
	op->crc = new_crc;
	return 0;
}

static int bch2_write_decrypt(struct bch_write_op *op)
{
	struct bch_fs *c = op->c;
	struct nonce nonce = extent_nonce(op->version, op->crc);
	struct bch_csum csum;

	if (!bch2_csum_type_is_encryption(op->crc.csum_type))
		return 0;

	/*
	 * If we need to decrypt data in the write path, we'll no longer be able
	 * to verify the existing checksum (poly1305 mac, in this case) after
	 * it's decrypted - this is the last point we'll be able to reverify the
	 * checksum:
	 */
	csum = bch2_checksum_bio(c, op->crc.csum_type, nonce, &op->wbio.bio);
	if (bch2_crc_cmp(op->crc.csum, csum))
		return -EIO;

	bch2_encrypt_bio(c, op->crc.csum_type, nonce, &op->wbio.bio);
	op->crc.csum_type = 0;
	op->crc.csum = (struct bch_csum) { 0, 0 };
	return 0;
}

static enum prep_encoded_ret {
	PREP_ENCODED_OK,
	PREP_ENCODED_ERR,
	PREP_ENCODED_CHECKSUM_ERR,
	PREP_ENCODED_DO_WRITE,
} bch2_write_prep_encoded_data(struct bch_write_op *op, struct write_point *wp)
{
	struct bch_fs *c = op->c;
	struct bio *bio = &op->wbio.bio;

	if (!(op->flags & BCH_WRITE_DATA_ENCODED))
		return PREP_ENCODED_OK;

	BUG_ON(bio_sectors(bio) != op->crc.compressed_size);

	/* Can we just write the entire extent as is? */
	if (op->crc.uncompressed_size == op->crc.live_size &&
	    op->crc.compressed_size <= wp->sectors_free &&
	    op->crc.compression_type == op->compression_type) {
		if (!op->crc.compression_type &&
		    op->csum_type != op->crc.csum_type &&
		    bch2_write_rechecksum(c, op, op->csum_type))
			return PREP_ENCODED_CHECKSUM_ERR;

		return PREP_ENCODED_DO_WRITE;
	}

	/*
	 * If the data is compressed and we couldn't write the entire extent as
	 * is, we have to decompress it:
	 */
	if (op->crc.compression_type) {
		struct bch_csum csum;

		if (bch2_write_decrypt(op))
			return PREP_ENCODED_CHECKSUM_ERR;

		/* Last point we can still verify checksum: */
		csum = bch2_checksum_bio(c, op->crc.csum_type,
					 extent_nonce(op->version, op->crc),
					 bio);
		if (bch2_crc_cmp(op->crc.csum, csum))
			return PREP_ENCODED_CHECKSUM_ERR;

		if (bch2_bio_uncompress_inplace(c, bio, &op->crc))
			return PREP_ENCODED_ERR;
	}

	/*
	 * No longer have compressed data after this point - data might be
	 * encrypted:
	 */

	/*
	 * If the data is checksummed and we're only writing a subset,
	 * rechecksum and adjust bio to point to currently live data:
	 */
	if ((op->crc.live_size != op->crc.uncompressed_size ||
	     op->crc.csum_type != op->csum_type) &&
	    bch2_write_rechecksum(c, op, op->csum_type))
		return PREP_ENCODED_CHECKSUM_ERR;

	/*
	 * If we want to compress the data, it has to be decrypted:
	 */
	if ((op->compression_type ||
	     bch2_csum_type_is_encryption(op->crc.csum_type) !=
	     bch2_csum_type_is_encryption(op->csum_type)) &&
	    bch2_write_decrypt(op))
		return PREP_ENCODED_CHECKSUM_ERR;

	return PREP_ENCODED_OK;
}

static int bch2_write_extent(struct bch_write_op *op, struct write_point *wp)
{
	struct bch_fs *c = op->c;
	struct bio *src = &op->wbio.bio, *dst = src;
	struct bvec_iter saved_iter;
	struct bkey_i *key_to_write;
	unsigned key_to_write_offset = op->insert_keys.top_p -
		op->insert_keys.keys_p;
	unsigned total_output = 0;
	bool bounce = false, page_alloc_failed = false;
	int ret, more = 0;

	BUG_ON(!bio_sectors(src));

	switch (bch2_write_prep_encoded_data(op, wp)) {
	case PREP_ENCODED_OK:
		break;
	case PREP_ENCODED_ERR:
		ret = -EIO;
		goto err;
	case PREP_ENCODED_CHECKSUM_ERR:
		goto csum_err;
	case PREP_ENCODED_DO_WRITE:
		init_append_extent(op, wp, op->version, op->crc);
		goto do_write;
	}

	if (op->compression_type ||
	    (op->csum_type &&
	     !(op->flags & BCH_WRITE_PAGES_STABLE)) ||
	    (bch2_csum_type_is_encryption(op->csum_type) &&
	     !(op->flags & BCH_WRITE_PAGES_OWNED))) {
		dst = bch2_write_bio_alloc(c, wp, src, &page_alloc_failed);
		bounce = true;
	}

	saved_iter = dst->bi_iter;

	do {
		struct bch_extent_crc_unpacked crc =
			(struct bch_extent_crc_unpacked) { 0 };
		struct bversion version = op->version;
		size_t dst_len, src_len;

		if (page_alloc_failed &&
		    bio_sectors(dst) < wp->sectors_free &&
		    bio_sectors(dst) < c->sb.encoded_extent_max)
			break;

		BUG_ON(op->compression_type &&
		       (op->flags & BCH_WRITE_DATA_ENCODED) &&
		       bch2_csum_type_is_encryption(op->crc.csum_type));
		BUG_ON(op->compression_type && !bounce);

		crc.compression_type = op->compression_type
			?  bch2_bio_compress(c, dst, &dst_len, src, &src_len,
					     op->compression_type)
			: 0;
		if (!crc.compression_type) {
			dst_len = min(dst->bi_iter.bi_size, src->bi_iter.bi_size);
			dst_len = min_t(unsigned, dst_len, wp->sectors_free << 9);

			if (op->csum_type)
				dst_len = min_t(unsigned, dst_len,
						c->sb.encoded_extent_max << 9);

			if (bounce) {
				swap(dst->bi_iter.bi_size, dst_len);
				bio_copy_data(dst, src);
				swap(dst->bi_iter.bi_size, dst_len);
			}

			src_len = dst_len;
		}

		BUG_ON(!src_len || !dst_len);

		if (bch2_csum_type_is_encryption(op->csum_type)) {
			if (bversion_zero(version)) {
				version.lo = atomic64_inc_return(&c->key_version) + 1;
			} else {
				crc.nonce = op->nonce;
				op->nonce += src_len >> 9;
			}
		}

		if ((op->flags & BCH_WRITE_DATA_ENCODED) &&
		    !crc.compression_type &&
		    bch2_csum_type_is_encryption(op->crc.csum_type) ==
		    bch2_csum_type_is_encryption(op->csum_type)) {
			/*
			 * Note: when we're using rechecksum(), we need to be
			 * checksumming @src because it has all the data our
			 * existing checksum covers - if we bounced (because we
			 * were trying to compress), @dst will only have the
			 * part of the data the new checksum will cover.
			 *
			 * But normally we want to be checksumming post bounce,
			 * because part of the reason for bouncing is so the
			 * data can't be modified (by userspace) while it's in
			 * flight.
			 */
			if (bch2_rechecksum_bio(c, src, version, op->crc,
					&crc, &op->crc,
					src_len >> 9,
					bio_sectors(src) - (src_len >> 9),
					op->csum_type))
				goto csum_err;
		} else {
			if ((op->flags & BCH_WRITE_DATA_ENCODED) &&
			    bch2_rechecksum_bio(c, src, version, op->crc,
					NULL, &op->crc,
					src_len >> 9,
					bio_sectors(src) - (src_len >> 9),
					op->crc.csum_type))
				goto csum_err;

			crc.compressed_size	= dst_len >> 9;
			crc.uncompressed_size	= src_len >> 9;
			crc.live_size		= src_len >> 9;

			swap(dst->bi_iter.bi_size, dst_len);
			bch2_encrypt_bio(c, op->csum_type,
					 extent_nonce(version, crc), dst);
			crc.csum = bch2_checksum_bio(c, op->csum_type,
					 extent_nonce(version, crc), dst);
			crc.csum_type = op->csum_type;
			swap(dst->bi_iter.bi_size, dst_len);
		}

		init_append_extent(op, wp, version, crc);

		if (dst != src)
			bio_advance(dst, dst_len);
		bio_advance(src, src_len);
		total_output += dst_len;
	} while (dst->bi_iter.bi_size &&
		 src->bi_iter.bi_size &&
		 wp->sectors_free &&
		 !bch2_keylist_realloc(&op->insert_keys,
				      op->inline_keys,
				      ARRAY_SIZE(op->inline_keys),
				      BKEY_EXTENT_U64s_MAX));

	more = src->bi_iter.bi_size != 0;

	dst->bi_iter = saved_iter;

	if (!bounce && more) {
		dst = bio_split(src, total_output >> 9,
				GFP_NOIO, &c->bio_write);
		wbio_init(dst)->put_bio = true;
	}

	dst->bi_iter.bi_size = total_output;

	/* Free unneeded pages after compressing: */
	if (bounce)
		while (dst->bi_vcnt > DIV_ROUND_UP(dst->bi_iter.bi_size, PAGE_SIZE))
			mempool_free(dst->bi_io_vec[--dst->bi_vcnt].bv_page,
				     &c->bio_bounce_pages);
do_write:
	/* might have done a realloc... */

	key_to_write = (void *) (op->insert_keys.keys_p + key_to_write_offset);

	dst->bi_end_io	= bch2_write_endio;
	dst->bi_private	= &op->cl;
	bio_set_op_attrs(dst, REQ_OP_WRITE, 0);

	closure_get(dst->bi_private);

	bch2_submit_wbio_replicas(to_wbio(dst), c, BCH_DATA_USER,
				  key_to_write);
	return more;
csum_err:
	bch_err(c, "error verifying existing checksum while "
		"rewriting existing data (memory corruption?)");
	ret = -EIO;
err:
	if (bounce) {
		bch2_bio_free_pages_pool(c, dst);
		bio_put(dst);
	}

	return ret;
}

static void __bch2_write(struct closure *cl)
{
	struct bch_write_op *op = container_of(cl, struct bch_write_op, cl);
	struct bch_fs *c = op->c;
	struct write_point *wp;
	int ret;

	do {
		if (op->open_buckets_nr + op->nr_replicas >
		    ARRAY_SIZE(op->open_buckets))
			continue_at(cl, bch2_write_index, index_update_wq(op));

		/* for the device pointers and 1 for the chksum */
		if (bch2_keylist_realloc(&op->insert_keys,
					op->inline_keys,
					ARRAY_SIZE(op->inline_keys),
					BKEY_EXTENT_U64s_MAX))
			continue_at(cl, bch2_write_index, index_update_wq(op));

		wp = bch2_alloc_sectors_start(c,
			op->devs,
			op->write_point,
			&op->devs_have,
			op->nr_replicas,
			op->nr_replicas_required,
			op->alloc_reserve,
			op->flags,
			(op->flags & BCH_WRITE_ALLOC_NOWAIT) ? NULL : cl);
		EBUG_ON(!wp);

		if (unlikely(IS_ERR(wp))) {
			if (unlikely(PTR_ERR(wp) != -EAGAIN)) {
				ret = PTR_ERR(wp);
				goto err;
			}

			/*
			 * If we already have some keys, must insert them first
			 * before allocating another open bucket. We only hit
			 * this case if open_bucket_nr > 1.
			 */
			if (!bch2_keylist_empty(&op->insert_keys))
				continue_at(cl, bch2_write_index,
					    index_update_wq(op));

			/*
			 * If we've looped, we're running out of a workqueue -
			 * not the bch2_write() caller's context - and we don't
			 * want to block the workqueue:
			 */
			if (op->flags & BCH_WRITE_LOOPED)
				continue_at(cl, __bch2_write, op->io_wq);

			/*
			 * Otherwise, we do want to block the caller on alloc
			 * failure instead of letting it queue up more and more
			 * writes:
			 * XXX: this technically needs a try_to_freeze() -
			 * except that that's not safe because caller may have
			 * issued other IO... hmm..
			 */
			closure_sync(cl);
			continue;
		}

		ret = bch2_write_extent(op, wp);

		BUG_ON(op->open_buckets_nr + wp->nr_ptrs_can_use >
		       ARRAY_SIZE(op->open_buckets));
		bch2_open_bucket_get(c, wp,
				     &op->open_buckets_nr,
				     op->open_buckets);
		bch2_alloc_sectors_done(c, wp);

		if (ret < 0)
			goto err;
	} while (ret);

	op->flags |= BCH_WRITE_DONE;
	continue_at(cl, bch2_write_index, index_update_wq(op));
err:
	/*
	 * Right now we can only error here if we went RO - the
	 * allocation failed, but we already checked for -ENOSPC when we
	 * got our reservation.
	 *
	 * XXX capacity might have changed, but we don't check for that
	 * yet:
	 */
	op->error = ret;
	op->flags |= BCH_WRITE_DONE;

	/*
	 * No reason not to insert keys for whatever data was successfully
	 * written (especially for a cmpxchg operation that's moving data
	 * around)
	 */
	continue_at(cl, !bch2_keylist_empty(&op->insert_keys)
		    ? bch2_write_index
		    : bch2_write_done, index_update_wq(op));
}

/**
 * bch_write - handle a write to a cache device or flash only volume
 *
 * This is the starting point for any data to end up in a cache device; it could
 * be from a normal write, or a writeback write, or a write to a flash only
 * volume - it's also used by the moving garbage collector to compact data in
 * mostly empty buckets.
 *
 * It first writes the data to the cache, creating a list of keys to be inserted
 * (if the data won't fit in a single open bucket, there will be multiple keys);
 * after the data is written it calls bch_journal, and after the keys have been
 * added to the next journal write they're inserted into the btree.
 *
 * If op->discard is true, instead of inserting the data it invalidates the
 * region of the cache represented by op->bio and op->inode.
 */
void bch2_write(struct closure *cl)
{
	struct bch_write_op *op = container_of(cl, struct bch_write_op, cl);
	struct bch_fs *c = op->c;

	BUG_ON(!op->nr_replicas);
	BUG_ON(!op->write_point.v);
	BUG_ON(!bkey_cmp(op->pos, POS_MAX));
	BUG_ON(bio_sectors(&op->wbio.bio) > U16_MAX);

	memset(&op->failed, 0, sizeof(op->failed));

	bch2_keylist_init(&op->insert_keys, op->inline_keys);
	wbio_init(&op->wbio.bio)->put_bio = false;

	if (c->opts.nochanges ||
	    !percpu_ref_tryget(&c->writes)) {
		__bcache_io_error(c, "read only");
		op->error = -EROFS;
		if (!(op->flags & BCH_WRITE_NOPUT_RESERVATION))
			bch2_disk_reservation_put(c, &op->res);
		closure_return(cl);
	}

	bch2_increment_clock(c, bio_sectors(&op->wbio.bio), WRITE);

	continue_at_nobarrier(cl, __bch2_write, NULL);
}

/* Cache promotion on read */

struct promote_op {
	struct closure		cl;
	struct migrate_write	write;
	struct bio_vec		bi_inline_vecs[0]; /* must be last */
};

static void promote_done(struct closure *cl)
{
	struct promote_op *op =
		container_of(cl, struct promote_op, cl);
	struct bch_fs *c = op->write.op.c;

	percpu_ref_put(&c->writes);
	bch2_bio_free_pages_pool(c, &op->write.op.wbio.bio);
	kfree(op);
}

static void promote_start(struct promote_op *op, struct bch_read_bio *rbio)
{
	struct bch_fs *c = rbio->c;
	struct closure *cl = &op->cl;
	struct bio *bio = &op->write.op.wbio.bio;

	BUG_ON(!rbio->split || !rbio->bounce);

	if (!percpu_ref_tryget(&c->writes))
		return;

	trace_promote(&rbio->bio);

	/* we now own pages: */
	BUG_ON(rbio->bio.bi_vcnt > bio->bi_max_vecs);
	swap(bio->bi_vcnt, rbio->bio.bi_vcnt);
	rbio->promote = NULL;

	bch2_write_op_init(&op->write.op, c);
	op->write.op.csum_type = bch2_data_checksum_type(c, rbio->opts.data_checksum);
	op->write.op.compression_type =
		bch2_compression_opt_to_type(rbio->opts.compression);

	op->write.move_dev	= -1;
	op->write.op.devs	= c->fastest_devs;
	op->write.op.write_point = writepoint_hashed((unsigned long) current);
	op->write.op.flags	|= BCH_WRITE_ALLOC_NOWAIT;
	op->write.op.flags	|= BCH_WRITE_CACHED;

	bch2_migrate_write_init(&op->write, rbio);

	closure_init(cl, NULL);
	closure_call(&op->write.op.cl, bch2_write, c->wq, cl);
	closure_return_with_destructor(cl, promote_done);
}

/*
 * XXX: multiple promotes can race with each other, wastefully. Keep a list of
 * outstanding promotes?
 */
static struct promote_op *promote_alloc(struct bch_read_bio *rbio)
{
	struct promote_op *op;
	struct bio *bio;
	/* data might have to be decompressed in the write path: */
	unsigned pages = DIV_ROUND_UP(rbio->pick.crc.uncompressed_size,
				      PAGE_SECTORS);

	BUG_ON(!rbio->bounce);
	BUG_ON(pages < rbio->bio.bi_vcnt);

	op = kzalloc(sizeof(*op) + sizeof(struct bio_vec) * pages,
		     GFP_NOIO);
	if (!op)
		return NULL;

	bio = &op->write.op.wbio.bio;
	bio_init(bio, bio->bi_inline_vecs, pages);

	memcpy(bio->bi_io_vec, rbio->bio.bi_io_vec,
	       sizeof(struct bio_vec) * rbio->bio.bi_vcnt);

	return op;
}

/* only promote if we're not reading from the fastest tier: */
static bool should_promote(struct bch_fs *c,
			   struct extent_pick_ptr *pick, unsigned flags)
{
	if (!(flags & BCH_READ_MAY_PROMOTE))
		return false;

	if (percpu_ref_is_dying(&c->writes))
		return false;

	return c->fastest_tier &&
		c->fastest_tier < c->tiers + pick->ca->mi.tier;
}

/* Read */

static void bch2_read_nodecode_retry(struct bch_fs *, struct bch_read_bio *,
				     struct bvec_iter, u64,
				     struct bch_devs_mask *, unsigned);

#define READ_RETRY_AVOID	1
#define READ_RETRY		2
#define READ_ERR		3

enum rbio_context {
	RBIO_CONTEXT_NULL,
	RBIO_CONTEXT_HIGHPRI,
	RBIO_CONTEXT_UNBOUND,
};

static inline struct bch_read_bio *
bch2_rbio_parent(struct bch_read_bio *rbio)
{
	return rbio->split ? rbio->parent : rbio;
}

__always_inline
static void bch2_rbio_punt(struct bch_read_bio *rbio, work_func_t fn,
			   enum rbio_context context,
			   struct workqueue_struct *wq)
{
	if (context <= rbio->context) {
		fn(&rbio->work);
	} else {
		rbio->work.func		= fn;
		rbio->context		= context;
		queue_work(wq, &rbio->work);
	}
}

static inline struct bch_read_bio *bch2_rbio_free(struct bch_read_bio *rbio)
{
	struct bch_read_bio *parent = rbio->parent;

	BUG_ON(!rbio->split);

	if (rbio->promote)
		kfree(rbio->promote);
	if (rbio->bounce)
		bch2_bio_free_pages_pool(rbio->c, &rbio->bio);
	bio_put(&rbio->bio);

	return parent;
}

static void bch2_rbio_done(struct bch_read_bio *rbio)
{
	if (rbio->promote)
		kfree(rbio->promote);
	rbio->promote = NULL;

	if (rbio->split)
		rbio = bch2_rbio_free(rbio);
	bio_endio(&rbio->bio);
}

static void bch2_rbio_retry(struct work_struct *work)
{
	struct bch_read_bio *rbio =
		container_of(work, struct bch_read_bio, work);
	struct bch_fs *c		= rbio->c;
	struct bvec_iter iter		= rbio->bvec_iter;
	unsigned flags			= rbio->flags;
	u64 inode			= rbio->pos.inode;
	struct bch_devs_mask avoid;

	trace_read_retry(&rbio->bio);

	memset(&avoid, 0, sizeof(avoid));

	if (rbio->retry == READ_RETRY_AVOID)
		__set_bit(rbio->pick.ca->dev_idx, avoid.d);

	if (rbio->promote)
		kfree(rbio->promote);
	rbio->promote = NULL;

	if (rbio->split)
		rbio = bch2_rbio_free(rbio);
	else
		rbio->bio.bi_status = 0;

	if (!(flags & BCH_READ_NODECODE))
		flags |= BCH_READ_MUST_CLONE;
	flags |= BCH_READ_IN_RETRY;
	flags &= ~BCH_READ_MAY_PROMOTE;

	if (flags & BCH_READ_NODECODE)
		bch2_read_nodecode_retry(c, rbio, iter, inode, &avoid, flags);
	else
		__bch2_read(c, rbio, iter, inode, &avoid, flags);
}

static void bch2_rbio_error(struct bch_read_bio *rbio, int retry,
			    blk_status_t error)
{
	rbio->retry = retry;

	if (rbio->flags & BCH_READ_IN_RETRY)
		return;

	if (retry == READ_ERR) {
		bch2_rbio_parent(rbio)->bio.bi_status = error;
		bch2_rbio_done(rbio);
	} else {
		bch2_rbio_punt(rbio, bch2_rbio_retry,
			       RBIO_CONTEXT_UNBOUND, system_unbound_wq);
	}
}

static void bch2_rbio_narrow_crcs(struct bch_read_bio *rbio)
{
	struct bch_fs *c = rbio->c;
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bkey_i_extent *e;
	BKEY_PADDED(k) new;
	struct bch_extent_crc_unpacked new_crc;
	unsigned offset;
	int ret;

	if (rbio->pick.crc.compression_type)
		return;

	bch2_btree_iter_init(&iter, c, BTREE_ID_EXTENTS, rbio->pos,
			     BTREE_ITER_INTENT);
retry:
	k = bch2_btree_iter_peek(&iter);
	if (IS_ERR_OR_NULL(k.k))
		goto out;

	if (!bkey_extent_is_data(k.k))
		goto out;

	bkey_reassemble(&new.k, k);
	e = bkey_i_to_extent(&new.k);

	if (!bch2_extent_matches_ptr(c, extent_i_to_s_c(e),
				     rbio->pick.ptr,
				     rbio->pos.offset -
				     rbio->pick.crc.offset) ||
	    bversion_cmp(e->k.version, rbio->version))
		goto out;

	/* Extent was merged? */
	if (bkey_start_offset(&e->k) < rbio->pos.offset ||
	    e->k.p.offset > rbio->pos.offset + rbio->pick.crc.uncompressed_size)
		goto out;

	/* The extent might have been partially overwritten since we read it: */
	offset = rbio->pick.crc.offset + (bkey_start_offset(&e->k) - rbio->pos.offset);

	if (bch2_rechecksum_bio(c, &rbio->bio, rbio->version,
				rbio->pick.crc, NULL, &new_crc,
				offset, e->k.size,
				rbio->pick.crc.csum_type)) {
		bch_err(c, "error verifying existing checksum while narrowing checksum (memory corruption?)");
		goto out;
	}

	if (!bch2_extent_narrow_crcs(e, new_crc))
		goto out;

	ret = bch2_btree_insert_at(c, NULL, NULL, NULL,
				   BTREE_INSERT_ATOMIC|
				   BTREE_INSERT_NOFAIL|
				   BTREE_INSERT_NOWAIT,
				   BTREE_INSERT_ENTRY(&iter, &e->k_i));
	if (ret == -EINTR)
		goto retry;
out:
	bch2_btree_iter_unlock(&iter);
}

static bool should_narrow_crcs(struct bkey_s_c_extent e,
			       struct extent_pick_ptr *pick,
			       unsigned flags)
{
	return !(flags & BCH_READ_IN_RETRY) &&
		bch2_can_narrow_extent_crcs(e, pick->crc);
}

/* Inner part that may run in process context */
static void __bch2_read_endio(struct work_struct *work)
{
	struct bch_read_bio *rbio =
		container_of(work, struct bch_read_bio, work);
	struct bch_fs *c = rbio->c;
	struct bio *src = &rbio->bio, *dst = &bch2_rbio_parent(rbio)->bio;
	struct bvec_iter dst_iter = rbio->bvec_iter;
	struct bch_extent_crc_unpacked crc = rbio->pick.crc;
	struct nonce nonce = extent_nonce(rbio->version, crc);
	struct bch_csum csum;

	/* Reset iterator for checksumming and copying bounced data: */
	if (rbio->bounce) {
		src->bi_iter.bi_size		= crc.compressed_size << 9;
		src->bi_iter.bi_idx		= 0;
		src->bi_iter.bi_bvec_done	= 0;
	} else {
		src->bi_iter			= rbio->bvec_iter;
	}

	csum = bch2_checksum_bio(c, crc.csum_type, nonce, src);
	if (bch2_crc_cmp(csum, rbio->pick.crc.csum))
		goto csum_err;

	if (unlikely(rbio->narrow_crcs))
		bch2_rbio_narrow_crcs(rbio);

	if (rbio->flags & BCH_READ_NODECODE)
		goto nodecode;

	/* Adjust crc to point to subset of data we want: */
	crc.offset     += rbio->bvec_iter.bi_sector - rbio->pos.offset;
	crc.live_size	= bvec_iter_sectors(rbio->bvec_iter);

	if (crc.compression_type != BCH_COMPRESSION_NONE) {
		bch2_encrypt_bio(c, crc.csum_type, nonce, src);
		if (bch2_bio_uncompress(c, src, dst, dst_iter, crc))
			goto decompression_err;
	} else {
		/* don't need to decrypt the entire bio: */
		nonce = nonce_add(nonce, crc.offset << 9);
		bio_advance(src, crc.offset << 9);

		BUG_ON(src->bi_iter.bi_size < dst_iter.bi_size);
		src->bi_iter.bi_size = dst_iter.bi_size;

		bch2_encrypt_bio(c, crc.csum_type, nonce, src);

		if (rbio->bounce) {
			struct bvec_iter src_iter = src->bi_iter;
			bio_copy_data_iter(dst, &dst_iter, src, &src_iter);
		}
	}

	if (rbio->promote) {
		/*
		 * Re encrypt data we decrypted, so it's consistent with
		 * rbio->crc:
		 */
		bch2_encrypt_bio(c, crc.csum_type, nonce, src);
		promote_start(rbio->promote, rbio);
	}
nodecode:
	if (likely(!(rbio->flags & BCH_READ_IN_RETRY)))
		bch2_rbio_done(rbio);
	return;
csum_err:
	/*
	 * Checksum error: if the bio wasn't bounced, we may have been
	 * reading into buffers owned by userspace (that userspace can
	 * scribble over) - retry the read, bouncing it this time:
	 */
	if (!rbio->bounce && (rbio->flags & BCH_READ_USER_MAPPED)) {
		rbio->flags |= BCH_READ_MUST_BOUNCE;
		bch2_rbio_error(rbio, READ_RETRY, BLK_STS_IOERR);
		return;
	}

	bch2_dev_io_error(rbio->pick.ca,
		"data checksum error, inode %llu offset %llu: expected %0llx%0llx got %0llx%0llx (type %u)",
		rbio->pos.inode, (u64) rbio->bvec_iter.bi_sector,
		rbio->pick.crc.csum.hi, rbio->pick.crc.csum.lo,
		csum.hi, csum.lo, crc.csum_type);
	bch2_rbio_error(rbio, READ_RETRY_AVOID, BLK_STS_IOERR);
	return;
decompression_err:
	__bcache_io_error(c, "decompression error, inode %llu offset %llu",
			  rbio->pos.inode,
			  (u64) rbio->bvec_iter.bi_sector);
	bch2_rbio_error(rbio, READ_ERR, BLK_STS_IOERR);
	return;
}

static void bch2_read_endio(struct bio *bio)
{
	struct bch_read_bio *rbio =
		container_of(bio, struct bch_read_bio, bio);
	struct bch_fs *c = rbio->c;
	struct workqueue_struct *wq = NULL;
	enum rbio_context context = RBIO_CONTEXT_NULL;

	bch2_latency_acct(rbio->pick.ca, rbio->submit_time_us, READ);

	percpu_ref_put(&rbio->pick.ca->io_ref);

	if (!rbio->split)
		rbio->bio.bi_end_io = rbio->end_io;

	if (bch2_dev_io_err_on(bio->bi_status, rbio->pick.ca, "data read")) {
		bch2_rbio_error(rbio, READ_RETRY_AVOID, bio->bi_status);
		return;
	}

	if (rbio->pick.ptr.cached &&
	    (((rbio->flags & BCH_READ_RETRY_IF_STALE) && race_fault()) ||
	     ptr_stale(rbio->pick.ca, &rbio->pick.ptr))) {
		atomic_long_inc(&c->read_realloc_races);

		if (rbio->flags & BCH_READ_RETRY_IF_STALE)
			bch2_rbio_error(rbio, READ_RETRY, BLK_STS_AGAIN);
		else
			bch2_rbio_error(rbio, READ_ERR, BLK_STS_AGAIN);
		return;
	}

	if (rbio->narrow_crcs ||
	    rbio->pick.crc.compression_type ||
	    bch2_csum_type_is_encryption(rbio->pick.crc.csum_type))
		context = RBIO_CONTEXT_UNBOUND,	wq = system_unbound_wq;
	else if (rbio->pick.crc.csum_type)
		context = RBIO_CONTEXT_HIGHPRI,	wq = system_highpri_wq;

	bch2_rbio_punt(rbio, __bch2_read_endio, context, wq);
}

int __bch2_read_extent(struct bch_fs *c, struct bch_read_bio *orig,
		       struct bvec_iter iter, struct bkey_s_c_extent e,
		       struct extent_pick_ptr *pick, unsigned flags)
{
	struct bch_read_bio *rbio;
	bool split = false, bounce = false, read_full = false;
	bool promote = false, narrow_crcs = false;
	struct bpos pos = bkey_start_pos(e.k);
	int ret = 0;

	lg_local_lock(&c->usage_lock);
	bucket_io_clock_reset(c, pick->ca,
			PTR_BUCKET_NR(pick->ca, &pick->ptr), READ);
	lg_local_unlock(&c->usage_lock);

	narrow_crcs = should_narrow_crcs(e, pick, flags);

	if (flags & BCH_READ_NODECODE) {
		BUG_ON(iter.bi_size < pick->crc.compressed_size << 9);
		iter.bi_size = pick->crc.compressed_size << 9;
		goto noclone;
	}

	if (narrow_crcs && (flags & BCH_READ_USER_MAPPED))
		flags |= BCH_READ_MUST_BOUNCE;

	EBUG_ON(bkey_start_offset(e.k) > iter.bi_sector ||
		e.k->p.offset < bvec_iter_end_sector(iter));

	if (pick->crc.compression_type != BCH_COMPRESSION_NONE ||
	    (pick->crc.csum_type != BCH_CSUM_NONE &&
	     (bvec_iter_sectors(iter) != pick->crc.uncompressed_size ||
	      (bch2_csum_type_is_encryption(pick->crc.csum_type) &&
	       (flags & BCH_READ_USER_MAPPED)) ||
	      (flags & BCH_READ_MUST_BOUNCE)))) {
		read_full = true;
		bounce = true;
	}

	promote = should_promote(c, pick, flags);
	/* could also set read_full */
	if (promote)
		bounce = true;

	if (!read_full) {
		EBUG_ON(pick->crc.compression_type);
		EBUG_ON(pick->crc.csum_type &&
			(bvec_iter_sectors(iter) != pick->crc.uncompressed_size ||
			 bvec_iter_sectors(iter) != pick->crc.live_size ||
			 pick->crc.offset ||
			 iter.bi_sector != pos.offset));

		pick->ptr.offset += pick->crc.offset +
			(iter.bi_sector - pos.offset);
		pick->crc.compressed_size	= bvec_iter_sectors(iter);
		pick->crc.uncompressed_size	= bvec_iter_sectors(iter);
		pick->crc.offset		= 0;
		pick->crc.live_size		= bvec_iter_sectors(iter);
		pos.offset			= iter.bi_sector;
	}

	if (bounce) {
		unsigned sectors = pick->crc.compressed_size;

		rbio = rbio_init(bio_alloc_bioset(GFP_NOIO,
					DIV_ROUND_UP(sectors, PAGE_SECTORS),
					&c->bio_read_split),
				 orig->opts);

		bch2_bio_alloc_pages_pool(c, &rbio->bio, sectors << 9);
		split = true;
	} else if (flags & BCH_READ_MUST_CLONE) {
		/*
		 * Have to clone if there were any splits, due to error
		 * reporting issues (if a split errored, and retrying didn't
		 * work, when it reports the error to its parent (us) we don't
		 * know if the error was from our bio, and we should retry, or
		 * from the whole bio, in which case we don't want to retry and
		 * lose the error)
		 */
		rbio = rbio_init(bio_clone_fast(&orig->bio, GFP_NOIO,
						&c->bio_read_split),
				 orig->opts);
		rbio->bio.bi_iter = iter;
		split = true;
	} else {
noclone:
		rbio = orig;
		rbio->bio.bi_iter = iter;
		split = false;
		BUG_ON(bio_flagged(&rbio->bio, BIO_CHAIN));
	}

	BUG_ON(bio_sectors(&rbio->bio) != pick->crc.compressed_size);

	rbio->c			= c;
	if (split)
		rbio->parent	= orig;
	else
		rbio->end_io	= orig->bio.bi_end_io;
	rbio->bvec_iter		= iter;
	rbio->submit_time_us	= local_clock_us();
	rbio->flags		= flags;
	rbio->bounce		= bounce;
	rbio->split		= split;
	rbio->narrow_crcs	= narrow_crcs;
	rbio->retry		= 0;
	rbio->context		= 0;
	rbio->devs_have		= bch2_extent_devs(e);
	rbio->pick		= *pick;
	rbio->pos		= pos;
	rbio->version		= e.k->version;
	rbio->promote		= promote ? promote_alloc(rbio) : NULL;
	INIT_WORK(&rbio->work, NULL);

	bio_set_dev(&rbio->bio, pick->ca->disk_sb.bdev);
	rbio->bio.bi_opf	= orig->bio.bi_opf;
	rbio->bio.bi_iter.bi_sector = pick->ptr.offset;
	rbio->bio.bi_end_io	= bch2_read_endio;

	if (bounce)
		trace_read_bounce(&rbio->bio);

	bch2_increment_clock(c, bio_sectors(&rbio->bio), READ);
	this_cpu_add(pick->ca->io_done->sectors[READ][BCH_DATA_USER],
		     bio_sectors(&rbio->bio));

	if (likely(!(flags & BCH_READ_IN_RETRY))) {
		submit_bio(&rbio->bio);
	} else {
		submit_bio_wait(&rbio->bio);

		rbio->context = RBIO_CONTEXT_UNBOUND;
		bch2_read_endio(&rbio->bio);

		ret = rbio->retry;
		if (rbio->split)
			rbio = bch2_rbio_free(rbio);
		if (!ret)
			bch2_rbio_done(rbio);
	}

	return ret;
}

static void bch2_read_nodecode_retry(struct bch_fs *c, struct bch_read_bio *rbio,
				     struct bvec_iter bvec_iter, u64 inode,
				     struct bch_devs_mask *avoid, unsigned flags)
{
	struct extent_pick_ptr pick;
	struct btree_iter iter;
	BKEY_PADDED(k) tmp;
	struct bkey_s_c k;
	int ret;

	bch2_btree_iter_init(&iter, c, BTREE_ID_EXTENTS,
			     POS(inode, bvec_iter.bi_sector),
			     BTREE_ITER_SLOTS);
retry:
	k = bch2_btree_iter_peek_slot(&iter);
	if (btree_iter_err(k)) {
		bch2_btree_iter_unlock(&iter);
		goto err;
	}

	bkey_reassemble(&tmp.k, k);
	k = bkey_i_to_s_c(&tmp.k);
	bch2_btree_iter_unlock(&iter);

	if (!bkey_extent_is_data(k.k) ||
	    !bch2_extent_matches_ptr(c, bkey_i_to_s_c_extent(&tmp.k),
				     rbio->pick.ptr,
				     rbio->pos.offset -
				     rbio->pick.crc.offset) ||
	    bkey_start_offset(k.k) != bvec_iter.bi_sector)
		goto err;

	bch2_extent_pick_ptr(c, k, avoid, &pick);
	if (IS_ERR(pick.ca)) {
		bcache_io_error(c, &rbio->bio, "no device to read from");
		bio_endio(&rbio->bio);
		return;
	}

	if (!pick.ca)
		goto err;

	if (pick.crc.compressed_size > bvec_iter_sectors(bvec_iter)) {
		percpu_ref_put(&pick.ca->io_ref);
		goto err;

	}

	ret = __bch2_read_extent(c, rbio, bvec_iter, bkey_s_c_to_extent(k),
				 &pick, flags);
	switch (ret) {
	case READ_RETRY_AVOID:
		__set_bit(pick.ca->dev_idx, avoid->d);
	case READ_RETRY:
		goto retry;
	case READ_ERR:
		bio_endio(&rbio->bio);
		return;
	};

	return;
err:
	/*
	 * extent we wanted to read no longer exists, or
	 * was merged or partially overwritten (and thus
	 * possibly bigger than the memory that was
	 * originally allocated)
	 */
	rbio->bio.bi_status = BLK_STS_AGAIN;
	bio_endio(&rbio->bio);
	return;
}

void __bch2_read(struct bch_fs *c, struct bch_read_bio *rbio,
		 struct bvec_iter bvec_iter, u64 inode,
		 struct bch_devs_mask *avoid, unsigned flags)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret;

	EBUG_ON(flags & BCH_READ_NODECODE);
retry:
	for_each_btree_key(&iter, c, BTREE_ID_EXTENTS,
			   POS(inode, bvec_iter.bi_sector),
			   BTREE_ITER_SLOTS, k) {
		BKEY_PADDED(k) tmp;
		struct extent_pick_ptr pick;
		struct bvec_iter fragment;

		/*
		 * Unlock the iterator while the btree node's lock is still in
		 * cache, before doing the IO:
		 */
		bkey_reassemble(&tmp.k, k);
		k = bkey_i_to_s_c(&tmp.k);
		bch2_btree_iter_unlock(&iter);

		bch2_extent_pick_ptr(c, k, avoid, &pick);
		if (IS_ERR(pick.ca)) {
			bcache_io_error(c, &rbio->bio, "no device to read from");
			bio_endio(&rbio->bio);
			return;
		}

		fragment = bvec_iter;
		fragment.bi_size = (min_t(u64, k.k->p.offset,
					  bvec_iter_end_sector(bvec_iter)) -
				    bvec_iter.bi_sector) << 9;

		if (pick.ca) {
			if (fragment.bi_size != bvec_iter.bi_size) {
				bio_inc_remaining(&rbio->bio);
				flags |= BCH_READ_MUST_CLONE;
				trace_read_split(&rbio->bio);
			}

			ret = __bch2_read_extent(c, rbio, fragment,
						 bkey_s_c_to_extent(k),
						 &pick, flags);
			switch (ret) {
			case READ_RETRY_AVOID:
				__set_bit(pick.ca->dev_idx, avoid->d);
			case READ_RETRY:
				goto retry;
			case READ_ERR:
				rbio->bio.bi_status = BLK_STS_IOERR;
				bio_endio(&rbio->bio);
				return;
			};
		} else {
			zero_fill_bio_iter(&rbio->bio, fragment);

			if (fragment.bi_size == bvec_iter.bi_size)
				bio_endio(&rbio->bio);
		}

		if (fragment.bi_size == bvec_iter.bi_size)
			return;

		bio_advance_iter(&rbio->bio, &bvec_iter, fragment.bi_size);
	}

	/*
	 * If we get here, it better have been because there was an error
	 * reading a btree node
	 */
	ret = bch2_btree_iter_unlock(&iter);
	BUG_ON(!ret);
	bcache_io_error(c, &rbio->bio, "btree IO error %i", ret);
	bio_endio(&rbio->bio);
}
