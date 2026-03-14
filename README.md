# GVSoC VFIO Test Environment

This repository contains the **host-side infrastructure** used to interact with the GVSoC PCIe VFIO bridge.

It includes:

* Linux kernel module
* DMA tests
* ELF loader
* Build scripts

---

# Repository Structure

```
Makefile
vfio_bridge_host_dma_driver.c
vfio_bridge_dma_test.c
vfio_bridge_elf_loader.c
```

## Makefile

The Makefile builds:

* the **kernel module**
* the **test utilities**

Typical commands:

```
make
make load
make unload
```

---

# Kernel Module

File:

```
vfio_bridge_host_dma_driver.c
```

Responsibilities:

* exposes the VFIO PCI device to userspace
* configures **DMA buffers**
* handles **ioctl commands**
* allows user programs to control DMA

The driver allocates DMA buffers and exposes them to user applications through `mmap`.

---

# DMA Test

File:

```
vfio_bridge_dma_test.c
```

This program performs a **DMA transfer test** between the host memory and the simulated device.

Steps:

1. open the VFIO device
2. map the DMA buffer
3. write test data
4. trigger DMA
5. verify the result

Used to validate the **data path between QEMU and GVSoC**.

---

# ELF Loader

File:

```
vfio_bridge_elf_loader.c
```

This program loads an **ELF binary into the simulated device memory**.

Responsibilities:

* parse ELF headers
* read program segments
* write segments through PCIe BAR
* optionally apply address remapping

Used to boot programs inside the simulated accelerator.

---

# QEMU Setup

## 1 — Build QEMU

Clone and compile:

```
git clone https://github.com/qemu/qemu
cd qemu
./configure --target-list=x86_64-softmmu
make -j8
```

---

## 2 — Download Debian image

```
wget https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-nocloud-arm64.qcow2
```

---

# Launch QEMU

Example command:

```
./build/qemu-system-x86_64 \
  -object memory-backend-memfd,id=mem,size=2G,share=on \
  -machine q35,memory-backend=mem \
  -nodefaults \
  -display none \
  -serial none \
  -monitor tcp:127.0.0.1:45454,server,nowait \
  -drive id=hd0,file=/home/gvsoc/Documents/toolchain/qemu-img/debian-12-nocloud-amd64.qcow2,format=qcow2,if=none \
  -device virtio-blk-pci,drive=hd0 \
  -device e1000,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device pcie-root-port,id=rp1 \
  --device '{"driver":"vfio-user-pci","socket":{"type":"unix","path":"/tmp/gvsoc.sock"}}'
```

This attaches the **VFIO-user PCIe device** to the VM.

---

# Prepare the Debian VM

The Debian cloud image is minimal and requires development tools.

Inside the VM install:

```
apt update
apt install build-essential make git
```

---

# Access the VM

```
ssh -p 2222 root@127.0.0.1
```

---

# Clone Test Repository

Inside the VM:

```
git clone https://github.com/TheSSDGuy/gvsoc-vfio-test
cd gvsoc-vfio-test
make
```

---

# Load the Kernel Module

```
make load
```

This loads the VFIO bridge kernel module.

---

# Running the DMA Test

```
./vfio_bridge_dma_test
```

This performs a DMA transaction between host memory and the simulated device.

---

# Running the ELF Loader

```
./vfio_bridge_elf_loader <binary>
```

This loads the binary into the accelerator memory through PCIe.

Typical usage:

```
./vfio_bridge_elf_loader test.elf
```

---

# Workflow Summary

```
Start GVSoC
      ↓
Start QEMU
      ↓
Boot Debian VM
      ↓
Load kernel module
      ↓
Run DMA tests or ELF loader
```

This environment allows development and testing of:

* PCIe communication
* DMA transfers
* host-side drivers
* accelerator boot flow
