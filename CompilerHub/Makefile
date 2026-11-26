# CompilerHub Makefile
# --------------------
# 构建： make (默认)   make release
# 使用： make test     make clean

CC       ?= gcc
CSTD     := -std=gnu11
WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
OPT      := -O2
CFLAGS   := $(CSTD) $(WARN) $(OPT) -Isrc

# capstone 可选支持：自动探测头文件和库
HAS_CS_H := $(shell (test -f /opt/homebrew/include/capstone/capstone.h || test -f /usr/local/include/capstone/capstone.h || test -f /usr/include/capstone.h) && echo 1)
HAS_CS_L := $(shell (test -f /opt/homebrew/lib/libcapstone.dylib || test -f /opt/homebrew/lib/libcapstone.so || test -f /usr/local/lib/libcapstone.dylib || test -f /usr/local/lib/libcapstone.so) && echo 1)

ifeq ($(HAS_CS_H)$(HAS_CS_L),11)
  CFLAGS += -DHAS_CAPSTONE=1
  LDFLAGS += -lcapstone
endif

SRCS := src/main.c src/utils.c src/compiler.c src/packager.c src/decompile.c src/server.c
OBJS := $(SRCS:.c=.o)
BIN  := ch
LDFLAGS += -lpthread

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

release: OPT += -DNDEBUG
release: clean all

clean:
	rm -f $(OBJS) $(BIN)

test: $(BIN)
	@echo "== version =="
	./$(BIN) version
	@echo "== detect  =="
	echo 'int main(){return 0;}' > /tmp/ch_sample.c
	echo 'print("hi")'         > /tmp/ch_sample.py
	./$(BIN) detect /tmp/ch_sample.c
	./$(BIN) detect /tmp/ch_sample.py
	@echo "== compile exe =="
	./$(BIN) compile -i /tmp/ch_sample.c -o /tmp/ch_sample
	ls -l /tmp/ch_sample
	@echo "== compile pkg (script) =="
	./$(BIN) compile -i /tmp/ch_sample.py -t pkg -o /tmp/ch_sample.pkg
	ls -l /tmp/ch_sample.pkg
	@echo "== compile apk stub =="
	./$(BIN) compile -i /tmp/ch_sample.c -t apk -o /tmp/ch_sample.apk
	ls -l /tmp/ch_sample.apk
	@echo "== decompile =="
	./$(BIN) decompile -i /tmp/ch_sample --pseudo-c | head -20
	@echo "== all OK =="

.PHONY: all clean release test
