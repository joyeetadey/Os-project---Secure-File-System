#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "myfs.h"

/* Mount a myfs partition */
struct dentry *myfs_mount(struct file_system_type *fs_type,int flags,const char *dev_name,void *data)
{
    struct dentry *dentry =
        mount_bdev(fs_type, flags, dev_name, data, myfs_fill_super);
    if (IS_ERR(dentry))
        pr_err("'%s' mount failure\n", dev_name);
    else
        pr_info("'%s' mount success\n", dev_name);

    return dentry;
}

/* Unmount a myfs partition */
void myfs_kill_sb(struct super_block *sb)
{
    kill_block_super(sb);

    pr_info("unmounted disk\n");
}

static struct file_system_type myfs_file_system_type = {
    .owner = THIS_MODULE,
    .name = "myfs",
    .mount = myfs_mount,
    .kill_sb = myfs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
    .next = NULL,
};

//init function
static int __init myfs_init(void)
{
    //create inode cache
    int ret = myfs_init_inode_cache();
    if (ret) {
        pr_err("inode cache creation failed\n");
        goto end;
    }
	
    //register filesystem
    ret = register_filesystem(&myfs_file_system_type);	//file system type
    if (ret) {
        pr_err("register_filesystem() failed\n");
        goto end;
    }

    pr_info("module loaded\n");
end:
    return ret;
}

//exit function
static void __exit myfs_exit(void)
{
    int ret = unregister_filesystem(&myfs_file_system_type);
    if (ret)
        pr_err("unregister_filesystem() failed\n");

    myfs_destroy_inode_cache();

    pr_info("module unloaded\n");
}

module_init(myfs_init);
module_exit(myfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joyeeta Dey");
MODULE_DESCRIPTION("a simple file system");

