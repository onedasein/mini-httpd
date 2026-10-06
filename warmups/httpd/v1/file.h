/* file.h — URL 路径 → www 根下的真实文件（含路径穿越防护） */
#ifndef MINI_HTTPD_FILE_H
#define MINI_HTTPD_FILE_H

#include "httpd.h"

enum file_status {
    FILE_OK = 0,
    FILE_BAD_PATH,     /* 400：路径本身非法（.. 段、反斜杠、非绝对路径） */
    FILE_FORBIDDEN,    /* 403：realpath 之后逃出了 www 根（符号链接 / EACCES） */
    FILE_NOT_FOUND,    /* 404 */
    FILE_IO_ERROR      /* 500 */
};

struct static_file {
    int fd;
    long long size;
    char real[HTTPD_MAX_PATH];   /* realpath 之后的结果，Content-Type 靠它的扩展名 */
};

/* root_real 必须是启动时算好的 www 根 realpath。
 * 成功时 out->fd 已经打开，调用方负责 close。 */
enum file_status file_open_under_root(const char *root_real, const char *url_path,
                                      struct static_file *out);

#endif
