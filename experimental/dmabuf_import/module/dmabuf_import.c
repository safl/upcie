// SPDX-License-Identifier: GPL-2.0
/*
 * udmabuf_import - standalone (out-of-tree) dma-buf importer
 *
 * Extraction of the udmabuf "import a dma-buf + share its DMA addresses
 * with userspace" ioctls, originally delivered as a patch to the in-tree
 * udmabuf driver. Because Ubuntu ships CONFIG_UDMABUF=y (built into
 * vmlinuz), changing udmabuf's code required rebuilding the kernel. The
 * importer functionality, however, is purely additive and self-contained:
 * it never calls a udmabuf-internal function -- it only uses EXPORTED
 * dma-buf core APIs (dma_buf_get/attach/map_attachment_unlocked/... ) plus
 * rbtree. So it is lifted here into its own module that registers its own
 * misc device (/dev/udmabuf_import) and COEXISTS with the stock built-in
 * udmabuf. That makes it buildable + shippable out-of-tree (DKMS) against a
 * stock kernel -- no kernel rebuild, no custom kernel.
 *
 * Userspace keeps using the stock /dev/udmabuf for UDMABUF_CREATE and uses
 * /dev/udmabuf_import for UDMABUF_ATTACH / UDMABUF_GET_MAP / UDMABUF_DETACH.
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/uaccess.h>
#include <linux/rbtree.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>

#include "dmabuf_import.h"

/* Forward decl so the ioctl handlers can reach the device for dma_buf_attach. */
static struct miscdevice udmabuf_import_misc;

/* ---- imported dma-buf tracking ------------------------------------------ */

/*
 * Per-open-file state. The original patch kept one global rbtree keyed by the
 * raw dma-buf fd number, which is wrong once more than one process uses the
 * device: fd numbers are per-process, so two processes that each hold an
 * unrelated dma-buf at, say, fd 7 collide on the same key. The loser of the
 * insert silently reads the other process's DMA addresses, and either one can
 * tear down the other's attachment with UDMABUF_DETACH. Scoping the tree to the
 * open file makes the key unambiguous, and it also bounds the lifetime of the
 * attachments: whatever is left at close() is torn down instead of leaked until
 * module unload.
 *
 * The lock is a mutex, not the original rwlock, because everything it now
 * covers may sleep (dma_buf_attach() and friends take dma_resv_lock).
 */
struct udmabuf_import_ctx {
	struct mutex lock;
	struct rb_root tree;
};

struct udmabuf_dma_buf_desc {
	int				dma_buf_fd;
	enum dma_data_direction		dir;
	struct dma_buf_attachment	*attach;
	struct dma_buf			*dma_buf;
	struct sg_table			*sgt;
	struct rb_node			node;
};

static struct udmabuf_dma_buf_desc *udmabuf_dma_tree_find(struct rb_root *root, int dma_buf_fd)
{
	struct udmabuf_dma_buf_desc *this;
	struct rb_node *node = root->rb_node;

	while (node) {
		this = container_of(node, struct udmabuf_dma_buf_desc, node);

		if (dma_buf_fd < this->dma_buf_fd)
			node = node->rb_left;
		else if (dma_buf_fd > this->dma_buf_fd)
			node = node->rb_right;
		else
			return this;
	}
	return NULL;
}

static int udmabuf_dma_tree_insert(struct rb_root *root, struct udmabuf_dma_buf_desc *desc)
{
	struct udmabuf_dma_buf_desc *this;
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		this = container_of(*link, struct udmabuf_dma_buf_desc, node);
		parent = *link;

		if (desc->dma_buf_fd < this->dma_buf_fd)
			link = &parent->rb_left;
		else if (desc->dma_buf_fd > this->dma_buf_fd)
			link = &parent->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&desc->node, parent, link);
	rb_insert_color(&desc->node, root);

	return 0;
}

/*
 * Look up an attachment by fd, and confirm the fd still names the dma-buf it
 * named at UDMABUF_ATTACH time. Without that check a closed-and-reused fd
 * number resolves to the stale descriptor and hands back the DMA addresses of
 * a completely different buffer. Caller must hold ctx->lock.
 */
static struct udmabuf_dma_buf_desc *udmabuf_get_dma_buf_desc(struct udmabuf_import_ctx *ctx,
							     int dma_buf_fd)
{
	struct udmabuf_dma_buf_desc *desc;
	struct dma_buf *dmabuf;

	desc = udmabuf_dma_tree_find(&ctx->tree, dma_buf_fd);
	if (!desc)
		return ERR_PTR(-EINVAL);

	dmabuf = dma_buf_get(dma_buf_fd);
	if (IS_ERR(dmabuf))
		return ERR_CAST(dmabuf);

	if (dmabuf != desc->dma_buf) {
		dma_buf_put(dmabuf);
		return ERR_PTR(-EINVAL);
	}
	dma_buf_put(dmabuf);

	return desc;
}

/*
 * Tear down a desc's dma-buf attachment and free it. The desc must already be
 * out of (or never in) the tree.
 */
static void udmabuf_detach(struct udmabuf_dma_buf_desc *desc)
{
	if (!desc)
		return;

	if (desc->sgt)
		dma_buf_unmap_attachment_unlocked(desc->attach, desc->sgt, desc->dir);
	if (desc->attach)
		dma_buf_detach(desc->dma_buf, desc->attach);
	if (desc->dma_buf)
		dma_buf_put(desc->dma_buf);

	kfree(desc);
}

/* Caller must hold ctx->lock. */
static int udmabuf_attach(struct udmabuf_import_ctx *ctx,
			  struct udmabuf_dma_buf_desc **desc, int dma_buf_fd,
			  struct device *dev, enum dma_data_direction dir)
{
	struct udmabuf_dma_buf_desc *tmp;
	int ret;

	if (WARN_ON_ONCE(!dev))
		return -EFAULT;

	tmp = udmabuf_get_dma_buf_desc(ctx, dma_buf_fd);
	if (!IS_ERR(tmp)) {
		/* Don't fail if dma-buf is already attached. */
		*desc = tmp;
		return 0;
	}
	/* A stale entry (fd number reused for a different dma-buf) is replaced
	 * rather than reported, so re-attaching on a recycled fd works. */
	tmp = udmabuf_dma_tree_find(&ctx->tree, dma_buf_fd);
	if (tmp) {
		rb_erase(&tmp->node, &ctx->tree);
		udmabuf_detach(tmp);
	}

	tmp = kzalloc(sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	tmp->dir = dir;
	tmp->dma_buf = dma_buf_get(dma_buf_fd);
	if (IS_ERR(tmp->dma_buf)) {
		ret = PTR_ERR(tmp->dma_buf);
		tmp->dma_buf = NULL;
		goto err;
	}

	tmp->attach = dma_buf_attach(tmp->dma_buf, dev);
	if (IS_ERR(tmp->attach)) {
		ret = PTR_ERR(tmp->attach);
		tmp->attach = NULL;
		goto err;
	}

	tmp->sgt = dma_buf_map_attachment_unlocked(tmp->attach, dir);
	if (IS_ERR(tmp->sgt)) {
		ret = PTR_ERR(tmp->sgt);
		tmp->sgt = NULL;
		goto err;
	}

	tmp->dir = dir;
	tmp->dma_buf_fd = dma_buf_fd;

	ret = udmabuf_dma_tree_insert(&ctx->tree, tmp);
	if (ret)
		goto err;

	*desc = tmp;
	return 0;
err:
	udmabuf_detach(tmp);
	return ret;
}

/* ---- ioctls ------------------------------------------------------------- */

static long udmabuf_ioctl_attach(struct file *filp, unsigned long arg)
{
	struct udmabuf_import_ctx *ctx = filp->private_data;
	struct device *dev = udmabuf_import_misc.this_device;
	struct udmabuf_attach __user *uattach;
	struct udmabuf_attach attach;
	struct udmabuf_dma_buf_desc *desc;
	int ret;

	uattach = (void __user *)arg;

	if (copy_from_user(&attach, uattach, sizeof(attach)))
		return -EFAULT;

	mutex_lock(&ctx->lock);
	ret = udmabuf_attach(ctx, &desc, attach.fd, dev, DMA_BIDIRECTIONAL);
	if (!ret)
		attach.count = desc->sgt->nents;
	mutex_unlock(&ctx->lock);
	if (ret)
		return ret;

	if (copy_to_user(uattach, &attach, sizeof(attach)))
		return -EFAULT;

	return 0;
}

static long udmabuf_ioctl_detach(struct file *filp, unsigned long arg)
{
	struct udmabuf_import_ctx *ctx = filp->private_data;
	struct udmabuf_dma_buf_desc *desc;
	int fd;

	if (copy_from_user(&fd, (void __user *)arg, sizeof(fd)))
		return -EFAULT;

	mutex_lock(&ctx->lock);
	desc = udmabuf_dma_tree_find(&ctx->tree, fd);
	if (desc)
		rb_erase(&desc->node, &ctx->tree);
	mutex_unlock(&ctx->lock);

	if (!desc)
		return -EINVAL;

	udmabuf_detach(desc);
	return 0;
}

static long udmabuf_ioctl_get_map(struct file *filp, unsigned long arg)
{
	struct udmabuf_import_ctx *ctx = filp->private_data;
	struct udmabuf_dma_buf_desc *desc;
	struct udmabuf_get_map __user *uget_map;
	struct udmabuf_get_map get_map;
	struct udmabuf_dma_map map;
	struct udmabuf_dma_map __user *umap;
	struct scatterlist *sg;
	int ret = 0;
	int i;

	uget_map = (void __user *)arg;

	if (copy_from_user(&get_map, uget_map, sizeof(get_map)))
		return -EFAULT;

	/*
	 * Hold the lock across the copy_to_user loop: the descriptor and its
	 * sg_table belong to the tree, and a concurrent UDMABUF_DETACH on
	 * another thread of the same process would otherwise free them under us.
	 */
	mutex_lock(&ctx->lock);
	desc = udmabuf_get_dma_buf_desc(ctx, get_map.fd);
	if (IS_ERR(desc)) {
		mutex_unlock(&ctx->lock);
		return PTR_ERR(desc);
	}

	umap = uget_map->dma_arr;
	for_each_sgtable_dma_sg(desc->sgt, sg, i) {
		/* Userspace allocated get_map.count entries (0..count-1); stop
		 * before writing umap[count] to avoid a one-past-the-end write. */
		if (i >= get_map.count)
			break;

		map.dma_addr = sg_dma_address(sg);
		map.dma_len = sg_dma_len(sg);
		if (copy_to_user(umap + i, &map, sizeof(map))) {
			ret = -EFAULT;
			break;
		}
	}
	mutex_unlock(&ctx->lock);

	return ret;
}

static int udmabuf_import_open(struct inode *inode, struct file *filp)
{
	struct udmabuf_import_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	ctx->tree = RB_ROOT;
	filp->private_data = ctx;

	return 0;
}

static int udmabuf_import_release(struct inode *inode, struct file *filp)
{
	struct udmabuf_import_ctx *ctx = filp->private_data;
	struct udmabuf_dma_buf_desc *desc, *tmp;

	if (!ctx)
		return 0;

	/* Last reference to the file: no other user of this ctx remains. */
	rbtree_postorder_for_each_entry_safe(desc, tmp, &ctx->tree, node)
		udmabuf_detach(desc);

	mutex_destroy(&ctx->lock);
	kfree(ctx);

	return 0;
}

static long udmabuf_import_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case UDMABUF_ATTACH:
		return udmabuf_ioctl_attach(filp, arg);
	case UDMABUF_DETACH:
		return udmabuf_ioctl_detach(filp, arg);
	case UDMABUF_GET_MAP:
		return udmabuf_ioctl_get_map(filp, arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations udmabuf_import_fops = {
	.owner		= THIS_MODULE,
	.open		= udmabuf_import_open,
	.release	= udmabuf_import_release,
	.unlocked_ioctl	= udmabuf_import_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice udmabuf_import_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "udmabuf_import",
	.fops	= &udmabuf_import_fops,
	/* Handing out device DMA addresses is a privileged operation; keep the
	 * node root-only rather than relying on the distro default. */
	.mode	= 0600,
};

static int __init udmabuf_import_init(void)
{
	int ret;

	ret = misc_register(&udmabuf_import_misc);
	if (ret)
		return ret;

	/*
	 * The importing device needs a DMA mask for dma_buf_map_attachment to
	 * hand back DMA addresses; a bare misc device has none. The original
	 * in-tree patch reused udmabuf's own miscdevice (also mask-less) and
	 * was reported working, so this may be redundant -- kept explicit for
	 * robustness. If it changes observed addresses, drop it to match the
	 * in-tree behaviour exactly.
	 */
	dma_coerce_mask_and_coherent(udmabuf_import_misc.this_device, DMA_BIT_MASK(64));

	return 0;
}

static void __exit udmabuf_import_exit(void)
{
	/* Attachments are owned by open files and released with them, and the
	 * module cannot be unloaded while any of those are open, so there is
	 * nothing left to drain here. */
	misc_deregister(&udmabuf_import_misc);
}

module_init(udmabuf_import_init);
module_exit(udmabuf_import_exit);

/* dma_buf_map_attachment_unlocked() & friends are exported in the DMA_BUF
 * symbol namespace. The MODULE_IMPORT_NS spelling changed to a string
 * literal in v6.13. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Karl Bonde Torp <k.torp@samsung.com>");
MODULE_DESCRIPTION("Out-of-tree dma-buf importer (udmabuf import ioctls)");
MODULE_VERSION("0.1.0");
