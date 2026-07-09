cmake_minimum_required(VERSION 3.22.1)
project("gcam_core")

# ── C++ standard ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler optimisation flags ──────────────────────────────────────────────
add_compile_options(
    -O3
    -ffast-math
    -march=armv8-a+simd
    -funroll-loops
)

# ── Dependencies via FetchContent ─────────────────────────────────────────────
include(FetchContent)
include(ExternalProject)

# ── Google Highway (portable SIMD, header-only) ───────────────────────────────
FetchContent_Declare(
    highway
    GIT_REPOSITORY https://github.com/google/highway.git
    GIT_TAG        1.2.0
    GIT_SHALLOW    TRUE
)
set(HWY_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(highway)

# ── libjpeg-turbo via ExternalProject_Add ────────────────────────────────────
# libjpeg-turbo's CMakeLists.txt explicitly forbids add_subdirectory()
# (which FetchContent_MakeAvailable uses), so we must use ExternalProject_Add.
set(LIBJPEG_TURBO_VERSION "3.0.3")
set(LIBJPEG_TURBO_PREFIX  "${CMAKE_BINARY_DIR}/libjpeg-turbo")
set(LIBJPEG_TURBO_INSTALL "${LIBJPEG_TURBO_PREFIX}/install")

ExternalProject_Add(libjpeg_turbo_ext
    URL           https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/${LIBJPEG_TURBO_VERSION}.tar.gz
    PREFIX        ${LIBJPEG_TURBO_PREFIX}
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
        -DANDROID_NDK=${ANDROID_NDK}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${LIBJPEG_TURBO_INSTALL}
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        -DWITH_JAVA=OFF
        -DWITH_TURBOJPEG=OFF
        -DREQUIRE_SIMD=OFF
    BUILD_BYPRODUCTS
        ${LIBJPEG_TURBO_INSTALL}/lib/libjpeg.a
    INSTALL_DIR   ${LIBJPEG_TURBO_INSTALL}
)

# Create an imported static target so we can link against it normally
add_library(jpeg-static STATIC IMPORTED GLOBAL)
set_target_properties(jpeg-static PROPERTIES
    IMPORTED_LOCATION ${LIBJPEG_TURBO_INSTALL}/lib/libjpeg.a
)
add_dependencies(jpeg-static libjpeg_turbo_ext)

# ── Halide Generated Static Libraries ─────────────────────────────────────────
add_library(temporal_fusion STATIC IMPORTED GLOBAL)
set_target_properties(temporal_fusion PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/halide_generated/temporal_fusion.a
)

add_library(halide_runtime STATIC IMPORTED GLOBAL)
set_target_properties(halide_runtime PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/halide_generated/halide_runtime.a
)

# ── Native shared library ─────────────────────────────────────────────────────
add_library(gcam_core SHARED
    gcam_core.cpp
    pipeline.cpp
    stages/align_stage.cpp
    stages/fusion_stage.cpp
    stages/debayer_stage.cpp
    stages/tonemap_stage.cpp
    stages/encode_stage.cpp
)

target_include_directories(gcam_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/halide_generated
    ${LIBJPEG_TURBO_INSTALL}/include
)

# Make sure libjpeg-turbo is built before our sources try to compile
add_dependencies(gcam_core libjpeg_turbo_ext)

# ── Android system libraries ──────────────────────────────────────────────────
find_library(android-lib   android)
find_library(log-lib       log)
find_library(mediandk-lib  mediandk)
find_library(z-lib         z)
find_library(egl-lib       EGL)
find_library(gles3-lib     GLESv3)

target_link_libraries(gcam_core
    ${android-lib}
    ${log-lib}
    ${mediandk-lib}
    ${z-lib}
    ${egl-lib}
    ${gles3-lib}
    hwy
    jpeg-static   # libjpeg-turbo static lib target name
)