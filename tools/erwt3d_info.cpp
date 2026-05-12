#include "erwt3d/reader.hpp"
#include <iostream>
#include <iomanip>
#include <sys/stat.h>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <input.erwt3d>" << std::endl;
        return 1;
    }
    
    std::string inputPath = argv[1];
    erwt3d::ERWT3DReader reader(inputPath);
    
    const auto& header = reader.getHeader();
    
    std::cout << "ERWT3D File Information" << std::endl;
    std::cout << "======================" << std::endl;
    std::cout << "Dimensions: " << header.nx << " x " << header.ny << " x " << header.nz << std::endl;
    std::cout << "Data type: float32" << std::endl;
    std::cout << "Superblock size: " << header.super_x << " x " << header.super_y << " x " << header.super_z << std::endl;
    std::cout << "Leaf block size: " << header.leaf_x << " x " << header.leaf_y << " x " << header.leaf_z << std::endl;
    std::cout << "Data offset: " << header.data_offset << " bytes" << std::endl;
    
    if (erwt3d::hasAnyPanels(header)) {
        std::cout << "Panels: ";
        if (erwt3d::hasXPanels(header)) std::cout << "X(stride=" << erwt3d::getPanelStrideX(header) << ") ";
        if (erwt3d::hasYPanels(header)) std::cout << "Y(stride=" << erwt3d::getPanelStrideY(header) << ") ";
        if (erwt3d::hasZPanels(header)) std::cout << "Z(stride=" << erwt3d::getPanelStrideZ(header) << ") ";
        std::cout << std::endl;
        std::cout << "Panel data offset: " << erwt3d::getPanelDataOffset(header) << " bytes" << std::endl;
        std::cout << "Panel storage: " << erwt3d::getPanelStorageBytes(header) << " bytes ("
                  << std::fixed << std::setprecision(2) << erwt3d::getPanelStorageBytes(header) / (1024.0*1024.0) << " MB)" << std::endl;
    }
    
    uint64_t rawSize = erwt3d::getRawSize(header);
    std::cout << "Estimated raw size: " << rawSize << " bytes (" 
              << std::fixed << std::setprecision(2) << rawSize / (1024.0 * 1024.0) << " MB)" << std::endl;
    
    // Get actual file size
    struct stat st;
    if (stat(inputPath.c_str(), &st) == 0) {
        uint64_t fileSize = st.st_size;
        std::cout << "Actual file size: " << fileSize << " bytes ("
                  << std::fixed << std::setprecision(2) << fileSize / (1024.0 * 1024.0) << " MB)" << std::endl;
        
        double ratio = static_cast<double>(fileSize) / rawSize;
        std::cout << "Storage ratio: " << std::fixed << std::setprecision(3) << ratio << "x" << std::endl;
    }
    
    return 0;
}