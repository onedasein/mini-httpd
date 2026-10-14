#define _GNU_SOURCE

#include "file.h"
#include <stdlib.h>
#include <limits.h>  // PATH_MAX
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
/* 逐段检查，必须以'/'开头、不含'\\', 不含恰好是".."的段
 * 避免误杀a..b / ...
 */
static int path_is_safe(const char* p) {
    if (p == NULL || p[0] != '/') {
        return 0;
    }
    size_t i = 0;
    while (p[i] != '\0') {
        if (p[i] == '\\') {
            return 0;
        }

        size_t seg_start = i;
        while (p[i] != '\0' && p[i] != '/') {
            i++;
        }
        size_t seg_len = i - seg_start;

        if (seg_len == 2 && p[seg_start] == '.' && p[seg_start + 1] == '.') {
            return 0;
        }

        if (p[i] == '/') {
            i++;
        }
    }
    return 1;
}

/* root_real的末尾可能带'/', 或者就是'/', 做前缀比较时统一剥掉*/

static size_t strip_trailing_slashes(const char* root) {
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    return n;
}

file_status file_open_under_root(const char* root_real, const char* url_path, static_file* out) {
    if (root_real == NULL || root_real[0] != '/') return FILE_BAD_PATH;
    if (out == NULL) return FILE_IO_ERROR;

    out->fd = -1;
    out->size = 0;
    out->real[0] = '\0';

    // 路径字符串层面不合法
    if (root_real == NULL || !path_is_safe(url_path)) return FILE_BAD_PATH;
    size_t root_len = strip_trailing_slashes(root_real);
    size_t url_len = strlen(url_path);

    // 目录请求补 index.html（CONTRACTS.md §1 的 200 行）。
    // 只对"以 '/' 结尾"的路径补；补完还是不存在的目录照样 404 —— 这**不是**目录列表，
    // 「不做目录列表」的语义仍然成立（见 e2e 的 /nolist/ 用例）。
    const char *tail = (url_len > 0 && url_path[url_len - 1] == '/') ? "index.html" : "";
    size_t tail_len = strlen(tail);

    // root_real + url_path + tail + NUL
    char joined[FILE_MAX_PATH];
    if (root_len + url_len + tail_len + 1 > sizeof(joined)) return FILE_BAD_PATH;  // ★ 先检查再拷贝
    memcpy(joined, root_real, root_len);
    memcpy(joined + root_len, url_path, url_len);
    memcpy(joined + root_len + url_len, tail, tail_len);
    joined[root_len + url_len + tail_len] = '\0';

    // realpath解开./ .. 和符号链接
    char resolved[FILE_MAX_PATH];
    if (realpath(joined, resolved) == NULL) {
        switch (errno) {
            case ENOENT:
            case ENOTDIR:
                return FILE_NOT_FOUND;
            case EACCES:
                return FILE_FORBIDDEN;
            case ENAMETOOLONG:
                return FILE_BAD_PATH;
            default:
                return FILE_IO_ERROR;
        }
    }
    // 前缀 + 边界
    if (root_len == 1 && root_real[0] == '/') {
        // 文档根就是"/", resolved一定在它之下
    } else {
        if (strncmp(resolved, root_real, root_len) != 0) return FILE_FORBIDDEN;
        char boundary = resolved[root_len];
        if (boundary != '\0' && boundary != '/') {
            return FILE_FORBIDDEN;
        }
    }

    // 打开, 使用解析后的绝对路径，已经不含符号链接和".."
    int fd = open(resolved, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        switch (errno) {
            case ENOENT:
            case ENOTDIR:
                return FILE_NOT_FOUND;
            case EACCES:
                return FILE_FORBIDDEN;
            case ENAMETOOLONG:
                return FILE_BAD_PATH;
            default:
                return FILE_IO_ERROR;
        }
    }

    // 从此处开始，任何失败都要close(fd)
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return FILE_IO_ERROR;
    }

    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return FILE_NOT_FOUND;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return FILE_FORBIDDEN;
    }

    size_t rlen = strlen(resolved);
    if (rlen + 1 > FILE_MAX_PATH) {
        close(fd);
        return FILE_IO_ERROR;
    }
    memcpy(out->real, resolved, rlen + 1);

    out->fd = fd;
    out->size = (long long)st.st_size;
    return FILE_OK;
}
