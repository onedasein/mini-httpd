# mini-httpd —— 探针 A
#   make debug    开发用：-O0 -g3，方便 gdb
#   make release  压测用：-O2，QPS 对比必须用这个二进制
#   make asan     体检用：AddressSanitizer + UBSan
#   make run      编译并启动在 127.0.0.1:8080
CC     ?= gcc
BIN     = mini-httpd
SRC     = $(wildcard src/*.c)
WARN    = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes

.PHONY: all debug release asan run clean

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

clean:
	rm -rf build
