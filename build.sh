#!/bin/sh
# Cross-compile the Windows exe from macOS/Linux with mingw-w64.
#   brew install mingw-w64      (macOS)
set -e
cd "$(dirname "$0")"

CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
OUT=dist/LegendsNudge.exe

mkdir -p dist build
$WINDRES -I res res/app.rc -O coff -o build/app.res
$CC -O2 -Wall -Wextra -Wno-unused-parameter \
    -mwindows -static -static-libgcc \
    -o "$OUT" src/main.c build/app.res \
    -lwinmm -luser32 -lgdi32 -lshell32 -ladvapi32 -lcomctl32
x86_64-w64-mingw32-strip "$OUT" 2>/dev/null || true
ls -l "$OUT"
