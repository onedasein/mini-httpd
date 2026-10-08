# mini-httpd —— 探针 A
#   make debug    开发用：-O0 -g3，方便 gdb
#   make release  压测用：-O2，QPS 对比必须用这个二进制
#   make asan     体检用：AddressSanitizer + UBSan
#   make run      编译并启动在 127.0.0.1:8080
#
#   测试入口：都是转发到各 warmup 自己的 Makefile，不依赖根 src/ 是否落地
#   make test       各 warmup 的单元测试（未分层的 warmup 仍是脚本回归，见下）
#   make e2e        各 warmup 的端到端脚本回归
#   make check      test + e2e（提交前跑这个）
#   make test-asan  test 的 ASan 变体
#   make e2e-asan   e2e 的 ASan 变体
CC     ?= gcc
BIN     = mini-httpd
SRC     = $(wildcard src/*.c)
WARN    = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes

# 测试分层约定见 TASKS.md「测试分层约定」。
# echo 已分层：test = 单元测试，e2e = 脚本回归（test-asan 是单元的 ASan 版）
# echo_pre / httpd/v1 / shell 尚未分层：它们的 test 仍是原来的 tests/run.sh 脚本回归
WARMUPS      := echo echo_pre httpd/v1 shell
WARMUPS_E2E  := echo            # 有独立 e2e 目标的（未分层的 warmup 不在列）
WARMUPS_ASAN := echo httpd/v1   # 有 test-asan 的

.PHONY: all debug release asan tsan run clean test test-asan e2e e2e-asan check

all: debug

debug:
	@mkdir -p build/debug
	$(CC) $(WARN) -O0 -g3 -DDEBUG -o build/debug/$(BIN) $(SRC)

release:
	@mkdir -p build/release
	$(CC) $(WARN) -O2 -DNDEBUG -o build/release/$(BIN) $(SRC)

asan:
	@mkdir -p build/asan
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o build/asan/$(BIN) $(SRC)

tsan:  # 只在 T0.5 的生产者-消费者队列上用
	@mkdir -p build/tsan
	$(CC) $(WARN) -O1 -g -fsanitize=thread -o build/tsan/$(BIN) $(SRC)

run: debug
	./build/debug/$(BIN) 8080

# 每个 warmup 目录里都有自己的 Makefile，这里只做转发；
# 子 make 挂了就立刻停（|| exit 1），所以整仓测试的退出码是可信的
test:
	@for w in $(WARMUPS); do \
	  printf '\n========== warmups/%s : test ==========\n' "$$w"; \
	  $(MAKE) -C warmups/$$w test || exit 1; \
	done

e2e:
	@for w in $(WARMUPS_E2E); do \
	  printf '\n========== warmups/%s : e2e ==========\n' "$$w"; \
	  $(MAKE) -C warmups/$$w e2e || exit 1; \
	done

test-asan:
	@for w in $(WARMUPS_ASAN); do \
	  printf '\n========== warmups/%s : test-asan ==========\n' "$$w"; \
	  $(MAKE) -C warmups/$$w test-asan || exit 1; \
	done

e2e-asan:
	@for w in $(WARMUPS_E2E); do \
	  printf '\n========== warmups/%s : e2e-asan ==========\n' "$$w"; \
	  $(MAKE) -C warmups/$$w e2e-asan || exit 1; \
	done

check: test e2e

clean:
	rm -rf build
