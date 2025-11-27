cmake_minimum_required(VERSION 3.10)

if (NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

project(tarantool_s3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --------------------------------------------------------------------
# Опция: использовать curl из Tarantool вместо системного libcurl
# --------------------------------------------------------------------
option(S3_USE_TARANTOOL_CURL "Use Tarantool's embedded libcurl" ON)

# --------------------------------------------------------------------
# Пути к модульным файлам CMake (FindTarantool.cmake и т.п.)
# --------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --------------------------------------------------------------------
# Внешние зависимости
# --------------------------------------------------------------------

# pthread
find_package(Threads REQUIRED)

# Tarantool
find_package(Tarantool REQUIRED)
message(STATUS "Using Tarantool includes: ${TARANTOOL_INCLUDE_DIRS}")
message(STATUS "Using Tarantool libs: ${TARANTOOL_LIBRARIES}")

# Системный libcurl нужен только если мы НЕ используем tarantool/curl.h
if (NOT S3_USE_TARANTOOL_CURL)
    find_package(CURL REQUIRED)
    message(STATUS "Found CURL: ${CURL_LIBRARIES}")
    message(STATUS "CURL include dirs: ${CURL_INCLUDE_DIRS}")
endif()

# --------------------------------------------------------------------
# Small — bundled
# --------------------------------------------------------------------
include(FindPackageMessage)

if (NOT THIRD_PARTY_SRC_DIR)
    set(THIRD_PARTY_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
endif()

if (NOT THIRD_PARTY_BIN_DIR)
    set(THIRD_PARTY_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/third_party")
endif()

set(SMALL_EMBEDDED ON)
add_subdirectory(${THIRD_PARTY_SRC_DIR}/small
                 ${THIRD_PARTY_BIN_DIR}/small
                 EXCLUDE_FROM_ALL)

set(SMALL_LIBRARIES small)

set(SMALL_INCLUDE_DIRS
    ${THIRD_PARTY_SRC_DIR}/small/include
    ${THIRD_PARTY_SRC_DIR}/small/third_party
)

find_package_message(Small "Using bundled Small"
    "target: ${SMALL_LIBRARIES};includes: ${SMALL_INCLUDE_DIRS}"
)

message(STATUS "Using Small libs: ${SMALL_LIBRARIES}")
message(STATUS "Using Small include dirs: ${SMALL_INCLUDE_DIRS}")

# --------------------------------------------------------------------
# Общие include’ы
# --------------------------------------------------------------------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SMALL_INCLUDE_DIRS}
)

# --------------------------------------------------------------------
# Библиотека s3_client (ядро)
# --------------------------------------------------------------------
add_library(s3_client STATIC
    src/client.c
    src/alloc.c
    src/s3_internal.h          # только для IDE. TODO: убрать
    src/http/curl_easy_factory.c
    src/http/http_easy.c
    src/http/http_multi.c
    src/http/curl_init.c
    src/http/parser.c
    src/http/http_util.c
)

# В tarantool-режиме не линкуемся ни с каким libcurl — символы подтянет /usr/bin/tarantool
target_link_libraries(s3_client
    PUBLIC
        Threads::Threads
    PRIVATE
        ${SMALL_LIBRARIES}
        $<$<NOT:$<BOOL:${S3_USE_TARANTOOL_CURL}>>:${CURL_LIBRARIES}>
)

target_include_directories(s3_client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SMALL_INCLUDE_DIRS}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${TARANTOOL_INCLUDE_DIRS}
        $<$<NOT:$<BOOL:${S3_USE_TARANTOOL_CURL}>>:${CURL_INCLUDE_DIRS}>
)

# Говорим коду, что нужно использовать <tarantool/curl.h> через curl_compat.h
target_compile_definitions(s3_client
    PRIVATE
        $<$<BOOL:${S3_USE_TARANTOOL_CURL}>:S3_USE_TARANTOOL_CURL>
)

# --------------------------------------------------------------------
# Tarantool Lua module: s3.so
# --------------------------------------------------------------------
add_library(s3 MODULE
    src/tarantool_s3.c
)

target_link_libraries(s3
    PRIVATE
        s3_client
        Threads::Threads
        ${TARANTOOL_LIBRARIES}
        ${SMALL_LIBRARIES}
        $<$<NOT:$<BOOL:${S3_USE_TARANTOOL_CURL}>>:${CURL_LIBRARIES}>
)

target_include_directories(s3
    PRIVATE
        ${TARANTOOL_INCLUDE_DIRS}
        ${SMALL_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        $<$<NOT:$<BOOL:${S3_USE_TARANTOOL_CURL}>>:${CURL_INCLUDE_DIRS}>
)

set_target_properties(s3 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "s3"
)

if (UNIX AND NOT APPLE)
    set_target_properties(s3 PROPERTIES
        SUFFIX ".so"
    )
endif()

if (APPLE)
    set_target_properties(s3 PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
endif()
