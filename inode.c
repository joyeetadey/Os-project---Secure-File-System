#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "bitmap.h"
#include "myfs.h"

static const struct inode_operations myfs_inode_ops;
static const struct inode_operations symlink_inode_ops;

/* Get inode number from disk */
struct inode *myfs_iget(struct super_block *sb, unsigned long ino)
{
    struct inode *inode = NULL;
    struct myfs_inode *cinode = NULL;	
    struct myfs_inode_info *ci = NULL;	
    struct myfs_sb_info *sbi = MYFS_SB(sb);
    struct buffer_head *bh = NULL;
    uint32_t inode_block = (ino / MYFS_INODES_PER_BLOCK) + 1;
    uint32_t inode_shift = ino % MYFS_INODES_PER_BLOCK;
    int ret;

    /* Fail if ino is out of range */
    if (ino >= sbi->nr_inodes)
        return ERR_PTR(-EINVAL);

    /* Get a locked inode from Linux */
    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    /* If inode is in cache, return it */
    if (!(inode->i_state & I_NEW))
        return inode;

    ci = MYFS_INODE(inode);
    /* Read inode from disk and initialize */
    bh = sb_bread(sb, inode_block);
    if (!bh) {
        ret = -EIO;
        goto failed;
    }
    cinode = (struct myfs_inode *) bh->b_data;
    cinode += inode_shift;

    inode->i_ino = ino;	//inode number
    inode->i_sb = sb;		//super block to which this inode belongs to 
    inode->i_op = &myfs_inode_ops;	//pointers to inode operation structures

    inode->i_mode = le32_to_cpu(cinode->i_mode);	//mode
    i_uid_write(inode, le32_to_cpu(cinode->i_uid));	
    i_gid_write(inode, le32_to_cpu(cinode->i_gid));
    inode->i_size = le32_to_cpu(cinode->i_size);	//inode size
    inode->i_ctime.tv_sec = (time64_t) le32_to_cpu(cinode->i_ctime);	//create time
    inode->i_ctime.tv_nsec = 0;					
    inode->i_atime.tv_sec = (time64_t) le32_to_cpu(cinode->i_atime);	//access time
    inode->i_atime.tv_nsec = 0;
    inode->i_mtime.tv_sec = (time64_t) le32_to_cpu(cinode->i_mtime);	//change time
    inode->i_mtime.tv_nsec = 0;
    inode->i_blocks = le32_to_cpu(cinode->i_blocks);	//the number of blocks used by the file
    set_nlink(inode, le32_to_cpu(cinode->i_nlink));	//the number of names entries (dentries) that use this inode

    if (S_ISDIR(inode->i_mode)) {
        ci->dir_block = le32_to_cpu(cinode->dir_block);
        inode->i_fop = &myfs_dir_ops;
    } else if (S_ISREG(inode->i_mode)) {
        ci->ei_block = le32_to_cpu(cinode->ei_block);
        inode->i_fop = &myfs_file_ops;
        inode->i_mapping->a_ops = &myfs_aops;
    } else if (S_ISLNK(inode->i_mode)) {
        strncpy(ci->i_data, cinode->i_data, sizeof(ci->i_data));
        inode->i_link = ci->i_data;
        inode->i_op = &symlink_inode_ops;
    }

    brelse(bh);

    /* Unlock the inode to make it usable */
    unlock_new_inode(inode);

    return inode;

failed:
    brelse(bh);
    iget_failed(inode);
    return ERR_PTR(ret);

}

/*
 * Look for dentry in dir.
 * Fill dentry with NULL if not in dir, with the corresponding inode if found.
 * Returns NULL on success.
 */
static struct dentry *myfs_lookup(struct inode *dir,struct dentry *dentry,unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct myfs_inode_info *ci_dir = MYFS_INODE(dir);
    struct inode *inode = NULL;
    struct buffer_head *bh = NULL;
    struct myfs_dir_block *dblock = NULL;
    struct myfs_file *f = NULL;
    int i;

    /* Check filename length */
    if (dentry->d_name.len > MYFS_FILENAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    /* Read the directory block on disk */
    bh = sb_bread(sb, ci_dir->dir_block);
    if (!bh)
        return ERR_PTR(-EIO);
    dblock = (struct myfs_dir_block *) bh->b_data;

    /* Search for the file in directory */
    for (i = 0; i < MYFS_MAX_SUBFILES; i++) {
        f = &dblock->files[i];
        if (!f->inode)
            break;
        if (!strncmp(f->filename, dentry->d_name.name, MYFS_FILENAME_LEN)) {
            inode = myfs_iget(sb, f->inode);
            break;
        }
    }
    brelse(bh);

    /* Update directory access time */
    dir->i_atime = current_time(dir);
    mark_inode_dirty(dir);

    /* Fill the dentry with the inode */
    d_add(dentry, inode);

    return NULL;
}

/* Create a new inode in dir */
static struct inode *myfs_new_inode(struct inode *dir, mode_t mode)
{
    struct inode *inode;
    struct myfs_inode_info *ci;
    struct super_block *sb;
    struct myfs_sb_info *sbi;
    uint32_t ino, bno;
    int ret;

    /* Check mode before doing anything to avoid undoing everything */
    if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) {
        pr_err(
            "File type not supported (only directory, regular file and symlink "
            "supported)\n");
        return ERR_PTR(-EINVAL);
    }

    /* Check if inodes are available */
    sb = dir->i_sb;
    sbi = MYFS_SB(sb);
    if (sbi->nr_free_inodes == 0 || sbi->nr_free_blocks == 0)
        return ERR_PTR(-ENOSPC);

    /* Get a new free inode */
    ino = get_free_inode(sbi);
    if (!ino)
        return ERR_PTR(-ENOSPC);

    inode = myfs_iget(sb, ino);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto put_ino;
    }

    if (S_ISLNK(mode)) {
        inode_init_owner(inode, dir, mode);
        set_nlink(inode, 1);
        inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);
        inode->i_op = &symlink_inode_ops;
        return inode;
    }

    ci = MYFS_INODE(inode);

    /* Get a free block for this new inode's index */
    bno = get_free_blocks(sbi, 1);
    if (!bno) {
        ret = -ENOSPC;
        goto put_inode;
    }

    /* Initialize inode */
    inode_init_owner(inode, dir, mode);	//inode, directory and mode in owner function
    inode->i_blocks = 1;
    if (S_ISDIR(mode)) {
        ci->dir_block = bno;
        inode->i_size = MYFS_BLOCK_SIZE;
        inode->i_fop = &myfs_dir_ops;
        set_nlink(inode, 2); /* . and .. */
    } else if (S_ISREG(mode)) {
        ci->ei_block = bno;
        inode->i_size = 0;
        inode->i_fop = &myfs_file_ops;
        inode->i_mapping->a_ops = &myfs_aops;
        set_nlink(inode, 1);
    }

    inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);	//at the time of creation all times are same

    return inode;

put_inode:
    iput(inode);
put_ino:
    put_inode(sbi, ino);

    return ERR_PTR(ret);
}

/*
 * Create a file or directory in this way:
 *   - check filename length and if the parent directory is not full
 *   - create the new inode (allocate inode and blocks)
 *   - cleanup index block of the new inode
 *   - add new file/directory in parent index
 */
static int myfs_create(struct inode *dir,struct dentry *dentry,umode_t mode,bool excl)
{
    struct super_block *sb;
    struct inode *inode;
    struct myfs_inode_info *ci_dir;
    struct myfs_dir_block *dblock;
    char *fblock;
    struct buffer_head *bh, *bh2;
    int ret = 0, i;

    /* Check filename length */
    if (strlen(dentry->d_name.name) > MYFS_FILENAME_LEN)
        return -ENAMETOOLONG;

    /* Read parent directory index */
    ci_dir = MYFS_INODE(dir);
    sb = dir->i_sb;
    bh = sb_bread(sb, ci_dir->dir_block);
    if (!bh)
        return -EIO;

    dblock = (struct myfs_dir_block *) bh->b_data;

    /* Check if parent directory is full */
    if (dblock->files[MYFS_MAX_SUBFILES - 1].inode != 0) {
        ret = -EMLINK;
        goto end;
    }

    /* Get a new free inode */
    inode = myfs_new_inode(dir, mode);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto end;
    }

    /*
     * Scrub ei_block/dir_block for new file/directory to avoid previous data
     * messing with new file/directory.
     */
    bh2 = sb_bread(sb, MYFS_INODE(inode)->ei_block);
    if (!bh2) {
        ret = -EIO;
        goto iput;
    }
    fblock = (char *) bh2->b_data;
    memset(fblock, 0, MYFS_BLOCK_SIZE);
    mark_buffer_dirty(bh2);
    brelse(bh2);

    /* Find first free slot in parent index and register new inode */
    for (i = 0; i < MYFS_MAX_SUBFILES; i++)
        if (dblock->files[i].inode == 0)
            break;
    dblock->files[i].inode = inode->i_ino;
    strncpy(dblock->files[i].filename, dentry->d_name.name,
            MYFS_FILENAME_LEN);
    mark_buffer_dirty(bh);
    brelse(bh);

    /* Update stats and mark dir and new inode dirty */
    mark_inode_dirty(inode);
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
    if (S_ISDIR(mode))
        inc_nlink(dir);
    mark_inode_dirty(dir);

    /* setup dentry */
    d_instantiate(dentry, inode);

    return 0;

iput:
    put_blocks(MYFS_SB(sb), MYFS_INODE(inode)->ei_block, 1);
    put_inode(MYFS_SB(sb), inode->i_ino);
    iput(inode);
end:
    brelse(bh);
    return ret;
}

/*
 * Remove a link for a file including the reference in the parent directory.
 * If link count is 0, destroy file in this way:
 *   - remove the file from its parent directory.
 *   - cleanup blocks containing data
 *   - cleanup file index block
 *   - cleanup inode
 */
static int myfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct myfs_sb_info *sbi = MYFS_SB(sb);
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh = NULL, *bh2 = NULL;
    struct myfs_dir_block *dir_block = NULL;
    struct myfs_file_ei_block *file_block = NULL;
    int i, j, f_id = -1, nr_subs = 0;

    uint32_t ino = inode->i_ino;
    uint32_t bno = 0;

    /* Read parent directory index */
    bh = sb_bread(sb, MYFS_INODE(dir)->dir_block);
    if (!bh)
        return -EIO;
    dir_block = (struct myfs_dir_block *) bh->b_data;

    /* Search for inode in parent index and get number of subfiles */
    for (i = 0; i < MYFS_MAX_SUBFILES; i++) {
        if (strncmp(dir_block->files[i].filename, dentry->d_name.name,
                    MYFS_FILENAME_LEN) == 0)
            f_id = i;
        else if (dir_block->files[i].inode == 0)
            break;
    }
    nr_subs = i;

    /* Remove file from parent directory */
    if (f_id != MYFS_MAX_SUBFILES - 1)
        memmove(dir_block->files + f_id, dir_block->files + f_id + 1,
                (nr_subs - f_id - 1) * sizeof(struct myfs_file));
    memset(&dir_block->files[nr_subs - 1], 0, sizeof(struct myfs_file));
    mark_buffer_dirty(bh);
    brelse(bh);

    if (S_ISLNK(inode->i_mode))
        goto clean_inode;

    /* Update inode stats */
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
    if (S_ISDIR(inode->i_mode)) {
        drop_nlink(dir);
        drop_nlink(inode);
    }
    mark_inode_dirty(dir);

    if (inode->i_nlink > 1) {
        inode_dec_link_count(inode);
        return 0;
    }

    /*
     * Cleanup pointed blocks if unlinking a file. If we fail to read the
     * index block, cleanup inode anyway and lose this file's blocks
     * forever. If we fail to scrub a data block, don't fail (too late
     * anyway), just put the block and continue.
     */
    bno = MYFS_INODE(inode)->ei_block;
    bh = sb_bread(sb, bno);
    if (!bh)
        goto clean_inode;
    file_block = (struct myfs_file_ei_block *) bh->b_data;
    if (S_ISDIR(inode->i_mode))
        goto scrub;
    for (i = 0; i < MYFS_MAX_EXTENTS; i++) {
        char *block;

        if (!file_block->extents[i].ee_start)
            break;

        put_blocks(sbi, file_block->extents[i].ee_start,
                   file_block->extents[i].ee_len);

        /* Scrub the extent */
        for (j = 0; j < file_block->extents[i].ee_len; j++) {
            bh2 = sb_bread(sb, file_block->extents[i].ee_start + j);
            if (!bh2)
                continue;
            block = (char *) bh2->b_data;
            memset(block, 0, MYFS_BLOCK_SIZE);
            mark_buffer_dirty(bh2);
            brelse(bh2);
        }
    }

scrub:
    /* Scrub index block */
    memset(file_block, 0, MYFS_BLOCK_SIZE);
    mark_buffer_dirty(bh);
    brelse(bh);

clean_inode:
    /* Cleanup inode and mark dirty */
    inode->i_blocks = 0;
    MYFS_INODE(inode)->ei_block = 0;
    inode->i_size = 0;
    i_uid_write(inode, 0);
    i_gid_write(inode, 0);
    inode->i_mode = 0;
    inode->i_ctime.tv_sec = inode->i_mtime.tv_sec = inode->i_atime.tv_sec = 0;
    drop_nlink(inode);
    mark_inode_dirty(inode);

    /* Free inode and index block from bitmap */
    put_blocks(sbi, bno, 1);
    put_inode(sbi, ino);

    return 0;
}

static int myfs_mkdir(struct inode *dir,
                          struct dentry *dentry,
                          umode_t mode)
{
    return myfs_create(dir, dentry, mode | S_IFDIR, 0);
}

static int myfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh;
    struct myfs_dir_block *dblock;

    /* If the directory is not empty, fail */
    if (inode->i_nlink > 2)
        return -ENOTEMPTY;
    bh = sb_bread(sb, MYFS_INODE(inode)->dir_block);
    if (!bh)
        return -EIO;
    dblock = (struct myfs_dir_block *) bh->b_data;
    if (dblock->files[0].inode != 0) {
        brelse(bh);
        return -ENOTEMPTY;
    }
    brelse(bh);

    /* Remove directory with unlink */
    return myfs_unlink(dir, dentry);
}

/*
Binds the new dentry to the inode.
Increments the i_nlink field of the inode.
Marks the inode as dirty using the mark_inode_dirty() function.
*/
static int myfs_link(struct dentry *old_dentry,struct inode *dir,struct dentry *dentry)
{
    struct inode *inode = d_inode(old_dentry);
    struct super_block *sb = inode->i_sb;
    struct myfs_inode_info *ci_dir = MYFS_INODE(dir);
    struct myfs_dir_block *dir_block;
    struct buffer_head *bh;
    int f_pos = -1, ret = 0, i = 0;

    bh = sb_bread(sb, ci_dir->dir_block);
    if (!bh)
        return -EIO;
    dir_block = (struct myfs_dir_block *) bh->b_data;

    if (dir_block->files[MYFS_MAX_SUBFILES - 1].inode != 0) {
        ret = -EMLINK;
        printk(KERN_INFO "directory is full");
        goto end;
    }

    for (i = 0; i < MYFS_MAX_SUBFILES; i++) {
        if (dir_block->files[i].inode == 0) {
            f_pos = i;
            break;
        }
    }

    dir_block->files[f_pos].inode = inode->i_ino;
    strncpy(dir_block->files[f_pos].filename, dentry->d_name.name,
            MYFS_FILENAME_LEN);
    mark_buffer_dirty(bh);
    inode_inc_link_count(inode);
    d_instantiate(dentry, inode);
end:
    brelse(bh);
    return ret;
}

/*
The symbolic link creation function is indicated by the symlink field in the inode_operations structure.
In the minix case, the function is minix_symlink() . The operations to be performed are similar to
minix_link with the differences being given by the fact that a symbolic link is created.
*/
static int myfs_symlink(struct inode *dir,struct dentry *dentry,const char *symname)
{
    struct super_block *sb = dir->i_sb;
    unsigned int l = strlen(symname) + 1;
    struct inode *inode = myfs_new_inode(dir, S_IFLNK | S_IRWXUGO);
    struct myfs_inode_info *ci = MYFS_INODE(inode);
    struct myfs_inode_info *ci_dir = MYFS_INODE(dir);
    struct myfs_dir_block *dir_block;
    struct buffer_head *bh;
    int f_pos = 0, i = 0;

    /* Check if symlink content is not too long */
    if (l > sizeof(ci->i_data))
        return -ENAMETOOLONG;

    /* fill directory data block */
    bh = sb_bread(sb, ci_dir->dir_block);

    if (!bh)
        return -EIO;
    dir_block = (struct myfs_dir_block *) bh->b_data;

    if (dir_block->files[MYFS_MAX_SUBFILES - 1].inode != 0) {
        printk(KERN_INFO "directory is full\n");
        return -EMLINK;
    }

    for (i = 0; i < MYFS_MAX_SUBFILES; i++) {
        if (dir_block->files[i].inode == 0) {
            f_pos = i;
            break;
        }
    }

    dir_block->files[f_pos].inode = inode->i_ino;
    strncpy(dir_block->files[f_pos].filename, dentry->d_name.name,
            MYFS_FILENAME_LEN);
    mark_buffer_dirty(bh);
    brelse(bh);

    inode->i_link = (char *) ci->i_data;
    memcpy(inode->i_link, symname, l);
    inode->i_size = l - 1;
    mark_inode_dirty(inode);
    d_instantiate(dentry, inode);

    return 0;
}

//get link
static const char *myfs_get_link(struct dentry *dentry,struct inode *inode,struct delayed_call *done)
{
    return inode->i_link;
}

//inode operations structure
static const struct inode_operations myfs_inode_ops = {
    .lookup = myfs_lookup,
    .create = myfs_create,
    .unlink = myfs_unlink,
    .mkdir = myfs_mkdir,
    .rmdir = myfs_rmdir,
    .link = myfs_link,
    .symlink = myfs_symlink,
};

static const struct inode_operations symlink_inode_ops = {
    .get_link = myfs_get_link,
};
