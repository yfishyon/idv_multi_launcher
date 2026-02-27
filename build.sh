#!/bin/bash
set -e
x86_64-w64-mingw32-windres idv_multi_launcher.rc --output-format=coff -o idv_multi_launcher.res
x86_64-w64-mingw32-g++ idv_multi_launcher.cpp idv_multi_launcher.res -o idv_multi_launcher.exe -static -mwindows -lpsapi -ladvapi32
echo "Build OK: idv_multi_launcher.exe"
