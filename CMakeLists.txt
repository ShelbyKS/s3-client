cmake_minimum_required(VERSION 3.12)
project(s3c C)

# Allow local Find*.cmake modules (you have FindJansson.cmake and FindTarantool.cmake)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------
find_package(Tarantool REQUIRED)
find_package(Jansson REQUIRED)

# -----------------------------------------------------------------------------
# Sanitizers
# -----------------------------------------------------------------------------
set(S3C_SANITIZER_FLAGS "")
option(S3C_ASAN_ENABLED "Enable AddressSanitizer" OFF)
if(S3C_ASAN_ENABLED)
    string(JOIN " " S3C_SANITIZER_FLAGS "-fno-omit-frame-pointer"
                              "-fsanitize=address"
                              "-fsanitize-recover=address")
endif()

# -----------------------------------------------------------------------------
# User provided extra flags
# -----------------------------------------------------------------------------
set(S3C_EXTRA_COMPILER_FLAGS "" CACHE STRING "Extra flags to pass to C/C++ compilers")

# Core compiler flags (sensible defaults oriented to safety and diagnostics)
set(S3C_COMPILER_FLAGS " \
-ggdb3 -O2 -pipe -D_FORTIFY_SOURCE=2 \
-fstack-protector-strong -grecord-gcc-switches -fstack-clash-protection \
-Wall -Wextra -Werror=unused-result -Werror=format-security -Werror=switch-unreachable \
-Wshadow -Werror=implicit-fallthrough ${S3C_EXTRA_COMPILER_FLAGS}")

option(DIAGNOSTIC_COLOR "Force use of -fdiagnostic-color=always" OFF)
if (DIAGNOSTIC_COLOR)
    string(APPEND S3C_COMPILER_FLAGS " -fdiagnostics-color=always")
endif()

# Prevent CMake from forcing a C++ standard flag on us
set(CMAKE_CXX_STANDARD_DEFAULT "")

# Apply flags
set(CMAKE_C_FLAGS "-fPIC -std=gnu11 -Werror=implicit-function-declaration -Werror=jump-misses-init ${CMAKE_C_FLAGS} ${S3C_COMPILER_FLAGS} ${S3C_SANITIZER_FLAGS}")
set(CMAKE_CXX_FLAGS "-std=gnu++17 -fexceptions ${S3C_COMPILER_FLAGS} ${S3C_SANITIZER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${S3C_SANITIZER_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${S3C_SANITIZER_FLAGS}")

# -----------------------------------------------------------------------------
# Project layout variables
# -----------------------------------------------------------------------------
set(S3C_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(S3C_LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs)
set(S3C_LIBS_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/libs)

# -----------------------------------------------------------------------------
# Build bundled 'small' allocator from libs/small if present
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/small/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/small)
    set(SMALL_LIBRARIES ${S3C_LIBS_BIN_DIR}/small/libsmall.a)
    set(SMALL_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/libs/small/include ${CMAKE_CURRENT_SOURCE_DIR}/libs/small/third_party)

    # Exclude small's tests
    set_target_properties(
        slab_cache.test region.test ibuf.test obuf.test rlist.test rb.test
        rb_aug.test rb_rand.test mempool.test small_class.test small_class_branchless.test
        small_granularity.test lf_lifo.test slab_arena.test arena_mt.test matras.test
        lsregion.test quota.test quota_lessor.test static.test
        PROPERTIES EXCLUDE_FROM_ALL 1 EXCLUDE_FROM_DEFAULT_BUILD 1
    )
else()
    set(SMALL_LIBRARIES "")
    set(SMALL_INCLUDE_DIRS "")
endif()

# -----------------------------------------------------------------------------
# Sources and target
# -----------------------------------------------------------------------------
file(GLOB S3C_SOURCES "${S3C_SOURCE_DIR}/*.c")
file(GLOB S3C_HEADERS "${S3C_SOURCE_DIR}/*.h")

add_library(s3c SHARED ${S3C_SOURCES})

option(S3C_HLP_ALLOC_STAT "Enable allocators stat scraping (may cause perf decrease)" ON)
if (S3C_HLP_ALLOC_STAT)
    target_compile_definitions(s3c PRIVATE S3C_HLP_ALLOC_STAT=1)
endif()

# Link required libraries
target_link_libraries(s3c PRIVATE ${SMALL_LIBRARIES} Jansson::Jansson ${TARANTOOL_LIBRARIES})

# Include directories
target_include_directories(s3c PRIVATE ${S3C_SOURCE_DIR} ${SMALL_INCLUDE_DIRS} ${TARANTOOL_INCLUDE_DIRS})

# Compiler definitions and options
target_compile_options(s3c PRIVATE -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE=1)
set_target_properties(s3c PROPERTIES PREFIX "" OUTPUT_NAME s3c C_STANDARD 11 CXX_STANDARD 17)
set_property(TARGET s3c PROPERTY POSITION_INDEPENDENT_CODE ON)

# Ensure small is built before s3c when we use the bundled one
if(TARGET small)
    add_dependencies(s3c small)
endif()

# -----------------------------------------------------------------------------
# Installation rules
# -----------------------------------------------------------------------------
install(TARGETS s3c LIBRARY DESTINATION ${TARANTOOL_INSTALL_LIBDIR}/${PROJECT_NAME}/)
install(FILES ${S3C_HEADERS} DESTINATION ${TARANTOOL_INSTALL_INCLUDEDIR}/${PROJECT_NAME}/)
