# SPDX-License-Identifier: LGPL-3.0-or-later
SRCS = libluo.c
OBJS = $(SRCS:.c=.o)
INCLUDE_DIR = include
HEADERS = $(wildcard $(INCLUDE_DIR)/*.h)

CC = gcc
AR = ar
CFLAGS = -Wall -Wextra -fPIC -O2 -g -I$(INCLUDE_DIR)
LDFLAGS = -shared

LIB_NAME = libluo
STATIC_LIB = $(LIB_NAME).a
SHARED_LIB = $(LIB_NAME).so

.PHONY: all clean install

all: $(STATIC_LIB) $(SHARED_LIB)

$(STATIC_LIB): $(OBJS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(STATIC_LIB) $(SHARED_LIB)

install: all
	install -d $(DESTDIR)/usr/local/lib
	install -d $(DESTDIR)/usr/local/include
	install -m 644 $(STATIC_LIB) $(DESTDIR)/usr/local/lib
	install -m 755 $(SHARED_LIB) $(DESTDIR)/usr/local/lib
	install -m 644 $(HEADERS) $(DESTDIR)/usr/local/include
