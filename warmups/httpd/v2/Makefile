# M2 · epoll 边沿触发（ET）非阻塞 reactor（v2）—— 与 v1 同功能，换事件循环
#   make debug / release / asan   三个目标各自产出独立二进制
#   make run                      编译并在 127.0.0.1:8080 起服务
#
# 源文件计划（M2）：conn.c/conn.h（struct conn + 缓冲区 roff/rlen、woff/wlen）、
#                   epoll.c（epoll_create1/ctl/wait 事件循环）、http.c（复用 v1 的解析思路）、file.c
# 铁律（TASKS.md §4）：ET 下 read/accept 必须循环到 EAGAIN；EPOLLOUT 只在写不完时注册、写完立刻摘；
#                      close 前先 epoll_ctl(DEL)；keep-alive 用 memmove 搬残余字节。
CC      ?= gcc
BIN     := httpd_v2
SRC     := $(wildcard *.c)
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wmissing-prototypes -Wstrict-prototypes \
           -Werror=implicit-function-declaration
LDLIBS  :=

.PHONY: all debug release asan clean run guard

all: debug

guard:
	@if [ -z "$(SRC)" ]; then \
	  echo "v2 还没有源文件：先写 conn.c / conn.h / epoll.c / http.c / http.h / file.c（见 ../README.md）"; \
	  exit 1; \
	fi

debug: guard
	@mkdir -p build/debug
	$(CC) $(WARN) -O0 -g3 -DDEBUG -o build/debug/$(BIN) $(SRC) $(LDLIBS)

release: guard
	@mkdir -p build/release
	$(CC) $(WARN) -O2 -DNDEBUG -o build/release/$(BIN) $(SRC) $(LDLIBS)

asan: guard
	@mkdir -p build/asan
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o build/asan/$(BIN) $(SRC) $(LDLIBS)

run: debug
	./build/debug/$(BIN) 8080

clean:
	rm -rf build
