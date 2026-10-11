/*
 * 上限常量 + 版本串
 */
#pragma once
// 各字段上限（用于解析阶段的早期拒绝，避免缓冲膨胀）

// 请求方法 （GET/POST/...）,超出返回501 Not Implemented
#define MAX_METHOD_LEN  16

// 请求目标（path + query），超出返回414 URI Too Long
#define MAX_PATH_LEN    2048

// HTTP版本串("HTTP/1.1"), 超出返回505 HTTP Version Not Supported
#define MAX_VERSION_LEN 16

// 单行（请求行或任意header行），超出返回414 URI Too Long
#define MAX_LINE_LEN    4096

// 所有header行累计最大长度；超出返回431 Request Header Fields Too Large
#define MAX_HEADER_LEN  8192

// header字段数量上限; 超出返回431 Request Header Fields Too Large
#define MAX_HEADERS     64

// Content-Length允许的最大值;超出返回 413 Payload Too Large
#define MAX_BODY        (1024 * 1024)

//-------派生常量--------
#define RBUF_SIZE       (MAX_HEADER_LEN + MAX_LINE_LEN)

// 出现在 Server：响应头里的版本串
#define SERVER_VERSION  "mini-httpd/0.1"
