# tarantool-s3-client

## Структура файлов:
```
tnt-s3-client/
├── CMakeLists.txt
├── include/
│   ├── s3_client.h          # Публичное API для C/Tarantool
│   ├── s3_error.h           # enum s3_error, s3_error_info
│   ├── s3_types.h           # базовые типы/опции, если разрастётся
│   └── s3_config.h          # опционально: дефайны версии, фичи
├── src/
│   ├── s3_env.c             # init/destroy глобальной среды (libcurl init и т.п.)
│   ├── s3_client.c          # реализация публичного API (get_fd/put_fd + coio_call)
│   ├── s3_http_backend.c    # создание backend (easy/multi), vtable
│   ├── s3_curl_easy.c       # реализация backend на curl_easy_perform
│   ├── s3_signer.c          # AWS SigV4 / заголовки (можно заглушку сначала)
│   ├── s3_error.c           # человекочитаемые ошибки, маппинг кодов
│   └── tnt_s3_module.c      # glue-код для Tarantool (module.h, lua API)
├── cmake/
│   └── FindTarantool.cmake  # поиск tarantool через pkg-config или руками
├── tests/
│   └── test_s3_client.c     # unit-тесты на "сырую" библиотеку (без Tarantool)
└── examples/
    └── simple_get_put.c     # пример использования C-API
```

## Мультиплексирование
### curl easy:
вызов s3_client_put_fd в файбере на tx треде (файбер блокируется)-> coio_call -> (на coio треде дальше) -> формируем curl easy -> [ curl_easy_perform ]-> возвращаем управление файберу на tx треде

### curl_multi:
вызов s3_client_put_fd в файбере на tx треде (файбер блокируется)-> coio_call -> (на coio треде дальше) -> формируем curl easy -> [ кладём curl easy в очередь pending (этот coio воркер блокируется на ожидании своего запроса в завершённых) -> поток с curl multi берёт пачками запросы из очереди и выполняет curl_multi_add_handle, curl_multi_perform, складывает завершенные задачи в отдельную очередь -> разблокирует coio воркер, который формировал easy запрос ] -> возвращаем управление файберу на tx треде