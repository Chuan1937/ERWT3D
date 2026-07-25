# K-Disk Phase 2-3 Results Summary

## 20GB (cup_3d_small.dat: 801×2405×2501)

| Config | Threads | T_composite | X_rand | Y_rand | Z_rand | Storage | 
|--------|---------|-------------|--------|--------|--------|---------|
| L2 LZ4 basic | 8 | 8.950s | 35.28s | 8.76s | 6.78s | 0.479x |
| **L3 LZ4+YZ** ★ | **8** | **7.068s** | **24.51s** | **8.47s** | **6.85s** | **0.853x** |

## 50GB (cup_3d_big.dat: 2001×2201×3000)

| Config | Threads | T_composite | X_rand | Y_rand | Z_rand | Storage |
|--------|---------|-------------|--------|--------|--------|---------|
| R1 RZFP legacy | 8 | 83.125s | 144.7s | 144.6s | 145.2s | 0.421x |
| R3 axis t=8 | 8 | 16.353s | 30.9s | 30.6s | 26.8s | 1.295x |
| **R3 axis t=6** ★ | **6** | **12.136s** | **92% shared read** | | | **1.295x** |

## K-Disk Candidates
- 20GB: LZ4 + YZ whole-plane, threads=8, 7.07s
- 50GB: RZFP axis leaf, threads=6, 12.14s
