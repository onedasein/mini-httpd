# pc 热身（T0.5）— 生产者-消费者队列（CSAPP sbuf 风格：sem_t 信号量版）
#   make debug / release / asan     三个目标各自产出独立二进制
#   make run ARGS="4 4 20000 1024"  编（增量）+ 跑
#   make test       单元测试：进程内直接调用 pc_t_*，毫秒级
#   make e2e        端到端：起进程跑并发形状，跑 tests/e2e/run.sh
#   make tsan       TSan 版单元测试 —— 本项（T0.5）的验收目标
#   make check      test + e2e + tsan（提交前用；比仓库总约定多了 tsan，因为本项验收就是它）
#   make test-asan / e2e-asan        发布前双跑
CC      ?= gcc
BIN     := pc
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wmissing-prototypes -Wstrict-prototypes \
           -Werror=implicit-function-declaration
THREAD  := -pthread
LDLIBS  := -pthread

ARGS    ?= 4 4 20000 1024               # make run ARGS="1 4 5000 1"

SRC_DIR  := src
SRC      := $(wildcard $(SRC_DIR)/*.c)
# 被测模块 = 除 main 之外的所有源文件。
# 这个 filter-out 是「单元测试能直接调用」的机械前提：库里再有一个 main 就 duplicate symbol。
LIB_SRC  := $(filter-out $(SRC_DIR)/main.c, $(SRC))

DBG      := build/debug/$(BIN)
ASAN_BIN := build/asan/$(BIN)

# 第三方测试框架（vendored，MIT）：Unity，版本见 tests/unit/unity/.VERSION
UNITY_DIR   := tests/unit/unity
UNITY_DEBUG := build/debug/unity.o
UNITY_ASAN  := build/asan/unity.o
UNITY_TSAN  := build/tsan/unity.o

# 自动发现：新增 tests/unit/test_xxx.c 就自动多一个测试可执行，不用改这个文件
TEST_SRC      := $(wildcard tests/unit/test_*.c)
TEST_BIN      := $(TEST_SRC:tests/unit/%.c=build/debug/%)
TEST_ASAN_BIN := $(TEST_SRC:tests/unit/%.c=build/asan/%)
TEST_TSAN_BIN := $(TEST_SRC:tests/unit/%.c=build/tsan/%)

# WSL2 上 TSan 会概率性 FATAL: unexpected memory mapping（ASLR 熵过高，实测 6 次挂 4 次）。
# setarch -R 关掉 ASLR → 实测稳定；没有 setarch 时退化成直接跑。
ARCH    := $(shell uname -m)
SETARCH := $(shell command -v setarch >/dev/null 2>&1 && echo "setarch $(ARCH) -R")
# 单条测试的墙钟上限：本模块的失败模式是「死锁」而不是断言失败，不设上限 make test 会一直挂着
TMO     ?= 20

.PHONY: all debug release asan clean run gdb run-asan \
        test test-asan tsan e2e e2e-asan check

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
# Unity 在 -Wconversion -Wshadow 下会刷屏，所以用 -w
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
# -Wno-missing-prototypes：别对每个 setUp/tearDown 都告警
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

# TSan：既看退出码，也显式 grep 告警（TSan 有告警时退出码是 66）
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

# ---------- 端到端：起进程跑并发形状，脚本负责调度与比对 ----------
# tests/e2e/run.sh 收二进制路径作为 $1，所以同一套断言能在 debug / asan 上双跑
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
