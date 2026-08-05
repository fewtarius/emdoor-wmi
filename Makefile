# SPDX-License-Identifier: GPL-2.0-or-later
#
# Out-of-tree Emdoor EmdAcpi power and keyboard RGB driver.

obj-m := emdoor-wmi.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
