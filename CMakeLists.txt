cmake_minimum_required(VERSION 3.10)
project(tarantool-s3c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

#find_package(Tarantool REQUIRED)
find_package(PkgConfig REQUIRED)

set(CURL_INCLUDE_DIR "/opt/homebrew/opt/curl/include")
set(CURL_LIBRARY "/opt/homebrew/opt/curl/lib/libcurl.dylib")

include_directories(${CURL_INCLUDE_DIR})

# Явно указываем, где лежит Tarantool
set(TARANTOOL_INCLUDE_DIRS /usr/local/include)
set(TARANTOOL_LIBRARY_DIRS /usr/local/lib)
set(TARANTOOL_LIBRARIES tarantool)

include_directories(
	/usr/local/include/tarantool
    ${TARANTOOL_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
link_directories(
    ${TARANTOOL_LIBRARY_DIRS}
    ${CURL_LIBRARY_DIRS}
)

set(SRCS
    src/s3c.c
    src/lua/s3c.c
)

add_library(s3c SHARED ${SRCS})
set_target_properties(s3c PROPERTIES
    PREFIX ""
    OUTPUT_NAME "s3c"
	LINK_FLAGS "-undefined dynamic_lookup"
)

target_compile_options(s3c PRIVATE -fPIC -Wall)
target_link_libraries(s3c PRIVATE
    ${CURL_LIBRARY}
    pthread
)

install(TARGETS s3c DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/tarantool)
install(FILES src/lua/s3c.lua DESTINATION ${CMAKE_INSTALL_PREFIX}/share/tarantool)
