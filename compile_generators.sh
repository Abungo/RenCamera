#!/bin/bash
set -e

WORKSPACE_DIR="/home/abungo/projects/personal/RenCamera"
HALIDE_DIR="${WORKSPACE_DIR}/halide_dist"
GENERATOR_DIR="${WORKSPACE_DIR}/halide_generators"
OUTPUT_DIR="${WORKSPACE_DIR}/app/src/main/cpp/halide_generated"

mkdir -p "${OUTPUT_DIR}"

echo "1. Compiling Halide generator binary..."
g++ -std=c++17 \
    -I "${HALIDE_DIR}/include" \
    "${GENERATOR_DIR}/fusion_generator.cpp" \
    -L "${HALIDE_DIR}/lib" \
    -lHalide_GenGen -lHalide \
    -lpthread -ldl \
    -Wl,-rpath,"${HALIDE_DIR}/lib" \
    -o "${GENERATOR_DIR}/fusion_gen"

echo "2. Running generator to produce Android ARM64 library..."
# Generate ARM64 Android AOT target (with NEON and custom vector sizes)
"${GENERATOR_DIR}/fusion_gen" \
    -g temporal_fusion \
    -o "${OUTPUT_DIR}" \
    -e static_library,h \
    -f temporal_fusion \
    target=arm-64-android-armv8a-no_asserts

echo "Halide compilation completed successfully!"
ls -la "${OUTPUT_DIR}"
