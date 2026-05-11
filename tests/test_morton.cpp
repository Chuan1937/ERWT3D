#include "erwt3d/morton.hpp"
#include <iostream>
#include <cassert>
#include <cstdint>

void testBasicEncoding() {
    std::cout << "Testing basic Morton encoding..." << std::endl;
    
    // Test (0,0,0)
    assert(erwt3d::morton3D(0, 0, 0) == 0);
    
    // Test (1,0,0)
    assert(erwt3d::morton3D(1, 0, 0) == 1);
    
    // Test (0,1,0)
    assert(erwt3d::morton3D(0, 1, 0) == 2);
    
    // Test (0,0,1)
    assert(erwt3d::morton3D(0, 0, 1) == 4);
    
    // Test (1,1,1)
    assert(erwt3d::morton3D(1, 1, 1) == 7);
    
    // Test (2,0,0)
    assert(erwt3d::morton3D(2, 0, 0) == 8);
    
    std::cout << "  PASSED" << std::endl;
}

void testRoundTrip() {
    std::cout << "Testing Morton round-trip..." << std::endl;
    
    for (uint32_t x = 0; x < 64; ++x) {
        for (uint32_t y = 0; y < 64; ++y) {
            for (uint32_t z = 0; z < 64; ++z) {
                uint64_t code = erwt3d::morton3D(x, y, z);
                uint32_t rx, ry, rz;
                erwt3d::unmorton3D(code, rx, ry, rz);
                assert(rx == x);
                assert(ry == y);
                assert(rz == z);
            }
        }
    }
    
    std::cout << "  PASSED" << std::endl;
}

void testOrdering() {
    std::cout << "Testing Morton ordering..." << std::endl;
    
    // Morton codes should be unique for different coordinates
    uint64_t code1 = erwt3d::morton3D(1, 2, 3);
    uint64_t code2 = erwt3d::morton3D(3, 2, 1);
    uint64_t code3 = erwt3d::morton3D(2, 3, 1);
    
    assert(code1 != code2);
    assert(code2 != code3);
    assert(code1 != code3);
    
    std::cout << "  PASSED" << std::endl;
}

int main() {
    std::cout << "Morton Encoding Tests" << std::endl;
    std::cout << "=====================" << std::endl;
    
    testBasicEncoding();
    testRoundTrip();
    testOrdering();
    
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}