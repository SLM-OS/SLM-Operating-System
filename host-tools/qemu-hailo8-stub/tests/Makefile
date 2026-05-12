# Standalone test build for the hailo8_corpus module.
#
# The module is QEMU-independent; this Makefile builds a test binary
# directly with the system toolchain so the test suite can run without
# any QEMU build.

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11 \
           -D_POSIX_C_SOURCE=200809L \
           -Wno-unused-parameter -Wno-unused-variable
LDFLAGS ?=

SRC_DIR := ../src
OBJS    := test_corpus.o hailo8_corpus.o

.PHONY: all test clean

all: test_corpus

test_corpus: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

test_corpus.o: test_corpus.c $(SRC_DIR)/hailo8_corpus.h
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

hailo8_corpus.o: $(SRC_DIR)/hailo8_corpus.c $(SRC_DIR)/hailo8_corpus.h
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

test: test_corpus
	./test_corpus

clean:
	rm -f test_corpus $(OBJS)
