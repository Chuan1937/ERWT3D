#include "erwt3d/axis_plane.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace erwt3d;

static int testsPassed = 0;
static int testsFailed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n"; \
        ++testsFailed; \
    } else { ++testsPassed; } \
} while(0)

static void test_axis_enum_mapping() {
    CHECK(axisToSliceAxis(PlaneAxis::X) == SliceAxis::X, "X->SliceX");
    CHECK(axisToSliceAxis(PlaneAxis::Y) == SliceAxis::Y, "Y->SliceY");
    CHECK(axisToSliceAxis(PlaneAxis::Z) == SliceAxis::Z, "Z->SliceZ");

    CHECK(sliceAxisToPlaneAxis(SliceAxis::X) == PlaneAxis::X, "SliceX->X");
    CHECK(sliceAxisToPlaneAxis(SliceAxis::Y) == PlaneAxis::Y, "SliceY->Y");
    CHECK(sliceAxisToPlaneAxis(SliceAxis::Z) == PlaneAxis::Z, "SliceZ->Z");
}

static void test_axis_shape() {
    auto shapeX = makeAxisPlaneShape(PlaneAxis::X, 40, 30, 50);
    CHECK(shapeX.plane_count == 40, "X: plane_count=40");
    CHECK(shapeX.dim_a == 30, "X: dim_a=30 (ny)");
    CHECK(shapeX.dim_b == 50, "X: dim_b=50 (nz)");
    CHECK(shapeX.plane_elements == 1500, "X: elements=1500");
    CHECK(shapeX.plane_bytes == 1500 * sizeof(float), "X: bytes");

    auto shapeY = makeAxisPlaneShape(PlaneAxis::Y, 40, 30, 50);
    CHECK(shapeY.plane_count == 30, "Y: plane_count=30");
    CHECK(shapeY.dim_a == 40, "Y: dim_a=40 (nx)");
    CHECK(shapeY.dim_b == 50, "Y: dim_b=50 (nz)");
    CHECK(shapeY.plane_elements == 2000, "Y: elements=2000");
    CHECK(shapeY.plane_bytes == 2000 * sizeof(float), "Y: bytes");

    auto shapeZ = makeAxisPlaneShape(PlaneAxis::Z, 40, 30, 50);
    CHECK(shapeZ.plane_count == 50, "Z: plane_count=50");
    CHECK(shapeZ.dim_a == 40, "Z: dim_a=40 (nx)");
    CHECK(shapeZ.dim_b == 30, "Z: dim_b=30 (ny)");
    CHECK(shapeZ.plane_elements == 1200, "Z: elements=1200");
    CHECK(shapeZ.plane_bytes == 1200 * sizeof(float), "Z: bytes");
}

static void test_header_sizes() {
    CHECK(sizeof(AxisPlaneHeader) == 256, "AxisPlaneHeader is 256 bytes");
    CHECK(sizeof(AxisPlaneIndexEntry) == 16, "AxisPlaneIndexEntry is 16 bytes");
}

static void test_header_init_and_validate() {
    AxisPlaneHeader h;
    initAxisPlaneHeader(h);

    CHECK(std::memcmp(h.magic, AXISPLANE_MAGIC, 8) == 0, "magic set");
    CHECK(h.version == AXISPLANE_VERSION_V2, "version v2");

    h.axis = static_cast<uint8_t>(PlaneAxis::X);
    h.nx = 100; h.ny = 200; h.nz = 300;

    CHECK(validateAxisPlaneHeader(h, 100, 200, 300), "valid header");
    CHECK(!validateAxisPlaneHeader(h, 99, 200, 300), "invalid nx");
    CHECK(!validateAxisPlaneHeader(h, 100, 199, 300), "invalid ny");

    // Bad axis value
    AxisPlaneHeader h2;
    initAxisPlaneHeader(h2);
    h2.axis = 5;
    h2.nx = 100; h2.ny = 200; h2.nz = 300;
    CHECK(!validateAxisPlaneHeader(h2, 100, 200, 300), "bad axis rejected");
}

static void test_sidecar_path() {
    std::string main = "/data/big.erwt3d";
    CHECK(axisPlaneSidecarPath(main, PlaneAxis::X) == "/data/big.erwt3d.xp", "xp path");
    CHECK(axisPlaneSidecarPath(main, PlaneAxis::Y) == "/data/big.erwt3d.yp", "yp path");
    CHECK(axisPlaneSidecarPath(main, PlaneAxis::Z) == "/data/big.erwt3d.zp", "zp path");

    std::string rzfp = "/data/big.rzfp";
    CHECK(axisPlaneSidecarPath(rzfp, PlaneAxis::X) == "/data/big.rzfp.xp", "rzfp xp");
    CHECK(axisPlaneSidecarPath(rzfp, PlaneAxis::Y) == "/data/big.rzfp.yp", "rzfp yp");
    CHECK(axisPlaneSidecarPath(rzfp, PlaneAxis::Z) == "/data/big.rzfp.zp", "rzfp zp");
}

static void test_axis_extension() {
    CHECK(std::string(axisExtension(PlaneAxis::X)) == ".xp", "xp ext");
    CHECK(std::string(axisExtension(PlaneAxis::Y)) == ".yp", "yp ext");
    CHECK(std::string(axisExtension(PlaneAxis::Z)) == ".zp", "zp ext");
}

static void test_output_index() {
    CHECK(axisPlaneOutputIndex(PlaneAxis::X, 2, 3, 30, 50) == static_cast<uint64_t>(2*50 + 3),
          "X output index: y*nz+z");
    CHECK(axisPlaneOutputIndex(PlaneAxis::Y, 1, 4, 40, 50) == static_cast<uint64_t>(1*50 + 4),
          "Y output index: x*nz+z");
    CHECK(axisPlaneOutputIndex(PlaneAxis::Z, 1, 5, 40, 30) == static_cast<uint64_t>(1*30 + 5),
          "Z output index: x*ny+y");
}

static void test_axis_labels() {
    CHECK(std::string(axisLabel(PlaneAxis::X)) == "X", "label X");
    CHECK(std::string(axisLabel(PlaneAxis::Y)) == "Y", "label Y");
    CHECK(std::string(axisLabel(PlaneAxis::Z)) == "Z", "label Z");
}

int main() {
    test_axis_enum_mapping();
    test_axis_shape();
    test_header_sizes();
    test_header_init_and_validate();
    test_sidecar_path();
    test_axis_extension();
    test_output_index();
    test_axis_labels();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
