#/bin/sh

CC=gcc

set -x

$CC -std=gnu99 -ggdb -Wall -shared -fPIC -o libwrclient.so -Ilinux64libs/include/ \
	src/main.c src/ogg.c src/mp3.c src/decode_html_ents.c -lm \
	libsrc/libogg/src/bitwise.c libsrc/libogg/src/framing.c \
	libsrc/libvorbis/lib/mdct.c libsrc/libvorbis/lib/smallft.c \
	libsrc/libvorbis/lib/block.c libsrc/libvorbis/lib/envelope.c \
	libsrc/libvorbis/lib/window.c libsrc/libvorbis/lib/lsp.c \
	libsrc/libvorbis/lib/lpc.c libsrc/libvorbis/lib/analysis.c \
	libsrc/libvorbis/lib/synthesis.c libsrc/libvorbis/lib/psy.c \
	libsrc/libvorbis/lib/info.c libsrc/libvorbis/lib/floor1.c \
	libsrc/libvorbis/lib/floor0.c libsrc/libvorbis/lib/res0.c \
	libsrc/libvorbis/lib/mapping0.c libsrc/libvorbis/lib/registry.c \
	libsrc/libvorbis/lib/codebook.c libsrc/libvorbis/lib/sharedbook.c \
	libsrc/libvorbis/lib/lookup.c libsrc/libvorbis/lib/bitrate.c \
	-DOPT_GENERIC \
	libsrc/mpg123/src/libmpg123/compat.c \
	libsrc/mpg123/src/libmpg123/dct64.c \
	libsrc/mpg123/src/libmpg123/equalizer.c \
	libsrc/mpg123/src/libmpg123/feature.c \
	libsrc/mpg123/src/libmpg123/format.c \
	libsrc/mpg123/src/libmpg123/frame.c \
	libsrc/mpg123/src/libmpg123/icy2utf8.c \
	libsrc/mpg123/src/libmpg123/icy.c \
	libsrc/mpg123/src/libmpg123/id3.c \
	libsrc/mpg123/src/libmpg123/index.c \
	libsrc/mpg123/src/libmpg123/layer1.c \
	libsrc/mpg123/src/libmpg123/layer2.c \
	libsrc/mpg123/src/libmpg123/layer3.c \
	libsrc/mpg123/src/libmpg123/lfs_alias.c \
	libsrc/mpg123/src/libmpg123/libmpg123.c \
	libsrc/mpg123/src/libmpg123/ntom.c \
	libsrc/mpg123/src/libmpg123/optimize.c \
	libsrc/mpg123/src/libmpg123/parse.c \
	libsrc/mpg123/src/libmpg123/readers.c \
	libsrc/mpg123/src/libmpg123/stringbuf.c \
	libsrc/mpg123/src/libmpg123/synth_8bit.c \
	libsrc/mpg123/src/libmpg123/synth.c \
	libsrc/mpg123/src/libmpg123/synth_real.c \
	libsrc/mpg123/src/libmpg123/synth_s32.c \
	libsrc/mpg123/src/libmpg123/tabinit.c \
	linux64libs/lib/libcurl.a
