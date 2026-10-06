/* file.c — 路径安全是这里唯一的重点
 *
 * 双保险（概念图 §8）：① 先逐段拒绝 ".."；② 再 realpath() 把符号链接也解开，
 * 确认结果仍落在 www 根之下。只做①挡不住指向外部的符号链接；只做②不够直观、也挡不住
 * 「先创建一个带 .. 的路径再去打开」这类 TOCTOU 思路。两条都做才稳。
 *
 * 注：realpath 属于 XSI 选项组，只定义 _POSIX_C_SOURCE 时 glibc 不声明它，
 *     所以这里用 _XOPEN_SOURCE 700（它同时包含 POSIX.1-2008）。
 */
#define _XOPEN_SOURCE 700

#include "file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* 逐段判断，而不是 strstr(path, "..")：后者会误杀 a..b 这种合法文件名 */
static int has_dotdot_segment(const char *path)
{
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - seg);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return 1;
    }
    return 0;
}

enum file_status file_open_under_root(const char *root_real, const char *url_path,
                                      struct static_file *out)
{
    if (!url_path || url_path[0] != '/') return FILE_BAD_PATH;
    if (strchr(url_path, '\\'))      return FILE_BAD_PATH;
    if (has_dotdot_segment(url_path)) return FILE_BAD_PATH;

    /* 目录请求补 index.html（第一版明确不做目录列表） */
    size_t plen = strlen(url_path);
    const char *tail = (plen > 0 && url_path[plen - 1] == '/') ? "index.html" : "";

    char full[HTTPD_MAX_PATH];
    int n = snprintf(full, sizeof full, "%s%s%s", root_real, url_path, tail);
    if (n < 0 || (size_t)n >= sizeof full) return FILE_BAD_PATH;

    char real[HTTPD_MAX_PATH];
    errno = 0;
    if (!realpath(full, real)) {
        if (errno == EACCES) return FILE_FORBIDDEN;
        return FILE_NOT_FOUND;                 /* ENOENT / ENOTDIR / 名字太长 → 一律 404 */
    }

    /* ② 符号链接解开之后必须仍在根之下，且边界要么结束要么正好是 '/' */
    size_t rl = strlen(root_real);
    if (strncmp(real, root_real, rl) != 0) return FILE_FORBIDDEN;
    if (real[rl] != '\0' && real[rl] != '/') return FILE_FORBIDDEN;

    int fd = open(real, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES) return FILE_FORBIDDEN;
        if (errno == ENOENT || errno == ENOTDIR) return FILE_NOT_FOUND;
        return FILE_IO_ERROR;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return FILE_IO_ERROR; }
    if (!S_ISREG(st.st_mode)) { close(fd); return FILE_NOT_FOUND; }  /* 目录/设备/管道：不泄漏 */

    out->fd   = fd;
    out->size = (long long)st.st_size;
    memcpy(out->real, real, strlen(real) + 1);
    return FILE_OK;
}
