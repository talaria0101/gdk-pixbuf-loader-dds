#!/usr/bin/env python3
import struct
import os

def make_fourcc(s): return struct.unpack('<I', s.encode('ascii'))[0]
def write_dds(path, width, height, pf_flags, fourcc, rgb_bits, rmask, gmask, bmask, amask, data, dx10=None, pitch=None):
    # header 124 bytes
    flags = 0x1 | 0x2 | 0x4 | 0x1000  # caps|height|width|pixelformat
    if fourcc:
        flags |= 0x80000  # linear size
        pitch_val = len(data)  # approximate
    else:
        flags |= 0x8  # pitch
        pitch_val = pitch if pitch is not None else width * ((rgb_bits+7)//8)
    hdr = bytearray(124)
    struct.pack_into('<I', hdr, 0, 124)
    struct.pack_into('<I', hdr, 4, flags)
    struct.pack_into('<I', hdr, 8, height)
    struct.pack_into('<I', hdr, 12, width)
    struct.pack_into('<I', hdr, 16, pitch_val)
    struct.pack_into('<I', hdr, 20, 0)  # depth
    struct.pack_into('<I', hdr, 24, 1)  # mipmaps
    # reserved1 44 bytes zero already
    # pixel format 32 bytes at 72
    struct.pack_into('<I', hdr, 72, 32)
    struct.pack_into('<I', hdr, 76, pf_flags)
    struct.pack_into('<I', hdr, 80, fourcc if fourcc else 0)
    struct.pack_into('<I', hdr, 84, rgb_bits)
    struct.pack_into('<I', hdr, 88, rmask)
    struct.pack_into('<I', hdr, 92, gmask)
    struct.pack_into('<I', hdr, 96, bmask)
    struct.pack_into('<I', hdr,100, amask)
    struct.pack_into('<I', hdr,104, 0x1000) # caps
    # rest zero
    with open(path,'wb') as f:
        f.write(b'DDS ')
        f.write(hdr)
        if dx10 is not None:
            f.write(dx10)
        f.write(data)

os.makedirs('/tmp/test-images', exist_ok=True)

# DXT1 4x4 all red
# block: c0=F800 red, c1=07E0 green, bits=0
dxt1_block = struct.pack('<HHI', 0xF800, 0x07E0, 0x00000000)
# 4x4 image = 1 block
write_dds('/tmp/test-images/dxt1_4x4.dds', 4,4, 0x4, make_fourcc('DXT1'),0,0,0,0,0, dxt1_block)
print("dxt1_4x4")

# DXT1 8x8 checker: 4 blocks
# top-left red, top-right green, bottom-left blue, bottom-right white? Use simple
# encode blocks
def pack_dxt1(c0,c1,bits):
    return struct.pack('<HHI', c0,c1,bits)
# colors 565: red F800, green 07E0, blue 001F, white FFFF
blocks = b''.join([
    pack_dxt1(0xF800,0x0000,0), # red
    pack_dxt1(0x07E0,0x0000,0), # green
    pack_dxt1(0x001F,0x0000,0), # blue
    pack_dxt1(0xFFFF,0x0000,0), # white
])
write_dds('/tmp/test-images/dxt1_8x8.dds', 8,8, 0x4, make_fourcc('DXT1'),0,0,0,0,0, blocks)
print("dxt1_8x8")

# DXT5 4x4 semi-transparent red (alpha gradient)
# alpha block: a0=255, a1=0, indices: create gradient
# alpha: a0 FF, a1 00, then 6 bytes of indices: pack 3 bits per pixel
# For 4x4, indices 0..7 repeating gives gradient
# Use a0=255, a1=0 => alpha table: 255,0,205,154,102,51,0,255? Wait with a0>a1 table is 255,0,219,182,146,109,73,36 roughly? Actually formula: 6*a0+1*a1 /7 etc.
# We'll just set a0=0xFF, a1=0x00, and indices all 0 => alpha 255 opaque
alpha_block = struct.pack('BB', 0xFF, 0x00) + b'\x00'*6
color_block = struct.pack('<HHI', 0xF800, 0x0000, 0)
dxt5_block = alpha_block + color_block
write_dds('/tmp/test-images/dxt5_4x4.dds', 4,4, 0x4, make_fourcc('DXT5'),0,0,0,0,0, dxt5_block)
print("dxt5_4x4")

# DXT3 4x4 explicit alpha: checker alpha vs color
# first 8 bytes alpha explicit: 4 bits per pixel, each byte packs 2 pixels
# Use alpha 0xFF for opaque? 0xFF nibble = 15*17=255
# We'll set all alpha = 0xFF
dxt3_alpha = b'\xff'*8
dxt3_color = struct.pack('<HHI', 0x07E0, 0x0000, 0)
dxt3_block = dxt3_alpha + dxt3_color
write_dds('/tmp/test-images/dxt3_4x4.dds', 4,4, 0x4, make_fourcc('DXT3'),0,0,0,0,0, dxt3_block)
print("dxt3_4x4")

# Uncompressed 32-bit A8R8G8B8 4x4 gradient
# masks: R 00FF0000, G 0000FF00, B 000000FF, A FF000000, bpp 32
data = bytearray()
for y in range(4):
    for x in range(4):
        r = x*64
        g = y*64
        b = 128
        a = 255
        pixel = (a<<24)|(r<<16)|(g<<8)|b
        data += struct.pack('<I', pixel)
write_dds('/tmp/test-images/rgba_4x4.dds', 4,4, 0x40|0x1, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000, data)
print("rgba_4x4")

# Uncompressed 24-bit R8G8B8 4x4
data24 = bytearray()
for y in range(4):
    for x in range(4):
        r = x*64
        g = y*64
        b = 128
        data24 += struct.pack('BBB', b,g,r)  # BGR order for masks? Actually file stores pixel as R<<16|G<<8|B but little endian bytes are B,G,R. We'll write B,G,R.
write_dds('/tmp/test-images/rgb24_4x4.dds', 4,4, 0x40, 0, 24, 0x00ff0000, 0x0000ff00, 0x000000ff, 0, data24)
print("rgb24_4x4")

# 16-bit R5G6B5 4x4
data16 = bytearray()
for y in range(4):
    for x in range(4):
        r5 = (x*8) & 0x1F
        g6 = (y*8) & 0x3F
        b5 = 0x10
        pixel = (r5<<11)|(g6<<5)|b5
        data16 += struct.pack('<H', pixel)
write_dds('/tmp/test-images/r5g6b5_4x4.dds', 4,4, 0x40, 0, 16, 0xF800, 0x07E0, 0x001F, 0, data16)
print("r5g6b5_4x4")

# L8 4x4 luminance
dataL = bytearray([ (x+y)*32 for y in range(4) for x in range(4)])
write_dds('/tmp/test-images/l8_4x4.dds', 4,4, 0x20000, 0, 8, 0xFF,0,0,0, dataL)
print("l8_4x4")

# DX10 BC1 4x4 (DXGI 71)
dx10_hdr = struct.pack('<IIIII', 71, 3, 0, 1, 0)  # dxgi, dim 3=texture2d, misc, arraysize 1, misc2
write_dds('/tmp/test-images/dx10_bc1_4x4.dds', 4,4, 0x4, make_fourcc('DX10'),0,0,0,0,0, dxt1_block, dx10=dx10_hdr)
print("dx10_bc1")

# Non-power-of-two 5x3 DXT1
# ceil(5/4)=2, ceil(3/4)=1 => 2 blocks
blocks_5x3 = pack_dxt1(0xF800,0,0) + pack_dxt1(0x07E0,0,0)
write_dds('/tmp/test-images/dxt1_5x3.dds', 5,3, 0x4, make_fourcc('DXT1'),0,0,0,0,0, blocks_5x3)
print("5x3")

print("done")
# list
for f in sorted(os.listdir('/tmp/test-images')):
    print(f, os.path.getsize(os.path.join('/tmp/test-images',f)))
