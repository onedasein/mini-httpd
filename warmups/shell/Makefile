CC ?= gcc
SRC = $(wildcard *.c)
TARGET = build/debug/shell
CFLAGS = -std=c11 -Wall -Wextra -Wmissing-prototypes -Werror=implicit-function-declaration -g

.PHONY: all debug clean test

all: debug

debug: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o  $@ $^

# tests/run.sh 收二进制路径作为 $1；STAGE=2 make test 可连管道/重定向用例一起跑
test: debug
	bash ./tests/run.sh

clean:
	rm -rf build	