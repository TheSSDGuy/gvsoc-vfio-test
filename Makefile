obj-m += vfio_bridge_host_dma_driver.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

MODULE := vfio_bridge_host_dma_driver
DMA_TEST := vfio_bridge_dma_test
LOADER := pcie_elf_loader

ccflags-y += -Wall -O2

all: module dma_test elf_loader

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

dma_test: vfio_bridge_dma_test.c
	$(CC) -O2 -Wall -Wextra vfio_bridge_dma_test.c -o $(DMA_TEST)

elf_loader: vfio_bridge_elf_loader.c
	$(CC) -O2 -Wall -Wextra vfio_bridge_elf_loader.c -o $(LOADER)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(DMA_TEST) $(LOADER)

load: module
	sudo insmod $(MODULE).ko

unload:
	-sudo rmmod $(MODULE)

reload: unload load
