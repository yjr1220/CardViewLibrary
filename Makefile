CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

SRC := src/mtp_daemon.c
HDR := src/mtp_daemon.h
OUT := mtp_daemon

all: $(OUT)

$(OUT): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(OUT)

install: $(OUT)
	install -Dm755 $(OUT) $(DESTDIR)$(BINDIR)/$(OUT)

.PHONY: all clean install
