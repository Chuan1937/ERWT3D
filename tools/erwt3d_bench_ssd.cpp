#include "erwt3d/io_profile.hpp"
#include "erwt3d/unified_read_config.hpp"
#include "erwt3d/ssd/ssd_config.hpp"

#include <iostream>

int main() {
    std::cout << "ERWT3D SSD Benchmark Tool\n";
    std::cout << "IO profiles available: auto, hdd, ssd, wsl-ssd\n";

    bool wsl = erwt3d::detectWSL();
    std::cout << "WSL detected: " << (wsl ? "yes" : "no") << "\n";

    auto cfg = erwt3d::makeUnifiedConfig(
        erwt3d::IOProfileType::Auto, ".", 8, 4096, 0);
    std::cout << "Auto profile: " << erwt3d::ioProfileTypeName(cfg.io_profile)
              << " (" << cfg.resolved_profile_reason << ")\n";
    std::cout << "Filesystem: " << cfg.filesystem_type << "\n";
    std::cout << "HDD window: " << cfg.hdd.read_window_bytes / 1048576 << " MB\n";
    std::cout << "HDD gap: " << cfg.hdd.max_gap_bytes / 1024 << " KB\n";
    std::cout << "SSD read threads: " << cfg.ssd.read_threads << "\n";
    std::cout << "SSD decode threads: " << cfg.ssd.decode_threads << "\n";
    std::cout << "SSD window: " << cfg.ssd.read_window_bytes / 1048576 << " MB\n";
    std::cout << "SSD gap: " << cfg.ssd.max_gap_bytes / 1024 << " KB\n";

    return 0;
}
