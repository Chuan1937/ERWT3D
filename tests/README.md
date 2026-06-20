# 测试套件

## 目录结构

```
tests/
├── hdd/
│   ├── 01_single_slice.sh      # 单切片读取延迟
│   ├── 02_multi_slice.sh       # 多切片顺序读取
│   ├── 03_batch_vs_single.sh   # Batch vs 逐切片对比
│   ├── 04_full_hdd.sh          # 完整 HDD 测试 (100+10)
│   └── 05_verify.sh            # 正确性验证
└── ssd/                         # 待编写
```

## 使用

```bash
make test-hdd     # 运行 HDD 测试
```
