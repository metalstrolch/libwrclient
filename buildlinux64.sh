#/bin/sh

CC=gcc

set -x

$CC -std=gnu99 -ggdb -Wall -shared -fPIC -o libwrclient.so -Ilinux64libs/include/ \
	src/main.c src/ogg.c src/mp3.c src/decode_html_ents.c -lm \
	libsrc/libogg-1.3.2/src/bitwise.c libsrc/libogg-1.3.2/src/framing.c \
	libsrc/libvorbis-1.3.5/lib/mdct.c libsrc/libvorbis-1.3.5/lib/smallft.c \
	libsrc/libvorbis-1.3.5/lib/block.c libsrc/libvorbis-1.3.5/lib/envelope.c \
	libsrc/libvorbis-1.3.5/lib/window.c libsrc/libvorbis-1.3.5/lib/lsp.c \
	libsrc/libvorbis-1.3.5/lib/lpc.c libsrc/libvorbis-1.3.5/lib/analysis.c \
	libsrc/libvorbis-1.3.5/lib/synthesis.c libsrc/libvorbis-1.3.5/lib/psy.c \
	libsrc/libvorbis-1.3.5/lib/info.c libsrc/libvorbis-1.3.5/lib/floor1.c \
	libsrc/libvorbis-1.3.5/lib/floor0.c libsrc/libvorbis-1.3.5/lib/res0.c \
	libsrc/libvorbis-1.3.5/lib/mapping0.c libsrc/libvorbis-1.3.5/lib/registry.c \
	libsrc/libvorbis-1.3.5/lib/codebook.c libsrc/libvorbis-1.3.5/lib/sharedbook.c \
	libsrc/libvorbis-1.3.5/lib/lookup.c libsrc/libvorbis-1.3.5/lib/bitrate.c \
	-DOPT_GENERIC \
	libsrc/mpg123-1.22.2/src/libmpg123/compat.c \
	libsrc/mpg123-1.22.2/src/libmpg123/dct64.c \
	libsrc/mpg123-1.22.2/src/libmpg123/equalizer.c \
	libsrc/mpg123-1.22.2/src/libmpg123/feature.c \
	libsrc/mpg123-1.22.2/src/libmpg123/format.c \
	libsrc/mpg123-1.22.2/src/libmpg123/frame.c \
	libsrc/mpg123-1.22.2/src/libmpg123/icy2utf8.c \
	libsrc/mpg123-1.22.2/src/libmpg123/icy.c \
	libsrc/mpg123-1.22.2/src/libmpg123/id3.c \
	libsrc/mpg123-1.22.2/src/libmpg123/index.c \
	libsrc/mpg123-1.22.2/src/libmpg123/layer1.c \
	libsrc/mpg123-1.22.2/src/libmpg123/layer2.c \
	libsrc/mpg123-1.22.2/src/libmpg123/layer3.c \
	libsrc/mpg123-1.22.2/src/libmpg123/lfs_alias.c \
	libsrc/mpg123-1.22.2/src/libmpg123/libmpg123.c \
	libsrc/mpg123-1.22.2/src/libmpg123/ntom.c \
	libsrc/mpg123-1.22.2/src/libmpg123/optimize.c \
	libsrc/mpg123-1.22.2/src/libmpg123/parse.c \
	libsrc/mpg123-1.22.2/src/libmpg123/readers.c \
	libsrc/mpg123-1.22.2/src/libmpg123/stringbuf.c \
	libsrc/mpg123-1.22.2/src/libmpg123/synth_8bit.c \
	libsrc/mpg123-1.22.2/src/libmpg123/synth.c \
	libsrc/mpg123-1.22.2/src/libmpg123/synth_real.c \
	libsrc/mpg123-1.22.2/src/libmpg123/synth_s32.c \
	libsrc/mpg123-1.22.2/src/libmpg123/tabinit.c \
	linux64libs/lib/libcurl.a
