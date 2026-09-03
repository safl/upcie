# Top-level build for the DKMS source tree, where each module has its own
# subdirectory. DKMS calls this via dkms.conf (MAKE[0]) with KDIR set to
# ${kernel_source_dir}; the per-module Makefiles do the actual kbuild.
#
# This file is only ever used from an installed /usr/src/upcie-experimental-*
# tree, whose layout differs from the repository: there the module sources sit
# directly in dmabuf_import/ and iommu_map_pa/ rather than under module/.

MODULES := dmabuf_import iommu_map_pa

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean $(MODULES)

all: $(MODULES)

$(MODULES):
	$(MAKE) -C $@ KDIR=$(KDIR)

clean:
	for m in $(MODULES); do $(MAKE) -C $$m KDIR=$(KDIR) clean; done
