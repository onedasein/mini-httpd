#include "http.h"

/* TODO(T1.2c)：按 CONTRACTS.md §2 的契约实现。
 * 现在只占位，别让它污染零告警规则（-Wunused-parameter）。 */
http_err http_request_parse(const char *rbuf, size_t len, http_request *req) {
    (void)rbuf;
    (void)len;
    (void)req;
    return HTTP_OK;
}
