SRC = $(wildcard *.c)
all: debug

debug:
	@mkdir -p build/debug
	gcc -o build/debug/shell $(SRC)
clean:
	rm -rf build