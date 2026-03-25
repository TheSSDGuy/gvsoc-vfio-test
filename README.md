# GVSoC VFIO Test Environment

## Overview

This repository contains the **host-side software stack** used to validate the GVSoC PCIe VFIO bridge from a Linux guest running inside QEMU.

It includes:

- a minimal **Linux PCI driver** for the custom VFIO-user endpoint
- a **DMA round-trip test**
- an **ELF loader** that programs memory and starts execution through BAR0
- a simple **Makefile** to build and load everything

The repository is meant to be used together with the `pcie_vfio_bridge` model in GVSoC.

---

## Repository Layout

```text
Makefile
gvsoc_vfio_kernel_module.c
dma_test.c
elf_loader.c
README.md
```

### `gvsoc_vfio_kernel_module.c`

Minimal guest-side PCI driver for the custom endpoint exposed by the GVSoC bridge.

Main responsibilities:

- bind to the PCI device identified by vendor `0x1d1d` and device `0x0001`
- map **BAR0**
- allocate one coherent DMA buffer
- use **MSI-X** to detect DMA completion
- expose a miscdevice to userspace
- support `mmap()` of the DMA buffer and a few control `ioctl`s

Supported ioctls:

- `VFIO_BRIDGE_IOC_GET_INFO`: return DMA buffer address and size
- `VFIO_BRIDGE_IOC_SUBMIT`: submit one DMA request
- `VFIO_BRIDGE_IOC_BAR0_WRITE32`: write a 32-bit BAR0 register from userspace

### `dma_test.c`

Userspace round-trip DMA validation tool.

What it does:

1. open the miscdevice
2. query DMA buffer information
3. `mmap()` the DMA buffer
4. write a known pattern into host memory
5. submit a **host-to-card** DMA
6. clear the host buffer
7. submit a **card-to-host** DMA
8. compare the returned data with the original pattern

This is the quickest way to validate the DMA path end-to-end.

### `elf_loader.c`

Userspace ELF loader using the same kernel driver and DMA path.

What it does:

- parse an ELF32 or ELF64 image
- remap each loadable segment address by subtracting a user-provided base address
- copy segment payloads to device memory through DMA
- zero-initialize the `memsz - filesz` tail when needed
- program `BAR0_ENTRY_POINT`
- program `BAR0_FETCH_ENABLE = 1`

This is the tool used to load and start code on the accelerator side through the PCIe bridge.

### `Makefile`

Build targets:

```bash
make
make module
make dma_test
make elf_loader
make load
make unload
make reload
make clean
```

Behavior:

- `make` builds kernel module and userspace binaries
- `make load` loads the module with `insmod`
- `make unload` removes it with `rmmod`

---

## Build

Inside the guest VM:

```bash
make
```

This produces:

- `gvsoc_vfio_kernel_module.ko`
- `dma_test`
- `elf_loader`

Kernel module build requirements:

- kernel headers matching the running guest kernel
- a working build environment (`make`, `gcc`, etc.)

Typical install on Debian guest:

```bash
apt update
apt install build-essential make linux-headers-$(uname -r)
```

---

## Device Node

The kernel module registers the miscdevice as:

```text
/dev/gvsoc
```

Both userspace tools now use `/dev/gvsoc` by default.

---

## Loading the Driver

Build and load the module inside the guest:

```bash
make load
```

You can verify probe success with:

```bash
dmesg | tail
ls -l /dev/gvsoc
```

The driver should report the probed device, DMA handle, buffer size and IRQ vector.

---

## DMA Test Usage

Syntax:

```bash
./dma_test [len] [card_addr] [device]
```

Arguments:

- `len`: number of bytes to transfer, default `4096`
- `card_addr`: device-local address/offset, default `0x0`
- `device`: miscdevice path, default `/dev/gvsoc`

Expected behavior:

- perform one host-to-card DMA
- perform one card-to-host DMA
- verify that the returned buffer matches the original pattern
- print `Round-trip OK` on success

---

## ELF Loader Usage

Syntax:

```bash
./elf_loader <program.elf> <base_addr> [device] [timeout_ms]
```

Arguments:

- `program.elf`: ELF32 or ELF64 image to load
- `base_addr`: value subtracted from `p_paddr` and `e_entry` before programming the device
- `device`: miscdevice path, default `/dev/gvsoc`
- `timeout_ms`: DMA timeout, default `5000`

Example:

```bash
./elf_loader test.elf 0xC0000000 /dev/gvsoc 5000
```

On success the loader will:

- DMA all PT_LOAD segments to the device
- zero-fill BSS-like tails
- print the detected ELF entry point
- write `BAR0_ENTRY_POINT`
- write `BAR0_FETCH_ENABLE = 1`

---

## Expected System Setup

Typical end-to-end setup:

1. start **GVSoC** with the `pcie_vfio_bridge` model enabled
2. start **QEMU** with the `vfio-user-pci` endpoint attached to the GVSoC UNIX socket
3. boot the guest Linux system
4. build and load this repository inside the guest
5. run `dma_test` and/or `elf_loader`

At a high level:

```text
GVSoC bridge <-> vfio-user socket <-> QEMU guest PCI device <-> guest driver/tools
```

---

## Notes

- The kernel driver is intentionally minimal and single-device oriented.
- The DMA buffer size is currently `4096` bytes in the driver.
- The userspace loader already handles payloads larger than one DMA chunk by splitting transfers.
- The BAR0 protocol and DMA CSR layout are expected to match the current GVSoC bridge implementation.
