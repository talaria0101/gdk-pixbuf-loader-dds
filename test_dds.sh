#!/bin/sh
set -e
echo "==> generating test DDS images"
python3 gen_dds.py

echo "==> building test harness"
# compile test if not already built - reuse /tmp if needed
TMPDIR=/tmp
CC=${CC:-gcc}
PKG_CFLAGS=$(pkg-config --cflags gdk-pixbuf-2.0 2>/dev/null || PKG_CONFIG_PATH=/tmp/prefix/usr/lib64/pkgconfig pkg-config --cflags gdk-pixbuf-2.0)
PKG_LIBS=$(pkg-config --libs gdk-pixbuf-2.0 2>/dev/null || PKG_CONFIG_PATH=/tmp/prefix/usr/lib64/pkgconfig pkg-config --libs gdk-pixbuf-2.0)

mkdir -p /tmp/dds_loaders
cp libpixbufloader-dds.so /tmp/dds_loaders/
GDK_PIXBUF_MODULEDIR=/tmp/dds_loaders gdk-pixbuf-query-loaders > /tmp/dds_loaders/loaders.cache

cat > /tmp/test_all_dds.c <<'EOF'
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
void test_file(const char *path, int exp_w, int exp_h) {
    GError *err=NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (!pb) { printf("FAIL %s: %s\n", path, err?err->message:"unknown"); if(err) g_error_free(err); exit(1); }
    int w=gdk_pixbuf_get_width(pb), h=gdk_pixbuf_get_height(pb);
    if (w!=exp_w || h!=exp_h) { printf("FAIL %s: got %dx%d expected %dx%d\n", path,w,h,exp_w,exp_h); exit(1); }
    printf("PASS %s: %dx%d\n", path,w,h);
    g_object_unref(pb);
}
int main(){
    GError *err=NULL;
    if(!gdk_pixbuf_init_modules("/tmp/dds_loaders",&err)){printf("init_modules failed %s\n",err->message); return 1;}
    test_file("/tmp/test-images/dxt1_4x4.dds",4,4);
    test_file("/tmp/test-images/dxt1_8x8.dds",8,8);
    test_file("/tmp/test-images/dxt3_4x4.dds",4,4);
    test_file("/tmp/test-images/dxt5_4x4.dds",4,4);
    test_file("/tmp/test-images/rgba_4x4.dds",4,4);
    test_file("/tmp/test-images/rgb24_4x4.dds",4,4);
    test_file("/tmp/test-images/r5g6b5_4x4.dds",4,4);
    test_file("/tmp/test-images/l8_4x4.dds",4,4);
    test_file("/tmp/test-images/dx10_bc1_4x4.dds",4,4);
    test_file("/tmp/test-images/dxt1_5x3.dds",5,3);
    printf("all tests passed\n");
    return 0;
}
EOF
$CC /tmp/test_all_dds.c -o /tmp/test_all_dds $PKG_CFLAGS $PKG_LIBS
/tmp/test_all_dds
echo "==> thumbnailer test"
GDK_PIXBUF_MODULE_FILE=/tmp/dds_loaders/loaders.cache gdk-pixbuf-thumbnailer -s 64 /tmp/test-images/dxt1_8x8.dds /tmp/thumb.png
file /tmp/thumb.png
echo "==> ristretto check: ristretto uses gdk-pixbuf, so if thumbnails work, ristretto will open DDS"
echo "all good"
