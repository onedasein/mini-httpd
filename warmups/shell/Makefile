CC ?= gcc
SRC = $(wildcard *.c)
TARGET = build/debug/shell

.PHONY: all debug clean

all: debug

debug: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(dir $@)
	$(CC) -o  $@ $^
clean:
	rm -rf build	