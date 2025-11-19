#include "tnt_s3_adapter.h"

/* worker, выполняющийся на coio-треде */
static ssize_t
tnt_s3_put_worker(va_list ap)
{
    s3_client_t *client = va_arg(ap, s3_client_t *);
    const char  *bucket = va_arg(ap, const char *);
    const char  *key    = va_arg(ap, const char *);
    int          src_fd = va_arg(ap, int);
    const struct s3_put_params *params = va_arg(ap, const struct s3_put_params *);
    struct s3_error_info *err          = va_arg(ap, struct s3_error_info *);

    int rc = s3_client_put_fd(client, bucket, key, src_fd, params, err);
    return rc; /* вернётся в coio_call как ssize_t */
}

int
tnt_s3_put_fd_coio(s3_client_t *client,
                   const char *bucket,
                   const char *key,
                   int src_fd,
                   const struct s3_put_params *params,
                   struct s3_error_info *err)
{
    if (err)
        s3_error_reset(err);

    /* coio_call:
     *   - вызывает tnt_s3_put_worker на worker-треде,
     *   - текущий файбер на tx-треде ждёт результата.
     */
    ssize_t rc = coio_call(tnt_s3_put_worker,
                           client, bucket, key, src_fd, params, err);
    if (rc < 0) {
        /* сама coio-задача не смогла стартовать, errno говорит почему */
        if (err) {
            s3_error_reset(err);
            err->code = S3_ECANCELLED;
            snprintf(err->msg, sizeof(err->msg),
                     "coio_call failed, errno=%d", errno);
        }
        return S3_ECANCELLED;
    }

    /* tnt_s3_put_worker возвращает rc = s3_client_put_fd(...) */
    return (int)rc;
}
