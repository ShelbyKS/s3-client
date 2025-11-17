### Слои архтектуры библиотеки:

## 0. ОС + сеть + libcurl

Основная идея:
Сетевой I/O мы делаем через libcurl multi interface (CURLM).

libcurl умеет:
- говорить: “вот этот сокет слушай на READ/WRITE”;
- говорить: “мне нужен таймер через N мс”;
- “крутиться” через curl_multi_socket_action(...).

Наша задача: не крутить свой event loop, а встроиться в чужой (Tarantool, libev, что угодно) → для этого есть слой reactor.

## 1. Reactor: glue к event loop
Файлы:

- `include/s3/reactor.h`
- `src/reactor/reactor_interface.c` (позже — helper’ы)
- `src/reactor/reactor_tarantool.c` (конкретная реализация для Tarantool/libev)

В `reactor.h` мы описали:

```
typedef struct s3_reactor_vtable {
    s3_reactor_io_handle_t (*io_subscribe)(...);
    void (*io_update)(...);
    void (*io_unsubscribe)(...);

    s3_reactor_timer_handle_t (*timer_start)(...);
    void (*timer_cancel)(...);
} s3_reactor_vtable_t;

typedef struct s3_reactor {
    const s3_reactor_vtable_t *vtable;
    void                       *reactor_ud; // контекст event loop'а
} s3_reactor_t;
```

- libcurl говорит: “подпишись на сокет fd на READ/WRITE”.
- Наш curl-слой (см. далее `curl_multi_driver.c`) дергает `reactor->vtable->io_subscribe(...)`.
- Конкретная реализация (например, Tarantool) в `reactor_tarantool.c` внутри делает: 
	- `ev_io_init(...)` / `ev_io_start(...)` / `ev_io_stop(...)`;
	- `ev_timer_init(...)` / `ev_timer_start(...)` / `ev_timer_stop(...)`.

Когда в event loop’е готов fd или таймер — reactor вызывает наши callbacks:
- `s3_reactor_io_cb(events, cb_data)`
- `s3_reactor_timer_cb(cb_data)`

А внутри этих cb мы уже делаем `curl_multi_socket_action` и смотрим, какие запросы завершились.

## 2. Core: s3_client и s3_request
Файлы:
- `include/s3/client.h` (API)
- `include/s3/config.h`
- `include/s3/types.h`
- `src/core/client.c`
- `src/core/request.c`
- `src/core/error.c`
- `src/core/config.c`
- `src/core/signer_v4.c`

Содержит:
```
struct s3_client {
    s3_client_config_t cfg;   // endpoint, creds, reactor, таймауты ...
    CURLM             *multi; // multi handle libcurl
    // список активных запросов, мапы fd → handle, и т.п.
};

struct s3_request {
    s3_client_t        *client;
    CURL               *easy;       // easy handle libcurl
    s3_request_done_cb  done_cb;
    void               *user_data;
    // ещё: тип (GET/PUT), callbacks data/read/headers, состояние...
};
```

### Задача core-слоя
- Хранит конфиг (`s3_client_config_t`) + reactor + CURLM.
- Умеет:
	- `s3_client_new(cfg)` — инициализировать всё:
		- `curl_global_init` (если нужно),
		- `curl_multi_init`,
		- сохранить reactor.
	- `s3_object_get(...)` / `s3_object_put(...)`:
		- создать `s3_request`,
		- через `curl_easy_s3.c` сделать `CURL *easy` и натравить его на нужный `URL`;
		- добавить `easy` в `multi`.
	- Обрабатывать завершения:
		- обработать `curl_multi_info_read`,
		- найти наш `s3_request` по `CURL *easy`,
		- собрать `s3_error_t`,
		- вызвать `done_cb(req, &err, user_data)`,
		- удалить `easy` и `s3_request`.

- **Core ничего не знает про Tarantool** и даже про highlevel-обёртки: он работает только с callback’ами и reactor’ом.

## 3. Curl glue: связываем libcurl ⟷ reactor
Файлы:
- `src/curl/curl_easy_s3.c`
- `src/curl/curl_multi_driver.c`

### Задачи `curl_easy_s3.c`:
- создать CURL *easy под конкретный запрос;
- настроить:
	- URL: (прим. https://endpoint/bucket/key)
	- TLS
	- метод
	- заголовки
	- callbacks:
		- `CURLOPT_WRITEFUNCTION` / `WRITEDATA` → вызывают `s3_object_get_data_cb`;
		- `CURLOPT_HEADERFUNCTION` → `s3_object_get_headers_cb`;
		- `CURLOPT_READFUNCTION` → `s3_object_put_read_cb`;
	- CURLOPT_AWS_SIGV4.

### `curl_multi_driver.c`

Здесь мы:

1. В `s3_client_new`:
	- делаем `client->multi = curl_multi_init()`;
	- ставим:
		```
		curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, socket_cb);
		curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, client);

		curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, timer_cb);
		curl_multi_setopt(multi, CURLMOPT_TIMERDATA, client);
		```
1. Реализуем `socket_cb`:
	
	Когда libcurl говорит:

	“этот сокет fd слушай на READ/WRITE/REMOVE”

	мы переводим это в вызовы:
	- `reactor->io_subscribe(reactor_ud, fd, events, s3_io_cb, data)`
	- `reactor->io_update(...)`
	- `reactor->io_unsubscribe(...)`

1. Реализуем `timer_cb`:

	Когда libcurl говорит:

	“нужен таймер через timeout_ms”

	→ вызываем `reactor->timer_start(...)` / `reactor->timer_cancel(...)`.

1. Реализуем `s3_reactor_io_cb` и `s3_reactor_timer_cb`:

	Когда event loop говорит: “fd готов” / “таймер сработал”:

	- делаем curl_multi_socket_action(client->multi, fd, ...)
	- потом в цикле:
		```
		CURLMsg *msg;
		while ((msg = curl_multi_info_read(multi, &msgs_in_queue)) != NULL) {
			if (msg->msg == CURLMSG_DONE) {
				// находим s3_request для msg->easy_handle
				// формируем s3_error_t
				// вызываем done_cb
				// чистим запрос
			}
		}
		```

**То есть curl_multi_driver.c — это мост**:

reactor (fd + timer) ↔ libcurl multi ↔ core (`s3_request`)

## 4. Highlevel: буферы и файловые дескрипторы

Файлы:
- `src/highlevel/get_buffer.c`
- `src/highlevel/get_fd.c`
- `src/highlevel/put_buffer.c`
- `src/highlevel/put_fd.c`

Это слой “человеческого” API, который уже поверх low-level `s3_object_get/put`.

Важно: highlevel ничего не знает про Tarantool, ни про reactor — он просто пользуется:
- `s3_object_get`
- `s3_object_put`
- callbacks из `types.h`

## 5. Tarantool слой: Lua + sync API
Файлы:

- `include/s3-tarantool.h` — узкий C-API модуля.
- `src/tarantool/module.c` — точка входа модуля, регистрация Lua-функций.
- `src/tarantool/sync_get.c`
- `src/tarantool/sync_put.c`

### Что делает этот слой:

При загрузке модуля Tarantool:
1. Создаёт/инициализирует один `s3_client_t` с нужным конфигом:
	- `endpoint`,
	- `region`,
	- `credentials`,
	- `reactor` = tarantool/libev.

1. Регистрирует Lua-функции:
	- `s3.get_to_file(bucket, key, path)`
	- `s3.put_from_file(bucket, key, path)`
	- ...

1. Реализует их через **синхронные** обёртки, которые:
	- открывают/создают файл;
	- запускают highlevel-запрос:
		- `s3_object_get_to_fd(...)` или `s3_object_put_from_fd(...)`
	- **ждут** завершения через `fiber_yield()`:
		- в `done_cb` → `fiber_wakeup(fiber)` с результатом;
	- закрывают файл;
	- возвращают `(true, nil)` или `(nil, err)` в Lua.

1. Даёт C-API для других модулей (через `s3-tarantool.h`):
	```
	s3_tarantool_sync_api_t {
		get_to_fd(...);
		put_from_fd(...);
		...
	};
	```

То есть **тарнтул-слой — просто “обёртка” над highlevel API**, + использует fiber/coio, чтобы не блокировать event loop.


# Взаимодействие слоёв друг с другом

1. ### “Пользовательские колбэки” (S3 API):

get_data_cb, put_read_cb, done_cb, get_headers_cb
→ бизнес-логика: “куда писать байты, откуда читать байты, что делать по завершении”.

1. ### “Колбэки libcurl ↔ reactor” (I/O уровень):

s3_curl_multi_socket_cb, s3_curl_multi_timer_cb
(libcurl говорит нам, за чем следить)

s3_curl_reactor_io_cb, s3_curl_reactor_timer_cb
(reactor говорит, что fd готов / таймер сработал → мы крутим curl_multi_socket_action)

→ это чисто про сокеты и таймеры, без понимания S3.

1. ### “Shim easy-колбэки” (libcurl easy ↔ S3-callbacks):

s3_curl_write_cb ↔ get_data_cb

s3_curl_read_cb ↔ put_read_cb

s3_curl_header_cb ↔ get_headers_cb

→ это мост между байтами HTTP-body и пользовательскими колбэками.