CC ?= gcc
SRC = $(wildcard *.c)
TARGET = build/debug/shell
CFLAGS = -std=c11 -Wall -Wextra -Wmissing-prototypes -Werror=implicit-function-declaration -g

.PHONY: all debug clean

all: debug

debug: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o  $@ $^
clean:
	rm -rf build	