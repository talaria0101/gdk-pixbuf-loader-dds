PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
THUMB_DIR ?= $(PREFIX)/share/thumbnailers
# Loader dir queried from pkg-config; fallback to LIBDIR path
LOADER_DIR ?= $(shell $(PKG_CONFIG) --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0 2>/dev/null || PKG_CONFIG_PATH=/tmp/prefix/usr/lib64/pkgconfig $(PKG_CONFIG) --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0 2>/dev/null || echo "$(LIBDIR)/gdk-pixbuf-2.0/2.10.0/loaders")

PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
# Try system pkg-config first, fall back to extracted prefix for CI sandbox without -devel
CFLAGS += $(shell $(PKG_CONFIG) --cflags gdk-pixbuf-2.0 2>/dev/null || PKG_CONFIG_PATH=/tmp/prefix/usr/lib64/pkgconfig $(PKG_CONFIG) --cflags gdk-pixbuf-2.0 2>/dev/null || echo "-I/tmp/prefix/usr/include/gdk-pixbuf-2.0 -I/tmp/prefix/usr/include/glib-2.0 -I/tmp/prefix/usr/lib64/glib-2.0/include")
LDFLAGS ?=
LDLIBS = $(shell $(PKG_CONFIG) --libs gdk-pixbuf-2.0 2>/dev/null || PKG_CONFIG_PATH=/tmp/prefix/usr/lib64/pkgconfig $(PKG_CONFIG) --libs gdk-pixbuf-2.0 2>/dev/null || echo "-lgdk_pixbuf-2.0 -lgobject-2.0 -lglib-2.0")

TARGET = libpixbufloader-dds.so

all: $(TARGET)

$(TARGET): io-dds.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< $(LDFLAGS) $(LDLIBS) -lm
	@echo "Built $@ ($$(stat -c %s $@) bytes)"

clean:
	rm -f $(TARGET) *.o

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(LOADER_DIR)/$(TARGET)
	install -Dm644 dds.thumbnailer $(DESTDIR)$(THUMB_DIR)/dds.thumbnailer
	@echo "Run: gdk-pixbuf-query-loaders --update-cache"

test: $(TARGET)
	./test_dds.sh

.PHONY: all clean install test
