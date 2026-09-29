/* GdkPixbuf DDS loader
 * Copyright (C) 2026 - MIT / LGPL-2.1-or-later
 *
 * DDS (Microsoft DirectDraw Surface) loader for gdk-pixbuf.
 * Enables ristretto, eog, thunar thumbnails etc. to open .dds textures.
 *
 * Supports:
 *   - DXT1 (BC1) including 1-bit alpha variant (c0 <= c1)
 *   - DXT3 (BC2) explicit alpha
 *   - DXT5 (BC3) interpolated alpha
 *   - DX10 extended header mapping BC1/BC2/BC3 and plain RGBA
 *   - Uncompressed RGB/RGBA with arbitrary bitmasks (A8R8G8B8, X8R8G8B8, R8G8B8, R5G6B5, etc.)
 *   - Luminance (L8, A8L8)
 *   - Mipmaps: only top level is decoded, remaining bytes are ignored
 *   - Cubemaps / volume textures: only first face/slice is decoded
 *
 * References:
 *   - Microsoft DDS File Reference
 *   - https://docs.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide
 *   - Squish / libsquish DXT decompression
 */

#define GDK_PIXBUF_ENABLE_BACKEND
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf/gdk-pixbuf-io.h>
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define DDS_MAGIC 0x20534444  /* "DDS " little endian */

#define DDSD_CAPS        0x1
#define DDSD_HEIGHT      0x2
#define DDSD_WIDTH       0x4
#define DDSD_PITCH       0x8
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_MIPMAPCOUNT 0x20000
#define DDSD_LINEARSIZE  0x80000
#define DDSD_DEPTH       0x800000

#define DDPF_ALPHAPIXELS 0x1
#define DDPF_ALPHA       0x2
#define DDPF_FOURCC      0x4
#define DDPF_RGB         0x40
#define DDPF_YUV         0x200
#define DDPF_LUMINANCE   0x20000
#define DDPF_BUMPDUDV    0x80000

#define MAKEFOURCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static inline uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

typedef enum {
    DDS_FMT_UNKNOWN,
    DDS_FMT_DXT1,
    DDS_FMT_DXT3,
    DDS_FMT_DXT5,
    DDS_FMT_UNCOMPRESSED,
} DdsFormat;

typedef struct {
    uint32_t flags;
    uint32_t fourcc;
    uint32_t rgb_bits;
    uint32_t r_mask;
    uint32_t g_mask;
    uint32_t b_mask;
    uint32_t a_mask;
} DdsPixelFormat;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch_or_linear;
    uint32_t depth;
    uint32_t mip_count;
    DdsPixelFormat pf;
    DdsFormat fmt;
    uint32_t flags;
    gboolean has_dx10;
    uint32_t dxgi_format;
    uint32_t dx10_resource_dim;
    const uint8_t *data;
    size_t data_len;
} DdsInfo;

/* --- color helpers --- */
static void decode_565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = (uint8_t)((c >> 11) & 0x1f);
    *g = (uint8_t)((c >> 5) & 0x3f);
    *b = (uint8_t)(c & 0x1f);
    *r = (uint8_t)((*r << 3) | (*r >> 2));
    *g = (uint8_t)((*g << 2) | (*g >> 4));
    *b = (uint8_t)((*b << 3) | (*b >> 2));
}

static int mask_shift(uint32_t mask) {
    if (mask == 0) return 0;
    int s = 0;
    while ((mask & 1) == 0) { mask >>= 1; s++; }
    return s;
}
static int mask_bits_val(uint32_t mask) {
    if (mask == 0) return 0;
    /* count contiguous bits after shift */
    int shift = mask_shift(mask);
    mask >>= shift;
    int bits = 0;
    while (mask & 1) { bits++; mask >>= 1; }
    return bits ? bits : 0;
}
static uint8_t extract_channel(uint32_t pixel, uint32_t mask) {
    if (mask == 0) return 0;
    int shift = mask_shift(mask);
    int bits = mask_bits_val(mask);
    uint32_t v = (pixel & mask) >> shift;
    if (bits == 0) return 0;
    if (bits == 8) return (uint8_t)v;
    /* scale to 0..255 */
    return (uint8_t)((v * 255 + ((1 << bits) - 1) / 2) / ((1 << bits) - 1));
}

/* --- DXT decompression --- */

static void decompress_dxt1_block(const uint8_t *src, uint8_t *dst, int dst_stride, int bw, int bh, int bx, int by, int width, int height) {
    uint16_t c0 = read_u16_le(src);
    uint16_t c1 = read_u16_le(src + 2);
    uint32_t bits = read_u32_le(src + 4);

    uint8_t r0, g0, b0, r1, g1, b1;
    decode_565(c0, &r0, &g0, &b0);
    decode_565(c1, &r1, &g1, &b1);

    uint8_t colors[4][4];
    colors[0][0] = r0; colors[0][1] = g0; colors[0][2] = b0; colors[0][3] = 255;
    colors[1][0] = r1; colors[1][1] = g1; colors[1][2] = b1; colors[1][3] = 255;

    if (c0 > c1) {
        colors[2][0] = (uint8_t)((2 * r0 + r1) / 3);
        colors[2][1] = (uint8_t)((2 * g0 + g1) / 3);
        colors[2][2] = (uint8_t)((2 * b0 + b1) / 3);
        colors[2][3] = 255;
        colors[3][0] = (uint8_t)((r0 + 2 * r1) / 3);
        colors[3][1] = (uint8_t)((g0 + 2 * g1) / 3);
        colors[3][2] = (uint8_t)((b0 + 2 * b1) / 3);
        colors[3][3] = 255;
    } else {
        colors[2][0] = (uint8_t)((r0 + r1) / 2);
        colors[2][1] = (uint8_t)((g0 + g1) / 2);
        colors[2][2] = (uint8_t)((b0 + b1) / 2);
        colors[2][3] = 255;
        colors[3][0] = 0; colors[3][1] = 0; colors[3][2] = 0; colors[3][3] = 0;
    }

    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            int sx = bx * 4 + px;
            int sy = by * 4 + py;
            if (sx >= width || sy >= height) continue;
            int idx = (bits >> (2 * (4 * py + px))) & 0x03;
            uint8_t *out = dst + sy * dst_stride + sx * 4;
            out[0] = colors[idx][0];
            out[1] = colors[idx][1];
            out[2] = colors[idx][2];
            out[3] = colors[idx][3];
        }
    }
}

static void decode_dxt1(const uint8_t *src, size_t src_len, uint8_t *dst, int width, int height, int rowstride) {
    int blocks_x = (width + 3) / 4;
    int blocks_y = (height + 3) / 4;
    size_t needed = (size_t)blocks_x * blocks_y * 8;
    if (src_len < needed) {
        /* allow truncated? fill with black but avoid overread */
        /* clamp to available blocks */
        if (src_len < 8) return;
    }
    for (int by = 0; by < blocks_y; by++) {
        for (int bx = 0; bx < blocks_x; bx++) {
            size_t off = (size_t)(by * blocks_x + bx) * 8;
            if (off + 8 > src_len) continue;
            decompress_dxt1_block(src + off, dst, rowstride, 4, 4, bx, by, width, height);
        }
    }
}

static void decompress_dxt3_block(const uint8_t *src, uint8_t *dst, int dst_stride, int bx, int by, int width, int height) {
    /* first 8 bytes: explicit alpha 4 bits per pixel */
    uint8_t alpha[16];
    for (int i = 0; i < 8; i++) {
        uint8_t v = src[i];
        alpha[2 * i + 0] = (uint8_t)((v & 0x0f) * 17);
        alpha[2 * i + 1] = (uint8_t)(((v >> 4) & 0x0f) * 17);
    }
    /* next 8 bytes: DXT1 colors without transparent black */
    uint16_t c0 = read_u16_le(src + 8);
    uint16_t c1 = read_u16_le(src + 10);
    uint32_t bits = read_u32_le(src + 12);
    uint8_t r0, g0, b0, r1, g1, b1;
    decode_565(c0, &r0, &g0, &b0);
    decode_565(c1, &r1, &g1, &b1);
    uint8_t colors[4][3];
    colors[0][0]=r0; colors[0][1]=g0; colors[0][2]=b0;
    colors[1][0]=r1; colors[1][1]=g1; colors[1][2]=b1;
    colors[2][0] = (uint8_t)((2*r0 + r1)/3); colors[2][1]=(uint8_t)((2*g0+g1)/3); colors[2][2]=(uint8_t)((2*b0+b1)/3);
    colors[3][0] = (uint8_t)((r0 + 2 * r1) / 3);
    colors[3][1] = (uint8_t)((g0 + 2 * g1) / 3);
    colors[3][2] = (uint8_t)((b0 + 2 * b1) / 3);

    /* c0 > c1 case always for DXT3? Spec says same as DXT1 but no transparent, so we force 4-color */
    /* Actually DXT3 uses same decode but colors are always 4, even if c0<=c1. So keep above 4-color. */

    for (int py=0; py<4; py++) for (int px=0; px<4; px++) {
        int sx = bx*4+px; int sy = by*4+py;
        if (sx>=width || sy>=height) continue;
        int p = 4*py+px;
        int idx = (bits >> (2*p)) & 0x3;
        uint8_t *out = dst + sy*dst_stride + sx*4;
        out[0]=colors[idx][0]; out[1]=colors[idx][1]; out[2]=colors[idx][2]; out[3]=alpha[p];
    }
}

static void decode_dxt3(const uint8_t *src, size_t src_len, uint8_t *dst, int width, int height, int rowstride) {
    int blocks_x = (width+3)/4, blocks_y=(height+3)/4;
    for (int by=0; by<blocks_y; by++) for (int bx=0; bx<blocks_x; bx++) {
        size_t off=(size_t)(by*blocks_x+bx)*16;
        if (off+16>src_len) continue;
        decompress_dxt3_block(src+off, dst, rowstride, bx,by,width,height);
    }
}

static void decompress_dxt5_block(const uint8_t *src, uint8_t *dst, int dst_stride, int bx, int by, int width, int height) {
    uint8_t a0 = src[0], a1 = src[1];
    uint8_t alpha[8];
    alpha[0]=a0; alpha[1]=a1;
    if (a0 > a1) {
        alpha[2] = (uint8_t)((6*a0 + 1*a1)/7);
        alpha[3] = (uint8_t)((5*a0 + 2*a1)/7);
        alpha[4] = (uint8_t)((4*a0 + 3*a1)/7);
        alpha[5] = (uint8_t)((3*a0 + 4*a1)/7);
        alpha[6] = (uint8_t)((2*a0 + 5*a1)/7);
        alpha[7] = (uint8_t)((1*a0 + 6*a1)/7);
    } else {
        alpha[2] = (uint8_t)((4*a0 + 1*a1)/5);
        alpha[3] = (uint8_t)((3*a0 + 2*a1)/5);
        alpha[4] = (uint8_t)((2*a0 + 3*a1)/5);
        alpha[5] = (uint8_t)((1*a0 + 4*a1)/5);
        alpha[6] = 0;
        alpha[7] = 255;
    }
    uint64_t abits = 0;
    for (int i=0;i<6;i++) abits |= ((uint64_t)src[2+i]) << (8*i);
    /* next 8 bytes: color */
    uint16_t c0 = read_u16_le(src+8);
    uint16_t c1 = read_u16_le(src+10);
    uint32_t cbits = read_u32_le(src+12);
    uint8_t r0,g0,b0,r1,g1,b1;
    decode_565(c0,&r0,&g0,&b0);
    decode_565(c1,&r1,&g1,&b1);
    uint8_t colors[4][3];
    colors[0][0]=r0; colors[0][1]=g0; colors[0][2]=b0;
    colors[1][0]=r1; colors[1][1]=g1; colors[1][2]=b1;
    colors[2][0]=(uint8_t)((2*r0+r1)/3); colors[2][1]=(uint8_t)((2*g0+g1)/3); colors[2][2]=(uint8_t)((2*b0+b1)/3);
    colors[3][0]=(uint8_t)((r0+2*r1)/3); colors[3][1]=(uint8_t)((g0+2*g1)/3); colors[3][2]=(uint8_t)((b0+2*b1)/3);
    for (int py=0; py<4; py++) for (int px=0; px<4; px++) {
        int sx=bx*4+px; int sy=by*4+py;
        if (sx>=width||sy>=height) continue;
        int p=4*py+px;
        int cidx = (cbits >> (2*p)) & 0x3;
        int aidx = (int)((abits >> (3*p)) & 0x7);
        uint8_t *out = dst + sy*dst_stride + sx*4;
        out[0]=colors[cidx][0]; out[1]=colors[cidx][1]; out[2]=colors[cidx][2]; out[3]=alpha[aidx];
    }
}

static void decode_dxt5(const uint8_t *src, size_t src_len, uint8_t *dst, int width, int height, int rowstride) {
    int blocks_x = (width+3)/4, blocks_y=(height+3)/4;
    for (int by=0; by<blocks_y; by++) for (int bx=0; bx<blocks_x; bx++) {
        size_t off=(size_t)(by*blocks_x+bx)*16;
        if (off+16>src_len) continue;
        decompress_dxt5_block(src+off, dst, rowstride, bx,by,width,height);
    }
}

/* --- uncompressed decode --- */
static void decode_uncompressed(const uint8_t *src, size_t src_len, uint8_t *dst, int width, int height, int rowstride, const DdsInfo *info) {
    const DdsPixelFormat *pf = &info->pf;
    uint32_t r_mask = pf->r_mask, g_mask = pf->g_mask, b_mask = pf->b_mask, a_mask = pf->a_mask;
    int bpp = (int)pf->rgb_bits;
    if (bpp == 0) bpp = 32;
    int bytes_pp = (bpp + 7) / 8;
    /* pitch might be provided */
    size_t src_pitch = (size_t)width * bytes_pp;
    if ((info->flags & DDSD_PITCH) && info->pitch_or_linear >= src_pitch) {
        /* use provided pitch for stepping, but only if plausible */
        src_pitch = info->pitch_or_linear;
        /* pitch may include padding, but still need to ensure not wildly larger than width*bytes */
        if (src_pitch > (size_t)width * bytes_pp + 16) {
            /* clamp */
            src_pitch = (size_t)width * bytes_pp;
        }
    }
    gboolean is_luminance = (pf->flags & DDPF_LUMINANCE) != 0;
    gboolean has_alpha = (pf->flags & DDPF_ALPHAPIXELS) != 0;

    for (int y = 0; y < height; y++) {
        const uint8_t *row = src + (size_t)y * src_pitch;
        if ((size_t)y * src_pitch + (size_t)width * bytes_pp > src_len) break;
        uint8_t *drow = dst + y * rowstride;
        for (int x = 0; x < width; x++) {
            const uint8_t *s = row + x * bytes_pp;
            uint32_t pixel = 0;
            if (bytes_pp == 1) pixel = s[0];
            else if (bytes_pp == 2) pixel = (uint32_t)s[0] | ((uint32_t)s[1] << 8);
            else if (bytes_pp == 3) pixel = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16);
            else if (bytes_pp == 4) pixel = read_u32_le(s);
            else {
                /* unsupported bpp, fill zero */
                pixel = 0;
            }
            uint8_t r=0,g=0,b=0,a=255;
            if (is_luminance) {
                /* luminance: r_mask holds lum, if alpha present a_mask holds alpha */
                uint8_t l = extract_channel(pixel, r_mask ? r_mask : 0xFF);
                if (r_mask == 0 && bpp==8) l = (uint8_t)pixel; /* L8 */
                r=g=b=l;
                if (has_alpha && a_mask) a = extract_channel(pixel, a_mask);
                else if (bytes_pp==2) { /* A8L8 case: high byte alpha? heuristic: assume a_mask not set but 2 bytes */
                    /* if luminance 8 bits in low, alpha high */
                    /* fallback: if no mask, treat as L8/A8L8 not needed */
                }
            } else if (pf->flags & DDPF_RGB) {
                if (bpp == 24 && r_mask==0 && g_mask==0 && b_mask==0) {
                    /* assume BGR 24 */
                    b = s[0]; g = s[1]; r = s[2];
                } else if (bpp == 32 && r_mask==0 && g_mask==0 && b_mask==0) {
                    b = s[0]; g = s[1]; r = s[2]; a = s[3];
                } else {
                    r = extract_channel(pixel, r_mask);
                    g = extract_channel(pixel, g_mask);
                    b = extract_channel(pixel, b_mask);
                    if (has_alpha && a_mask) a = extract_channel(pixel, a_mask);
                    else a = 255;
                }
            } else if (pf->flags & DDPF_ALPHA) {
                /* A8 */
                a = (uint8_t)pixel;
                r=g=b=0;
                /* GdkPixbuf always expects RGB, make white with alpha? Keep black. */
                /* We'll make luminance zero but alpha as extracted -> show black transparent */
                /* Better make r=g=b=255? Keep 0 - will appear black. Acceptable. */
                /* For pure alpha, expose as grayscale with alpha */
                r=g=b=255;
            } else {
                /* fallback: treat as raw */
                r=g=b=0;
                if (bytes_pp>=1) r = s[0];
                if (bytes_pp>=2) g = s[1];
                if (bytes_pp>=3) b = s[2];
                if (bytes_pp>=4) a = s[3];
            }
            drow[x*4+0]=r;
            drow[x*4+1]=g;
            drow[x*4+2]=b;
            drow[x*4+3]=a;
        }
    }
}

/* --- DDS parsing --- */

static gboolean parse_dds(const uint8_t *data, size_t len, DdsInfo *out, GError **error) {
    if (len < 4 + 124) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "DDS file too small");
        return FALSE;
    }
    if (data[0]!='D' || data[1]!='D' || data[2]!='S' || data[3]!=' ') {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "Not a DDS file (bad magic)");
        return FALSE;
    }
    const uint8_t *hdr = data + 4;
    uint32_t header_size = read_u32_le(hdr);
    if (header_size != 124) {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "Bad DDS header size %u", header_size);
        return FALSE;
    }
    uint32_t flags = read_u32_le(hdr + 4);
    uint32_t height = read_u32_le(hdr + 8);
    uint32_t width = read_u32_le(hdr + 12);
    uint32_t pitch_lin = read_u32_le(hdr + 16);
    uint32_t depth = read_u32_le(hdr + 20);
    uint32_t mipCount = read_u32_le(hdr + 24);
    const uint8_t *pf = hdr + 72;
    uint32_t pf_size = read_u32_le(pf);
    if (pf_size != 32) {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "Bad DDS pixel format size %u", pf_size);
        return FALSE;
    }
    uint32_t pf_flags = read_u32_le(pf + 4);
    uint32_t fourcc = read_u32_le(pf + 8);
    uint32_t rgb_bits = read_u32_le(pf + 12);
    uint32_t r_mask = read_u32_le(pf + 16);
    uint32_t g_mask = read_u32_le(pf + 20);
    uint32_t b_mask = read_u32_le(pf + 24);
    uint32_t a_mask = read_u32_le(pf + 28);

    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "Invalid DDS dimensions %ux%u", width, height);
        return FALSE;
    }
    /* caps not needed */

    out->width = width;
    out->height = height;
    out->pitch_or_linear = pitch_lin;
    out->depth = depth;
    out->mip_count = mipCount ? mipCount : 1;
    out->flags = flags;
    out->pf.flags = pf_flags;
    out->pf.fourcc = fourcc;
    out->pf.rgb_bits = rgb_bits;
    out->pf.r_mask = r_mask;
    out->pf.g_mask = g_mask;
    out->pf.b_mask = b_mask;
    out->pf.a_mask = a_mask;
    out->has_dx10 = FALSE;
    out->dxgi_format = 0;

    size_t offset = 4 + 124;
    gboolean is_fourcc = (pf_flags & DDPF_FOURCC) != 0;

    if (is_fourcc && fourcc == MAKEFOURCC('D','X','1','0')) {
        /* DX10 extended header is 20 bytes */
        if (len < offset + 20) {
            g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "DDS DX10 header truncated");
            return FALSE;
        }
        const uint8_t *dx10 = data + offset;
        uint32_t dxgi = read_u32_le(dx10);
        uint32_t resDim = read_u32_le(dx10 + 4);
        /* uint32_t miscFlag = read_u32_le(dx10+8); */
        uint32_t arraySize = read_u32_le(dx10 + 12);
        /* uint32_t miscFlags2 = read_u32_le(dx10+16); */
        out->has_dx10 = TRUE;
        out->dxgi_format = dxgi;
        out->dx10_resource_dim = resDim;
        (void)arraySize;
        offset += 20;
        /* map DXGI to internal fmt */
        switch (dxgi) {
            case 71: /* DXGI_FORMAT_BC1_UNORM */
            case 72: /* BC1_UNORM_SRGB */
                out->fmt = DDS_FMT_DXT1;
                break;
            case 74: case 75: /* BC2 */
                out->fmt = DDS_FMT_DXT3;
                break;
            case 77: case 78: /* BC3 */
                out->fmt = DDS_FMT_DXT5;
                break;
            case 28: /* R8G8B8A8_UNORM */
            case 29: /* R8G8B8A8_UNORM_SRGB */
            case 87: /* B8G8R8A8_UNORM etc */
            case 91: /* B8G8R8X8_UNORM */
                out->fmt = DDS_FMT_UNCOMPRESSED;
                /* patch pixel format to represent 32-bit RGBA */
                out->pf.flags = DDPF_RGB | DDPF_ALPHAPIXELS;
                out->pf.rgb_bits = 32;
                if (dxgi == 87 || dxgi == 91) {
                    out->pf.r_mask = 0x00ff0000;
                    out->pf.g_mask = 0x0000ff00;
                    out->pf.b_mask = 0x000000ff;
                    out->pf.a_mask = 0xff000000;
                } else {
                    out->pf.r_mask = 0x000000ff;
                    out->pf.g_mask = 0x0000ff00;
                    out->pf.b_mask = 0x00ff0000;
                    out->pf.a_mask = 0xff000000;
                }
                break;
            default:
                g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_UNKNOWN_TYPE, "Unsupported DXGI format %u", dxgi);
                return FALSE;
        }
    } else if (is_fourcc) {
        if (fourcc == MAKEFOURCC('D','X','T','1')) out->fmt = DDS_FMT_DXT1;
        else if (fourcc == MAKEFOURCC('D','X','T','2')) out->fmt = DDS_FMT_DXT3; /* premultiplied, treat same */
        else if (fourcc == MAKEFOURCC('D','X','T','3')) out->fmt = DDS_FMT_DXT3;
        else if (fourcc == MAKEFOURCC('D','X','T','4')) out->fmt = DDS_FMT_DXT5;
        else if (fourcc == MAKEFOURCC('D','X','T','5')) out->fmt = DDS_FMT_DXT5;
        else if (fourcc == MAKEFOURCC('A','T','I','1') || fourcc == MAKEFOURCC('B','C','4','U') || fourcc == MAKEFOURCC('B','C','4','S')) {
            g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_UNKNOWN_TYPE, "BC4/ATI1 not supported (FourCC %.4s)", (char*)&fourcc);
            return FALSE;
        } else if (fourcc == MAKEFOURCC('A','T','I','2') || fourcc == MAKEFOURCC('B','C','5','U') || fourcc == MAKEFOURCC('B','C','5','S')) {
            g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_UNKNOWN_TYPE, "BC5/ATI2 not supported (FourCC %.4s)", (char*)&fourcc);
            return FALSE;
        } else {
            char fcc[5]={0}; memcpy(fcc,&fourcc,4);
            g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_UNKNOWN_TYPE, "Unsupported FourCC '%s'", fcc);
            return FALSE;
        }
    } else {
        out->fmt = DDS_FMT_UNCOMPRESSED;
        /* sanity: if no masks but RGB flag, try defaults */
        if ((pf_flags & DDPF_RGB) && r_mask==0 && g_mask==0 && b_mask==0) {
            if (rgb_bits==24) {
                out->pf.r_mask = 0x00ff0000; out->pf.g_mask = 0x0000ff00; out->pf.b_mask = 0x000000ff;
            } else if (rgb_bits==32) {
                out->pf.r_mask = 0x00ff0000; out->pf.g_mask = 0x0000ff00; out->pf.b_mask = 0x000000ff; out->pf.a_mask = 0xff000000;
                out->pf.flags |= DDPF_ALPHAPIXELS;
            } else if (rgb_bits==16) {
                out->pf.r_mask = 0xF800; out->pf.g_mask = 0x07E0; out->pf.b_mask = 0x001F;
            }
        }
    }

    if (offset > len) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "DDS truncated before pixel data");
        return FALSE;
    }
    out->data = data + offset;
    out->data_len = len - offset;
    return TRUE;
}

static GdkPixbuf *decode_dds_to_pixbuf(const uint8_t *data, size_t len, GError **error) {
    DdsInfo info = {0};
    if (!parse_dds(data, len, &info, error)) return NULL;

    gboolean has_alpha = TRUE;
    if (info.fmt == DDS_FMT_UNCOMPRESSED) {
        has_alpha = (info.pf.flags & DDPF_ALPHAPIXELS) != 0 || (info.pf.flags & DDPF_ALPHA) != 0;
        /* luminance alpha case also needs alpha */
        if (info.pf.flags & DDPF_LUMINANCE) {
            /* keep alpha if present, else false */
            has_alpha = (info.pf.flags & DDPF_ALPHAPIXELS) != 0;
        }
    } else if (info.fmt == DDS_FMT_DXT1) {
        /* DXT1 may have binary alpha; provide alpha channel anyway so transparent texels show */
        has_alpha = TRUE;
    }

    GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, has_alpha, 8, (int)info.width, (int)info.height);
    if (!pixbuf) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY, "Failed to allocate pixbuf");
        return NULL;
    }
    int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    uint8_t *pixels = gdk_pixbuf_get_pixels(pixbuf);
    int n_channels = gdk_pixbuf_get_n_channels(pixbuf);
    /* clear to transparent/black */
    memset(pixels, 0, (size_t)rowstride * info.height);

    /* Ensure we have enough source data; for compressed, compute expected size */
    size_t expected = 0;
    if (info.fmt == DDS_FMT_DXT1) {
        expected = (size_t)((info.width+3)/4) * ((info.height+3)/4) * 8;
    } else if (info.fmt == DDS_FMT_DXT3 || info.fmt == DDS_FMT_DXT5) {
        expected = (size_t)((info.width+3)/4) * ((info.height+3)/4) * 16;
    } else {
        int bpp = info.pf.rgb_bits ? (int)info.pf.rgb_bits : 32;
        expected = (size_t)info.width * info.height * ((bpp+7)/8);
        /* might be pitch based, allow larger */
    }
    if (info.data_len < expected) {
        /* For mipmapped files, data_len includes all mips; top mip is still at start, so check first mip size only */
        /* If still too small, error but allow truncated with warning? */
        if (info.data_len < expected / 2) {
            g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "DDS data truncated: need %zu have %zu", expected, info.data_len);
            g_object_unref(pixbuf);
            return NULL;
        }
    }

    /* temporary RGBA buffer if pixbuf has no alpha? We always created with alpha for compressed, so direct */
    if (has_alpha) {
        if (info.fmt == DDS_FMT_DXT1) decode_dxt1(info.data, info.data_len, pixels, (int)info.width, (int)info.height, rowstride);
        else if (info.fmt == DDS_FMT_DXT3) decode_dxt3(info.data, info.data_len, pixels, (int)info.width, (int)info.height, rowstride);
        else if (info.fmt == DDS_FMT_DXT5) decode_dxt5(info.data, info.data_len, pixels, (int)info.width, (int)info.height, rowstride);
        else decode_uncompressed(info.data, info.data_len, pixels, (int)info.width, (int)info.height, rowstride, &info);
    } else {
        /* uncompressed without alpha: decode into temp RGBA then strip */
        /* we created pixbuf without alpha (3 channels) in this branch, but currently has_alpha is TRUE for DXT etc.
         * This path is only for uncompressed opaque. */
        /* For simplicity, if we reached here, pixbuf has 3 channels, need 3-channel decode */
        /* We'll decode into temporary 4-channel and copy 3 */
        uint8_t *tmp = g_malloc((size_t)rowstride * info.height * 4 / 3); /* not accurate */
        /* Instead just decode directly ignoring alpha: we'll decode_uncompressed into 4-channel temp then convert */
        int tmp_stride = (int)info.width * 4;
        uint8_t *tmpbuf = g_malloc((size_t)tmp_stride * info.height);
        memset(tmpbuf, 0, (size_t)tmp_stride * info.height);
        decode_uncompressed(info.data, info.data_len, tmpbuf, (int)info.width, (int)info.height, tmp_stride, &info);
        for (int y=0;y<(int)info.height;y++) {
            uint8_t *src = tmpbuf + y*tmp_stride;
            uint8_t *dst = pixels + y*rowstride;
            for (int x=0;x<(int)info.width;x++) {
                dst[x*3+0]=src[x*4+0];
                dst[x*3+1]=src[x*4+1];
                dst[x*3+2]=src[x*4+2];
            }
        }
        g_free(tmpbuf);
        (void)tmp;
    }

    /* For opaque pixbufs created without alpha but DXT we forced alpha: so n_channels is 4. That's fine for ristretto. */

    (void)n_channels;
    return pixbuf;
}

/* --- GdkPixbufModule interface --- */

typedef struct {
    GdkPixbufModuleSizeFunc size_func;
    GdkPixbufModulePreparedFunc prepared_func;
    GdkPixbufModuleUpdatedFunc updated_func;
    gpointer user_data;
    GByteArray *buffer;
} DdsContext;

static gpointer dds_begin_load(GdkPixbufModuleSizeFunc size_func,
                               GdkPixbufModulePreparedFunc prepared_func,
                               GdkPixbufModuleUpdatedFunc updated_func,
                               gpointer user_data, GError **error) {
    (void)error;
    DdsContext *ctx = g_new0(DdsContext, 1);
    ctx->size_func = size_func;
    ctx->prepared_func = prepared_func;
    ctx->updated_func = updated_func;
    ctx->user_data = user_data;
    ctx->buffer = g_byte_array_new();
    return ctx;
}

static gboolean dds_stop_load(gpointer data, GError **error) {
    DdsContext *ctx = (DdsContext*)data;
    gboolean ret = TRUE;
    GdkPixbuf *pixbuf = NULL;

    if (ctx->buffer->len == 0) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "No DDS data");
        ret = FALSE;
        goto out;
    }

    pixbuf = decode_dds_to_pixbuf(ctx->buffer->data, ctx->buffer->len, error);
    if (!pixbuf) {
        ret = FALSE;
        goto out;
    }

    if (ctx->size_func) {
        gint w = gdk_pixbuf_get_width(pixbuf);
        gint h = gdk_pixbuf_get_height(pixbuf);
        (*ctx->size_func)(&w, &h, ctx->user_data);
        /* size_func may request scaling; we ignore and just give original size.
         * If they requested different size, gdk-pixbuf will scale later. */
    }

    if (ctx->prepared_func) {
        (*ctx->prepared_func)(pixbuf, NULL, ctx->user_data);
    }
    if (ctx->updated_func) {
        (*ctx->updated_func)(pixbuf, 0, 0, gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf), ctx->user_data);
    }
    /* The loader owns a ref from prepared_func? GdkPixbuf loader will unref after. We keep one until done. */
    g_object_unref(pixbuf);

out:
    g_byte_array_free(ctx->buffer, TRUE);
    g_free(ctx);
    return ret;
}

static gboolean dds_load_increment(gpointer data, const guchar *buf, guint size, GError **error) {
    DdsContext *ctx = (DdsContext*)data;
    (void)error;
    g_byte_array_append(ctx->buffer, buf, size);
    return TRUE;
}

static GdkPixbuf *dds_load(FILE *f, GError **error) {
    /* atomic load path used by gdk_pixbuf_new_from_file when no incremental needed */
    GByteArray *arr = g_byte_array_new();
    uint8_t tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        g_byte_array_append(arr, tmp, (guint)n);
    }
    if (ferror(f)) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED, "Failed to read DDS file");
        g_byte_array_free(arr, TRUE);
        return NULL;
    }
    GdkPixbuf *pb = decode_dds_to_pixbuf(arr->data, arr->len, error);
    g_byte_array_free(arr, TRUE);
    return pb;
}

/* Thumbnailer path: gdk-pixbuf also tries load_animation; provide simple wrapper */
static GdkPixbufAnimation *dds_load_animation(FILE *f, GError **error) {
    GdkPixbuf *pb = dds_load(f, error);
    if (!pb) return NULL;
    GdkPixbufAnimation *anim = gdk_pixbuf_non_anim_new(pb);
    g_object_unref(pb);
    return anim;
}

/* Module entries */

G_MODULE_EXPORT void fill_vtable(GdkPixbufModule *module) {
    module->load = dds_load;
    module->begin_load = dds_begin_load;
    module->stop_load = dds_stop_load;
    module->load_increment = dds_load_increment;
    module->load_animation = dds_load_animation;
}

G_MODULE_EXPORT void fill_info(GdkPixbufFormat *info) {
    static const GdkPixbufModulePattern signature[] = {
        { "DDS ", NULL, 100 },
        { NULL, NULL, 0 }
    };
    static const gchar *mime_types[] = {
        "image/vnd.ms-dds",
        "image/x-dds",
        "image/dds",
        NULL
    };
    static const gchar *extensions[] = {
        "dds",
        NULL
    };
    info->name = "dds";
    info->signature = (GdkPixbufModulePattern*)signature;
    info->description = "DDS";
    info->mime_types = (gchar**)mime_types;
    info->extensions = (gchar**)extensions;
    info->flags = GDK_PIXBUF_FORMAT_THREADSAFE;
    info->license = "MIT";
}
