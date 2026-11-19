# upkg -- host (development) build.  On DER/32 itself upkg is built by the
# base system (src/usr.sbin/upkg) with BSD make and the base libraries.
#
#   make            build ./upkg against libarchive, sqlite3, OpenSSL, libcurl
#   make test       run the integration test suite
#   make clean

CC      ?= cc
PKGCONF ?= pkg-config
PKGS     = libarchive sqlite3 libcrypto libcurl
CFLAGS  ?= -O2 -g
WARN     = -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes \
           -Wstrict-prototypes -Wshadow -Wpointer-arith -Wformat=2
CPPFLAGS += -Iinclude -D_GNU_SOURCE -DUPKG_WITH_LIBCURL
PKG_CFLAGS != $(PKGCONF) --cflags $(PKGS)
PKG_LIBS   != $(PKGCONF) --libs $(PKGS)

SRCS = src/main.c src/util.c src/version.c src/meta.c src/config.c \
       src/sig.c src/fetch.c src/repo.c src/pkgfile.c src/db.c src/txn.c \
       src/ops.c src/build.c
OBJS = $(SRCS:.c=.o)

all: upkg

upkg: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

.c.o:
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(OBJS): include/upkg.h

test: upkg
	sh tests/run.sh ./upkg

clean:
	rm -f upkg $(OBJS)

.PHONY: all test clean
