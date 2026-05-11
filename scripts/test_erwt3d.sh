#!/bin/bash

# Test ERWT3D functionality

set -e

echo "Testing ERWT3D functionality..."
echo "=============================="

# Create test data
echo "Creating test data..."
python3 -c "
import numpy as np
import struct

# Create small test volume
nx, ny, nz = 32, 32, 32
data = np.zeros((nz, ny, nx), dtype=np.float32)
for z in range(nz):
    for y in range(ny):
        for x in range(nx):
            data[z, y, x] = x + 1000*y + 1000000*z

# Write to file
with open('/tmp/test_data.raw', 'wb') as f:
    f.write(data.tobytes())

print(f'Created test data: {nx}x{ny}x{nz}')
"

# Convert raw to ERWT3D
echo ""
echo "Converting raw to ERWT3D..."
/home/yuan/code/ERWT3D/build/tools/erwt3d_convert \
    --input /tmp/test_data.raw \
    --output /tmp/test_data.erwt3d \
    --nx 32 --ny 32 --nz 32 \
    --threads 4

# Get file info
echo ""
echo "File info:"
/home/yuan/code/ERWT3D/build/tools/erwt3d_info /tmp/test_data.erwt3d

# Verify correctness
echo ""
echo "Verifying correctness..."
/home/yuan/code/ERWT3D/build/tools/erwt3d_verify \
    --raw /tmp/test_data.raw \
    --erwt3d /tmp/test_data.erwt3d \
    --nx 32 --ny 32 --nz 32

# Read slices
echo ""
echo "Reading slices..."
/home/yuan/code/ERWT3D/build/tools/erwt3d_slice \
    --input /tmp/test_data.erwt3d \
    --axis z \
    --index 16 \
    --output /tmp/test_z16.raw

/home/yuan/code/ERWT3D/build/tools/erwt3d_slice \
    --input /tmp/test_data.erwt3d \
    --axis y \
    --index 16 \
    --output /tmp/test_y16.raw

/home/yuan/code/ERWT3D/build/tools/erwt3d_slice \
    --input /tmp/test_data.erwt3d \
    --axis x \
    --index 16 \
    --output /tmp/test_x16.raw

# Read line
echo ""
echo "Reading X line..."
/home/yuan/code/ERWT3D/build/tools/erwt3d_slice \
    --input /tmp/test_data.erwt3d \
    --line-x \
    --y 16 --z 16 \
    --output /tmp/test_line_x.raw

# Convert back to raw
echo ""
echo "Converting ERWT3D back to raw..."
/home/yuan/code/ERWT3D/build/tools/erwt3d_convert \
    --input /tmp/test_data.erwt3d \
    --output /tmp/test_restored.raw \
    --to-raw

# Verify restored data
echo ""
echo "Verifying restored data..."
/home/yuan/code/ERWT3D/build/tools/erwt3d_verify \
    --raw-a /tmp/test_data.raw \
    --raw-b /tmp/test_restored.raw \
    --nx 32 --ny 32 --nz 32

# Run benchmark
echo ""
echo "Running benchmark..."
/home/yuan/code/ERWT3D/build/tools/erwt3d_bench \
    --input /tmp/test_data.erwt3d \
    --output-dir /tmp/bench_out \
    --random-count 10 \
    --continuous-count 5 \
    --threads 4

# Cleanup
echo ""
echo "Cleaning up..."
rm -f /tmp/test_data.raw /tmp/test_data.erwt3d /tmp/test_restored.raw
rm -f /tmp/test_z16.raw /tmp/test_y16.raw /tmp/test_x16.raw /tmp/test_line_x.raw
rm -rf /tmp/bench_out

echo ""
echo "All tests passed!"