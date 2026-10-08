# echo 热身（T0.3）— 最小 TCP echo server
#
#   产品二进制
#   make debug / release / asan     三个目标各自产出独立二进制
#   make run                        编（增量）+ 跑，默认 127.0.0.1 8080
#   make run ARGS="127.0.0.1 9090"  换参数
#   make gdb / make run-asan        调试用
#
#   测试分两层（分层理由见 README「测试分层」）
#   make test       单元测试：进程内直接调用 parse/dump 的函数，毫秒级
#   make e2e        端到端回归：起进程、走网络，跑 tests/e2e/run.sh
#   make check      test + e2e（提交前 / CI 用）
#   每个目标都有 -asan 变体：test-asan / e2e-asan，发布前双跑
CC      ?= gcc
BIN     := server
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wmissing-prototypes -Wstrict-prototypes \
           -Werror=implicit-function-declaration
LDLIBS  :=

ARGS    ?= 127.0.0.1 8080               # make run ARGS="127.0.0.1 9090"

SRC_DIR  := src
SRC      := $(wildcard $(SRC_DIR)/*.c)
# 被测模块 = 除 main 之外的所有源文件。
# 这个 filter-out 是「单元测试能直接调用」的机械前提：
# 库里再有一个 main，链接测试程序时就会 duplicate symbol。
LIB_SRC  := $(filter-out $(SRC_DIR)/main.c, $(SRC))

DBG      := build/debug/$(BIN)
ASAN_BIN := build/asan/$(BIN)

# 第三方测试框架（vendored，MIT）：Unity，版本见 tests/unit/unity/.VERSION
UNITY_DIR   := tests/unit/unity
UNITY_DEBUG := build/debug/unity.o
UNITY_ASAN  := build/asan/unity.o

# 自动发现：新增 tests/unit/test_xxx.c 就自动多一个测试可执行，不用改这个文件
TEST_SRC      := $(wildcard tests/unit/test_*.c)
TEST_BIN      := $(TEST_SRC:tests/unit/%.c=build/debug/%)
TEST_ASAN_BIN := $(TEST_SRC:tests/unit/%.c=build/asan/%)

.PHONY: all debug release asan clean run gdb run-asan \
        test test-asan e2e e2e-asan check

all: debug

# ---------- 产品二进制 ----------
debug: $(DBG)
$(DBG): $(SRC) Makefile
	@mkdir -p $(@D)                     # $(@D) = build/debug
	$(CC) $(WARN) -I$(SRC_DIR) -g -O0 -o $@ $(SRC) $(LDLIBS)

release: build/release/$(BIN)
build/release/$(BIN): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -I$(SRC_DIR) -O2 -DNDEBUG -o $@ $(SRC) $(LDLIBS)

asan: $(ASAN_BIN)
$(ASAN_BIN): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -I$(SRC_DIR) -g -O1 -fsanitize=address,undefined \
	      -fno-omit-frame-pointer -o $@ $(SRC) $(LDLIBS)

# ---------- 第三方源码单独编：不套自己的严格告警 ----------
# Unity 在 -Wconversion -Wshadow 下会刷屏，所以用 -w
$(UNITY_DEBUG): $(UNITY_DIR)/unity.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -g -O0 -w -I$(UNITY_DIR) -c -o $@ $<

$(UNITY_ASAN): $(UNITY_DIR)/unity.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -w -I$(UNITY_DIR) -c -o $@ $<

# ---------- 单元测试：一个测试文件 → 一个可执行 ----------
# -Wno-missing-prototypes：别对每个 setUp/tearDown 都告警
build/debug/test_%: tests/unit/test_%.c $(LIB_SRC) $(UNITY_DEBUG) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) \
	      -g -O0 -o $@ $< $(LIB_SRC) $(UNITY_DEBUG) $(LDLIBS)

build/asan/test_%: tests/unit/test_%.c $(LIB_SRC) $(UNITY_ASAN) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) \
	      -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o $@ $< $(LIB_SRC) $(UNITY_ASAN) $(LDLIBS)

# 空套件报「全部通过」是最危险的假绿，所以先挡住
test: $(TEST_BIN)
	@if [ -z "$(TEST_BIN)" ]; then \
	  echo "没找到任何单元测试（tests/unit/test_*.c）"; exit 1; \
	fi; \
	rc=0; for t in $(TEST_BIN); do \
	  printf '\n===== %s =====\n' "$$t"; \
	  "$$t" || rc=1; \
	done; \
	if [ $$rc -eq 0 ]; then printf '\n单元测试：全部通过\n'; \
	else printf '\n单元测试：有失败\n'; fi; \
	exit $$rc

test-asan: $(TEST_ASAN_BIN)
	@if [ -z "$(TEST_ASAN_BIN)" ]; then \
	  echo "没找到任何单元测试（tests/unit/test_*.c）"; exit 1; \
	fi; \
	rc=0; for t in $(TEST_ASAN_BIN); do \
	  printf '\n===== %s =====\n' "$$t"; \
	  "$$t" || rc=1; \
	done; \
	if [ $$rc -eq 0 ]; then printf '\n单元测试(ASan)：全部通过\n'; \
	else printf '\n单元测试(ASan)：有失败\n'; fi; \
	exit $$rc

# ---------- 端到端：起进程、走网络，脚本负责调度与比对 ----------
# tests/e2e/run.sh 收二进制路径作为 $1，所以同一套断言能在 debug / asan 上双跑
e2e: debug
	bash ./tests/e2e/run.sh ./$(DBG)

e2e-asan: asan
	bash ./tests/e2e/run.sh ./$(ASAN_BIN)

check: test e2e

# ---------- 手动跑 ----------
run: debug
	./$(DBG) $(ARGS)
gdb: debug
	gdb -q --args ./$(DBG) $(ARGS)
run-asan: asan
	./$(ASAN_BIN) $(ARGS)

clean:
	rm -rf build
