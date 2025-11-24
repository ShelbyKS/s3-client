# tarantool-s3-client

## Структура файлов:
```
s3-tarantool/
├── CMakeLists.txt

├── include/
│   └── s3/
│       ├── client.h              # публичный API: init, destroy, put_fd, get_fd, options
│       ├── alloc.h               # абстракция аллокатора
│       └── curl_easy_factory.h   # интерфейс фабрики curl easy (для внутреннего использования)

├── src/
│   ├── client.c                  # реализация s3_client_t, init/delete, вызовы backend’ов
│   ├── alloc.c                   # дефолтный аллокатор и интеграция со small
│   ├── s3_internal.h             # внутренние структуры: client, vtable backend'ов

│   ├── http/
│   │   ├── curl_init.c           # curl_global_init / cleanup
│   │   ├── curl_easy_factory.c   # создание easy handles, колбэки, URL, headers
│   │   ├── http_easy.c           # backend на curl_easy
│   │   └── http_multi.c          # backend на curl_multi

└── README.md (опционально)

```

## Мультиплексирование
### curl easy:
вызов s3_client_put_fd в файбере на tx треде (файбер блокируется)-> coio_call -> (на coio треде дальше) -> формируем curl easy -> [ curl_easy_perform ]-> возвращаем управление файберу на tx треде

### curl_multi:
вызов s3_client_put_fd в файбере на tx треде (файбер блокируется)-> coio_call -> (на coio треде дальше) -> формируем curl easy -> [ кладём curl easy в очередь pending (этот coio воркер блокируется на ожидании своего запроса в завершённых) -> поток с curl multi берёт пачками запросы из очереди и выполняет curl_multi_add_handle, curl_multi_perform, складывает завершенные задачи в отдельную очередь -> разблокирует coio воркер, который формировал easy запрос ] -> возвращаем управление файберу на tx треде