#!/bin/sh
# Build both binaries into dist/
#   dist/ConservationMode      Linux x86_64 (needs only glibc + GTK 3 at runtime)
#   dist/ConservationMode.exe  Windows x64 (static, no runtime needed)
#
# Build requirements (Ubuntu): sudo apt install gcc libgtk-3-dev gcc-mingw-w64-x86-64
set -e
cd "$(dirname "$0")"
mkdir -p dist

echo "== Linux"
gcc -O2 -Wall -Wextra -o dist/ConservationMode src/conservation_mode_linux.c \
    $(pkg-config --cflags --libs gtk+-3.0) -lm -Wl,--as-needed
strip dist/ConservationMode

echo "== Windows"
x86_64-w64-mingw32-windres -I src src/app.rc -O coff -o src/app.res
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static -s \
    -o dist/ConservationMode.exe src/conservation_mode_win.c src/app.res \
    -lcomctl32 -lshell32 -lsetupapi -lgdiplus
rm -f src/app.res

ls -l dist
