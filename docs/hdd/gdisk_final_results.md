# G-Disk /home Final Validation Results

## Environment
- System: Rocky Linux 9.8, NVMe xfs (/home)
- CPU: i9-10850K 16 vCPUs
- RAM: 62 GiB
- Branch: perf/hdd-kdisk-axis-strategy (tag: hdd-kdisk-candidate)
- Commit: 6acd4fb

## 20GB LZ4 + YZ Whole-Plane

| Run | IO Profile | Threads | T_composite | Storage |
|-----|-----------|---------|-------------|---------|
| 1 | auto→SSD | 8 | 1.452s | 0.853x |
| 2 | auto→SSD | 8 | 1.423s | 0.853x |
| 3 | auto→SSD | 8 | 1.414s | 0.853x |
| **Mean** | | | **1.430s** | |
| CV | | | 1.4% | |

## 50GB RZFP Axis Leaf

| Run | IO Profile | Threads | T_composite | Storage |
|-----|-----------|---------|-------------|---------|
| 1 (cold) | auto→HDD | 6 | 5.838s | 1.295x |
| 2 | auto→HDD | 6 | 4.601s | 1.295x |
| 3 | auto→HDD | 6 | 4.614s | 1.295x |
| **Mean** (2-3) | | | **4.608s** | |
| CV | | | 0.1% | |

## Comparison

| Dataset | K-disk (hgfs) | G-disk (xfs) | Ratio |
|---------|---------------|--------------|-------|
| 20GB LZ4 YZ | 7.068s | 1.430s | 4.9x faster |
| 50GB RZFP axis | 12.136s | 4.608s | 2.6x faster |

## Key Findings
1. auto IO profile selects optimal settings (SSD for LZ4, HDD with cache for RZFP)
2. Explicit --io-profile hdd degrades performance on NVMe (33s vs 1.4s for 20GB)
3. RZFP axis leaf outperforms legacy by 5-18x across environments
4. Both configurations well within 1.50x storage budget
