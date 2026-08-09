# SPDX-License-Identifier: GPL-2.0-or-later
#
# Out-of-tree Emdoor EmdAcpi power and keyboard RGB driver.

obj-m := emdoor-wmi.o

# Version from VERSION file (used by DKMS)
VERSION := $(shell cat VERSION 2>/dev/null || echo "1.0.0")

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build

# DKMS passes kernelver and kernel_source_dir
# Use them if provided, otherwise fall back to uname -r
ifdef kernelver
  KDIR := $(kernel_source_dir)
endif

# Compiler flags
ccflags-y += -Wall -Wextra -DDRIVER_VERSION=\"$(VERSION)\"

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# DKMS-specific targets
dkms-install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install dkms-install