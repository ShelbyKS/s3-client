cmake_minimum_required(VERSION 3.15)

project(tnt_s3_client C)

option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_TNT_MODULE  "Build Tarantool module" ON)
option(BUILD_TESTS       "Build tests" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Пути к заголовкам
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# -------- libcurl --------
find_package(CURL REQUIRED)

# -------- Tarantool (опционально) --------
if (BUILD_TNT_MODULE)
    set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

    find_package(PkgConfig REQUIRED)
    find_package(Tarantool REQUIRED)

    if (TARANTOOL_FOUND)
        message(STATUS "Found Tarantool via pkg-config")
        set(TARANTOOL_INCLUDE_DIR ${TARANTOOL_INCLUDE_DIRS})
        set(TARANTOOL_LIBRARIES   ${TARANTOOL_LIBRARIES})
    else()
        message(STATUS "Tarantool not found via pkg-config. "
                    "You can set TARANTOOL_INCLUDE_DIR and TARANTOOL_LIBRARIES manually.")
    endif()
endif()

# -------- Основная библиотека s3_client --------
set(S3_CLIENT_SOURCES
    src/s3_env.c
    src/s3_client.c
    src/s3_http_backend.c
    src/s3_curl_easy.c
    src/s3_signer.c
    src/s3_error.c
)

add_library(s3_client ${S3_CLIENT_SOURCES})

target_include_directories(s3_client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(s3_client
    PUBLIC
        CURL::libcurl
)

# Если нужны дефайны (например, версия)
target_compile_definitions(s3_client
    PUBLIC S3_CLIENT_VERSION="0.1.0"
)

# -------- Tarantool module (Lua-модуль) --------
if (BUILD_TNT_MODULE)
  if (NOT TARANTOOL_INCLUDE_DIR)
    message(FATAL_ERROR "Tarantool include dir not found. "
                        "Set TARANTOOL_INCLUDE_DIR or disable BUILD_TNT_MODULE.")
  endif()

  add_library(tnt_s3 STATIC
      src/tnt_s3_module.c
      src/tnt_s3_adapter.c
  )

  target_include_directories(tnt_s3
      PRIVATE
          ${TARANTOOL_INCLUDE_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/include
  )

  target_link_libraries(tnt_s3
      PRIVATE
          s3_client
          ${TARANTOOL_LIBRARIES}
  )

  # Tarantool ожидает модуль без префикса "lib"
  set_target_properties(tnt_s3 PROPERTIES
      PREFIX ""          # чтобы получилось s3.so, а не libs3.so
      OUTPUT_NAME "s3"   # имя модуля require('s3')
  )
endif()

# -------- tests (опционально) --------
if (BUILD_TESTS)
  enable_testing()
  add_executable(test_s3_client tests/test_s3_client.c)
  target_link_libraries(test_s3_client PRIVATE s3_client)
  add_test(NAME s3_client_tests COMMAND test_s3_client)
endif()

# -------- examples (опционально) --------
# add_executable(example_simple_get_put examples/simple_get_put.c)
# target_link_libraries(example_simple_get_put PRIVATE s3_client)
