#include <s3/config.h>
#include <string.h>

void
s3_client_config_init_default(s3_client_config_t *cfg)
{
    if (cfg == NULL)
        return;

    /* Обнуляем все поля. */
    (void)memset(cfg, 0, sizeof(*cfg));

    /* Разумные дефолты. Можно потом подкрутить под реальные требования. */

    cfg->endpoint_is_https = 1;      /* по умолчанию HTTPS */

    cfg->connect_timeout_ms = 5000;  /* 5 секунд на connect */
    cfg->request_timeout_ms = 0;     /* 0 => без общего таймаута */
    cfg->idle_timeout_ms    = 0;     /* 0 => либо без idle-таймаута, либо дефолт внутри */

    cfg->max_connections = 16;       /* небольшой разумный пул */

    cfg->verify_peer = 1;            /* проверяем сертификат */
    cfg->verify_host = 1;            /* проверяем имя хоста */
    cfg->ca_path     = NULL;         /* libcurl возьмёт системные значения */
    cfg->ca_file     = NULL;

    /* reactor и log_fn должны быть заполнены пользователем. */
    cfg->reactor.vtable     = NULL;
    cfg->reactor.reactor_ud = NULL;

    cfg->log_fn        = NULL;
    cfg->log_user_data = NULL;

    cfg->_reserved_flags = 0;
    cfg->_reserved_ptr   = NULL;
}
