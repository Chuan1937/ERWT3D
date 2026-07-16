# 三维数据生成工具 genenator

# 介绍

本工具用于随机生成三维数据集，供测试三维数据集的算法性能。
支持Windows、Linux跨平台编译运行，具体平台编译方法请参考下方说明

# 编译方法

## Windows

### 编译环境

1. Visual Studio 2017及以上
2. CMake 3.14及以上
3. C++11及以上
4. Qt5

### 编译步骤

1. Visual Studio可视化编译  
1.1 Visual Studio打开CMakeLists.txt文件，选择生成项目;  
1.2 生成项目，生成项目文件;  
1.3 等待生成完成，运行项目;
2. 命令行编译
2.1 打开visual studio对应平台命令行终端；
2.2 cd 本项目路径;
2.3 mkdir build;
2.4 cmake -S . -B build -DCMAKE\_INSTALL\_PREFIX=.
2.5 cd build;
2.6 nmake \&\& nmake install  
2.7 运行bin目录下的exe文件

## Linux

### 编译环境

1. gcc 7.5.0及以上或clang 7.0.0及以上编译器
2. CMake 3.14及以上
3. C++11及以上
4. Qt5

### 编译步骤

1. 命令行编译  
1.1  cd 本项目路径;
1.2  mkdir build;
1.4 cmake -S . -B build -DCMAKE\_INSTALL\_PREFIX=.
1.5 cd build;
1.6 make \&\& make install  
1.7 运行bin目录下的可执行文件

