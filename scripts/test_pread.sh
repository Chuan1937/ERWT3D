#!/bin/bash
# 测量 pread 实际延迟：100 个随机 1MB pread
set -e

cat > /tmp/test_pread.c << 'EOF'
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main() {
    int fd = open("/mnt/g/CUP/cup_3d_small.erwt3d", O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    
    char buf[1048576];
    struct timespec t0, t1;
    
    // 100 个随机 1MB pread
    uint64_t offsets[] = {
        0, 13*1048576, 26*1048576, 39*1048576, 52*1048576,
        65*1048576, 78*1048576, 91*1048576, 104*1048576, 117*1048576,
        130*1048576, 143*1048576, 156*1048576, 169*1048576, 182*1048576,
        195*1048576, 208*1048576, 221*1048576, 234*1048576, 247*1048576
    };
    
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 100; i++) {
        uint64_t off = offsets[i % 20];
        pread(fd, buf, 1048576, off);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("100 x pread(1MB): %.1f ms  (%.2f ms/op)\n", ms, ms / 100);
    
    // 100 个随机 128MB pread
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 100; i++) {
        uint64_t off = offsets[i % 20];
        pread(fd, buf, 128*1048576, off);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    
    ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("100 x pread(128MB): %.1f ms  (%.2f ms/op)\n", ms, ms / 100);
    
    close(fd);
    return 0;
}
EOF

gcc -O2 -o /tmp/test_pread /tmp/test_pread.c && /tmp/test_pread
