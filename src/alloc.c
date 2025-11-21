/* src/alloc.c */

#include "s3/alloc.h"

#include <stdlib.h>   /* malloc, free, realloc */
#include <string.h>   /* memcpy */

#include <small/small.h> /* struct small_alloc, smalloc, smfree */

/* ----------------- default allocator (malloc/free/realloc) ----------------- */

static void *
s3_default_malloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void *
s3_default_realloc(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    return realloc(ptr, size);
}

static void
s3_default_free(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

static const s3_allocator_t s3_default_allocator = {
    .ctx    = NULL,
    .malloc = s3_default_malloc,
    .realloc = s3_default_realloc,
    .free   = s3_default_free,
};

const s3_allocator_t *
s3_allocator_default(void)
{
    return &s3_default_allocator;
}

/* ----------------- small allocator adapter ----------------- */

/*
 * Для small_alloc у нас есть API:
 *
 *   void *smalloc(struct small_alloc *alloc, size_t size);
 *   void  smfree (struct small_alloc *alloc, void *ptr, size_t size);
 *
 * Small не хранит размер блока, поэтому smfree требует size.
 * Чтобы сделать generic malloc/free/realloc, мы будем прятать
 * размер перед возвращаемым пользователю указателем.
 *
 * layout:
 *
 *   [ size_t stored_size ][ user bytes ... ]
 *   ^                     ^
 *   raw ptr from smalloc  user ptr (возвращается вызывающему)
 */

struct s3_small_hdr {
    size_t size;
};

static void *
s3_small_malloc(void *ctx, size_t size)
{
    struct small_alloc *sa = (struct small_alloc *)ctx;
    size_t total = sizeof(struct s3_small_hdr) + size;

    void *raw = smalloc(sa, total);
    if (raw == NULL)
        return NULL;

    struct s3_small_hdr *hdr = (struct s3_small_hdr *)raw;
    hdr->size = total;

    return (void *)(hdr + 1);
}

static void
s3_small_free(void *ctx, void *ptr)
{
    if (ptr == NULL)
        return;

    struct small_alloc *sa = (struct small_alloc *)ctx;

    struct s3_small_hdr *hdr = ((struct s3_small_hdr *)ptr) - 1;
    size_t total = hdr->size;

    smfree(sa, (void *)hdr, total);
}

static void *
s3_small_realloc(void *ctx, void *ptr, size_t size)
{
    struct small_alloc *sa = (struct small_alloc *)ctx;

    if (ptr == NULL) {
        /* Поведение как у обычного realloc(NULL, size). */
        return s3_small_malloc(ctx, size);
    }

    if (size == 0) {
        /* Поведение как у realloc(ptr, 0) — освобождаем память. */
        s3_small_free(ctx, ptr);
        return NULL;
    }

    /* Старый блок. */
    struct s3_small_hdr *old_hdr = ((struct s3_small_hdr *)ptr) - 1;
    size_t old_total = old_hdr->size;
    size_t old_user_size;

    if (old_total >= sizeof(struct s3_small_hdr))
        old_user_size = old_total - sizeof(struct s3_small_hdr);
    else
        old_user_size = 0; /* на всякий случай, не должен случиться */

    /* Новый блок. */
    void *new_ptr = s3_small_malloc(ctx, size);
    if (new_ptr == NULL)
        return NULL;

    size_t copy_size = old_user_size < size ? old_user_size : size;
    if (copy_size > 0)
        memcpy(new_ptr, ptr, copy_size);

    /* Освобождаем старый. */
    smfree(sa, (void *)old_hdr, old_total);

    return new_ptr;
}

void
s3_allocator_init_small(s3_allocator_t *a, void *small_ctx)
{
    /*
     * small_ctx ожидается как (struct small_alloc *),
     * но мы специально не тянем этот тип в публичный заголовок.
     */
    a->ctx     = small_ctx;
    a->malloc  = s3_small_malloc;
    a->realloc = s3_small_realloc;
    a->free    = s3_small_free;
}
