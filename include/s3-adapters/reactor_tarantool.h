#ifndef S3_REACTOR_TARANTOOL_H
#define S3_REACTOR_TARANTOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <s3/client.h>
#include <s3/reactor.h>

/* forward-declare из libev/Tarantool */
struct ev_loop;

/*
 * Инициализация s3_reactor_t поверх ev_loop Tarantool'а.
 *
 *  - r    — объект реактора;
 *  - loop — указатель на ev_loop, который крутит Tarantool
 *           (обычно ev_default_loop(0) в контексте этого процесса).
 *
 * Возвращает 0 при успехе, иначе <0.
 *
 * Это НИЗКОУРОВНЕВАЯ функция: её будут использовать либо
 * сам Tarantool-модуль (module.c), либо твой C-код-под-Tarantool,
 * если ты хочешь вручную собрать s3_client_t.
 */
int
s3_reactor_init_tarantool(s3_reactor_t *r, struct ev_loop *loop);

int
s3_client_config_init_tarantool(s3_client_config_t *cfg,
                                struct ev_loop     *loop);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S3_REACTOR_TARANTOOL_H */
