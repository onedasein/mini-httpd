# cv 热身（T0.5 条件变量版）— 生产者-消费者队列：mutex + condvar + while + close
#   make debug / release / asan     三个目标各自产出独立二进制
#   make run ARGS="4 4 20000 1024"  编（增量）+ 跑
#   make test       单元测试（毫秒级）
#   make e2e        端到端：起进程跑并发形状，跑 tests/e2e/run.sh（与信号量版同一套用例）
#   make tsan       TSan 版单元测试（走 setarch -R，本机 TSan 会概率性 FATAL）
#   make check      test + e2e + tsan（提交前用）
#   make mutation-check  变异验证：故意做坏的变体必须「红」，否则说明用例无效
#   make test-asan / e2e-asan        发布前双跑
CC      ?= gcc
BIN     := pc
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wmissing-prototypes -Wstrict-prototypes \
           -Werror=implicit-function-declaration
THREAD  := -pthread
LDLIBS  := -pthread

ARGS    ?= 4 4 20000 1024               # make run ARGS="1 8 50000 1"

SRC_DIR  := src
SRC      := $(wildcard $(SRC_DIR)/*.c)
# 被测模块 = 除 main 之外的所有源文件（库里再有一个 main 就 duplicate symbol）
LIB_SRC  := $(filter-out $(SRC_DIR)/main.c, $(SRC))

DBG      := build/debug/$(BIN)
ASAN_BIN := build/asan/$(BIN)

# 与信号量版共享同一份 vendored Unity（避免第三份副本 + 版本漂移）
UNITY_DIR   := ../tests/unit/unity
UNITY_DEBUG := build/debug/unity.o
UNITY_ASAN  := build/asan/unity.o
UNITY_TSAN  := build/tsan/unity.o

TEST_SRC      := $(wildcard tests/unit/test_*.c)
TEST_BIN      := $(TEST_SRC:tests/unit/%.c=build/debug/%)
TEST_ASAN_BIN := $(TEST_SRC:tests/unit/%.c=build/asan/%)
TEST_TSAN_BIN := $(TEST_SRC:tests/unit/%.c=build/tsan/%)

# WSL2 上 TSan 概率性 FATAL: unexpected memory mapping（ASLR 熵过高，实测 6 次挂 4 次）
ARCH    := $(shell uname -m)
SETARCH := $(shell command -v setarch >/dev/null 2>&1 && echo "setarch $(ARCH) -R")
# 单条测试的墙钟上限：本模块的失败模式是「死锁」而不是断言失败
TMO     ?= 20

.PHONY: all debug release asan clean run gdb run-asan \
        test test-asan tsan e2e e2e-asan check mutation-check

all: debug

# ---------- 产品二进制 ----------
debug: $(DBG)
$(DBG): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -I$(SRC_DIR) $(THREAD) -g -O0 -o $@ $(SRC) $(LDLIBS)

release: build/release/$(BIN)
build/release/$(BIN): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -I$(SRC_DIR) $(THREAD) -O2 -DNDEBUG -o $@ $(SRC) $(LDLIBS)

asan: $(ASAN_BIN)
$(ASAN_BIN): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -I$(SRC_DIR) $(THREAD) -g -O1 -fsanitize=address,undefined \
	      -fno-omit-frame-pointer -o $@ $(SRC) $(LDLIBS)

# ---------- 第三方源码单独编：不套自己的严格告警 ----------
$(UNITY_DEBUG): $(UNITY_DIR)/unity.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -g -O0 -w -I$(UNITY_DIR) -c -o $@ $<

$(UNITY_ASAN): $(UNITY_DIR)/unity.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -w -I$(UNITY_DIR) -c -o $@ $<

$(UNITY_TSAN): $(UNITY_DIR)/unity.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
	      -w -I$(UNITY_DIR) -c -o $@ $<

# ---------- 单元测试：一个测试文件 → 一个可执行 ----------
build/debug/test_%: tests/unit/test_%.c $(LIB_SRC) $(UNITY_DEBUG) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) $(THREAD) \
	      -g -O0 -o $@ $< $(LIB_SRC) $(UNITY_DEBUG) $(LDLIBS)

build/asan/test_%: tests/unit/test_%.c $(LIB_SRC) $(UNITY_ASAN) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) $(THREAD) \
	      -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o $@ $< $(LIB_SRC) $(UNITY_ASAN) $(LDLIBS)

build/tsan/test_%: tests/unit/test_%.c $(LIB_SRC) $(UNITY_TSAN) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) $(THREAD) \
	      -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
	      -o $@ $< $(LIB_SRC) $(UNITY_TSAN) $(LDLIBS)

# 空套件报「全部通过」是最危险的假绿，所以先挡住；每条测试一律套 timeout
test: $(TEST_BIN)
	@if [ -z "$(TEST_BIN)" ]; then \
	  echo "没找到任何单元测试（tests/unit/test_*.c）"; exit 1; \
	fi; \
	rc=0; for t in $(TEST_BIN); do \
	  printf '\n===== %s =====\n' "$$t"; \
	  timeout $(TMO) "$$t"; st=$$?; \
	  if [ $$st -eq 124 ]; then echo "!! $$t 超时（$(TMO)s）—— 大概率死锁"; rc=1; \
	  elif [ $$st -ne 0 ]; then rc=1; fi; \
	done; \
	if [ $$rc -eq 0 ]; then printf '\n单元测试：全部通过\n'; else printf '\n单元测试：有失败\n'; fi; \
	exit $$rc

test-asan: $(TEST_ASAN_BIN)
	@if [ -z "$(TEST_ASAN_BIN)" ]; then \
	  echo "没找到任何单元测试（tests/unit/test_*.c）"; exit 1; \
	fi; \
	rc=0; for t in $(TEST_ASAN_BIN); do \
	  printf '\n===== %s =====\n' "$$t"; \
	  timeout $(TMO) "$$t"; st=$$?; \
	  if [ $$st -eq 124 ]; then echo "!! $$t 超时（$(TMO)s）—— 大概率死锁"; rc=1; \
	  elif [ $$st -ne 0 ]; then rc=1; fi; \
	done; \
	if [ $$rc -eq 0 ]; then printf '\n单元测试(ASan)：全部通过\n'; else printf '\n单元测试(ASan)：有失败\n'; fi; \
	exit $$rc

tsan: $(TEST_TSAN_BIN)
	@if [ -z "$(TEST_TSAN_BIN)" ]; then \
	  echo "没找到任何单元测试（tests/unit/test_*.c）"; exit 1; \
	fi; \
	rc=0; for t in $(TEST_TSAN_BIN); do \
	  printf '\n===== [TSan] %s =====\n' "$$t"; \
	  out=$$($(SETARCH) timeout $(TMO) "$$t" 2>&1); st=$$?; \
	  printf '%s\n' "$$out"; \
	  if [ $$st -eq 124 ]; then echo "!! $$t 超时（$(TMO)s）—— 大概率死锁"; rc=1; \
	  elif printf '%s' "$$out" | grep -q 'WARNING: ThreadSanitizer'; then \
	    echo "!! $$t 有数据竞争告警"; rc=1; \
	  elif [ $$st -ne 0 ]; then rc=1; fi; \
	done; \
	if [ $$rc -eq 0 ]; then printf '\nTSan：无数据竞争告警\n'; else printf '\nTSan：有问题\n'; fi; \
	exit $$rc

# ---------- 变异验证：确认用例真的抓得住这两个经典 bug ----------
# pc.c 必须支持这两个编译开关（见 src/pc.c 顶部说明）：
#   -DPC_BUG_IF     把 insert/remove 的 while 换成 if
#   -DPC_BUG_SIGNAL 把 close() 的 broadcast 换成 signal
# 判据是「必须红」：变体居然绿了 → 说明用例无效（比用例全绿信息量大得多）
MUT_DIR    := build/mut
MUT_IF     := $(MUT_DIR)/test_cv_if
MUT_SIGNAL := $(MUT_DIR)/test_cv_signal

$(MUT_IF): tests/unit/test_pc_cv.c $(LIB_SRC) $(UNITY_DEBUG) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) $(THREAD) \
	      -g -O1 -DPC_BUG_IF -o $@ $< $(LIB_SRC) $(UNITY_DEBUG) $(LDLIBS)

$(MUT_SIGNAL): tests/unit/test_pc_cv.c $(LIB_SRC) $(UNITY_DEBUG) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) $(THREAD) \
	      -g -O1 -DPC_BUG_SIGNAL -o $@ $< $(LIB_SRC) $(UNITY_DEBUG) $(LDLIBS)

mutation-check: $(MUT_IF) $(MUT_SIGNAL)
	@rc=0; \
	for pair in "PC_BUG_IF:$(MUT_IF)" "PC_BUG_SIGNAL:$(MUT_SIGNAL)"; do \
	  name=$${pair%%:*}; t=$${pair#*:}; \
	  if timeout $(TMO) "$$t" >/dev/null 2>&1; then \
	    echo "!! 变体 $$name 竟然通过了 —— 用例抓不住这个 bug，用例无效"; rc=1; \
	  else \
	    echo "OK   变体 $$name 被用例抓住（红）"; \
	  fi; \
	done; \
	if [ $$rc -eq 0 ]; then printf '\n变异验证：两个经典 bug 都能被抓住\n'; \
	else printf '\n变异验证：有用例漏洞\n'; fi; \
	exit $$rc

# ---------- 端到端（与信号量版共用同一套用例：契约一致才能对照）----------
e2e: debug
	bash ./tests/e2e/run.sh ./$(DBG)

e2e-asan: asan
	bash ./tests/e2e/run.sh ./$(ASAN_BIN)

check: test e2e tsan

# ---------- 手动跑 ----------
run: debug
	./$(DBG) $(ARGS)
gdb: debug
	gdb -q --args ./$(DBG) $(ARGS)
run-asan: asan
	./$(ASAN_BIN) $(ARGS)

clean:
	rm -rf build
