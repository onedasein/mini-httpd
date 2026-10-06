/* httpd.h — v1（M1 阻塞式 HTTP）的全局上限与常量
 *
 * 这些数字不是拍脑袋：每一条都对应 TASKS.md T1.3 的「上限防护」和概念图 §8 的安全表。
 * 原则：**先有上限，再有解析**。没有上限的解析器 = 等着被一个 20000 字节的 header 打穿。
 */
#ifndef MINI_HTTPD_HTTPD_H
#define MINI_HTTPD_HTTPD_H

/* 单行上限（请求行 + 每个 header 字段行）→ 超了 431（T1.3） */
#define HTTPD_MAX_LINE      (8 * 1024)
/* header 总量上限（请求行 + 全部字段 + 结尾空行）→ 超了 431（T1.3） */
#define HTTPD_MAX_HEADER    (64 * 1024)
/* request-target 上限 → 超了 414（T1.3） */
#define HTTPD_MAX_URI       (2 * 1024)
/* header 字段个数上限 → 超了 431 */
#define HTTPD_MAX_HEADERS   64
/* 读缓冲 = 总量上限 + 一个超长行：保证「先攒到上限再判错」的过程中永不越界 */
#define HTTPD_RBUF_SIZE     (HTTPD_MAX_HEADER + HTTPD_MAX_LINE)
/* 路径拼接与 realpath 的缓冲（glibc 的 PATH_MAX 也是 4096） */
#define HTTPD_MAX_PATH      (4 * 1024)
/* 第一版只支持 GET/HEAD，请求体声明得比这个大就 413 */
#define HTTPD_MAX_BODY      (1 * 1024 * 1024)
/* v1 故意不设空闲超时：慢客户端必须能拖死整台服务器 —— 那是 M2 的对照组（T2.2） */
#define HTTPD_IDLE_MS       (-1)
#define HTTPD_SERVER_NAME   "mini-httpd/0.1 (v1 blocking)"

#endif
