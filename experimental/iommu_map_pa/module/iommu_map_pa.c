// SPDX-License-Identifier: GPL-2.0-only
/*
 * EXPERIMENTAL: install device-physical addresses into a device's live IOMMU
 * domain from userspace.
 *
 * Nothing here is tied to a device class or to where the addresses came from;
 * the case it was built for is direct NVMe<->GPU P2P DMA under VFIO isolation.
 * There, userspace must make GPU memory reachable by the NVMe's DMA, and VFIO's
 * userspace map API (VFIO_IOMMU_MAP_DMA) only accepts a pinnable host VA, so
 * GPU memory cannot be registered that way. The GPU's physical addresses are
 * already known (a phys_lut, e.g. resolved from a dma-buf by dmabuf_import);
 * the missing step is putting them into the domain the NVMe translates through.
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
#include <linux/file.h>
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

/* Forward decl so the map ioctl can recognise, and refuse, our own file. */
static const struct file_operations iommu_map_pa_fops;

/* One installed mapping; everything needed to tear it back down exactly. */
struct iommu_map_pa_mapping {
	u64 handle;			/* opaque id handed to userspace */
	struct pci_dev *pdev;		/* target device (ref held) */
	struct iommu_domain *domain;	/* domain we mapped into (for unmap) */
	unsigned long iova_base;	/* start IOVA of the mapping */
	size_t mapped_size;		/* bytes actually mapped (unmap length) */
	phys_addr_t first_phys;		/* what iova_base translated to at map */
	struct dma_buf *held_dmabuf;	/* optional: keeps GPU memory alive */
	struct file *held_device;	/* the caller's VFIO device fd, pinned */
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
		 * Only unmap if the device still uses the domain we mapped into.
		 * held_device keeps the ordinary teardown paths from reaching
		 * here: while that fd is open the group cannot be detached from
		 * its container, and under iommufd the attached device keeps the
		 * hwpt referenced. So the common case, userspace closing up shop
		 * with a mapping still installed, no longer frees the domain
		 * first.
		 *
		 * That is a narrowing, not a guarantee. An explicit detach ioctl
		 * on the same fd still detaches, and then the domain can be
		 * freed while we hold a pointer to it, which is why the pointer
		 * is treated as suspect: compare it against the device's current
		 * domain, and confirm the range still translates to what we
		 * installed. Neither check is conclusive alone, since a freed
		 * domain can be reallocated at the same address, but a recycled
		 * domain that also maps our IOVA to our exact physical is not
		 * something userspace hits by accident. Unmapping before tearing
		 * the VFIO setup down remains the only ordering that is simply
		 * correct.
		 */
		struct iommu_domain *cur = map->pdev ?
			iommu_get_domain_for_dev(&map->pdev->dev) : NULL;

		if (cur != map->domain)
			pr_warn("iommu_map_pa: domain changed before unmap, skipping iommu_unmap (handle=%llu)\n",
				map->handle);
		else if (iommu_iova_to_phys(cur, map->iova_base) != map->first_phys)
			pr_warn("iommu_map_pa: iova 0x%lx no longer translates to 0x%llx, skipping iommu_unmap (handle=%llu)\n",
				map->iova_base, (u64)map->first_phys, map->handle);
		else
			iommu_unmap(map->domain, map->iova_base, map->mapped_size);
	}
	if (map->held_dmabuf)
		dma_buf_put(map->held_dmabuf);
	/* After the unmap above, never before: this reference is what held the
	 * device attached while the mapping was installed. */
	if (map->held_device)
		fput(map->held_device);
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
	struct file *device = NULL;
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

	if (req.reserved)
		return -EINVAL;
	/* Required: pinning the caller's VFIO device fd is what stops the
	 * ordinary teardown paths from detaching the device, and with it the
	 * domain, while a mapping is installed. Rejected at <= 0 so a
	 * '{0}'-initialised request fails closed instead of naming descriptor 0. */
	if (req.device_fd <= 0) {
		pr_err("iommu_map_pa: device_fd is required; pass the open VFIO device fd for this BDF\n");
		return -EINVAL;
	}
	/* Reject undefined prot bits for the same reason as 'reserved': silently
	 * ignoring them would make the remaining bits unusable for future flags,
	 * since a new kernel could not tell an old caller's garbage from intent. */
	if (req.prot & ~(u32)(IOMMU_MAP_PA_PROT_READ | IOMMU_MAP_PA_PROT_WRITE))
		return -EINVAL;
	if (!req.page_size || !is_power_of_2(req.page_size) ||
	    req.page_size < PAGE_SIZE)
		return -EINVAL;
	if (!req.nphys || !req.user_phys_ptr)
		return -EINVAL;
	/* Bound the kernel-side array: nphys is a u32, so without a cap a bogus
	 * request asks kvmalloc_array() for tens of gigabytes. */
	if (req.nphys > IOMMU_MAP_PA_MAX_NPHYS)
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
	 * Pin the caller's VFIO device fd. An open device fd holds the group
	 * file and makes VFIO_GROUP_UNSET_CONTAINER fail, and under iommufd it
	 * keeps the hwpt referenced against IOMMU_DESTROY, so the domain we are
	 * about to map into cannot be freed by the paths userspace takes when it
	 * simply closes up shop. It is not absolute, an explicit detach ioctl
	 * still works, which is why the unmap-time checks stay.
	 *
	 * Note the domain lifetime follows device attachment, not the container
	 * file: pinning the container instead would look reassuring and prevent
	 * nothing, since vfio_iommu_type1_detach_group() frees the domain when
	 * the last group leaves, long before the container file is released.
	 */
	/* fget(), not fget_raw(): it refuses O_PATH descriptors, which pin nothing
	 * and would otherwise slip past the self-check below, an O_PATH open of
	 * our own node carrying empty_fops rather than ours. */
	device = fget(req.device_fd);
	if (!device) {
		pr_err("iommu_map_pa: fget(device_fd=%d) failed; pass the open VFIO device fd for this BDF\n",
		       req.device_fd);
		err = -EBADF;
		goto err_unwind;
	}
	/*
	 * Refuse our own file. It is not a VFIO device, so it protects nothing,
	 * and it would be self-referential: the mapping holds the file whose
	 * release() is what would tear the mapping down, so neither ever runs
	 * and the context, its mappings and the module reference leak for good.
	 */
	if (device->f_op == &iommu_map_pa_fops) {
		pr_err("iommu_map_pa: device_fd names this device; pass the VFIO device fd instead\n");
		err = -EINVAL;
		goto err_unwind;
	}

	/*
	 * The device must already be attached to an IOMMU domain. For a
	 * VFIO-controlled NVMe this returns the VFIO/iommufd-owned domain, i.e.
	 * the exact translation context the device uses for userspace I/O.
	 *
	 * Looked up after the pin above, deliberately: the other order would
	 * capture a domain pointer that a racing teardown could free before the
	 * mapping loop below ever touches it.
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

	/*
	 * Only an unmanaged domain is ours to write into: that is what VFIO and
	 * iommufd allocate for a userspace-owned device. A kernel driver's
	 * DMA domain has its IOVA space owned by the kernel iova allocator, so
	 * installing entries there would collide with addresses the kernel later
	 * hands out and silently redirect its DMA. An identity or passthrough
	 * domain has no translation to install into at all. Refuse both, so
	 * naming the wrong BDF is an error and not memory corruption.
	 */
	if (domain->type != IOMMU_DOMAIN_UNMANAGED) {
		pr_err("iommu_map_pa: %s is not in a userspace-owned IOMMU domain (type=%u); bind it to vfio-pci first\n",
		       bdf, domain->type);
		err = -EINVAL;
		goto err_unwind;
	}

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
			pr_warn("iommu_map_pa: MAPPING MISMATCH at base, page table not as expected!\n");
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
	/* What the page table itself reports, not what we asked for, so the
	 * teardown check compares against the installed translation. */
	map->first_phys = iommu_iova_to_phys(domain, (unsigned long)req.iova_base);
	map->held_dmabuf = held;
	map->held_device = device;
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
	if (device)
		fput(device);
	if (pdev)
		pci_dev_put(pdev);
	kvfree(phys);
	return err;
}

static long
iommu_map_pa_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	/*
	 * No capability check here, deliberately. MAP programs a DMA-capable
	 * device to reach caller-supplied physical addresses, i.e. whoever can
	 * open this node is root-equivalent; access is therefore governed by the
	 * node itself, which is registered 0600 root-only. An operator who wants
	 * to run the experiments unprivileged hands the node to a group with the
	 * udev rule shipped in the package docdir, which is an explicit local
	 * decision rather than something the module grants. Requiring a
	 * capability on top would not change who can reach the node, and would
	 * break running under a container's default capability set, which drops
	 * both CAP_SYS_ADMIN and CAP_SYS_RAWIO.
	 */
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
	/* Not the native handler: 'arg' from a 32-bit caller needs compat_ptr()
	 * before it can be used as a userspace pointer. */
	.compat_ioctl = compat_ptr_ioctl,
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
	int ret;

	ret = misc_register(&iommu_map_pa_misc);
	if (ret)
		return ret;

	/*
	 * Say out loud what this is. The module maps caller-supplied physical
	 * addresses into a live IOMMU domain, so anyone who can open the node
	 * can drive a device at arbitrary physical memory. A line in dmesg is
	 * the one place an operator is certain to see that, having typed
	 * modprobe rather than read the README.
	 */
	pr_warn("iommu_map_pa: EXPERIMENTAL. /dev/iommu_map_pa maps arbitrary physical addresses into a device's IOMMU domain; access to the node is root-equivalent\n");

	return 0;
}

static void __exit
iommu_map_pa_module_exit(void)
{
	misc_deregister(&iommu_map_pa_misc);
}

module_init(iommu_map_pa_module_init);
module_exit(iommu_map_pa_module_exit);

MODULE_AUTHOR("Jaeyoon Choi <j_yoon.choi@samsung.com>");
MODULE_DESCRIPTION("Map physical addresses into a device's live IOMMU domain");
MODULE_LICENSE("GPL");

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
