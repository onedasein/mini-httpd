# mini-httpd —— 探针 A
#   make debug    开发用：-O0 -g3，方便 gdb
#   make release  压测用：-O2，QPS 对比必须用这个二进制
#   make asan     体检用：AddressSanitizer + UBSan
#   make run      编译并启动在 127.0.0.1:8080
#   make test     把各 warmup 的回归套件跑一遍（仓库级入口，不依赖 src/ 是否落地）
#   make test-asan  同上，但用各 warmup 的 ASan 产物
CC     ?= gcc
BIN     = mini-httpd
SRC     = $(wildcard src/*.c)
WARN    = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes

# 有 tests/run.sh + Makefile 里带 test 目标的 warmup（pc/ 还没有套件，故不在列）
WARMUPS      := echo httpd/v1 shell
WARMUPS_ASAN := echo httpd/v1

.PHONY: all debug release asan tsan run clean test test-asan

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

# 每个 warmup 目录里都有自己的 Makefile + tests/run.sh，这里只做转发；
# 子 make 挂了就立刻停（|| exit 1），所以整仓测试的退出码是可信的
test:
	@for w in $(WARMUPS); do \
	  printf '\n========== warmups/%s ==========\n' "$$w"; \
	  $(MAKE) -C warmups/$$w test || exit 1; \
	done

test-asan:
	@for w in $(WARMUPS_ASAN); do \
	  printf '\n========== warmups/%s (ASan) ==========\n' "$$w"; \
	  $(MAKE) -C warmups/$$w test-asan || exit 1; \
	done

clean:
	rm -rf build
