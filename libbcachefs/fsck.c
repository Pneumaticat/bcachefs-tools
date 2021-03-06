
#include "bcachefs.h"
#include "btree_update.h"
#include "dirent.h"
#include "error.h"
#include "fs.h"
#include "fsck.h"
#include "inode.h"
#include "keylist.h"
#include "super.h"
#include "xattr.h"

#include <linux/dcache.h> /* struct qstr */
#include <linux/generic-radix-tree.h>

#define QSTR(n) { { { .len = strlen(n) } }, .name = n }

static int remove_dirent(struct bch_fs *c, struct btree_iter *iter,
			 struct bkey_s_c_dirent dirent)
{
	struct qstr name;
	struct bch_inode_unpacked dir_inode;
	struct bch_hash_info dir_hash_info;
	u64 dir_inum = dirent.k->p.inode;
	int ret;
	char *buf;

	name.len = bch2_dirent_name_bytes(dirent);
	buf = kmalloc(name.len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, dirent.v->d_name, name.len);
	buf[name.len] = '\0';
	name.name = buf;

	/* Unlock iter so we don't deadlock, after copying name: */
	bch2_btree_iter_unlock(iter);

	ret = bch2_inode_find_by_inum(c, dir_inum, &dir_inode);
	if (ret) {
		bch_err(c, "remove_dirent: err %i looking up directory inode", ret);
		goto err;
	}

	dir_hash_info = bch2_hash_info_init(c, &dir_inode);

	ret = bch2_dirent_delete(c, dir_inum, &dir_hash_info, &name, NULL);
	if (ret)
		bch_err(c, "remove_dirent: err %i deleting dirent", ret);
err:
	kfree(buf);
	return ret;
}

static int reattach_inode(struct bch_fs *c,
			  struct bch_inode_unpacked *lostfound_inode,
			  u64 inum)
{
	struct bch_hash_info lostfound_hash_info =
		bch2_hash_info_init(c, lostfound_inode);
	struct bkey_inode_buf packed;
	char name_buf[20];
	struct qstr name;
	int ret;

	snprintf(name_buf, sizeof(name_buf), "%llu", inum);
	name = (struct qstr) QSTR(name_buf);

	lostfound_inode->bi_nlink++;

	bch2_inode_pack(&packed, lostfound_inode);

	ret = bch2_btree_insert(c, BTREE_ID_INODES, &packed.inode.k_i,
			       NULL, NULL, NULL,
			       BTREE_INSERT_NOFAIL);
	if (ret) {
		bch_err(c, "error %i reattaching inode %llu while updating lost+found",
			ret, inum);
		return ret;
	}

	ret = bch2_dirent_create(c, lostfound_inode->bi_inum,
				 &lostfound_hash_info,
				 DT_DIR, &name, inum, NULL,
				 BTREE_INSERT_NOFAIL);
	if (ret) {
		bch_err(c, "error %i reattaching inode %llu while creating new dirent",
			ret, inum);
		return ret;
	}
	return ret;
}

struct inode_walker {
	bool			first_this_inode;
	bool			have_inode;
	u64			cur_inum;
	struct bch_inode_unpacked inode;
};

static struct inode_walker inode_walker_init(void)
{
	return (struct inode_walker) {
		.cur_inum	= -1,
		.have_inode	= false,
	};
}

static int walk_inode(struct bch_fs *c, struct inode_walker *w, u64 inum)
{
	w->first_this_inode	= inum != w->cur_inum;
	w->cur_inum		= inum;

	if (w->first_this_inode) {
		int ret = bch2_inode_find_by_inum(c, inum, &w->inode);

		if (ret && ret != -ENOENT)
			return ret;

		w->have_inode = !ret;
	}

	return 0;
}

struct hash_check {
	struct bch_hash_info	info;
	struct btree_iter	chain;
	struct btree_iter	iter;
	u64			next;
};

static void hash_check_init(const struct bch_hash_desc desc,
			    struct hash_check *h, struct bch_fs *c)
{
	bch2_btree_iter_init(&h->chain, c, desc.btree_id, POS_MIN, 0);
	bch2_btree_iter_init(&h->iter, c, desc.btree_id, POS_MIN, 0);
}

static void hash_check_set_inode(struct hash_check *h, struct bch_fs *c,
				 const struct bch_inode_unpacked *bi)
{
	h->info = bch2_hash_info_init(c, bi);
	h->next = -1;
}

static int hash_redo_key(const struct bch_hash_desc desc,
			 struct hash_check *h, struct bch_fs *c,
			 struct btree_iter *k_iter, struct bkey_s_c k,
			 u64 hashed)
{
	struct bkey_i *tmp;
	int ret = 0;

	tmp = kmalloc(bkey_bytes(k.k), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	bkey_reassemble(tmp, k);

	ret = bch2_btree_delete_at(k_iter, 0);
	if (ret)
		goto err;

	bch2_btree_iter_unlock(k_iter);

	bch2_hash_set(desc, &h->info, c, k_iter->pos.inode, NULL, tmp,
		      BTREE_INSERT_NOFAIL|
		      BCH_HASH_SET_MUST_CREATE);
err:
	kfree(tmp);
	return ret;
}

static int hash_check_key(const struct bch_hash_desc desc,
			  struct hash_check *h, struct bch_fs *c,
			  struct btree_iter *k_iter, struct bkey_s_c k)
{
	char buf[200];
	u64 hashed;
	int ret = 0;

	if (k.k->type != desc.whiteout_type &&
	    k.k->type != desc.key_type)
		return 0;

	if (k.k->p.offset != h->next) {
		if (!btree_iter_linked(&h->chain)) {
			bch2_btree_iter_link(k_iter, &h->chain);
			bch2_btree_iter_link(k_iter, &h->iter);
		}
		bch2_btree_iter_copy(&h->chain, k_iter);
	}
	h->next = k.k->p.offset + 1;

	if (k.k->type != desc.key_type)
		return 0;

	hashed = desc.hash_bkey(&h->info, k);

	if (fsck_err_on(hashed < h->chain.pos.offset ||
			hashed > k.k->p.offset, c,
			"hash table key at wrong offset: %llu, "
			"hashed to %llu chain starts at %llu\n%s",
			k.k->p.offset, hashed, h->chain.pos.offset,
			(bch2_bkey_val_to_text(c, bkey_type(0, desc.btree_id),
					       buf, sizeof(buf), k), buf))) {
		ret = hash_redo_key(desc, h, c, k_iter, k, hashed);
		if (ret) {
			bch_err(c, "hash_redo_key err %i", ret);
			return ret;
		}
		return 1;
	}

	if (!bkey_cmp(h->chain.pos, k_iter->pos))
		return 0;

	bch2_btree_iter_copy(&h->iter, &h->chain);
	while (bkey_cmp(h->iter.pos, k_iter->pos) < 0) {
		struct bkey_s_c k2 = bch2_btree_iter_peek(&h->iter);

		if (fsck_err_on(k2.k->type == desc.key_type &&
				!desc.cmp_bkey(k, k2), c,
				"duplicate hash table keys:\n%s",
				(bch2_bkey_val_to_text(c, bkey_type(0, desc.btree_id),
						       buf, sizeof(buf), k), buf))) {
			ret = bch2_hash_delete_at(desc, &h->info, &h->iter, NULL);
			if (ret)
				return ret;
			return 1;
		}
		bch2_btree_iter_next(&h->iter);
	}
fsck_err:
	return ret;
}

/*
 * Walk extents: verify that extents have a corresponding S_ISREG inode, and
 * that i_size an i_sectors are consistent
 */
noinline_for_stack
static int check_extents(struct bch_fs *c)
{
	struct inode_walker w = inode_walker_init();
	struct btree_iter iter;
	struct bkey_s_c k;
	u64 i_sectors;
	int ret = 0;

	for_each_btree_key(&iter, c, BTREE_ID_EXTENTS,
			   POS(BCACHEFS_ROOT_INO, 0), 0, k) {
		if (k.k->type == KEY_TYPE_DISCARD)
			continue;

		ret = walk_inode(c, &w, k.k->p.inode);
		if (ret)
			break;

		if (fsck_err_on(!w.have_inode, c,
			"extent type %u for missing inode %llu",
			k.k->type, k.k->p.inode) ||
		    fsck_err_on(w.have_inode &&
			!S_ISREG(w.inode.bi_mode) && !S_ISLNK(w.inode.bi_mode), c,
			"extent type %u for non regular file, inode %llu mode %o",
			k.k->type, k.k->p.inode, w.inode.bi_mode)) {
			bch2_btree_iter_unlock(&iter);

			ret = bch2_inode_truncate(c, k.k->p.inode, 0, NULL, NULL);
			if (ret)
				goto err;
			continue;
		}

		if (fsck_err_on(w.first_this_inode &&
			w.have_inode &&
			!(w.inode.bi_flags & BCH_INODE_I_SECTORS_DIRTY) &&
			w.inode.bi_sectors !=
			(i_sectors = bch2_count_inode_sectors(c, w.cur_inum)),
			c, "i_sectors wrong: got %llu, should be %llu",
			w.inode.bi_sectors, i_sectors)) {
			struct bkey_inode_buf p;

			w.inode.bi_sectors = i_sectors;

			bch2_btree_iter_unlock(&iter);

			bch2_inode_pack(&p, &w.inode);

			ret = bch2_btree_insert(c, BTREE_ID_INODES,
						&p.inode.k_i,
						NULL,
						NULL,
						NULL,
						BTREE_INSERT_NOFAIL);
			if (ret) {
				bch_err(c, "error in fs gc: error %i "
					"updating inode", ret);
				goto err;
			}

			/* revalidate iterator: */
			k = bch2_btree_iter_peek(&iter);
		}

		if (fsck_err_on(w.have_inode &&
			!(w.inode.bi_flags & BCH_INODE_I_SIZE_DIRTY) &&
			k.k->type != BCH_RESERVATION &&
			k.k->p.offset > round_up(w.inode.bi_size, PAGE_SIZE) >> 9, c,
			"extent type %u offset %llu past end of inode %llu, i_size %llu",
			k.k->type, k.k->p.offset, k.k->p.inode, w.inode.bi_size)) {
			bch2_btree_iter_unlock(&iter);

			ret = bch2_inode_truncate(c, k.k->p.inode,
					round_up(w.inode.bi_size, PAGE_SIZE) >> 9,
					NULL, NULL);
			if (ret)
				goto err;
			continue;
		}
	}
err:
fsck_err:
	return bch2_btree_iter_unlock(&iter) ?: ret;
}

/*
 * Walk dirents: verify that they all have a corresponding S_ISDIR inode,
 * validate d_type
 */
noinline_for_stack
static int check_dirents(struct bch_fs *c)
{
	struct inode_walker w = inode_walker_init();
	struct hash_check h;
	struct btree_iter iter;
	struct bkey_s_c k;
	unsigned name_len;
	char buf[200];
	int ret = 0;

	hash_check_init(bch2_dirent_hash_desc, &h, c);

	for_each_btree_key(&iter, c, BTREE_ID_DIRENTS,
			   POS(BCACHEFS_ROOT_INO, 0), 0, k) {
		struct bkey_s_c_dirent d;
		struct bch_inode_unpacked target;
		bool have_target;
		u64 d_inum;

		ret = walk_inode(c, &w, k.k->p.inode);
		if (ret)
			break;

		if (fsck_err_on(!w.have_inode, c,
				"dirent in nonexisting directory:\n%s",
				(bch2_bkey_val_to_text(c, BTREE_ID_DIRENTS,
						       buf, sizeof(buf), k), buf)) ||
		    fsck_err_on(!S_ISDIR(w.inode.bi_mode), c,
				"dirent in non directory inode type %u:\n%s",
				mode_to_type(w.inode.bi_mode),
				(bch2_bkey_val_to_text(c, BTREE_ID_DIRENTS,
						       buf, sizeof(buf), k), buf))) {
			ret = bch2_btree_delete_at(&iter, 0);
			if (ret)
				goto err;
			continue;
		}

		if (w.first_this_inode && w.have_inode)
			hash_check_set_inode(&h, c, &w.inode);

		ret = hash_check_key(bch2_dirent_hash_desc, &h, c, &iter, k);
		if (ret > 0) {
			ret = 0;
			continue;
		}

		if (ret)
			goto fsck_err;

		if (k.k->type != BCH_DIRENT)
			continue;

		d = bkey_s_c_to_dirent(k);
		d_inum = le64_to_cpu(d.v->d_inum);

		name_len = bch2_dirent_name_bytes(d);

		if (fsck_err_on(!name_len, c, "empty dirent") ||
		    fsck_err_on(name_len == 1 &&
				!memcmp(d.v->d_name, ".", 1), c,
				". dirent") ||
		    fsck_err_on(name_len == 2 &&
				!memcmp(d.v->d_name, "..", 2), c,
				".. dirent")) {
			ret = remove_dirent(c, &iter, d);
			if (ret)
				goto err;
			continue;
		}

		if (fsck_err_on(d_inum == d.k->p.inode, c,
				"dirent points to own directory:\n%s",
				(bch2_bkey_val_to_text(c, BTREE_ID_DIRENTS,
						       buf, sizeof(buf), k), buf))) {
			ret = remove_dirent(c, &iter, d);
			if (ret)
				goto err;
			continue;
		}

		ret = bch2_inode_find_by_inum(c, d_inum, &target);
		if (ret && ret != -ENOENT)
			break;

		have_target = !ret;
		ret = 0;

		if (fsck_err_on(!have_target, c,
				"dirent points to missing inode:\n%s",
				(bch2_bkey_val_to_text(c, BTREE_ID_DIRENTS,
						       buf, sizeof(buf), k), buf))) {
			ret = remove_dirent(c, &iter, d);
			if (ret)
				goto err;
			continue;
		}

		if (fsck_err_on(have_target &&
				d.v->d_type !=
				mode_to_type(target.bi_mode), c,
				"incorrect d_type: should be %u:\n%s",
				mode_to_type(target.bi_mode),
				(bch2_bkey_val_to_text(c, BTREE_ID_DIRENTS,
						       buf, sizeof(buf), k), buf))) {
			struct bkey_i_dirent *n;

			n = kmalloc(bkey_bytes(d.k), GFP_KERNEL);
			if (!n) {
				ret = -ENOMEM;
				goto err;
			}

			bkey_reassemble(&n->k_i, d.s_c);
			n->v.d_type = mode_to_type(target.bi_mode);

			ret = bch2_btree_insert_at(c, NULL, NULL, NULL,
					BTREE_INSERT_NOFAIL,
					BTREE_INSERT_ENTRY(&iter, &n->k_i));
			kfree(n);
			if (ret)
				goto err;

		}
	}
err:
fsck_err:
	bch2_btree_iter_unlock(&h.chain);
	bch2_btree_iter_unlock(&h.iter);
	return bch2_btree_iter_unlock(&iter) ?: ret;
}

/*
 * Walk xattrs: verify that they all have a corresponding inode
 */
noinline_for_stack
static int check_xattrs(struct bch_fs *c)
{
	struct inode_walker w = inode_walker_init();
	struct hash_check h;
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret = 0;

	hash_check_init(bch2_xattr_hash_desc, &h, c);

	for_each_btree_key(&iter, c, BTREE_ID_XATTRS,
			   POS(BCACHEFS_ROOT_INO, 0), 0, k) {
		ret = walk_inode(c, &w, k.k->p.inode);
		if (ret)
			break;

		if (fsck_err_on(!w.have_inode, c,
				"xattr for missing inode %llu",
				k.k->p.inode)) {
			ret = bch2_btree_delete_at(&iter, 0);
			if (ret)
				goto err;
			continue;
		}

		if (w.first_this_inode && w.have_inode)
			hash_check_set_inode(&h, c, &w.inode);

		ret = hash_check_key(bch2_xattr_hash_desc, &h, c, &iter, k);
		if (ret)
			goto fsck_err;
	}
err:
fsck_err:
	bch2_btree_iter_unlock(&h.chain);
	bch2_btree_iter_unlock(&h.iter);
	return bch2_btree_iter_unlock(&iter) ?: ret;
}

/* Get root directory, create if it doesn't exist: */
static int check_root(struct bch_fs *c, struct bch_inode_unpacked *root_inode)
{
	struct bkey_inode_buf packed;
	int ret;

	ret = bch2_inode_find_by_inum(c, BCACHEFS_ROOT_INO, root_inode);
	if (ret && ret != -ENOENT)
		return ret;

	if (fsck_err_on(ret, c, "root directory missing"))
		goto create_root;

	if (fsck_err_on(!S_ISDIR(root_inode->bi_mode), c,
			"root inode not a directory"))
		goto create_root;

	return 0;
fsck_err:
	return ret;
create_root:
	bch2_inode_init(c, root_inode, 0, 0, S_IFDIR|S_IRWXU|S_IRUGO|S_IXUGO,
			0, NULL);
	root_inode->bi_inum = BCACHEFS_ROOT_INO;

	bch2_inode_pack(&packed, root_inode);

	return bch2_btree_insert(c, BTREE_ID_INODES, &packed.inode.k_i,
				 NULL, NULL, NULL, BTREE_INSERT_NOFAIL);
}

/* Get lost+found, create if it doesn't exist: */
static int check_lostfound(struct bch_fs *c,
			   struct bch_inode_unpacked *root_inode,
			   struct bch_inode_unpacked *lostfound_inode)
{
	struct qstr lostfound = QSTR("lost+found");
	struct bch_hash_info root_hash_info =
		bch2_hash_info_init(c, root_inode);
	struct bkey_inode_buf packed;
	u64 inum;
	int ret;

	inum = bch2_dirent_lookup(c, BCACHEFS_ROOT_INO, &root_hash_info,
				 &lostfound);
	if (!inum) {
		bch_notice(c, "creating lost+found");
		goto create_lostfound;
	}

	ret = bch2_inode_find_by_inum(c, inum, lostfound_inode);
	if (ret && ret != -ENOENT)
		return ret;

	if (fsck_err_on(ret, c, "lost+found missing"))
		goto create_lostfound;

	if (fsck_err_on(!S_ISDIR(lostfound_inode->bi_mode), c,
			"lost+found inode not a directory"))
		goto create_lostfound;

	return 0;
fsck_err:
	return ret;
create_lostfound:
	root_inode->bi_nlink++;

	bch2_inode_pack(&packed, root_inode);

	ret = bch2_btree_insert(c, BTREE_ID_INODES, &packed.inode.k_i,
				NULL, NULL, NULL, BTREE_INSERT_NOFAIL);
	if (ret)
		return ret;

	bch2_inode_init(c, lostfound_inode, 0, 0, S_IFDIR|S_IRWXU|S_IRUGO|S_IXUGO,
			0, root_inode);

	ret = bch2_inode_create(c, lostfound_inode, BLOCKDEV_INODE_MAX, 0,
			       &c->unused_inode_hint);
	if (ret)
		return ret;

	ret = bch2_dirent_create(c, BCACHEFS_ROOT_INO, &root_hash_info, DT_DIR,
				 &lostfound, lostfound_inode->bi_inum, NULL,
				 BTREE_INSERT_NOFAIL);
	if (ret)
		return ret;

	return 0;
}

struct inode_bitmap {
	unsigned long	*bits;
	size_t		size;
};

static inline bool inode_bitmap_test(struct inode_bitmap *b, size_t nr)
{
	return nr < b->size ? test_bit(nr, b->bits) : false;
}

static inline int inode_bitmap_set(struct inode_bitmap *b, size_t nr)
{
	if (nr >= b->size) {
		size_t new_size = max(max(PAGE_SIZE * 8,
					  b->size * 2),
					  nr + 1);
		void *n;

		new_size = roundup_pow_of_two(new_size);
		n = krealloc(b->bits, new_size / 8, GFP_KERNEL|__GFP_ZERO);
		if (!n) {
			return -ENOMEM;
		}

		b->bits = n;
		b->size = new_size;
	}

	__set_bit(nr, b->bits);
	return 0;
}

struct pathbuf {
	size_t		nr;
	size_t		size;

	struct pathbuf_entry {
		u64	inum;
		u64	offset;
	}		*entries;
};

static int path_down(struct pathbuf *p, u64 inum)
{
	if (p->nr == p->size) {
		size_t new_size = max(256UL, p->size * 2);
		void *n = krealloc(p->entries,
				   new_size * sizeof(p->entries[0]),
				   GFP_KERNEL);
		if (!n)
			return -ENOMEM;

		p->entries = n;
		p->size = new_size;
	};

	p->entries[p->nr++] = (struct pathbuf_entry) {
		.inum = inum,
		.offset = 0,
	};
	return 0;
}

noinline_for_stack
static int check_directory_structure(struct bch_fs *c,
				     struct bch_inode_unpacked *lostfound_inode)
{
	struct inode_bitmap dirs_done = { NULL, 0 };
	struct pathbuf path = { 0, 0, NULL };
	struct pathbuf_entry *e;
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bkey_s_c_dirent dirent;
	bool had_unreachable;
	u64 d_inum;
	int ret = 0;

	/* DFS: */
restart_dfs:
	had_unreachable = false;

	ret = inode_bitmap_set(&dirs_done, BCACHEFS_ROOT_INO);
	if (ret) {
		bch_err(c, "memory allocation failure in inode_bitmap_set()");
		goto err;
	}

	ret = path_down(&path, BCACHEFS_ROOT_INO);
	if (ret) {
		return ret;
	}

	while (path.nr) {
next:
		e = &path.entries[path.nr - 1];

		if (e->offset == U64_MAX)
			goto up;

		for_each_btree_key(&iter, c, BTREE_ID_DIRENTS,
				   POS(e->inum, e->offset + 1), 0, k) {
			if (k.k->p.inode != e->inum)
				break;

			e->offset = k.k->p.offset;

			if (k.k->type != BCH_DIRENT)
				continue;

			dirent = bkey_s_c_to_dirent(k);

			if (dirent.v->d_type != DT_DIR)
				continue;

			d_inum = le64_to_cpu(dirent.v->d_inum);

			if (fsck_err_on(inode_bitmap_test(&dirs_done, d_inum), c,
					"directory %llu has multiple hardlinks",
					d_inum)) {
				ret = remove_dirent(c, &iter, dirent);
				if (ret)
					goto err;
				continue;
			}

			ret = inode_bitmap_set(&dirs_done, d_inum);
			if (ret) {
				bch_err(c, "memory allocation failure in inode_bitmap_set()");
				goto err;
			}

			ret = path_down(&path, d_inum);
			if (ret) {
				goto err;
			}

			bch2_btree_iter_unlock(&iter);
			goto next;
		}
		ret = bch2_btree_iter_unlock(&iter);
		if (ret) {
			bch_err(c, "btree error %i in fsck", ret);
			goto err;
		}
up:
		path.nr--;
	}

	for_each_btree_key(&iter, c, BTREE_ID_INODES, POS_MIN, 0, k) {
		if (k.k->type != BCH_INODE_FS ||
		    !S_ISDIR(le16_to_cpu(bkey_s_c_to_inode(k).v->bi_mode)))
			continue;

		if (fsck_err_on(!inode_bitmap_test(&dirs_done, k.k->p.inode), c,
				"unreachable directory found (inum %llu)",
				k.k->p.inode)) {
			bch2_btree_iter_unlock(&iter);

			ret = reattach_inode(c, lostfound_inode, k.k->p.inode);
			if (ret) {
				goto err;
			}

			had_unreachable = true;
		}
	}
	ret = bch2_btree_iter_unlock(&iter);
	if (ret)
		goto err;

	if (had_unreachable) {
		bch_info(c, "reattached unreachable directories, restarting pass to check for loops");
		kfree(dirs_done.bits);
		kfree(path.entries);
		memset(&dirs_done, 0, sizeof(dirs_done));
		memset(&path, 0, sizeof(path));
		goto restart_dfs;
	}

out:
	kfree(dirs_done.bits);
	kfree(path.entries);
	return ret;
err:
fsck_err:
	ret = bch2_btree_iter_unlock(&iter) ?: ret;
	goto out;
}

struct nlink {
	u32	count;
	u32	dir_count;
};

typedef GENRADIX(struct nlink) nlink_table;

static void inc_link(struct bch_fs *c, nlink_table *links,
		     u64 range_start, u64 *range_end,
		     u64 inum, bool dir)
{
	struct nlink *link;

	if (inum < range_start || inum >= *range_end)
		return;

	link = genradix_ptr_alloc(links, inum - range_start, GFP_KERNEL);
	if (!link) {
		bch_verbose(c, "allocation failed during fs gc - will need another pass");
		*range_end = inum;
		return;
	}

	if (dir)
		link->dir_count++;
	else
		link->count++;
}

noinline_for_stack
static int bch2_gc_walk_dirents(struct bch_fs *c, nlink_table *links,
			       u64 range_start, u64 *range_end)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bkey_s_c_dirent d;
	u64 d_inum;
	int ret;

	inc_link(c, links, range_start, range_end, BCACHEFS_ROOT_INO, false);

	for_each_btree_key(&iter, c, BTREE_ID_DIRENTS, POS_MIN, 0, k) {
		switch (k.k->type) {
		case BCH_DIRENT:
			d = bkey_s_c_to_dirent(k);
			d_inum = le64_to_cpu(d.v->d_inum);

			if (d.v->d_type == DT_DIR)
				inc_link(c, links, range_start, range_end,
					 d.k->p.inode, true);

			inc_link(c, links, range_start, range_end,
				 d_inum, false);

			break;
		}

		bch2_btree_iter_cond_resched(&iter);
	}
	ret = bch2_btree_iter_unlock(&iter);
	if (ret)
		bch_err(c, "error in fs gc: btree error %i while walking dirents", ret);

	return ret;
}

s64 bch2_count_inode_sectors(struct bch_fs *c, u64 inum)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	u64 sectors = 0;

	for_each_btree_key(&iter, c, BTREE_ID_EXTENTS, POS(inum, 0), 0, k) {
		if (k.k->p.inode != inum)
			break;

		if (bkey_extent_is_allocation(k.k))
			sectors += k.k->size;
	}

	return bch2_btree_iter_unlock(&iter) ?: sectors;
}

static int bch2_gc_do_inode(struct bch_fs *c,
			   struct bch_inode_unpacked *lostfound_inode,
			   struct btree_iter *iter,
			   struct bkey_s_c_inode inode, struct nlink link)
{
	struct bch_inode_unpacked u;
	int ret = 0;
	u32 i_nlink, real_i_nlink;
	bool do_update = false;

	ret = bch2_inode_unpack(inode, &u);
	if (bch2_fs_inconsistent_on(ret, c,
			 "error unpacking inode %llu in fsck",
			 inode.k->p.inode))
		return ret;

	i_nlink = u.bi_nlink + nlink_bias(u.bi_mode);

	fsck_err_on(i_nlink < link.count, c,
		    "inode %llu i_link too small (%u < %u, type %i)",
		    inode.k->p.inode, i_nlink,
		    link.count, mode_to_type(u.bi_mode));

	/* These should have been caught/fixed by earlier passes: */
	if (S_ISDIR(u.bi_mode)) {
		need_fsck_err_on(link.count > 1, c,
			"directory %llu with multiple hardlinks: %u",
			inode.k->p.inode, link.count);

		real_i_nlink = link.count * 2 + link.dir_count;
	} else {
		need_fsck_err_on(link.dir_count, c,
			"found dirents for non directory %llu",
			inode.k->p.inode);

		real_i_nlink = link.count + link.dir_count;
	}

	if (!link.count) {
		fsck_err_on(c->sb.clean, c,
			    "filesystem marked clean, "
			    "but found orphaned inode %llu",
			    inode.k->p.inode);

		if (fsck_err_on(S_ISDIR(u.bi_mode) &&
				bch2_empty_dir(c, inode.k->p.inode), c,
				"non empty directory with link count 0, "
				"inode nlink %u, dir links found %u",
				i_nlink, link.dir_count)) {
			ret = reattach_inode(c, lostfound_inode,
					     inode.k->p.inode);
			if (ret)
				return ret;
		}

		bch_verbose(c, "deleting inode %llu", inode.k->p.inode);

		ret = bch2_inode_rm(c, inode.k->p.inode);
		if (ret)
			bch_err(c, "error in fs gc: error %i "
				"while deleting inode", ret);
		return ret;
	}

	if (u.bi_flags & BCH_INODE_I_SIZE_DIRTY) {
		fsck_err_on(c->sb.clean, c,
			    "filesystem marked clean, "
			    "but inode %llu has i_size dirty",
			    inode.k->p.inode);

		bch_verbose(c, "truncating inode %llu", inode.k->p.inode);

		/*
		 * XXX: need to truncate partial blocks too here - or ideally
		 * just switch units to bytes and that issue goes away
		 */

		ret = bch2_inode_truncate(c, inode.k->p.inode,
				round_up(u.bi_size, PAGE_SIZE) >> 9,
				NULL, NULL);
		if (ret) {
			bch_err(c, "error in fs gc: error %i "
				"truncating inode", ret);
			return ret;
		}

		/*
		 * We truncated without our normal sector accounting hook, just
		 * make sure we recalculate it:
		 */
		u.bi_flags |= BCH_INODE_I_SECTORS_DIRTY;

		u.bi_flags &= ~BCH_INODE_I_SIZE_DIRTY;
		do_update = true;
	}

	if (u.bi_flags & BCH_INODE_I_SECTORS_DIRTY) {
		s64 sectors;

		fsck_err_on(c->sb.clean, c,
			    "filesystem marked clean, "
			    "but inode %llu has i_sectors dirty",
			    inode.k->p.inode);

		bch_verbose(c, "recounting sectors for inode %llu",
			    inode.k->p.inode);

		sectors = bch2_count_inode_sectors(c, inode.k->p.inode);
		if (sectors < 0) {
			bch_err(c, "error in fs gc: error %i "
				"recounting inode sectors",
				(int) sectors);
			return sectors;
		}

		u.bi_sectors = sectors;
		u.bi_flags &= ~BCH_INODE_I_SECTORS_DIRTY;
		do_update = true;
	}

	if (i_nlink != real_i_nlink) {
		fsck_err_on(c->sb.clean, c,
			    "filesystem marked clean, "
			    "but inode %llu has wrong i_nlink "
			    "(type %u i_nlink %u, should be %u)",
			    inode.k->p.inode, mode_to_type(u.bi_mode),
			    i_nlink, real_i_nlink);

		bch_verbose(c, "setting inode %llu nlinks from %u to %u",
			    inode.k->p.inode, i_nlink, real_i_nlink);
		u.bi_nlink = real_i_nlink - nlink_bias(u.bi_mode);
		do_update = true;
	}

	if (do_update) {
		struct bkey_inode_buf p;

		bch2_inode_pack(&p, &u);

		ret = bch2_btree_insert_at(c, NULL, NULL, NULL,
					  BTREE_INSERT_NOFAIL,
					  BTREE_INSERT_ENTRY(iter, &p.inode.k_i));
		if (ret && ret != -EINTR)
			bch_err(c, "error in fs gc: error %i "
				"updating inode", ret);
	}
fsck_err:
	return ret;
}

noinline_for_stack
static int bch2_gc_walk_inodes(struct bch_fs *c,
			      struct bch_inode_unpacked *lostfound_inode,
			      nlink_table *links,
			      u64 range_start, u64 range_end)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	struct nlink *link, zero_links = { 0, 0 };
	struct genradix_iter nlinks_iter;
	int ret = 0, ret2 = 0;
	u64 nlinks_pos;

	bch2_btree_iter_init(&iter, c, BTREE_ID_INODES, POS(range_start, 0), 0);
	nlinks_iter = genradix_iter_init(links, 0);

	while ((k = bch2_btree_iter_peek(&iter)).k &&
	       !btree_iter_err(k)) {
peek_nlinks:	link = genradix_iter_peek(&nlinks_iter, links);

		if (!link && (!k.k || iter.pos.inode >= range_end))
			break;

		nlinks_pos = range_start + nlinks_iter.pos;
		if (iter.pos.inode > nlinks_pos) {
			/* Should have been caught by dirents pass: */
			need_fsck_err_on(link && link->count, c,
				"missing inode %llu (nlink %u)",
				nlinks_pos, link->count);
			genradix_iter_advance(&nlinks_iter, links);
			goto peek_nlinks;
		}

		if (iter.pos.inode < nlinks_pos || !link)
			link = &zero_links;

		if (k.k && k.k->type == BCH_INODE_FS) {
			/*
			 * Avoid potential deadlocks with iter for
			 * truncate/rm/etc.:
			 */
			bch2_btree_iter_unlock(&iter);

			ret = bch2_gc_do_inode(c, lostfound_inode, &iter,
					      bkey_s_c_to_inode(k), *link);
			if (ret == -EINTR)
				continue;
			if (ret)
				break;

			if (link->count)
				atomic_long_inc(&c->nr_inodes);
		} else {
			/* Should have been caught by dirents pass: */
			need_fsck_err_on(link->count, c,
				"missing inode %llu (nlink %u)",
				nlinks_pos, link->count);
		}

		if (nlinks_pos == iter.pos.inode)
			genradix_iter_advance(&nlinks_iter, links);

		bch2_btree_iter_next(&iter);
		bch2_btree_iter_cond_resched(&iter);
	}
fsck_err:
	ret2 = bch2_btree_iter_unlock(&iter);
	if (ret2)
		bch_err(c, "error in fs gc: btree error %i while walking inodes", ret2);

	return ret ?: ret2;
}

noinline_for_stack
static int check_inode_nlinks(struct bch_fs *c,
			      struct bch_inode_unpacked *lostfound_inode)
{
	nlink_table links;
	u64 this_iter_range_start, next_iter_range_start = 0;
	int ret = 0;

	genradix_init(&links);

	do {
		this_iter_range_start = next_iter_range_start;
		next_iter_range_start = U64_MAX;

		ret = bch2_gc_walk_dirents(c, &links,
					  this_iter_range_start,
					  &next_iter_range_start);
		if (ret)
			break;

		ret = bch2_gc_walk_inodes(c, lostfound_inode, &links,
					 this_iter_range_start,
					 next_iter_range_start);
		if (ret)
			break;

		genradix_free(&links);
	} while (next_iter_range_start != U64_MAX);

	genradix_free(&links);

	return ret;
}

/*
 * Checks for inconsistencies that shouldn't happen, unless we have a bug.
 * Doesn't fix them yet, mainly because they haven't yet been observed:
 */
int bch2_fsck(struct bch_fs *c, bool full_fsck)
{
	struct bch_inode_unpacked root_inode, lostfound_inode;
	int ret;

	if (full_fsck) {
		bch_verbose(c, "checking extents");
		ret = check_extents(c);
		if (ret)
			return ret;

		bch_verbose(c, "checking dirents");
		ret = check_dirents(c);
		if (ret)
			return ret;

		bch_verbose(c, "checking xattrs");
		ret = check_xattrs(c);
		if (ret)
			return ret;

		bch_verbose(c, "checking root directory");
		ret = check_root(c, &root_inode);
		if (ret)
			return ret;

		bch_verbose(c, "checking lost+found");
		ret = check_lostfound(c, &root_inode, &lostfound_inode);
		if (ret)
			return ret;

		bch_verbose(c, "checking directory structure");
		ret = check_directory_structure(c, &lostfound_inode);
		if (ret)
			return ret;

		bch_verbose(c, "checking inode nlinks");
		ret = check_inode_nlinks(c, &lostfound_inode);
		if (ret)
			return ret;
	} else {
		bch_verbose(c, "checking root directory");
		ret = check_root(c, &root_inode);
		if (ret)
			return ret;

		bch_verbose(c, "checking lost+found");
		ret = check_lostfound(c, &root_inode, &lostfound_inode);
		if (ret)
			return ret;

		bch_verbose(c, "checking inode nlinks");
		ret = check_inode_nlinks(c, &lostfound_inode);
		if (ret)
			return ret;
	}

	bch2_flush_fsck_errs(c);

	return 0;
}
