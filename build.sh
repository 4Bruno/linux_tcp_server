if [[ ! -d build ]]; then
 mkdir build
 mkdir build/debug
 mkdir build/release
fi

gcc src/main.c -o build/debug/main.exe

