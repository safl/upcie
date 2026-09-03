// SPDX-License-Identifier: GPL-2.0-only
/*
 * EXPERIMENTAL: install device-physical addresses into a device's live IOMMU
 * domain from userspace.
 *
 * Problem this solves: to do direct NVMe<->GPU P2P DMA under VFIO isolation,
 * userspace must make GPU memory reachable by the NVMe's DMA. VFIO's userspace
 * map API (VFIO_IOMMU_MAP_DMA) only accepts a pinnable host VA, so GPU memory
 * cannot be registered that way. We already know the GPU's physical addresses
 * (a phys_lut, e.g. from udmabuf); the missing step is putting them into the
 * IOMMU domain the NVMe actually translates through.
 *
 * This misc device exposes exactly that step: given a BDF, a phys_lut and a
 * userspace-chosen IOVA base, it looks up the device's *current* IOMMU domain
 * (for a vfio-pci device this is the VFIO/iommufd-owned domain) and installs
 * iommu_map(iova_base + i*ps -> phys[i]) entries into it. Userspace then writes
 * the IOVA into NVMe PRPs; the IOMMU translates it back to the GPU physical.
 *
 * ioctls (see iommu_map_pa.h):
 *   IOMMU_MAP_PA    install a phys_lut -> IOVA mapping, return a handle
 *   IOMMU_UNMAP_PA  tear a mapping down by handle
 * Mappings are tracked per open fd and torn down on UNMAP or close().
 */

#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "iommu_map_pa.h"

/* One installed mapping; everything needed to tear it back down exactly. */
struct iommu_map_pa_mapping {
	u64 handle;			/* opaque id handed to userspace */
	struct pci_dev *pdev;		/* target device (ref held) */
	struct iommu_domain *domain;	/* domain we mapped into (for unmap) */
	unsigned long iova_base;	/* start IOVA of the mapping */
	size_t mapped_size;		/* bytes actually mapped (unmap length) */
	struct dma_buf *held_dmabuf;	/* optional: keeps GPU memory alive */
	struct list_head node;
};

/* Per-open-fd state: the set of mappings created through this fd. */
struct iommu_map_pa_ctx {
	struct mutex lock;
	struct list_head maps;
	u64 next_handle;
};

/* Tear down one mapping: iommu_unmap (if the domain still exists), drop the
 * dma_buf/pci refs, and free it. */
static void
iommu_map_pa_mapping_destroy(struct iommu_map_pa_mapping *map)
{
	if (!map)
		return;

	if (map->domain && map->mapped_size) {
		/*
		 * Only unmap if the device still uses the same domain. If
		 * userspace tore down VFIO first, that domain (and its page
		 * tables) is already gone, so touching it would be a
		 * use-after-free. Userspace is expected to UNMAP before closing
		 * its VFIO container.
		 */
		struct iommu_domain *cur = map->pdev ?
			iommu_get_domain_for_dev(&map->pdev->dev) : NULL;

		if (cur == map->domain)
			iommu_unmap(map->domain, map->iova_base, map->mapped_size);
		else
			pr_warn("iommu_map_pa: domain changed before unmap, skipping iommu_unmap (handle=%llu)\n",
				map->handle);
	}
	if (map->held_dmabuf)
		dma_buf_put(map->held_dmabuf);
	if (map->pdev)
		pci_dev_put(map->pdev);
	kfree(map);
}

/* "DDDD:BB:SS.F" string -> struct pci_dev* (takes a ref via pci_get_...). */
static int
iommu_map_pa_parse_bdf(const char *bdf, struct pci_dev **pdev_out)
{
	unsigned int domain, bus, slot, func;
	struct pci_dev *pdev;

	if (sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4)
		return -EINVAL;
	if (bus > 0xff || slot > PCI_SLOT(~0) || func > PCI_FUNC(~0))
		return -EINVAL;

	pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, func));
	if (!pdev)
		return -ENODEV;

	*pdev_out = pdev;
	return 0;
}

/* Look up a mapping by handle; caller must hold ctx->lock. */
static struct iommu_map_pa_mapping *
iommu_map_pa_find_locked(struct iommu_map_pa_ctx *ctx, u64 handle)
{
	struct iommu_map_pa_mapping *map;

	list_for_each_entry(map, &ctx->maps, node)
		if (map->handle == handle)
			return map;

	return NULL;
}

/* Remove a mapping by handle: unlink it from this fd's list and tear it down. */
static long
iommu_map_pa_ioctl_unmap(struct file *file, unsigned long arg)
{
	struct iommu_map_pa_ctx *ctx = file->private_data;
	struct iommu_unmap_pa_req req;
	struct iommu_map_pa_mapping *map;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&ctx->lock);
	map = iommu_map_pa_find_locked(ctx, req.map_handle);
	if (!map) {
		mutex_unlock(&ctx->lock);
		return -ENOENT;
	}

	list_del(&map->node);
	mutex_unlock(&ctx->lock);

	iommu_map_pa_mapping_destroy(map);
	return 0;
}

/*
 * Map a caller-provided list of device physical addresses (phys_lut)
 * into the IOMMU domain the target device already uses, and return the IOVA
 * base. For a VFIO-controlled NVMe this is the VFIO/iommufd-owned domain, i.e.
 * the exact translation context the device uses for userspace-driven I/O, so
 * the returned IOVA can be written directly into NVMe PRPs.
 */
static long
iommu_map_pa_ioctl_map(struct file *file, unsigned long arg)
{
	struct iommu_map_pa_ctx *ctx = file->private_data;
	struct iommu_map_pa_req req;
	struct iommu_map_pa_mapping *map = NULL;
	struct iommu_domain *domain = NULL;
	struct pci_dev *pdev = NULL;
	struct dma_buf *held = NULL;
	u64 *phys = NULL;
	size_t mapped = 0;
	char bdf[IOMMU_MAP_PA_BDF_LEN];
	u64 map_size;
	u64 last_iova;
	u64 handle;
	int prot;
	u32 i;
	int err;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
		pr_err("iommu_map_pa: iommu_map copy_from_user(req) failed\n");
		return -EFAULT;
	}

	pr_debug("iommu_map_pa: iommu_map req bdf=%.*s page_size=%u nphys=%u iova_base=0x%llx phys_ptr=0x%llx dmabuf_fd=%d\n",
		(int)sizeof(req.bdf), req.bdf, req.page_size, req.nphys,
		req.iova_base, req.user_phys_ptr, req.dmabuf_fd);

	if (!req.page_size || !is_power_of_2(req.page_size) ||
	    req.page_size < PAGE_SIZE)
		return -EINVAL;
	if (!req.nphys || !req.user_phys_ptr)
		return -EINVAL;
	if (!IS_ALIGNED(req.iova_base, req.page_size))
		return -EINVAL;
	if (check_mul_overflow((u64)req.nphys, (u64)req.page_size, &map_size))
		return -EOVERFLOW;
	if (map_size > SIZE_MAX)
		return -EOVERFLOW;
	if (check_add_overflow(req.iova_base, map_size - 1, &last_iova))
		return -EOVERFLOW;

	memcpy(bdf, req.bdf, sizeof(bdf));
	bdf[sizeof(bdf) - 1] = '\0';

	err = iommu_map_pa_parse_bdf(bdf, &pdev);
	if (err)
		return err;

	/*
	 * The device must already be attached to an IOMMU domain. For a
	 * VFIO-controlled NVMe this returns the VFIO/iommufd-owned domain, i.e.
	 * the exact translation context the device uses for userspace I/O.
	 */
	domain = iommu_get_domain_for_dev(&pdev->dev);
	if (!domain) {
		pr_err("iommu_map_pa: no IOMMU domain for %s (device not behind IOMMU / not VFIO-bound?)\n",
		       bdf);
		err = -ENODEV;
		goto err_unwind;
	}
	pr_debug("iommu_map_pa: domain=%p type=%u aperture=[0x%llx..0x%llx] pgsize_bitmap=0x%lx for %s\n",
		domain, domain->type, domain->geometry.aperture_start,
		domain->geometry.aperture_end, domain->pgsize_bitmap, bdf);

	/* Intel VT-d returns -EFAULT when iova+size exceeds the domain address
	 * width; pre-check against the aperture so misuse is obvious. */
	if (domain->geometry.aperture_end &&
	    last_iova > domain->geometry.aperture_end) {
		pr_err("iommu_map_pa: iova range [0x%llx..0x%llx] exceeds domain aperture_end=0x%llx\n",
		       req.iova_base, last_iova, domain->geometry.aperture_end);
		err = -ERANGE;
		goto err_unwind;
	}

	if (!req.prot) {
		prot = IOMMU_READ | IOMMU_WRITE;
	} else {
		prot = 0;
		if (req.prot & IOMMU_MAP_PA_PROT_READ)
			prot |= IOMMU_READ;
		if (req.prot & IOMMU_MAP_PA_PROT_WRITE)
			prot |= IOMMU_WRITE;
	}
	/* This path always maps peer device MMIO (GPU BAR), so request the MMIO
	 * memory type to get device (uncached) attributes in the page tables. */
	prot |= IOMMU_MMIO;

	phys = kvmalloc_array(req.nphys, sizeof(*phys), GFP_KERNEL);
	if (!phys) {
		err = -ENOMEM;
		goto err_unwind;
	}
	if (copy_from_user(phys, (void __user *)(uintptr_t)req.user_phys_ptr,
			   (size_t)req.nphys * sizeof(*phys))) {
		pr_err("iommu_map_pa: iommu_map copy_from_user(phys[%u]) from 0x%llx failed\n",
		       req.nphys, req.user_phys_ptr);
		err = -EFAULT;
		goto err_unwind;
	}

	if (req.dmabuf_fd >= 0) {
		held = dma_buf_get(req.dmabuf_fd);
		if (IS_ERR(held)) {
			err = PTR_ERR(held);
			pr_err("iommu_map_pa: dma_buf_get(fd=%d) failed err=%d\n",
			       req.dmabuf_fd, err);
			held = NULL;
			goto err_unwind;
		}
	}

	for (i = 0; i < req.nphys; i++) {
		unsigned long iova = (unsigned long)req.iova_base +
				     (size_t)i * req.page_size;

		if (!IS_ALIGNED(phys[i], req.page_size)) {
			err = -ERANGE;
			goto err_unwind;
		}

		err = iommu_map(domain, iova, (phys_addr_t)phys[i],
				req.page_size, prot, GFP_KERNEL);
		if (err) {
			pr_err("iommu_map_pa: iommu_map(iova=0x%lx phys=0x%llx size=%u prot=%d) failed err=%d at idx=%u\n",
			       iova, phys[i], req.page_size, prot, err, i);
			goto err_unwind;
		}

		mapped += req.page_size;
	}
	pr_debug("iommu_map_pa: iommu_map ok: %u pages, iova 0x%llx..0x%llx -> gpu\n",
		req.nphys, req.iova_base,
		req.iova_base + (u64)req.nphys * req.page_size);

	/* Read the page table back through the IOMMU API to prove the mapping is
	 * actually installed and translates to the expected GPU physical. */
	{
		u64 last_page_iova = req.iova_base +
				     (u64)(req.nphys - 1) * req.page_size;
		phys_addr_t p0 = iommu_iova_to_phys(domain,
						    (unsigned long)req.iova_base);
		phys_addr_t pn = iommu_iova_to_phys(domain,
						    (unsigned long)last_page_iova);

		pr_debug("iommu_map_pa: verify iova_to_phys: 0x%llx->0x%llx (exp 0x%llx), 0x%llx->0x%llx (exp 0x%llx)\n",
			req.iova_base, (u64)p0, phys[0],
			last_page_iova, (u64)pn, phys[req.nphys - 1]);
		if (p0 != (phys_addr_t)phys[0])
			pr_warn("iommu_map_pa: MAPPING MISMATCH at base — page table not as expected!\n");
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		err = -ENOMEM;
		goto err_unwind;
	}

	mutex_lock(&ctx->lock);
	handle = ++ctx->next_handle;
	if (!handle)
		handle = ++ctx->next_handle;
	mutex_unlock(&ctx->lock);

	/* Hand the handle to userspace before publishing the mapping; once it
	 * is on ctx->maps a concurrent UNMAP may free it. */
	req.map_handle = handle;
	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		pr_err("iommu_map_pa: iommu_map copy_to_user(req) failed\n");
		kfree(map);
		err = -EFAULT;
		goto err_unwind;
	}

	map->handle = handle;
	map->pdev = pdev;
	map->domain = domain;
	map->iova_base = (unsigned long)req.iova_base;
	map->mapped_size = mapped;
	map->held_dmabuf = held;
	mutex_lock(&ctx->lock);
	list_add_tail(&map->node, &ctx->maps);
	mutex_unlock(&ctx->lock);

	kvfree(phys);
	return 0;

err_unwind:
	if (mapped)
		iommu_unmap(domain, (unsigned long)req.iova_base, mapped);
	if (held)
		dma_buf_put(held);
	if (pdev)
		pci_dev_put(pdev);
	kvfree(phys);
	return err;
}

static long
iommu_map_pa_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case IOMMU_UNMAP_PA:
		return iommu_map_pa_ioctl_unmap(file, arg);
	case IOMMU_MAP_PA:
		return iommu_map_pa_ioctl_map(file, arg);
	default:
		return -ENOTTY;
	}
}

static int
iommu_map_pa_chrdev_open(struct inode *inode, struct file *file)
{
	struct iommu_map_pa_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->maps);
	file->private_data = ctx;
	return 0;
}

static int
iommu_map_pa_chrdev_release(struct inode *inode, struct file *file)
{
	struct iommu_map_pa_ctx *ctx = file->private_data;
	struct iommu_map_pa_mapping *map, *tmp;

	if (!ctx)
		return 0;

	list_for_each_entry_safe(map, tmp, &ctx->maps, node) {
		list_del(&map->node);
		iommu_map_pa_mapping_destroy(map);
	}

	kfree(ctx);
	return 0;
}

static const struct file_operations iommu_map_pa_fops = {
	.owner = THIS_MODULE,
	.open = iommu_map_pa_chrdev_open,
	.release = iommu_map_pa_chrdev_release,
	.unlocked_ioctl = iommu_map_pa_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = iommu_map_pa_ioctl,
#endif
};

static struct miscdevice iommu_map_pa_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "iommu_map_pa",
	.fops = &iommu_map_pa_fops,
	.mode = 0600,
};

static int __init
iommu_map_pa_module_init(void)
{
	return misc_register(&iommu_map_pa_misc);
}

static void __exit
iommu_map_pa_module_exit(void)
{
	misc_deregister(&iommu_map_pa_misc);
}

module_init(iommu_map_pa_module_init);
module_exit(iommu_map_pa_module_exit);

MODULE_AUTHOR("Jaeyoon Choi <j_yoon.choi@samsung.com>");
MODULE_DESCRIPTION("Out-of-tree physical-address IOMMU mapper");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.8.1");

/*
 * MODULE_IMPORT_NS() stringifies its argument before Linux 6.13, but expects
 * a string literal starting with Linux 6.13. Keep this helper buildable across
 * both forms.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
