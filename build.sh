if [[ ! -d build ]]; then
 mkdir build
 mkdir build/debug
 mkdir build/release
fi

#gcc -g src/main.c -o build/debug/main.exe
# ggdb debugging info for gdb debugger
# -std=gnull enable C99 inline semantics
gcc -std=gnu11 -ggdb src/main.c -o build/debug/main.exe

#gcc -g src/test.c -o build/debug/test.exe
