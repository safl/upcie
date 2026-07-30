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
#include <linux/spinlock.h>
#include <linux/scatterlist.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>

#include "udmabuf_import.h"

/* Forward decl so the ioctl handlers can reach the device for dma_buf_attach. */
static struct miscdevice udmabuf_import_misc;

/* ---- imported dma-buf tracking (verbatim from the original patch) ------- */

static struct rb_root udmabuf_dma_tree = RB_ROOT;
static DEFINE_RWLOCK(udmabuf_dma_treelock);

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

static struct udmabuf_dma_buf_desc *udmabuf_get_dma_buf_desc(int dma_buf_fd)
{
	struct udmabuf_dma_buf_desc *desc;

	read_lock(&udmabuf_dma_treelock);
	desc = udmabuf_dma_tree_find(&udmabuf_dma_tree, dma_buf_fd);
	read_unlock(&udmabuf_dma_treelock);
	if (!desc)
		return ERR_PTR(-EINVAL);

	return desc;
}

/*
 * Tear down a desc's dma-buf attachment and free it. The dma-buf teardown
 * sleeps (dma_buf_unmap_attachment_unlocked() and dma_buf_detach() take
 * dma_resv_lock), so this must NOT be called under udmabuf_dma_treelock; the
 * caller removes the node from the tree under the lock, then calls this after
 * dropping it. The desc must already be out of (or never in) the tree.
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

static int udmabuf_attach(struct udmabuf_dma_buf_desc **desc, int dma_buf_fd,
			  struct device *dev, enum dma_data_direction dir)
{
	struct udmabuf_dma_buf_desc *tmp;
	int ret;

	if (WARN_ON_ONCE(!dev))
		return -EFAULT;

	read_lock(&udmabuf_dma_treelock);
	tmp = udmabuf_dma_tree_find(&udmabuf_dma_tree, dma_buf_fd);
	read_unlock(&udmabuf_dma_treelock);
	if (tmp) {
		/* Don't fail if dma-buf is already attached. */
		*desc = tmp;
		return 0;
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

	write_lock(&udmabuf_dma_treelock);
	ret = udmabuf_dma_tree_insert(&udmabuf_dma_tree, tmp);
	write_unlock(&udmabuf_dma_treelock);
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
	struct device *dev = udmabuf_import_misc.this_device;
	struct udmabuf_attach __user *uattach;
	struct udmabuf_attach attach;
	struct udmabuf_dma_buf_desc *desc;
	int ret;

	uattach = (void __user *)arg;

	if (copy_from_user(&attach, uattach, sizeof(attach)))
		return -EFAULT;

	ret = udmabuf_attach(&desc, attach.fd, dev, DMA_BIDIRECTIONAL);
	if (ret)
		return ret;

	attach.count = desc->sgt->nents;

	if (copy_to_user(uattach, &attach, sizeof(attach)))
		return -EFAULT;

	return 0;
}

static long udmabuf_ioctl_detach(struct file *filp, unsigned long arg)
{
	struct udmabuf_dma_buf_desc *desc;
	int fd;

	if (copy_from_user(&fd, (void __user *)arg, sizeof(fd)))
		return -EFAULT;

	/* Remove the node from the tree under the lock (pointer work only), then
	 * do the sleeping dma-buf teardown after dropping it. */
	write_lock(&udmabuf_dma_treelock);
	desc = udmabuf_dma_tree_find(&udmabuf_dma_tree, fd);
	if (desc)
		rb_erase(&desc->node, &udmabuf_dma_tree);
	write_unlock(&udmabuf_dma_treelock);

	if (!desc)
		return -EINVAL;

	udmabuf_detach(desc);
	return 0;
}

static long udmabuf_ioctl_get_map(struct file *filp, unsigned long arg)
{
	struct udmabuf_dma_buf_desc *desc;
	struct udmabuf_get_map __user *uget_map;
	struct udmabuf_get_map get_map;
	struct udmabuf_dma_map map;
	struct udmabuf_dma_map __user *umap;
	struct scatterlist *sg;
	int i;

	uget_map = (void __user *)arg;

	if (copy_from_user(&get_map, uget_map, sizeof(get_map)))
		return -EFAULT;

	desc = udmabuf_get_dma_buf_desc(get_map.fd);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	umap = uget_map->dma_arr;
	for_each_sgtable_dma_sg(desc->sgt, sg, i) {
		if (i > get_map.count)
			break;

		map.dma_addr = sg_dma_address(sg);
		map.dma_len = sg_dma_len(sg);
		if (copy_to_user(umap + i, &map, sizeof(map)))
			return -EFAULT;
	}

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
	.unlocked_ioctl	= udmabuf_import_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice udmabuf_import_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "udmabuf_import",
	.fops	= &udmabuf_import_fops,
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
	misc_deregister(&udmabuf_import_misc);
	/* NB: descriptors still in the tree at unload are leaked; add a
	 * drain here if the module is meant to be unloaded while in use. */
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
