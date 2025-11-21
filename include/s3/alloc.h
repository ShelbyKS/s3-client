#ifndef TARANTOOL_S3_ALLOC_H_INCLUDED
#define TARANTOOL_S3_ALLOC_H_INCLUDED 1

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h> /* size_t */

/*
 * Абстракция аллокатора.
 *
 * Все функции обязаны вести себя аналогично malloc/free/realloc:
 * - malloc(ctx, n) -> возвращает выделенный блок или NULL
 * - free(ctx, ptr) -> аналог free(NULL) должен быть безопасен
 * - realloc(ctx, ptr, n) -> ведёт себя как обычный realloc
 *
 * ctx — произвольный пользовательский указатель.
 */
typedef struct s3_allocator {
    void  *ctx;

    void *(*malloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t size);
    void  (*free)(void *ctx, void *ptr);
} s3_allocator_t;


/*
 * Возвращает аллокатор по умолчанию (malloc/free/realloc).
 * Можно использовать без инициализации:
 *
 *     const s3_allocator_t *a = s3_allocator_default();
 *
 * Он хранится как глобальный статический singleton (read-only).
 */
const s3_allocator_t *
s3_allocator_default(void);


/*
 * Простые вспомогательные функции.
 * Они используют allocator->malloc/free/realloc.
 */

static inline void *
s3_alloc(const s3_allocator_t *a, size_t size)
{
    return a->malloc(a->ctx, size);
}

static inline void *
s3_realloc(const s3_allocator_t *a, void *ptr, size_t size)
{
    return a->realloc(a->ctx, ptr, size);
}

static inline void
s3_free(const s3_allocator_t *a, void *ptr)
{
    a->free(a->ctx, ptr);
}


/*
 * --- Интеграция с Tarantool small --- 
 *
 * Мы не включаем здесь <small/small.h>, чтобы не заставлять
 * клиентов подключать Tarantool в публичный заголовок.
 *
 * Реализация будет расположена в alloc.c,
 * и принимать "void *small" как непрозрачный указатель.
 *
 * Пользователь пишет:
 *      struct small_alloc small;
 *      small_alloc_create(&small, ...);
 *      s3_allocator_t a;
 *      s3_allocator_init_small(&a, &small);
 *
 * Внутри alloc.c мы привязываемся к small_alloc API.
 */

/*
 * Инициализация аллокатора, работающего поверх Tarantool small_alloc.
 *
 * small_ctx — это (struct small_alloc *).
 */
void
s3_allocator_init_small(s3_allocator_t *a, void *small_ctx);


#ifdef __cplusplus
}
#endif

#endif /* TARANTOOL_S3_ALLOC_H_INCLUDED */
