# Out-of-tree build for the experimental upcie_iommu_map module.
# See ../README.md before loading it.
#
#   make            # build upcie_iommu_map.ko against the running kernel
#   make KDIR=/path/to/kernel/build   # or a specific kernel tree
#   make clean
#
# DKMS calls this via dkms.conf (MAKE[0]); it passes KDIR as
# ${kernel_source_dir}.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

obj-m += iommu_map_pa.o

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
