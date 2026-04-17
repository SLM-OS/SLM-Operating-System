/*
 * sec2_peek — minimal BAR0 MMIO reader for NVIDIA GPU investigation.
 *
 * Usage:
 *   sec2_peek <bar0_phys> <bar0_size> <offset>        # read one u32
 *   sec2_peek <bar0_phys> <bar0_size> <offset> <n>    # read n u32 words
 *
 * bar0_phys and bar0_size in hex (with or without 0x). Offset in hex.
 *
 * Discover bar0_phys and bar0_size via:
 *   lspci -vvv -s 01:00.0 | grep -A0 "Region 0"
 *   # or
 *   cat /sys/bus/pci/devices/0000:01:00.0/resource  # line 1 is BAR0
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t parse_hex(const char *s) {
    return strtoull(s, NULL, 16);
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr,
                "usage: %s <bar0_phys_hex> <bar0_size_hex> <offset_hex> [count]\n",
                argv[0]);
        return 2;
    }

    uint64_t bar_phys = parse_hex(argv[1]);
    uint64_t bar_size = parse_hex(argv[2]);
    uint64_t offset = parse_hex(argv[3]);
    uint64_t count = (argc == 5) ? parse_hex(argv[4]) : 1;

    if (offset + count * 4 > bar_size) {
        fprintf(stderr, "offset+count*4 (%#" PRIx64 ") exceeds bar_size (%#" PRIx64 ")\n",
                offset + count * 4, bar_size);
        return 2;
    }

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem (need root + CONFIG_STRICT_DEVMEM off or iomem=relaxed)");
        return 1;
    }

    void *map = mmap(NULL, bar_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, bar_phys);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    volatile uint32_t *regs = (volatile uint32_t *)((uint8_t *)map + offset);
    for (uint64_t i = 0; i < count; i++) {
        uint32_t v = regs[i];
        printf("%08" PRIx64 ": %08" PRIx32 "\n", offset + i * 4, v);
    }

    munmap(map, bar_size);
    close(fd);
    return 0;
}
