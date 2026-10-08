# M1 · 阻塞式 HTTP 静态服务器（v1）—— 故意不优雅，当 M2 的对照组
#   make debug / release / asan   三个目标各自产出独立二进制
#   make run                      编译并在 127.0.0.1:8080 起服务
#   make test                     编 debug 版 + 跑 tests/run.sh 的验收套件
#   make test-asan                编 ASan 版 + 跑同一套（发布前必做）
#
# 源文件计划（M1）：main.c（listener + accept 串行循环）、http.c/http.h（增量解析 + 响应拼装）、
#                   httpd.h（常量与上限）、file.c（静态文件 + 路径校验）
# 约束：不要从 ../v2/ 或 ../../echo/ 收集源文件 —— v1 必须能被单独读成「阻塞版的正确实现」。
CC      ?= gcc
BIN     := httpd_v1
SRC     := $(wildcard *.c)
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wmissing-prototypes -Wstrict-prototypes \
           -Werror=implicit-function-declaration
LDLIBS  :=

.PHONY: all debug release asan clean run guard test test-asan

all: debug

# 源文件还没落地时给一句人话，而不是让链接器报 undefined reference to `main'
guard:
	@if [ -z "$(SRC)" ]; then \
	  echo "v1 还没有源文件：先写 main.c / http.c / http.h / httpd.h / file.c（见 ../README.md）"; \
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

# 程序收 <ip> <port> <www根目录>，所以这里必须给全（只写 8080 会被当成 ip）
ARGS    ?= 127.0.0.1 8080 www

run: debug
	./build/debug/$(BIN) $(ARGS)

# tests/run.sh 收二进制路径作为 $1，所以同一套断言能在 debug / asan 上双跑
test: debug
	bash ./tests/run.sh

test-asan: asan
	bash ./tests/run.sh ./build/asan/$(BIN)

clean:
	rm -rf build
