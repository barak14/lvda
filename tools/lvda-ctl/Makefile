# SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
DESTDIR ?=
PREFIX ?= /usr
BINDIR := $(DESTDIR)$(PREFIX)/bin

lvda-ctl: lvda-ctl.c ../../uapi/lvda.h
	$(CC) $(CFLAGS) -o $@ lvda-ctl.c

clean:
	rm -f lvda-ctl

install: lvda-ctl
	install -d $(BINDIR)
	install -m 0755 lvda-ctl $(BINDIR)/lvda-ctl

.PHONY: clean install
