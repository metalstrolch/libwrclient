#/bin/sh

CC=gcc

set -x

$CC -std=gnu99 -ggdb -Wall -shared -fPIC -o libwrclient.so -Ilinux64libs/libs/include/ src/main.c src/ogg.c src/mp3.c src/decode_html_ents.c -lm linux64libs/lib/libvorbis.a linux64libs/lib/libogg.a linux64libs/lib/libmpg123.a linux64libs/lib/libcurl.a
