cmake_minimum_required(VERSION 3.10)

# Просим CMake быть совместимым минимум с 3.5 (чтобы старые проекты, как small, не падали)
if (NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

project(tarantool_s3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Для модулей/шаредов
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --------------------------------------------------------------------
# Пути к модульным файлам CMake (FindTarantool.cmake и т.п.)
# --------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --------------------------------------------------------------------
# Внешние зависимости
# --------------------------------------------------------------------

# libcurl
find_package(CURL REQUIRED)
message(STATUS "Found CURL: ${CURL_LIBRARIES}")

# pthread
find_package(Threads REQUIRED)

# Tarantool (твоя FindTarantool.cmake)
find_package(Tarantool REQUIRED)
message(STATUS "Using Tarantool includes: ${TARANTOOL_INCLUDE_DIRS}")
message(STATUS "Using Tarantool libs: ${TARANTOOL_LIBRARIES}")

# --------------------------------------------------------------------
# Small — bundled
# Ожидаем, что THIRD_PARTY_SRC_DIR и THIRD_PARTY_BIN_DIR заданы
# на уровне верхнего CMakeLists или через -D.
# Например:
#   -DTHIRD_PARTY_SRC_DIR=${CMAKE_SOURCE_DIR}/third_party
#   -DTHIRD_PARTY_BIN_DIR=${CMAKE_BINARY_DIR}/third_party
# --------------------------------------------------------------------

include(FindPackageMessage)  # для find_package_message

if (NOT THIRD_PARTY_SRC_DIR)
    set(THIRD_PARTY_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
endif()

if (NOT THIRD_PARTY_BIN_DIR)
    set(THIRD_PARTY_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/third_party")
endif()

# Build bundled Small, но не включаем её тесты/перф в default target "all"
set(SMALL_EMBEDDED ON)
add_subdirectory(${THIRD_PARTY_SRC_DIR}/small
                 ${THIRD_PARTY_BIN_DIR}/small
                 EXCLUDE_FROM_ALL)

# В CMake small, как правило, объявляет таргет `small` (STATIC).
# Линкуемся на таргет, а не на путь к .a.
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
    src/s3_internal.h          # для IDE, не обязателен как компилируемый
    src/http/curl_easy_factory.c
    src/http/http_easy.c
    src/http/http_multi.c
    src/http/curl_init.c
)

target_link_libraries(s3_client
    PUBLIC
        CURL::libcurl
        Threads::Threads
    PRIVATE
        ${SMALL_LIBRARIES}
)

target_include_directories(s3_client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SMALL_INCLUDE_DIRS}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
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
        CURL::libcurl
        Threads::Threads
        ${TARANTOOL_LIBRARIES}
        ${SMALL_LIBRARIES}
)

target_include_directories(s3
    PRIVATE
        ${TARANTOOL_INCLUDE_DIRS}
        ${SMALL_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Tarantool ожидает модуль без префикса "lib"
set_target_properties(s3 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "s3"
)

# Для Linux/Unix: модуль без лишних версий
if (UNIX AND NOT APPLE)
    set_target_properties(s3 PROPERTIES
        SUFFIX ".so"
    )
endif()

# Под macOS позволяем неразрешённые символы (Lua, Tarantool API),
# они подтянутся из самого процесса Tarantool при require('s3').
if (APPLE)
    set_target_properties(s3 PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
endif()
