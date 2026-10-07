# echo 热身（T0.3）
#   make debug    开发用：-O0 -g，gdb 友好          → build/debug/server
#   make release  优化版：-O2                       → build/release/server
#   make asan     AddressSanitizer + UBSan 体检     → build/asan/server
#   make run      编（增量）+ 跑，默认 127.0.0.1 8080
#   make run ARGS="127.0.0.1 9090"      换参数
#   make gdb      直接进 gdb
#
# 注（接收方修正）：handoff §4 的版本让 debug/release/asan 共用同一个产物路径
# build/debug/echo_server，实测 `make debug && make release` 后 release 是空操作
# （"Nothing to be done"），且 `make clean && make asan` 会把 ASan 二进制写进
# build/debug/。此处改用与仓库根 Makefile 一致的三目录约定，三个目标各自真编。
CC      ?= gcc
SRC     := $(wildcard *.c)
BIN     := server
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wmissing-prototypes -Wstrict-prototypes \
           -Werror=implicit-function-declaration
LDLIBS  :=

ARGS    ?= 127.0.0.1 8080               # make run ARGS="127.0.0.1 9090"
DBG     := build/debug/$(BIN)

.PHONY: all debug release asan clean run gdb run-asan test

all: debug

# 文件目标：源文件没变就不重编（所以 make run 才是"零成本启动"）
debug: $(DBG)
$(DBG): $(SRC) Makefile
	@mkdir -p $(@D)                     # $(@D) = build/debug
	$(CC) $(WARN) -g -O0 -o $@ $(SRC) $(LDLIBS)

release: build/release/$(BIN)
build/release/$(BIN): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -O2 -DNDEBUG -o $@ $(SRC) $(LDLIBS)

asan: build/asan/$(BIN)
build/asan/$(BIN): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o $@ $(SRC) $(LDLIBS)

# 程序收 <ip> <port>，所以 ARGS 必须给全两个（只写 8080 会被当成 ip）
run: debug
	./$(DBG) $(ARGS)

gdb: debug
	gdb -q --args ./$(DBG) $(ARGS)

run-asan: asan
	./build/asan/$(BIN) $(ARGS)
test:
	bash ./tests/run.sh
clean:
	rm -rf build
