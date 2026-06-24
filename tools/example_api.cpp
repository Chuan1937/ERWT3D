#include "erwt3d/api.hpp"
#include <iostream>

int main() {
    // ========== HDD 测试 ==========
    // 等价于: ./build/erwt3d_bench_contest -i data.erwt3d -o out --hdd
    {
        auto cfg = erwt3d::hddConfig("/mnt/d/CUP/cup_3d_small.erwt3d", "/mnt/d/CUP/out_hdd");
        cfg.randomCount = 10;
        cfg.continuousCount = 5;

        auto result = erwt3d::benchmarkContest(cfg);
        std::cout << "T_composite = " << result.T_composite_ms / 1000.0 << " s" << std::endl;
    }

    // ========== 自定义配置 ==========
    // erwt3d::BenchConfig cfg;
    // cfg.input = "data.erwt3d";
    // cfg.outputDir = "out";
    // cfg.numThreads = 4;
    // cfg.hddMode = true;
    // cfg.readMode = erwt3d::SBReadMode::HDDReadWindow;
    // ...

    return 0;
}
