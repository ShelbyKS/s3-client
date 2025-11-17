# s3-client

## Сборка

1. Стандартная сборка (только ядро)
mkdir build && cd build
cmake -DBUILD_TARANTOOL_MODULE=OFF ..
cmake --build . --parallel

2. Полная сборка (ядро + tarantool adapter + Lua-модуль)
mkdir build && cd build
cmake -DBUILD_TARANTOOL_MODULE=ON ..
cmake --build . --parallel

Получается:

libs3client.a
libs3client_tarantool.a
s3.so