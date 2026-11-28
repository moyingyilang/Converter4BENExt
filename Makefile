CXX ?= clang++
CC ?= clang
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter -Isrc/third_party
CFLAGS ?= -O2 -std=c11 -Isrc/third_party
LDFLAGS ?= -lz -larchive
BIN = build/c4ben
LIB = build/libc4ben.a
OBJ = build/main.o build/cJSON.o
LIB_OBJ = build/main_lib.o build/cJSON.o

all: $(BIN) $(LIB)

lib: $(LIB)

$(LIB): $(LIB_OBJ) | build
	ar rcs $@ $(LIB_OBJ)

build/main_lib.o: src/main.cpp src/c4ben.h | build
	$(CXX) $(CXXFLAGS) -DC4BEN_NO_MAIN -c -o $@ $<
$(BIN): $(OBJ) | build
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)
build/main.o: src/main.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/cJSON.o: src/third_party/cJSON.c | build
	$(CC) $(CFLAGS) -c -o $@ $<
build:
	@mkdir -p build
clean:
	rm -rf build
.PHONY: all lib clean
