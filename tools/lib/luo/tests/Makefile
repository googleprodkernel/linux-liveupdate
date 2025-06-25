# SPDX-License-Identifier: LGPL-3.0-or-later
TESTS = test
INCLUDE_DIR = ../include
HEADERS = $(wildcard $(INCLUDE_DIR)/*.h)

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -I$(INCLUDE_DIR)
LDFLAGS = -L.. -l:libluo.a

.PHONY: all clean

all: $(TESTS)

test: test.c ../libluo.a $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TESTS)
