# Rocky K-Disk Benchmark Environment

## Timestamp
2026-07-25T00:40:00Z

## System
- Hostname: localhost.localdomain
- OS: Rocky Linux 9.8 (Blue Onyx)
- Kernel: 5.14.0-687.26.1.el9_8.x86_64
- Architecture: x86_64

## CPU
- Model: Intel Core i9-10850K @ 3.60GHz
- CPUs: 16 (8 cores, 2 threads each via VMware)

## Memory
- Total: 62 GiB
- Swap: 31 GiB

## Disk Layout
- System NVMe: 600GB (ROTA=0, SSD) — VMware NVMe
  - / (rlm-root): 70GB xfs
  - /home (rlm-home): 497.6GB xfs
- K-Disk: hgfs shared folder (ROTA=1, HDD) — 7.3T capacity
  - Mounted at /mnt/k, symlinked as /data
- G-Disk: hgfs shared folder (ROTA=1, HDD) — 7.3T capacity
  - Mounted at /mnt/g

## K-Disk Workspace
- Path: /data → /mnt/k/erwt3d-hdd-k
- Free: 5.8T

## Git
- Commit: 9b4a496 Merge pull request #59
- Branch: perf/hdd-kdisk-axis-strategy
- Tag: ssd-axis-leaf-final

## Compiler
- GCC: 11.5.0 20240719 (Red Hat 11.5.0-14)
- CMake: 3.31.8

## Raw Data
- cup_3d_small.dat: 19271755620 bytes (18 GiB)
- cup_3d_big.dat: 52850412000 bytes (49 GiB)
