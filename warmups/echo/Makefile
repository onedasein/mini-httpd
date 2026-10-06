# echo 热身（T0.3）
#   make debug    开发用：-O0 -g，gdb 友好
#   make release  优化版：-O2
#   make asan     AddressSanitizer + UBSan 体检
#
# 注（接收方修正）：handoff §4 的版本让 debug/release/asan 共用同一个产物路径
# build/debug/echo_server，实测 `make debug && make release` 后 release 是空操作
# （"Nothing to be done"），且 `make clean && make asan` 会把 ASan 二进制写进
# build/debug/。此处改用与仓库根 Makefile 一致的三目录约定，三个目标各自真编。
CC      ?= gcc
SRC     := $(wildcard *.c)
BIN     := echo_server
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wmissing-prototypes -Wstrict-prototypes \
           -Werror=implicit-function-declaration
LDLIBS  :=

.PHONY: all debug release asan clean run

all: debug

debug:
	@mkdir -p build/debug
	$(CC) $(WARN) -g -O0 -o build/debug/$(BIN) $(SRC) $(LDLIBS)

release:
	@mkdir -p build/release
	$(CC) $(WARN) -O2 -DNDEBUG -o build/release/$(BIN) $(SRC) $(LDLIBS)

asan:
	@mkdir -p build/asan
	$(CC) $(WARN) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o build/asan/$(BIN) $(SRC) $(LDLIBS)

run: debug
	./build/debug/$(BIN) 8080

clean:
	rm -rf build
