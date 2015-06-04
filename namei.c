/*
 * BRIEF DESCRIPTION
 *
 * Inode operations for directories.
 *
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "pmfs.h"
#include "xip.h"

/*
 * Couple of helper functions - make the code slightly cleaner.
 */
static inline void pmfs_inc_count(struct inode *inode, struct pmfs_inode *pi)
{
	inc_nlink(inode);
	pmfs_update_nlink(inode, pi);
}

static inline void pmfs_dec_count(struct inode *inode, struct pmfs_inode *pi)
{
	if (inode->i_nlink) {
		drop_nlink(inode);
		pmfs_update_nlink(inode, pi);
	}
}

static ino_t pmfs_inode_by_name(struct inode *dir, struct qstr *entry,
				 struct pmfs_log_direntry **res_entry)
{
	struct super_block *sb = dir->i_sb;
	struct pmfs_dir_node *node;
	struct pmfs_log_direntry *direntry;

	node = pmfs_find_dir_node_by_name(sb, NULL, dir,
					entry->name, entry->len);
	if (node == NULL)
		return 0;

	direntry = (struct pmfs_log_direntry *)pmfs_get_block(sb, node->nvmm);
	*res_entry = direntry;
	return direntry->ino;
}

static struct dentry *pmfs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct inode *inode = NULL;
	struct pmfs_log_direntry *de;
	ino_t ino;
	timing_t lookup_time;

	PMFS_START_TIMING(lookup_t, lookup_time);
	if (dentry->d_name.len > PMFS_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	pmfs_dbg_verbose("%s: %s\n", __func__, dentry->d_name.name);
	ino = pmfs_inode_by_name(dir, &dentry->d_name, &de);
	pmfs_dbg_verbose("%s: ino %lu\n", __func__, ino);
	if (ino) {
		inode = pmfs_iget(dir->i_sb, ino);
		if (inode == ERR_PTR(-ESTALE)) {
			pmfs_err(dir->i_sb, __func__,
				  "deleted inode referenced: %lu",
				  (unsigned long)ino);
			return ERR_PTR(-EIO);
		}
	}

	PMFS_END_TIMING(lookup_t, lookup_time);
	return d_splice_alias(inode, dentry);
}

/*
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int pmfs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
			bool excl)
{
	struct inode *inode = NULL;
	int err = PTR_ERR(inode);
	struct super_block *sb = dir->i_sb;
	struct pmfs_inode *pidir;
	u64 pi_addr = 0;
	struct pmfs_inode_info_header *sih;
	u64 tail = 0;
	u64 ino;
	timing_t create_time;

	PMFS_START_TIMING(create_t, create_time);

	pidir = pmfs_get_inode(sb, dir);
	if (!pidir)
		goto out_err;

	ino = pmfs_new_pmfs_inode(sb, &sih);
	if (ino == 0)
		goto out_err;

	err = pmfs_add_entry(dentry, &pi_addr, ino, 0, 1, 0, &tail);
	if (err)
		goto out_err;

	pmfs_dbg_verbose("%s: %s\n", __func__, dentry->d_name.name);
	inode = pmfs_new_vfs_inode(TYPE_CREATE, dir, pi_addr, sih, ino, mode,
					0, 0, &dentry->d_name);
	if (IS_ERR(inode))
		goto out_err;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	pmfs_update_tail(pidir, tail);
	PMFS_END_TIMING(create_t, create_time);
	return err;
out_err:
	pmfs_err(sb, "%s return %d\n", __func__, err);
	PMFS_END_TIMING(create_t, create_time);
	return err;
}

static int pmfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
		       dev_t rdev)
{
	struct inode *inode = NULL;
	int err = PTR_ERR(inode);
	struct super_block *sb = dir->i_sb;
	u64 pi_addr = 0;
	struct pmfs_inode *pidir;
	struct pmfs_inode_info_header *sih;
	u64 tail = 0;
	u64 ino;
	timing_t mknod_time;

	PMFS_START_TIMING(mknod_t, mknod_time);

	pidir = pmfs_get_inode(sb, dir);
	if (!pidir)
		goto out_err;

	ino = pmfs_new_pmfs_inode(sb, &sih);
	if (ino == 0)
		goto out_err;

	err = pmfs_add_entry(dentry, &pi_addr, ino, 0, 1, 0, &tail);
	if (err)
		goto out_err;

	inode = pmfs_new_vfs_inode(TYPE_MKNOD, dir, pi_addr, sih, ino, mode,
					0, rdev, &dentry->d_name);
	if (IS_ERR(inode))
		goto out_err;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	pmfs_update_tail(pidir, tail);
	PMFS_END_TIMING(mknod_t, mknod_time);
	return err;
out_err:
	pmfs_err(sb, "%s return %d\n", __func__, err);
	PMFS_END_TIMING(mknod_t, mknod_time);
	return err;
}

static int pmfs_symlink(struct inode *dir, struct dentry *dentry,
			 const char *symname)
{
	struct super_block *sb = dir->i_sb;
	int err = -ENAMETOOLONG;
	unsigned len = strlen(symname);
	struct inode *inode;
	u64 pi_addr = 0;
	struct pmfs_inode *pidir, *pi;
	struct pmfs_inode_info_header *sih;
	unsigned long blocknr = 0;
	int allocated;
	u64 tail = 0;
	u64 ino;
	timing_t symlink_time;

	PMFS_START_TIMING(symlink_t, symlink_time);
	if (len + 1 > sb->s_blocksize)
		goto out;

	pidir = pmfs_get_inode(sb, dir);
	if (!pidir)
		goto out_fail1;

	ino = pmfs_new_pmfs_inode(sb, &sih);
	if (ino == 0)
		goto out_fail1;

	err = pmfs_add_entry(dentry, &pi_addr, ino, 0, 1, 0, &tail);
	if (err)
		goto out_fail1;

	/* Pre-allocate symlink log page before allocating inode */
	allocated = pmfs_new_log_blocks(sb, &blocknr, 1,
						PMFS_BLOCK_TYPE_4K, 1);
	if (allocated != 1 || blocknr == 0)
		goto out_fail1;

	inode = pmfs_new_vfs_inode(TYPE_SYMLINK, dir, pi_addr, sih, ino,
					S_IFLNK|S_IRWXUGO, len, 0,
					&dentry->d_name);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		pmfs_free_log_blocks(sb, blocknr, 1,
					PMFS_BLOCK_TYPE_4K, NULL, 1);
		goto out_fail1;
	}

	pi = pmfs_get_inode(sb, inode);
	pmfs_block_symlink(sb, pi, inode, blocknr, symname, len);

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	pmfs_update_tail(pidir, tail);
out:
	PMFS_END_TIMING(symlink_t, symlink_time);
	return err;

out_fail1:
	pmfs_err(sb, "%s return %d\n", __func__, err);
	goto out;
}

static void pmfs_lite_transaction_for_time_and_link(struct super_block *sb,
	struct pmfs_inode *pi, struct pmfs_inode *pidir, struct inode *inode,
	u64 tail)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	struct pmfs_lite_journal_entry entry;
	u64 journal_tail;

	/* Commit a lite transaction */
	memset(&entry, 0, sizeof(struct pmfs_lite_journal_entry));
	entry.addrs[0] = (u64)pmfs_get_addr_off(sbi, &pi->i_ctime);
	entry.addrs[0] |= (u64)4 << 56;
	entry.values[0] = pi->i_ctime;
	entry.addrs[1] = (u64)pmfs_get_addr_off(sbi, &pi->i_links_count);
	entry.addrs[1] |= (u64)2 << 56;
	entry.values[1] = pi->i_links_count;

	entry.addrs[2] = (u64)pmfs_get_addr_off(sbi, &pidir->log_tail);
	entry.addrs[2] |= (u64)8 << 56;
	entry.values[2] = pidir->log_tail;

	mutex_lock(&sbi->lite_journal_mutex);
	journal_tail = pmfs_create_lite_transaction(sb, &entry);

	pi->i_links_count = cpu_to_le16(inode->i_nlink);
	pi->i_ctime = cpu_to_le32(inode->i_ctime.tv_sec);
	/* In the same cacheline */
	pmfs_flush_buffer(&pi->i_ctime, CACHELINE_SIZE, 1);
	pmfs_update_tail(pidir, tail);

	pmfs_commit_lite_transaction(sb, journal_tail);
	mutex_unlock(&sbi->lite_journal_mutex);
}

static int pmfs_link(struct dentry *dest_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = dest_dentry->d_inode;
	struct pmfs_inode *pi = pmfs_get_inode(sb, inode);
	struct pmfs_inode *pidir;
	u64 tail = 0;
	int err = -ENOMEM;
	timing_t link_time;

	PMFS_START_TIMING(link_t, link_time);
	if (inode->i_nlink >= PMFS_LINK_MAX)
		return -EMLINK;

	pidir = pmfs_get_inode(sb, dir);
	if (!pidir)
		return -EINVAL;

	ihold(inode);

	err = pmfs_add_entry(dentry, NULL, inode->i_ino, 0, 0, 0, &tail);
	if (!err) {
		inode->i_ctime = CURRENT_TIME_SEC;
		inc_nlink(inode);
		d_instantiate(dentry, inode);
	} else {
		iput(inode);
	}

	pmfs_lite_transaction_for_time_and_link(sb, pi, pidir, inode, tail);

	PMFS_END_TIMING(link_t, link_time);
	return err;
}

static int pmfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = dir->i_sb;
	int retval = -ENOMEM;
	struct pmfs_inode *pi = pmfs_get_inode(sb, inode);
	struct pmfs_inode *pidir;
	u64 tail = 0;
	timing_t unlink_time;

	PMFS_START_TIMING(unlink_t, unlink_time);

	pidir = pmfs_get_inode(sb, dir);
	if (!pidir)
		goto out;

	pmfs_dbg_verbose("%s: %s\n", __func__, dentry->d_name.name);
	retval = pmfs_remove_entry(dentry, 0, 0, &tail);
	if (retval)
		goto out;

//	if (inode->i_nlink == 1)
//		pmfs_truncate_add(inode, inode->i_size);
	inode->i_ctime = dir->i_ctime;

	if (inode->i_nlink) {
		drop_nlink(inode);
		/* FIXME: We still rely on this to find free inodes */
	}

	pmfs_lite_transaction_for_time_and_link(sb, pi, pidir, inode, tail);

	PMFS_END_TIMING(unlink_t, unlink_time);
	return 0;
out:
	pmfs_err(sb, "%s return %d\n", __func__, retval);
	PMFS_END_TIMING(unlink_t, unlink_time);
	return retval;
}

static int pmfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	struct pmfs_inode_info_header *sih;
	struct pmfs_inode *pidir, *pi;
	u64 pi_addr = 0;
	struct super_block *sb = dir->i_sb;
	u64 tail = 0;
	u64 ino;
	int err = -EMLINK;
	timing_t mkdir_time;

	PMFS_START_TIMING(mkdir_t, mkdir_time);
	if (dir->i_nlink >= PMFS_LINK_MAX)
		goto out;

	ino = pmfs_new_pmfs_inode(sb, &sih);
	if (ino == 0)
		goto out_err;

	err = pmfs_add_entry(dentry, &pi_addr, ino, 1, 1, 0, &tail);
	if (err) {
		pmfs_dbg("failed to add dir entry\n");
		goto out_err;
	}

	inode = pmfs_new_vfs_inode(TYPE_MKDIR, dir, pi_addr, sih, ino,
					S_IFDIR | mode, sb->s_blocksize,
					0, &dentry->d_name);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_err;
	}

	pi = pmfs_get_inode(sb, inode);
	pmfs_append_dir_init_entries(sb, pi, inode->i_ino, dir->i_ino);

	/* Build the dir tree */
	pmfs_rebuild_dir_inode_tree(sb, pi_addr, sih, inode->i_ino, NULL);

	pidir = pmfs_get_inode(sb, dir);
	inc_nlink(dir);
	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	pmfs_update_tail(pidir, tail);

out:
	PMFS_END_TIMING(mkdir_t, mkdir_time);
	return err;

out_err:
//	clear_nlink(inode);
	pmfs_err(sb, "%s return %d\n", __func__, err);
	goto out;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int pmfs_empty_dir(struct inode *inode)
{
	struct super_block *sb;
	struct pmfs_inode_info *si = PMFS_I(inode);
	struct pmfs_inode_info_header *sih = si->header;
	struct pmfs_dir_node *curr;
	struct pmfs_log_direntry *entry;
	struct rb_node *temp;

	sb = inode->i_sb;
	temp = rb_first(&sih->dir_tree);
	while (temp) {
		curr = container_of(temp, struct pmfs_dir_node, node);

		if (!curr || curr->nvmm == 0)
			BUG();

		entry = (struct pmfs_log_direntry *)
				pmfs_get_block(sb, curr->nvmm);
		if (!is_dir_init_entry(sb, entry))
			return 0;
		temp = rb_next(temp);
	}

	return 1;
}

static int pmfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct pmfs_log_direntry *de;
	struct super_block *sb = inode->i_sb;
	struct pmfs_inode *pi = pmfs_get_inode(sb, inode), *pidir;
	u64 tail = 0;
	struct pmfs_inode_info *si = PMFS_I(inode);
	struct pmfs_inode_info_header *sih = si->header;
	int err = -ENOTEMPTY;
	timing_t rmdir_time;

	PMFS_START_TIMING(rmdir_t, rmdir_time);
	if (!inode)
		return -ENOENT;

	pidir = pmfs_get_inode(sb, dir);
	if (!pidir)
		return -EINVAL;

	if (pmfs_inode_by_name(dir, &dentry->d_name, &de) == 0)
		return -ENOENT;

	if (!pmfs_empty_dir(inode))
		return err;

	if (inode->i_nlink != 2)
		pmfs_dbg("empty directory has nlink!=2 (%d)", inode->i_nlink);

	err = pmfs_remove_entry(dentry, -1, 0, &tail);
	if (err)
		goto end_rmdir;

	/*inode->i_version++; */
	clear_nlink(inode);
	inode->i_ctime = dir->i_ctime;

	/* add the inode to truncate list in case a crash happens before the
	 * subsequent evict_inode is called. It will be deleted from the
	 * truncate list during evict_inode.
	 */
//	pmfs_truncate_add(inode, inode->i_size);

	if (dir->i_nlink)
		drop_nlink(dir);

	pmfs_delete_dir_tree(sb, sih);

	pmfs_lite_transaction_for_time_and_link(sb, pi, pidir, inode, tail);

	PMFS_END_TIMING(rmdir_t, rmdir_time);
	return err;

end_rmdir:
	pmfs_err(sb, "%s return %d\n", __func__, err);
	PMFS_END_TIMING(rmdir_t, rmdir_time);
	return err;
}

static int pmfs_rename(struct inode *old_dir,
			struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	struct super_block *sb = old_inode->i_sb;
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	struct pmfs_inode *pi, *new_pidir = NULL, *old_pidir = NULL;
	struct pmfs_lite_journal_entry entry;
	u64 old_tail = 0, new_tail = 0, tail;
	int err = -ENOENT;
	bool need_trans = false;
	int inc_link = 0, dec_link = 0;
	u64 journal_tail;
	timing_t rename_time;

	PMFS_START_TIMING(rename_t, rename_time);

	if (new_inode) {
		err = -ENOTEMPTY;
		if (S_ISDIR(old_inode->i_mode) && !pmfs_empty_dir(new_inode))
			goto out;
	} else {
		if (S_ISDIR(old_inode->i_mode)) {
			err = -EMLINK;
			if (new_dir->i_nlink >= PMFS_LINK_MAX)
				goto out;
		}
		if (S_ISDIR(old_inode->i_mode)) {
			inc_link = 1;
			dec_link = -1;
		}
	}

	new_pidir = pmfs_get_inode(sb, new_dir);
	old_pidir = pmfs_get_inode(sb, old_dir);

	pi = pmfs_get_inode(sb, old_inode);

	if (new_inode) {
		/* First remove the old entry in the new directory */
		err = pmfs_remove_entry(new_dentry, 0,  0, &new_tail);
		if (err)
			goto out;
	}

	/* link into the new directory. */
	err = pmfs_add_entry(new_dentry, NULL, old_inode->i_ino,
					inc_link, 0, new_tail, &new_tail);
	if (err)
		goto out;

	/* and unlink the inode from the old directory ... */
	if (new_pidir == old_pidir)
		tail = new_tail;
	else
		tail = 0;

	err = pmfs_remove_entry(old_dentry, dec_link, tail, &old_tail);
	if (err)
		goto out;

	if (new_inode) {
		need_trans = true;
		pi = pmfs_get_inode(sb, new_inode);
		new_inode->i_ctime = CURRENT_TIME;

		if (S_ISDIR(old_inode->i_mode)) {
			if (new_inode->i_nlink)
				drop_nlink(new_inode);
		}
		if (new_inode->i_nlink)
			drop_nlink(new_inode);

//		if (!new_inode->i_nlink)
//			pmfs_truncate_add(new_inode, new_inode->i_size);
	}

	if (inc_link)
		inc_nlink(new_dir);
	if (dec_link < 0)
		drop_nlink(old_dir);

	if (need_trans && old_pidir == new_pidir) {
		pmfs_update_tail(new_pidir, old_tail);
	} else {
		memset(&entry, 0, sizeof(struct pmfs_lite_journal_entry));
		if (new_inode) {
			entry.addrs[0] = (u64)pmfs_get_addr_off(sbi,
							&pi->i_ctime);
			entry.addrs[0] |= (u64)4 << 56;
			entry.values[0] = pi->i_ctime;
			entry.addrs[1] = (u64)pmfs_get_addr_off(sbi,
							&pi->i_links_count);
			entry.addrs[1] |= (u64)2 << 56;
			entry.values[1] = pi->i_links_count;
		}

		entry.addrs[2] = (u64)pmfs_get_addr_off(sbi,
						&old_pidir->log_tail);
		entry.addrs[2] |= (u64)8 << 56;
		entry.values[2] = old_pidir->log_tail;
		entry.addrs[3] = (u64)pmfs_get_addr_off(sbi,
						&new_pidir->log_tail);
		entry.addrs[3] |= (u64)8 << 56;
		entry.values[3] = new_pidir->log_tail;

		mutex_lock(&sbi->lite_journal_mutex);
		journal_tail = pmfs_create_lite_transaction(sb, &entry);

		pmfs_update_tail(old_pidir, old_tail);
		pmfs_update_tail(new_pidir, new_tail);
		if (new_inode) {
			pi->i_ctime = cpu_to_le32(new_inode->i_ctime.tv_sec);
			pi->i_links_count = cpu_to_le16(new_inode->i_nlink);
			/* In the same cacheline */
			pmfs_flush_buffer(&pi->i_ctime, CACHELINE_SIZE, 1);
		}
		pmfs_commit_lite_transaction(sb, journal_tail);
		mutex_unlock(&sbi->lite_journal_mutex);
	}

	PMFS_END_TIMING(rename_t, rename_time);
	return 0;
out:
	pmfs_err(sb, "%s return %d\n", __func__, err);
	PMFS_END_TIMING(rename_t, rename_time);
	return err;
}

struct dentry *pmfs_get_parent(struct dentry *child)
{
	struct inode *inode;
	struct qstr dotdot = QSTR_INIT("..", 2);
	struct pmfs_log_direntry *de = NULL;
	ino_t ino;

	pmfs_inode_by_name(child->d_inode, &dotdot, &de);
	if (!de)
		return ERR_PTR(-ENOENT);
	ino = le64_to_cpu(de->ino);

	if (ino)
		inode = pmfs_iget(child->d_inode->i_sb, ino);
	else
		return ERR_PTR(-ENOENT);

	return d_obtain_alias(inode);
}

const struct inode_operations pmfs_dir_inode_operations = {
	.create		= pmfs_create,
	.lookup		= pmfs_lookup,
	.link		= pmfs_link,
	.unlink		= pmfs_unlink,
	.symlink	= pmfs_symlink,
	.mkdir		= pmfs_mkdir,
	.rmdir		= pmfs_rmdir,
	.mknod		= pmfs_mknod,
	.rename		= pmfs_rename,
	.setattr	= pmfs_notify_change,
	.get_acl	= NULL,
};

const struct inode_operations pmfs_special_inode_operations = {
	.setattr	= pmfs_notify_change,
	.get_acl	= NULL,
};
