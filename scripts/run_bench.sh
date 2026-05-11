#!/bin/bash

# ERWT3D Benchmark Script

set -e

# Default parameters
INPUT_FILE=""
OUTPUT_DIR="bench_results"
RANDOM_COUNT=100
CONTINUOUS_COUNT=10
THREADS=16
MEMORY_LIMIT_MB=2048
CACHE_MB=0
SEED=20260511

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --input)
            INPUT_FILE="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --random-count)
            RANDOM_COUNT="$2"
            shift 2
            ;;
        --continuous-count)
            CONTINUOUS_COUNT="$2"
            shift 2
            ;;
        --threads)
            THREADS="$2"
            shift 2
            ;;
        --memory-limit-mb)
            MEMORY_LIMIT_MB="$2"
            shift 2
            ;;
        --cache-mb)
            CACHE_MB="$2"
            shift 2
            ;;
        --seed)
            SEED="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Check required arguments
if [ -z "$INPUT_FILE" ]; then
    echo "Error: --input is required"
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Run benchmark
echo "Running ERWT3D benchmark..."
echo "Input: $INPUT_FILE"
echo "Output directory: $OUTPUT_DIR"
echo "Random slices: $RANDOM_COUNT per axis"
echo "Continuous slices: $CONTINUOUS_COUNT per axis"
echo "Threads: $THREADS"
echo "Memory limit: ${MEMORY_LIMIT_MB} MB"
echo "Cache: ${CACHE_MB} MB"
echo "Seed: $SEED"
echo ""

./build/tools/erwt3d_bench \
    --input "$INPUT_FILE" \
    --output-dir "$OUTPUT_DIR" \
    --random-count "$RANDOM_COUNT" \
    --continuous-count "$CONTINUOUS_COUNT" \
    --threads "$THREADS" \
    --memory-limit-mb "$MEMORY_LIMIT_MB" \
    --cache-mb "$CACHE_MB" \
    --seed "$SEED"

echo ""
echo "Benchmark completed. Results in $OUTPUT_DIR/"