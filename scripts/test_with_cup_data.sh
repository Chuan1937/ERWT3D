#!/bin/bash

# Test ERWT3D with CUP data format

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

echo "Testing ERWT3D with CUP data format"
echo "==================================="

# Create test data with small dimensions (similar to CUP format)
echo "Creating test data (100x100x100)..."
"$BUILD_DIR/gen_test_data" \
    --nx 100 --ny 100 --nz 100 \
    --output /tmp/test_cup_data.raw \
    --seed 42

# Convert raw to ERWT3D
echo ""
echo "Converting raw to ERWT3D..."
"$BUILD_DIR/erwt3d_convert" \
    --input /tmp/test_cup_data.raw \
    --output /tmp/test_cup_data.erwt3d \
    --nx 100 --ny 100 --nz 100 \
    --threads 4

# Get file info
echo ""
echo "File info:"
"$BUILD_DIR/erwt3d_info" /tmp/test_cup_data.erwt3d

# Verify correctness
echo ""
echo "Verifying correctness..."
"$BUILD_DIR/erwt3d_verify" \
    --raw /tmp/test_cup_data.raw \
    --erwt3d /tmp/test_cup_data.erwt3d \
    --nx 100 --ny 100 --nz 100

# Read slices for all axes
echo ""
echo "Reading Z slice..."
"$BUILD_DIR/erwt3d_slice" \
    --input /tmp/test_cup_data.erwt3d \
    --axis z \
    --index 50 \
    --output /tmp/test_z50.raw

echo ""
echo "Reading Y slice..."
"$BUILD_DIR/erwt3d_slice" \
    --input /tmp/test_cup_data.erwt3d \
    --axis y \
    --index 50 \
    --output /tmp/test_y50.raw

echo ""
echo "Reading X slice..."
"$BUILD_DIR/erwt3d_slice" \
    --input /tmp/test_cup_data.erwt3d \
    --axis x \
    --index 50 \
    --output /tmp/test_x50.raw

# Read line
echo ""
echo "Reading X line..."
"$BUILD_DIR/erwt3d_slice" \
    --input /tmp/test_cup_data.erwt3d \
    --line-x \
    --y 50 --z 50 \
    --output /tmp/test_line_x.raw

# Convert back to raw
echo ""
echo "Converting ERWT3D back to raw..."
"$BUILD_DIR/erwt3d_convert" \
    --input /tmp/test_cup_data.erwt3d \
    --output /tmp/test_restored.raw \
    --to-raw

# Verify restored data
echo ""
echo "Verifying restored data..."
"$BUILD_DIR/erwt3d_verify" \
    --raw-a /tmp/test_cup_data.raw \
    --raw-b /tmp/test_restored.raw \
    --nx 100 --ny 100 --nz 100

# Run benchmark
echo ""
echo "Running benchmark..."
"$BUILD_DIR/erwt3d_bench" \
    --input /tmp/test_cup_data.erwt3d \
    --output-dir /tmp/bench_out \
    --random-count 10 \
    --continuous-count 5 \
    --threads 4

# Cleanup
echo ""
echo "Cleaning up..."
rm -f /tmp/test_cup_data.raw /tmp/test_cup_data.erwt3d /tmp/test_restored.raw
rm -f /tmp/test_z50.raw /tmp/test_y50.raw /tmp/test_x50.raw /tmp/test_line_x.raw
rm -rf /tmp/bench_out

echo ""
echo "All tests passed!"