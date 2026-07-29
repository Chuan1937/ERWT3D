# ERWT3D

三维空间数据高效读写库。采用自定义单文件格式（.erwt3d），以 Morton Z-order 物理布局实现三轴切片访问均衡，支持 LZ4 和 RZFP 两种压缩存储。

## 1. 构建

推荐在实际运行设备上自行编译，以充分利用目标 CPU 指令集。预编译版本性能会明显下降。

### 依赖

- C++17 编译器（GCC ≥ 8、Clang ≥ 7）
- CMake ≥ 3.16
- LZ4

### 编译

```bash
cmake3 -S deps/zfp-src -B build-zfp \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/deps/zfp" \
  -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native" \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_UTILITIES=OFF \
  -DBUILD_TESTING=OFF \
  -DZFP_WITH_OPENMP=OFF
cmake3 --build build-zfp -j"$(nproc)"
cmake3 --install build-zfp

cmake3 -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$(command -v g++)" \
  -DCMAKE_PREFIX_PATH="$PWD/deps/zfp" \
  -DERWT3D_ENABLE_RZFP=ON \
  -DERWT3D_NATIVE_OPT=ON
cmake3 --build build -j"$(nproc)"
```

## 2. 使用

使用预编译版本时，将 `./build/` 替换为预编译包中的 `./bin/`。

### 2.1. 数据转换

正向转换（raw → .erwt3d）：

```bash
./build/erwt3d_convert \
  --input cup_3d_small.dat \
  --output cup_3d_small.erwt3d \
  --nx 801 --ny 2405 --nz 2501 \
  --threads auto --memory-limit-mb auto
```

反向转换（.erwt3d → raw）：

```bash
./build/erwt3d_convert \
  --input cup_3d_small.erwt3d \
  --output restored.dat \
  --to-raw
```

### 2.2. 切片读取

自助测试（自动生成坐标）：

```bash
./build/erwt3d_contest \
  --input cup_3d_small.erwt3d \
  --output-dir test_output \
  --seed 20260511
```

正式测试（指定坐标文件）：

```bash
./build/erwt3d_contest \
  --input cup_3d_small.erwt3d \
  --output-dir official_output \
  --positions-file positions_small.csv \
  --memory-limit-mb 8192
```

坐标文件为 CSV/TXT 格式，每行由轴向、访问类型和坐标索引组成：

```csv
axis,type,index
x,random,12
x,random,157
y,random,35
z,random,84
x,continuous,300
x,continuous,301
y,continuous,700
y,continuous,701
z,continuous,1000
z,continuous,1001
```

完整坐标文件应满足：
- X、Y、Z 方向各 100 个不重复 random 坐标
- X、Y、Z 方向各 10 个严格递增且相邻的 continuous 坐标
- 坐标索引从 0 开始，不超出对应维度范围

参考坐标文件位于 `positions/` 目录。


## 3. 预编译包

在centOS 7 Intel 上所编译
`dist/ERWT3D-1.0.0-linux-x86_64-v3-c7.tar.gz`

