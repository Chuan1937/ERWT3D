# 测试脚本

正确性验证，数据在 D 盘 HDD（`/mnt/d/CUP/`）。

| 脚本 | 功能 |
|------|------|
| `verify.sh` | ERWT3D 与 RAW 逐点比对 |
| `convert.sh` | RAW → ERWT3D 转换 + 验证 |

```bash
# 验证现有数据
bash tests/hdd/verify.sh /mnt/d/CUP/cup_3d_small.dat /mnt/d/CUP/cup_3d_small.erwt3d

# 转换并验证
bash tests/hdd/convert.sh /mnt/d/CUP/cup_3d_small.dat /mnt/d/CUP/test_hdd/out.erwt3d
```
