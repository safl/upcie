// SPDX-License-Identifier: GPL-2.0
/*
 * dmabuf_import - standalone (out-of-tree) dma-buf importer
 *
 * Imports an external dma-buf and shares its DMA addresses with userspace.
 * Any dma-buf will do, whoever exported it: a memfd wrapped by udmabuf, or
 * device memory from a GPU driver. Only exported dma-buf core APIs are used,
 * so it builds out-of-tree against a stock kernel.
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
#include <linux/dma-resv.h>

#include "dmabuf_import.h"

/* Forward decl: the ioctl handlers need the device for dma_buf_attach. */
static struct miscdevice dmabuf_import_misc;

/* ---- imported dma-buf tracking ----------------------------------------- */

static struct rb_root dmabuf_import_tree = RB_ROOT;
static DEFINE_RWLOCK(dmabuf_import_treelock);

struct dmabuf_import_desc {
	int				dma_buf_fd;
	enum dma_data_direction		dir;
	struct dma_buf_attachment	*attach;
	struct dma_buf			*dma_buf;
	struct sg_table			*sgt;
	bool				pinned;
	bool				dying;
	struct rb_node			node;
};

/*
 * The addresses handed to userspace are programmed into a device, so the
 * buffer must stay where it is; the attachment is pinned for its whole life.
 * move_notify exists because a dynamic attachment requires one, and a dynamic
 * attachment is what carries allow_peer2peer.
 *
 * Exporters also call this while an attachment is being set up, since pinning
 * may itself migrate the buffer, and again while it is torn down. Neither is a
 * move anybody needs to hear about, so only an import whose addresses have
 * been handed out is reported.
 */
static void dmabuf_import_move_notify(struct dma_buf_attachment *attach)
{
	struct dmabuf_import_desc *desc = attach->importer_priv;

	if (!desc || !desc->sgt || desc->dying)
		return;

	dev_warn_once(attach->dev, "exporter moved a pinned import; addresses are stale\n");
}

/*
 * allow_peer2peer tells an exporter that this importer can address device
 * memory over PCIe rather than only system memory. Without it a GPU exporter
 * refuses to map its VRAM and the attach fails with -EOPNOTSUPP, which is
 * exactly what device memory is wanted for here.
 */
static const struct dma_buf_attach_ops dmabuf_import_attach_ops = {
	.allow_peer2peer	= true,
	.move_notify		= dmabuf_import_move_notify,
};

static struct dmabuf_import_desc *dmabuf_import_tree_find(struct rb_root *root, int dma_buf_fd)
{
	struct dmabuf_import_desc *this;
	struct rb_node *node = root->rb_node;

	while (node) {
		this = container_of(node, struct dmabuf_import_desc, node);

		if (dma_buf_fd < this->dma_buf_fd)
			node = node->rb_left;
		else if (dma_buf_fd > this->dma_buf_fd)
			node = node->rb_right;
		else
			return this;
	}
	return NULL;
}

static int dmabuf_import_tree_insert(struct rb_root *root, struct dmabuf_import_desc *desc)
{
	struct dmabuf_import_desc *this;
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		this = container_of(*link, struct dmabuf_import_desc, node);
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

static struct dmabuf_import_desc *dmabuf_import_desc_lookup(int dma_buf_fd)
{
	struct dmabuf_import_desc *desc;

	read_lock(&dmabuf_import_treelock);
	desc = dmabuf_import_tree_find(&dmabuf_import_tree, dma_buf_fd);
	read_unlock(&dmabuf_import_treelock);
	if (!desc)
		return ERR_PTR(-EINVAL);

	return desc;
}

/*
 * Tear down a desc's dma-buf attachment and free it. The dma-buf teardown
 * sleeps and takes dma_resv_lock, so this must NOT be called under
 * dmabuf_import_treelock; the caller removes the node from the tree under the
 * lock, then calls this after dropping it. The desc must already be out of (or
 * never in) the tree.
 */
static void dmabuf_import_desc_destroy(struct dmabuf_import_desc *desc)
{
	if (!desc)
		return;

	desc->dying = true;

	if (desc->sgt || desc->pinned) {
		dma_resv_lock(desc->dma_buf->resv, NULL);
		if (desc->sgt)
			dma_buf_unmap_attachment(desc->attach, desc->sgt, desc->dir);
		if (desc->pinned)
			dma_buf_unpin(desc->attach);
		dma_resv_unlock(desc->dma_buf->resv);
	}
	if (desc->attach)
		dma_buf_detach(desc->dma_buf, desc->attach);
	if (desc->dma_buf)
		dma_buf_put(desc->dma_buf);

	kfree(desc);
}

static int dmabuf_import_attach_locked(struct dmabuf_import_desc **desc, int dma_buf_fd,
			  struct device *dev, enum dma_data_direction dir)
{
	struct dmabuf_import_desc *tmp;
	int ret;

	if (WARN_ON_ONCE(!dev))
		return -EFAULT;

	read_lock(&dmabuf_import_treelock);
	tmp = dmabuf_import_tree_find(&dmabuf_import_tree, dma_buf_fd);
	read_unlock(&dmabuf_import_treelock);
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

	tmp->attach = dma_buf_dynamic_attach(tmp->dma_buf, dev, &dmabuf_import_attach_ops, tmp);
	if (IS_ERR(tmp->attach)) {
		ret = PTR_ERR(tmp->attach);
		tmp->attach = NULL;
		goto err;
	}

	/* Pinned before mapping and held pinned: the caller programs these
	 * addresses into a device and cannot be told to re-read them. */
	dma_resv_lock(tmp->dma_buf->resv, NULL);
	ret = dma_buf_pin(tmp->attach);
	if (!ret) {
		tmp->pinned = true;

		tmp->sgt = dma_buf_map_attachment(tmp->attach, dir);
		if (IS_ERR(tmp->sgt)) {
			ret = PTR_ERR(tmp->sgt);
			tmp->sgt = NULL;
		}
	}
	dma_resv_unlock(tmp->dma_buf->resv);
	if (ret)
		goto err;

	tmp->dir = dir;
	tmp->dma_buf_fd = dma_buf_fd;

	write_lock(&dmabuf_import_treelock);
	ret = dmabuf_import_tree_insert(&dmabuf_import_tree, tmp);
	write_unlock(&dmabuf_import_treelock);
	if (ret)
		goto err;

	*desc = tmp;
	return 0;
err:
	dmabuf_import_desc_destroy(tmp);
	return ret;
}

/* ---- ioctls ------------------------------------------------------------- */

static long dmabuf_import_ioctl_attach(struct file *filp, unsigned long arg)
{
	struct device *dev = dmabuf_import_misc.this_device;
	struct dmabuf_import_attach __user *uattach;
	struct dmabuf_import_attach attach;
	struct dmabuf_import_desc *desc;
	int ret;

	uattach = (void __user *)arg;

	if (copy_from_user(&attach, uattach, sizeof(attach)))
		return -EFAULT;

	ret = dmabuf_import_attach_locked(&desc, attach.fd, dev, DMA_BIDIRECTIONAL);
	if (ret)
		return ret;

	attach.count = desc->sgt->nents;

	if (copy_to_user(uattach, &attach, sizeof(attach)))
		return -EFAULT;

	return 0;
}

static long dmabuf_import_ioctl_detach(struct file *filp, unsigned long arg)
{
	struct dmabuf_import_desc *desc;
	int fd;

	if (copy_from_user(&fd, (void __user *)arg, sizeof(fd)))
		return -EFAULT;

	/* Remove the node from the tree under the lock (pointer work only), then
	 * do the sleeping dma-buf teardown after dropping it. */
	write_lock(&dmabuf_import_treelock);
	desc = dmabuf_import_tree_find(&dmabuf_import_tree, fd);
	if (desc)
		rb_erase(&desc->node, &dmabuf_import_tree);
	write_unlock(&dmabuf_import_treelock);

	if (!desc)
		return -EINVAL;

	dmabuf_import_desc_destroy(desc);
	return 0;
}

static long dmabuf_import_ioctl_get_map(struct file *filp, unsigned long arg)
{
	struct dmabuf_import_desc *desc;
	struct dmabuf_import_get_map __user *uget_map;
	struct dmabuf_import_get_map get_map;
	struct dmabuf_import_dma_map map;
	struct dmabuf_import_dma_map __user *umap;
	struct scatterlist *sg;
	int i;

	uget_map = (void __user *)arg;

	if (copy_from_user(&get_map, uget_map, sizeof(get_map)))
		return -EFAULT;

	desc = dmabuf_import_desc_lookup(get_map.fd);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	umap = uget_map->dma_arr;
	for_each_sgtable_dma_sg(desc->sgt, sg, i) {
		/* Userspace allocated get_map.count entries (0..count-1); stop
		 * before writing umap[count] to avoid a one-past-the-end write. */
		if (i >= get_map.count)
			break;

		map.dma_addr = sg_dma_address(sg);
		map.dma_len = sg_dma_len(sg);
		if (copy_to_user(umap + i, &map, sizeof(map)))
			return -EFAULT;
	}

	return 0;
}

static long dmabuf_import_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case DMABUF_IMPORT_ATTACH:
		return dmabuf_import_ioctl_attach(filp, arg);
	case DMABUF_IMPORT_DETACH:
		return dmabuf_import_ioctl_detach(filp, arg);
	case DMABUF_IMPORT_GET_MAP:
		return dmabuf_import_ioctl_get_map(filp, arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations dmabuf_import_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= dmabuf_import_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice dmabuf_import_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "dmabuf_import",
	.fops	= &dmabuf_import_fops,
};

static int __init dmabuf_import_init(void)
{
	int ret;

	ret = misc_register(&dmabuf_import_misc);
	if (ret)
		return ret;

	/* dma_buf_map_attachment needs a DMA mask; a misc device has none. */
	dma_coerce_mask_and_coherent(dmabuf_import_misc.this_device, DMA_BIT_MASK(64));

	return 0;
}

static void __exit dmabuf_import_exit(void)
{
	misc_deregister(&dmabuf_import_misc);
	/* NB: descriptors still in the tree at unload are leaked; add a
	 * drain here if the module is meant to be unloaded while in use. */
}

module_init(dmabuf_import_init);
module_exit(dmabuf_import_exit);

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
MODULE_DESCRIPTION("Out-of-tree dma-buf importer");
MODULE_VERSION("0.2.0");
