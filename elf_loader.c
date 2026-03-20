#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <elf.h>

#define DEV_PATH "/dev/gvsoc"
#define DMA_CHUNK_SIZE 4096U

#define DMA_STATUS_BUSY  (1u << 0)
#define DMA_STATUS_DONE  (1u << 1)
#define DMA_STATUS_ERROR (1u << 2)

#define DMA_DIR_HOST_TO_CARD 0u
#define DMA_DIR_CARD_TO_HOST 1u

/* BAR0 CSRs exposed by the VFIO bridge */
#define BAR0_ENTRY_POINT   0x28u
#define BAR0_FETCH_ENABLE  0x2Cu

struct vfio_bridge_dma_info {
    uint64_t dma_addr;
    uint32_t size;
    uint32_t reserved;
};

struct vfio_bridge_dma_submit {
    uint32_t direction;
    uint32_t timeout_ms;
    uint64_t card_addr;
    uint32_t len;
    uint32_t status;
    uint32_t error;
    uint32_t reserved;
};

/* New generic BAR0 write ioctl payload */
struct vfio_bridge_bar0_write32 {
    uint32_t offset;
    uint32_t value;
};

#define VFIO_BRIDGE_IOC_MAGIC       'V'
#define VFIO_BRIDGE_IOC_GET_INFO    _IOR(VFIO_BRIDGE_IOC_MAGIC, 0x01, struct vfio_bridge_dma_info)
#define VFIO_BRIDGE_IOC_SUBMIT      _IOWR(VFIO_BRIDGE_IOC_MAGIC, 0x02, struct vfio_bridge_dma_submit)
#define VFIO_BRIDGE_IOC_BAR0_WRITE32 _IOW(VFIO_BRIDGE_IOC_MAGIC, 0x03, struct vfio_bridge_bar0_write32)

typedef struct {
    uint64_t entry;
    int is_32;
} elf_info_t;

static void die_perror(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static void die_msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

static void dump_submit_result(const char *tag, const struct vfio_bridge_dma_submit *req, int ret)
{
    printf("%s: ret=%d status=0x%08x error=0x%08x\n",
           tag, ret, req->status, req->error);
    printf("  flags: BUSY=%u DONE=%u ERROR=%u\n",
           !!(req->status & DMA_STATUS_BUSY),
           !!(req->status & DMA_STATUS_DONE),
           !!(req->status & DMA_STATUS_ERROR));
}

static uint64_t remap_card_addr(uint64_t elf_addr, uint64_t base_addr, int seg_idx)
{
    if (elf_addr < base_addr) {
        if (seg_idx >= 0) {
            fprintf(stderr,
                    "Segment %d has paddr=0x%016" PRIx64
                    " which is below base_addr=0x%016" PRIx64 "\n",
                    seg_idx, elf_addr, base_addr);
        } else {
            fprintf(stderr,
                    "Entry point 0x%016" PRIx64
                    " is below base_addr=0x%016" PRIx64 "\n",
                    elf_addr, base_addr);
        }
        exit(EXIT_FAILURE);
    }

    return elf_addr - base_addr;
}

static void bar0_write32(int fd, uint32_t offset, uint32_t value)
{
    struct vfio_bridge_bar0_write32 req;

    memset(&req, 0, sizeof(req));
    req.offset = offset;
    req.value  = value;

    if (ioctl(fd, VFIO_BRIDGE_IOC_BAR0_WRITE32, &req) < 0) {
        die_perror("ioctl(BAR0_WRITE32)");
    }
}

static void dma_h2c_submit(int fd, uint64_t card_addr, uint32_t len, uint32_t timeout_ms)
{
    struct vfio_bridge_dma_submit req;
    int ret;

    memset(&req, 0, sizeof(req));
    req.direction = DMA_DIR_HOST_TO_CARD;
    req.timeout_ms = timeout_ms;
    req.card_addr = card_addr;
    req.len = len;

    ret = ioctl(fd, VFIO_BRIDGE_IOC_SUBMIT, &req);
    dump_submit_result("H2C submit", &req, ret);
    if (ret < 0) {
        die_perror("ioctl(SUBMIT H2C)");
    }
    if ((req.status & DMA_STATUS_ERROR) != 0u) {
        fprintf(stderr, "DMA H2C error: status=0x%08x error=0x%08x\n", req.status, req.error);
        exit(EXIT_FAILURE);
    }
}

static void dma_write_buffered(int fd,
                               uint8_t *dma_buf,
                               uint32_t dma_buf_size,
                               uint64_t card_addr,
                               const uint8_t *src,
                               size_t len,
                               uint32_t timeout_ms)
{
    size_t offset = 0;

    if (dma_buf_size == 0) {
        die_msg("DMA buffer size is zero");
    }

    while (offset < len) {
        uint32_t chunk = (uint32_t)((len - offset) > dma_buf_size ? dma_buf_size : (len - offset));
        memcpy(dma_buf, src + offset, chunk);
        dma_h2c_submit(fd, card_addr + offset, chunk, timeout_ms);
        offset += chunk;
    }
}

static void dma_zero_buffered(int fd,
                              uint8_t *dma_buf,
                              uint32_t dma_buf_size,
                              uint64_t card_addr,
                              size_t len,
                              uint32_t timeout_ms)
{
    size_t offset = 0;

    if (dma_buf_size == 0) {
        die_msg("DMA buffer size is zero");
    }

    memset(dma_buf, 0, dma_buf_size);

    while (offset < len) {
        uint32_t chunk = (uint32_t)((len - offset) > dma_buf_size ? dma_buf_size : (len - offset));
        dma_h2c_submit(fd, card_addr + offset, chunk, timeout_ms);
        offset += chunk;
    }
}

static void check_elf_ident(const unsigned char *ident)
{
    if (memcmp(ident, ELFMAG, SELFMAG) != 0) {
        die_msg("Input file is not an ELF");
    }

    if (ident[EI_DATA] != ELFDATA2LSB) {
        die_msg("Only little-endian ELF files are supported");
    }

    if (ident[EI_CLASS] != ELFCLASS32 && ident[EI_CLASS] != ELFCLASS64) {
        die_msg("Unsupported ELF class");
    }
}

static void load_elf32_segments(const uint8_t *file_base,
                                size_t file_size,
                                elf_info_t *info,
                                int fd,
                                uint8_t *dma_buf,
                                uint32_t dma_buf_size,
                                uint32_t timeout_ms,
                                uint64_t base_addr)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)file_base;
    const Elf32_Phdr *phdrs;

    if (ehdr->e_phoff + ((size_t)ehdr->e_phnum * sizeof(Elf32_Phdr)) > file_size) {
        die_msg("ELF32 program header table exceeds file size");
    }

    phdrs = (const Elf32_Phdr *)(file_base + ehdr->e_phoff);
    info->entry = ehdr->e_entry;
    info->is_32 = 1;

    for (int i = 0; i < ehdr->e_phnum; ++i) {
        const Elf32_Phdr *seg = &phdrs[i];
        uint64_t remapped_paddr;

        if (seg->p_type != PT_LOAD || seg->p_memsz == 0) {
            continue;
        }

        if ((uint64_t)seg->p_offset + (uint64_t)seg->p_filesz > file_size) {
            fprintf(stderr, "ELF32 segment %d exceeds file size\n", i);
            exit(EXIT_FAILURE);
        }

        remapped_paddr = remap_card_addr((uint64_t)seg->p_paddr, base_addr, i);

        printf("[ELF32] LOAD %d: off=0x%08" PRIx32
               " paddr=0x%08" PRIx32
               " remap=0x%016" PRIx64
               " filesz=0x%08" PRIx32
               " memsz=0x%08" PRIx32 "\n",
               i,
               seg->p_offset,
               seg->p_paddr,
               remapped_paddr,
               seg->p_filesz,
               seg->p_memsz);

        if (seg->p_filesz > 0) {
            dma_write_buffered(fd,
                               dma_buf,
                               dma_buf_size,
                               remapped_paddr,
                               file_base + seg->p_offset,
                               seg->p_filesz,
                               timeout_ms);
        }

        if (seg->p_memsz > seg->p_filesz) {
            dma_zero_buffered(fd,
                              dma_buf,
                              dma_buf_size,
                              remapped_paddr + seg->p_filesz,
                              (size_t)seg->p_memsz - seg->p_filesz,
                              timeout_ms);
        }
    }
}

static void load_elf64_segments(const uint8_t *file_base,
                                size_t file_size,
                                elf_info_t *info,
                                int fd,
                                uint8_t *dma_buf,
                                uint32_t dma_buf_size,
                                uint32_t timeout_ms,
                                uint64_t base_addr)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file_base;
    const Elf64_Phdr *phdrs;

    if (ehdr->e_phoff + ((size_t)ehdr->e_phnum * sizeof(Elf64_Phdr)) > file_size) {
        die_msg("ELF64 program header table exceeds file size");
    }

    phdrs = (const Elf64_Phdr *)(file_base + ehdr->e_phoff);
    info->entry = ehdr->e_entry;
    info->is_32 = 0;

    for (int i = 0; i < ehdr->e_phnum; ++i) {
        const Elf64_Phdr *seg = &phdrs[i];
        uint64_t remapped_paddr;

        if (seg->p_type != PT_LOAD || seg->p_memsz == 0) {
            continue;
        }

        if (seg->p_offset + seg->p_filesz > file_size) {
            fprintf(stderr, "ELF64 segment %d exceeds file size\n", i);
            exit(EXIT_FAILURE);
        }

        remapped_paddr = remap_card_addr(seg->p_paddr, base_addr, i);

        printf("[ELF64] LOAD %d: off=0x%016" PRIx64
               " paddr=0x%016" PRIx64
               " remap=0x%016" PRIx64
               " filesz=0x%016" PRIx64
               " memsz=0x%016" PRIx64 "\n",
               i,
               seg->p_offset,
               seg->p_paddr,
               remapped_paddr,
               seg->p_filesz,
               seg->p_memsz);

        if (seg->p_filesz > 0) {
            dma_write_buffered(fd,
                               dma_buf,
                               dma_buf_size,
                               remapped_paddr,
                               file_base + seg->p_offset,
                               (size_t)seg->p_filesz,
                               timeout_ms);
        }

        if (seg->p_memsz > seg->p_filesz) {
            dma_zero_buffered(fd,
                              dma_buf,
                              dma_buf_size,
                              remapped_paddr + seg->p_filesz,
                              (size_t)(seg->p_memsz - seg->p_filesz),
                              timeout_ms);
        }
    }
}

static void load_elf_via_dma(const char *elf_path,
                             int fd,
                             uint8_t *dma_buf,
                             uint32_t dma_buf_size,
                             uint32_t timeout_ms,
                             uint64_t base_addr,
                             elf_info_t *info)
{
    int elf_fd;
    struct stat st;
    uint8_t *file_base;

    elf_fd = open(elf_path, O_RDONLY);
    if (elf_fd < 0) {
        die_perror("open(elf)");
    }

    if (fstat(elf_fd, &st) < 0) {
        close(elf_fd);
        die_perror("fstat(elf)");
    }

    if (st.st_size <= 0) {
        close(elf_fd);
        die_msg("ELF file is empty");
    }

    file_base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, elf_fd, 0);
    if (file_base == MAP_FAILED) {
        close(elf_fd);
        die_perror("mmap(elf)");
    }

    close(elf_fd);

    check_elf_ident(file_base);

    if (file_base[EI_CLASS] == ELFCLASS32) {
        load_elf32_segments(file_base, (size_t)st.st_size, info, fd, dma_buf, dma_buf_size, timeout_ms, base_addr);
    } else {
        load_elf64_segments(file_base, (size_t)st.st_size, info, fd, dma_buf, dma_buf_size, timeout_ms, base_addr);
    }

    munmap(file_base, (size_t)st.st_size);
}

static void program_entry_and_start(int fd, const elf_info_t *elf_info)
{
    printf("Programming BAR0 entry point:\n");
    printf("  ELF entry        = 0x%016" PRIx64 "\n", elf_info->entry);

    bar0_write32(fd, BAR0_ENTRY_POINT, (uint32_t)elf_info->entry);

    printf("Programming BAR0 fetch enable:\n");
    printf("  fetch_enable     = 1\n");

    bar0_write32(fd, BAR0_FETCH_ENABLE, 1u);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <program.elf> <base_addr> [device] [timeout_ms]\n"
            "  base_addr  address removed from ELF p_paddr and e_entry before programming/loading\n"
            "             example: 0xC0000000\n"
            "  device     default: %s\n"
            "  timeout_ms default: 5000\n",
            prog, DEV_PATH);
}

int main(int argc, char **argv)
{
    const char *elf_path;
    const char *dev_path = DEV_PATH;
    uint64_t base_addr;
    uint32_t timeout_ms = 5000;
    int fd;
    struct vfio_bridge_dma_info info;
    void *map;
    uint8_t *dma_buf;
    elf_info_t elf_info;

    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    elf_path = argv[1];
    base_addr = strtoull(argv[2], NULL, 0);

    if (argc > 3) {
        dev_path = argv[3];
    }
    if (argc > 4) {
        timeout_ms = (uint32_t)strtoul(argv[4], NULL, 0);
    }

    fd = open(dev_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        die_perror("open(device)");
    }

    if (ioctl(fd, VFIO_BRIDGE_IOC_GET_INFO, &info) < 0) {
        close(fd);
        die_perror("ioctl(GET_INFO)");
    }

    printf("DMA buffer info:\n");
    printf("  device-visible addr = 0x%016" PRIx64 "\n", info.dma_addr);
    printf("  size                = %u bytes\n", info.size);
    printf("  ELF remap base      = 0x%016" PRIx64 "\n", base_addr);

    if (info.size < DMA_CHUNK_SIZE) {
        fprintf(stderr,
                "Warning: DMA buffer is %u bytes; chunking will use buffer size instead of %u\n",
                info.size, DMA_CHUNK_SIZE);
    }

    map = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        die_perror("mmap(device)");
    }
    dma_buf = (uint8_t *)map;

    memset(&elf_info, 0, sizeof(elf_info));
    load_elf_via_dma(elf_path, fd, dma_buf, info.size, timeout_ms, base_addr, &elf_info);

    printf("ELF load completed. entry=0x%016" PRIx64 " (%s)\n",
           elf_info.entry,
           elf_info.is_32 ? "ELF32" : "ELF64");

    program_entry_and_start(fd, &elf_info);

    printf("Program start sequence completed.\n");

    munmap(map, info.size);
    close(fd);
    return EXIT_SUCCESS;
}