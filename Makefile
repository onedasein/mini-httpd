# mini-httpd 根构建（M1-T1.0b 建立，替代 2026-10-09 被置空的那份）
#
#   make debug / release / asan   三个目标各自产出独立二进制：build/<profile>/mini-httpd
#   make run                      ARGS 默认 "127.0.0.1 8080 tests/www"
#   make test   / test-asan       单元测试：tests/unit/test_*.c，进程内直接调用被测函数（毫秒级）
#   make e2e    / e2e-asan        端到端：tests/e2e/run.sh 起进程、走网络（秒级）
#   make check                    test + e2e（提交前用）
#   make clean
#
# 三条硬约定（都是踩过才知道的）：
#   1) 被测逻辑必须从 main 拆出来，编成**不含 main** 的 $(LIB_SRC) —— 库里有 main 就 duplicate symbol；
#   2) 空套件不许报「全部通过」：没找到 test_*.c、或 e2e 脚本还没写时，目标必须**失败退出**；
#   3) 第三方源码（Unity）单独用 -w 编译，别套本仓库的严格告警；测试文件加 -Wno-missing-prototypes。
CC      ?= gcc
BIN     := mini-httpd
SRC_DIR := src

SRC     := $(wildcard $(SRC_DIR)/*.c)
LIB_SRC := $(filter-out $(SRC_DIR)/main.c, $(SRC))

WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wmissing-prototypes -Wstrict-prototypes \
           -Werror=implicit-function-declaration
LDLIBS  :=

ARGS    ?= 127.0.0.1 8080 tests/www
TMO     ?= 30

DBG      := build/debug/$(BIN)
REL      := build/release/$(BIN)
ASAN_BIN := build/asan/$(BIN)

# 第三方测试框架（vendored，MIT）：Unity —— 版本记在 tests/unit/unity/.VERSION（v2.7.0）
UNITY_DIR   := tests/unit/unity
UNITY_DEBUG := build/debug/unity.o
UNITY_ASAN  := build/asan/unity.o

# 自动发现：新增 tests/unit/test_xxx.c 就自动多一个测试可执行，不用改这个文件
TEST_SRC      := $(wildcard tests/unit/test_*.c)
TEST_BIN      := $(TEST_SRC:tests/unit/%.c=build/debug/%)
TEST_ASAN_BIN := $(TEST_SRC:tests/unit/%.c=build/asan/%)

E2E := tests/e2e/run.sh

.PHONY: all guard debug release asan clean run gdb run-asan \
        test test-asan e2e e2e-asan check

all: debug

# ---------- 源文件还没落地时给一句人话，而不是让链接器报 undefined reference to `main' ----------
guard:
	@if [ -z "$(SRC)" ]; then \
	  echo "src/ 还是空的：先按 TASKS.md 的 T1.2a 定接口、T1.2e 写 listener"; exit 1; \
	fi

# ---------- 产品二进制 ----------
debug: guard $(DBG)
$(DBG): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -I$(SRC_DIR) -g -O0 -DDEBUG -o $@ $(SRC) $(LDLIBS)

release: guard $(REL)
$(REL): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -I$(SRC_DIR) -O2 -DNDEBUG -o $@ $(SRC) $(LDLIBS)

asan: guard $(ASAN_BIN)
$(ASAN_BIN): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -I$(SRC_DIR) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o $@ $(SRC) $(LDLIBS)

# ---------- Unity 单独编：不套本仓库的严格告警 ----------
$(UNITY_DEBUG): $(UNITY_DIR)/unity.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -g -O0 -w -I$(UNITY_DIR) -c -o $@ $<

$(UNITY_ASAN): $(UNITY_DIR)/unity.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -w -I$(UNITY_DIR) -c -o $@ $<

# ---------- 单元测试：一个测试文件 → 一个可执行 ----------
build/debug/test_%: tests/unit/test_%.c $(LIB_SRC) $(UNITY_DEBUG) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) \
	      -g -O0 -o $@ $< $(LIB_SRC) $(UNITY_DEBUG) $(LDLIBS)

build/asan/test_%: tests/unit/test_%.c $(LIB_SRC) $(UNITY_ASAN) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) \
	      -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o $@ $< $(LIB_SRC) $(UNITY_ASAN) $(LDLIBS)

# 每条测试套 timeout：服务器相关用例的失败模式可能是「阻塞不返回」，不设上限会把 make 挂住
test: $(TEST_BIN)
	@if [ -z "$(TEST_BIN)" ]; then \
	  echo "没找到任何单元测试（tests/unit/test_*.c）—— 空套件不许报「全部通过」"; exit 1; \
	fi; \
	rc=0; for t in $(TEST_BIN); do \
	  printf '\n===== %s =====\n' "$$t"; \
	  timeout $(TMO) "$$t"; st=$$?; \
	  if [ $$st -eq 124 ]; then echo "!! $$t 超时（$(TMO)s）"; rc=1; \
	  elif [ $$st -ne 0 ]; then rc=1; fi; \
	done; \
	if [ $$rc -eq 0 ]; then printf '\n单元测试：全部通过\n'; else printf '\n单元测试：有失败\n'; fi; \
	exit $$rc

test-asan: $(TEST_ASAN_BIN)
	@if [ -z "$(TEST_ASAN_BIN)" ]; then \
	  echo "没找到任何单元测试（tests/unit/test_*.c）—— 空套件不许报「全部通过」"; exit 1; \
	fi; \
	rc=0; for t in $(TEST_ASAN_BIN); do \
	  printf '\n===== %s (ASan+UBSan) =====\n' "$$t"; \
	  ASAN_OPTIONS=detect_leaks=1 timeout $(TMO) "$$t"; st=$$?; \
	  if [ $$st -eq 124 ]; then echo "!! $$t 超时（$(TMO)s）"; rc=1; \
	  elif [ $$st -ne 0 ]; then rc=1; fi; \
	done; \
	if [ $$rc -eq 0 ]; then printf '\n单元测试(ASan)：全部通过\n'; else printf '\n单元测试(ASan)：有失败\n'; fi; \
	exit $$rc

# ---------- 端到端：脚本收二进制路径作为 $1，同一套断言能在 debug / asan 上双跑 ----------
e2e: debug
	@if [ ! -f "$(E2E)" ]; then \
	  echo "$(E2E) 还没写（见 TASKS.md M1-T1.7a）—— 端到端没就绪时不许报通过"; exit 1; \
	fi
	bash $(E2E) ./$(DBG)

e2e-asan: asan
	@if [ ! -f "$(E2E)" ]; then \
	  echo "$(E2E) 还没写（见 TASKS.md M1-T1.7a）—— 端到端没就绪时不许报通过"; exit 1; \
	fi
	ASAN_OPTIONS=detect_leaks=1 bash $(E2E) ./$(ASAN_BIN)

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
