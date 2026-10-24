# pg_walex - PostgreSQL WAL Extractor: offline WAL reader and SQL reconstructor
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter \
           -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
           -Iinclude -Isrc
LDFLAGS ?=
LDLIBS  ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRC = src/pwe_util.c src/pwe_version.c src/pwe_wal.c src/pwe_tuple.c \
      src/pwe_catalog.c src/pwe_recon.c src/pwe_output.c src/pwe_main.c \
      src/pwe_http.c
OBJ = $(SRC:.c=.o)
BIN = pg_walex

# Optional compression libraries (only used for WAL full-page images /
# toasted values that were compressed with LZ4 or Zstandard).
EXTRA_INC = -I/opt/homebrew/include -I/usr/local/include
EXTRA_LIB = -L/opt/homebrew/lib -L/usr/local/lib

HAVE_LZ4 := $(shell printf '\#include <lz4.h>\nint main(void){return 0;}\n' | \
              $(CC) $(EXTRA_INC) -x c - -o /dev/null $(EXTRA_LIB) -llz4 >/dev/null 2>&1 && echo yes)
HAVE_ZSTD := $(shell printf '\#include <zstd.h>\nint main(void){return 0;}\n' | \
              $(CC) $(EXTRA_INC) -x c - -o /dev/null $(EXTRA_LIB) -lzstd >/dev/null 2>&1 && echo yes)

ifeq ($(HAVE_LZ4),yes)
CFLAGS  += -DPWE_WITH_LZ4 $(EXTRA_INC)
LDFLAGS += $(EXTRA_LIB)
LDLIBS  += -llz4
endif
ifeq ($(HAVE_ZSTD),yes)
CFLAGS  += -DPWE_WITH_ZSTD $(EXTRA_INC)
LDFLAGS += $(EXTRA_LIB)
LDLIBS  += -lzstd
endif

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(OBJ) $(BIN)

test: $(BIN)
	./tests/run_local.sh

.PHONY: all install clean test
