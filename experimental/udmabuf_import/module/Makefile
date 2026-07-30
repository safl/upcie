# Out-of-tree build for the udmabuf_import module.
#
#   make            # build udmabuf_import.ko against the running kernel
#   make KDIR=/path/to/kernel/build   # or a specific kernel tree
#   make clean
#
# DKMS calls this via dkms.conf (MAKE[0]); it passes KDIR as
# ${kernel_source_dir}.

obj-m += udmabuf_import.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
