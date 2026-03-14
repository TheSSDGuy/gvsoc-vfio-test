#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEV_PATH "/dev/vfio_bridge_dma"
#define DEFAULT_LEN 4096U
#define DEFAULT_CARD_ADDR 0U

#define DMA_STATUS_BUSY  (1u << 0)
#define DMA_STATUS_DONE  (1u << 1)
#define DMA_STATUS_ERROR (1u << 2)

#define DMA_DIR_HOST_TO_CARD 0u
#define DMA_DIR_CARD_TO_HOST 1u

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

#define VFIO_BRIDGE_IOC_MAGIC  'V'
#define VFIO_BRIDGE_IOC_GET_INFO  _IOR(VFIO_BRIDGE_IOC_MAGIC, 0x01, struct vfio_bridge_dma_info)
#define VFIO_BRIDGE_IOC_SUBMIT    _IOWR(VFIO_BRIDGE_IOC_MAGIC, 0x02, struct vfio_bridge_dma_submit)

static void die_perror(const char *msg)
{
    perror(msg);
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

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

static int verify_equal(const uint8_t *a, const uint8_t *b, size_t len, size_t *bad_idx)
{
    for (size_t i = 0; i < len; ++i) {
        if (a[i] != b[i]) {
            if (bad_idx) *bad_idx = i;
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev_path = DEV_PATH;
    uint32_t len = DEFAULT_LEN;
    uint64_t card_addr = DEFAULT_CARD_ADDR;
    int fd;
    struct vfio_bridge_dma_info info;
    void *map;
    uint8_t *dma_buf;
    uint8_t *expected;
    struct vfio_bridge_dma_submit req;
    int ret;
    size_t bad_idx;

    if (argc > 1) {
        len = (uint32_t)strtoul(argv[1], NULL, 0);
    }
    if (argc > 2) {
        card_addr = strtoull(argv[2], NULL, 0);
    }
    if (argc > 3) {
        dev_path = argv[3];
    }

    fd = open(dev_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        die_perror("open");
    }

    if (ioctl(fd, VFIO_BRIDGE_IOC_GET_INFO, &info) < 0) {
        die_perror("ioctl(GET_INFO)");
    }

    printf("DMA buffer info:\n");
    printf("  device-visible addr = 0x%016" PRIx64 "\n", info.dma_addr);
    printf("  size                = %u bytes\n", info.size);

    if (len == 0 || len > info.size) {
        fprintf(stderr, "Invalid len=%u (buffer size=%u)\n", len, info.size);
        close(fd);
        return EXIT_FAILURE;
    }

    map = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        die_perror("mmap");
    }

    dma_buf = (uint8_t *)map;
    expected = (uint8_t *)malloc(len);
    if (expected == NULL) {
        die_perror("malloc");
    }

    /* Step 1: prepare H2C payload in the host DMA buffer. */
    fill_pattern(dma_buf, len, 0x10);
    memcpy(expected, dma_buf, len);

    memset(&req, 0, sizeof(req));
    req.direction = DMA_DIR_HOST_TO_CARD;
    req.timeout_ms = 5000;
    req.card_addr = card_addr;
    req.len = len;

    ret = ioctl(fd, VFIO_BRIDGE_IOC_SUBMIT, &req);
    dump_submit_result("H2C submit", &req, ret);
    if (ret < 0) {
        die_perror("ioctl(SUBMIT H2C)");
    }

    /* Step 2: clear host buffer so the next copy back must refill it. */
    memset(dma_buf, 0, len);

    memset(&req, 0, sizeof(req));
    req.direction = DMA_DIR_CARD_TO_HOST;
    req.timeout_ms = 5000;
    req.card_addr = card_addr;
    req.len = len;

    ret = ioctl(fd, VFIO_BRIDGE_IOC_SUBMIT, &req);
    dump_submit_result("C2H submit", &req, ret);
    if (ret < 0) {
        die_perror("ioctl(SUBMIT C2H)");
    }

    if (verify_equal(expected, dma_buf, len, &bad_idx) != 0) {
        fprintf(stderr,
                "Round-trip mismatch at byte %zu: expected=0x%02x got=0x%02x\n",
                bad_idx, expected[bad_idx], dma_buf[bad_idx]);
        free(expected);
        munmap(map, info.size);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Round-trip OK (%u bytes, card_addr=0x%016" PRIx64 ")\n", len, card_addr);

    free(expected);
    munmap(map, info.size);
    close(fd);
    return EXIT_SUCCESS;
}
