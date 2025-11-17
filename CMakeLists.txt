cmake_minimum_required(VERSION 3.16)

project(s3_client_tarantool
    VERSION 0.1.0
    LANGUAGES C)

# ---------------------------------------------------------
# Опции сборки
# ---------------------------------------------------------

option(BUILD_TARANTOOL_MODULE "Build Tarantool integration (adapter + Lua module)" ON)
option(BUILD_SHARED_LIBS      "Build shared libraries instead of static" OFF)
option(BUILD_EXAMPLES         "Build example programs" OFF)
option(BUILD_TESTS            "Build tests" OFF)

# ---------------------------------------------------------
# Общие настройки компиляции
# ---------------------------------------------------------

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------
# Зависимость: libcurl
# ---------------------------------------------------------

find_package(CURL REQUIRED)

message(STATUS "Found CURL: ${CURL_VERSION_STRING}")
message(STATUS "CURL include dirs: ${CURL_INCLUDE_DIRS}")
message(STATUS "CURL libs: ${CURL_LIBRARIES}")

# ---------------------------------------------------------
# Общие include-директории
# ---------------------------------------------------------

set(S3CLIENT_PUBLIC_INCLUDE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

include_directories(
    ${S3CLIENT_PUBLIC_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

# ---------------------------------------------------------
# Источники основной loop-agnostic библиотеки s3client
# ---------------------------------------------------------

set(S3CLIENT_CORE_SOURCES
    src/core/client.c
    src/core/request.c
    src/core/error.c
    src/core/signer_v4.c
    src/core/config.c
)

set(S3CLIENT_CURL_SOURCES
    src/curl/curl_easy_s3.c
    src/curl/curl_multi_driver.c
)

set(S3CLIENT_HIGHLEVEL_SOURCES
    src/highlevel/get_buffer.c
    src/highlevel/get_fd.c
    src/highlevel/put_buffer.c
    src/highlevel/put_fd.c
)

add_library(s3client
    ${S3CLIENT_CORE_SOURCES}
    ${S3CLIENT_CURL_SOURCES}
    ${S3CLIENT_HIGHLEVEL_SOURCES}
)

target_include_directories(s3client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/curl
)

target_link_libraries(s3client
    PRIVATE
        ${CURL_LIBRARIES}
)

target_compile_definitions(s3client
    PRIVATE
        # например, CURL_STATICLIB, если понадобится:
        # CURL_STATICLIB
)

set_target_properties(s3client PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "s3client"
)

# ---------------------------------------------------------
# Tarantool integration (адаптер + sync-хелперы + Lua-модуль)
# ---------------------------------------------------------

if (BUILD_TARANTOOL_MODULE)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TARANTOOL REQUIRED tarantool)

    message(STATUS "Found Tarantool: ${TARANTOOL_VERSION}")
    message(STATUS "Tarantool include dirs: ${TARANTOOL_INCLUDE_DIRS}")
    message(STATUS "Tarantool libs: ${TARANTOOL_LIBRARIES}")

    # 1) Библиотека интеграции: адаптер reactor_tarantool + sync_get/put.
    #    Её можно линковать из C-кода под Tarantool (без Lua).
    set(S3CLIENT_TARANTOOL_ADAPTER_SOURCES
        src/adapters/reactor_tarantool.c
        src/tarantool/sync_get.c
        src/tarantool/sync_put.c
    )

    add_library(s3client_tarantool STATIC
        ${S3CLIENT_TARANTOOL_ADAPTER_SOURCES}
    )

    target_include_directories(s3client_tarantool
        PRIVATE
            ${S3CLIENT_PUBLIC_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include/s3-adapters
            ${TARANTOOL_INCLUDE_DIRS}
    )

    target_link_libraries(s3client_tarantool
        PRIVATE
            s3client
            ${TARANTOOL_LIBRARIES}
    )

    set_target_properties(s3client_tarantool PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME "s3client_tarantool"
    )

    # 2) Lua-модуль s3.so (опциональный), тонкая обёртка над s3client_tarantool.
    add_library(s3_tarantool SHARED
        src/tarantool/module.c
    )

    target_link_libraries(s3_tarantool
        PRIVATE
            s3client_tarantool
            ${TARANTOOL_LIBRARIES}
    )

    target_include_directories(s3_tarantool
        PRIVATE
            ${S3CLIENT_PUBLIC_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include/s3-adapters
            ${TARANTOOL_INCLUDE_DIRS}
    )

    set_target_properties(s3_tarantool PROPERTIES
        OUTPUT_NAME "s3"  # s3.so
        PREFIX ""
    )

endif()

# ---------------------------------------------------------
# Примеры
# ---------------------------------------------------------

# if (BUILD_EXAMPLES)
#     add_executable(example_simple_get_put
#         examples/simple_get_put.c
#     )
# 
#     target_link_libraries(example_simple_get_put
#         PRIVATE
#             s3client
#     )
# 
#     target_include_directories(example_simple_get_put
#         PRIVATE
#             ${S3CLIENT_PUBLIC_INCLUDE_DIR}
#     )
# endif()

# ---------------------------------------------------------
# Тесты (каркас)
# ---------------------------------------------------------

if (BUILD_TESTS)
    enable_testing()

    add_executable(test_client_basic
        tests/unit/test_client_basic.c
    )

    target_link_libraries(test_client_basic
        PRIVATE
            s3client
    )

    add_test(NAME test_client_basic
             COMMAND test_client_basic)
endif()

# ---------------------------------------------------------
# Установка
# ---------------------------------------------------------

install(TARGETS s3client
        EXPORT s3clientTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

if (BUILD_TARANTOOL_MODULE)
    install(TARGETS s3client_tarantool
            ARCHIVE DESTINATION lib
            LIBRARY DESTINATION lib
            RUNTIME DESTINATION lib)

    install(TARGETS s3_tarantool
            LIBRARY DESTINATION lib
            RUNTIME DESTINATION lib)
endif()

# core-хедеры
install(DIRECTORY include/s3
        DESTINATION include)

# адаптеры (reactor_tarantool.h и т.п.)
install(DIRECTORY include/s3-adapters
        DESTINATION include)

# Tarantool sync API
install(FILES include/s3-tarantool.h
        DESTINATION include)

# (опционально) экспорт для find_package:
# install(EXPORT s3clientTargets
#         FILE s3clientTargets.cmake
#         NAMESPACE s3::
#         DESTINATION lib/cmake/s3client)
