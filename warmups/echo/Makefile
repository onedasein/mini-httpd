CC ?= gcc
SRC := $(wildcard src/*.c)
BIN := server
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wmissing-prototypes -Wstrict-prototypes \
           -Werror=implicit-function-declaration
LDLIBS :=
ARGS ?= 127.0.0.1 8080
DBG := build/debug/$(BIN)

.PHONY: all debug release asan clean run gdb run-asan test test-asan

all: debug

debug: $(DBG)
$(DBG): $(SRC) Makefile
	@mkdir -p $(@D)  # $(@D) = build/debug
	$(CC) $(WARN) -g -O0 -o $@ $(SRC) $(LDLIBS)

release: build/release/$(BIN)
build/release/$(BIN): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -O2 -DNDEBUG -o $@ $(SRC) $(LDLIBS)

asan: build/asan/$(BIN)
build/asan/$(BIN): $(SRC) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -g -O1 -fsanitize=address,undefined \
-fno-omit-frame-pointer -o $@ $(SRC) $(LDLIBS)

run: debug
	./$(DBG)  $(ARGS)
gdb: debug
	gdb -q --args ./$(DBG) $(ARGS)
run-asan: asan
	./build/asan/$(BIN) $(ARGS)
test: debug
	bash ./tests/run.sh
test-asan: asan
	bash ./tests/run.sh ./build/asan/$(BIN)
clean:
	rm -rf build


