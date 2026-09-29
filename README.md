# gdk-pixbuf-loader-dds

GdkPixbuf loader for DDS (DirectDraw Surface) images so that `ristretto`, `eog`,
Thunar thumbnails, `gdk-pixbuf-thumbnailer`, and any `GdkPixbuf`-based viewer
can open `.dds` textures.

## Supported DDS variants

- DXT1 / BC1 (opaque + 1-bit alpha variant)
- DXT3 / BC2
- DXT5 / BC3
- Uncompressed RGB/RGBA with arbitrary bitmasks (`A8R8G8B8`, `X8R8G8B8`, `R8G8B8`, `R5G6B5`, `A1R5G5B5`, `A4R4G4B4`, …)
- Luminance `L8`, `A8L8`, `A8`
- DX10 extended header (`DX10` FourCC) for `BC1/BC2/BC3/R8G8B8A8/B8G8R8A8`
- Mipmaps: only the top level is displayed
- Cubemaps / volume textures: first face only

Non-BC compressed formats (BC4/BC5, BC6H, BC7, float) are reported as
`UNKNOWN_TYPE` rather than silently producing garbage.

## Build

```sh
make
```

Build uses `pkg-config gdk-pixbuf-2.0` for cflags/libs. On a stock Void Linux
install you need the -devel packages; if missing, the Makefile falls back to
the headers extracted under `/tmp/prefix` during CI (see `Makefile:PKG_CONFIG_PATH`).

## Install

```sh
sudo make install
sudo gdk-pixbuf-query-loaders --update-cache
# or per-loader dir update:
sudo gdk-pixbuf-query-loaders > /usr/lib64/gdk-pixbuf-2.0/2.10.0/loaders.cache
```

Files installed:

- `$(LIBDIR)/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-dds.so`
- `/usr/share/thumbnailers/dds.thumbnailer` (delegates to `gdk-pixbuf-thumbnailer`)

After updating the loader cache, `ristretto` picks up DDS without restart; for
Thunar, `thunar -q` or log out/in may be needed.

## Tumbler / Thunar thumbnails

Thunar does not thumbnail images itself; it talks to `tumblerd` (the XFCETumbler
D-Bus service, `org.freedesktop.thumbnails.Thumbnailer1`). Tumbler picks up DDS
through two independent paths, both covered by `make install`:

1. **Desktop thumbnailer plugin**: tumbler scans `$XDG_DATA_HOME/thumbnailers`
   and `$XDG_DATA_DIRS/thumbnailers` for `*.thumbnailer` keyfiles at startup.
   The installed `/usr/share/thumbnailers/dds.thumbnailer` registers
   `image/vnd.ms-dds`, `image/x-dds`, and `image/dds` and spawns
   `gdk-pixbuf-thumbnailer -s %s %i %o` per file.
2. **Pixbuf thumbnailer plugin**: builds its supported-type list at startup from
   `gdk_pixbuf_get_formats()`, so once the loader is in the system loaders.cache
   it can also handle DDS in-process.

After installing, make the running daemon rescan (it caches thumbnailers at
startup):

```sh
pkill tumblerd          # D-Bus activation restarts it on the next request
rm -rf ~/.cache/thumbnails   # only if stale non-DDS thumbnails were cached before
```

Verified end to end with tumbler 4.20.0: `GetSupported` lists all three DDS mime
types (once per plugin), and `Queue("file://…dxt1_8x8.dds", "image/vnd.ms-dds",
"large", "default", 0)` produced a correct `~/.cache/thumbnails/large/<md5>.png`
(256x256, pixel-identical decode) within one second.

## How it works

`io-dds.c` implements the `GdkPixbufModule` backend:

- `fill_info` advertises magic `DDS ` and mime types `image/vnd.ms-dds`, `image/x-dds`, `image/dds`
- `fill_vtable` plugs `load`, `begin_load`/`stop_load`/`load_increment`, `load_animation`
- Incremental loading buffers until `stop_load`, then decodes once (keeps memory bounded; DDS textures are normally <64MiB)
- Decoding is little-endian clean, clamps dimensions to 16384, and validates FourCC / DXGI before touching pixel data

## Limitations

- No cubemap array / volume slice navigation
- No BC4/BC5/BC6H/BC7 decoding (would require extra decompressors)
- Cubemap mip tails are ignored (first face, first mip)
- Thumbnailer is sRGB-unaware; DXT sRGB vs linear is not distinguished at the pixbuf level

## Test

```sh
make test
# or manually:
gdk-pixbuf-pixdata ./test-images/dxt1.dds | head
```

`test_dds.sh` generates tiny DXT1/DXT5/uncompressed DDS files with Python and
loads them via `gdk-pixbuf` Python bindings and raw loader checks.

## License

MIT, compatible with gdk-pixbuf LGPL-2.1+.
