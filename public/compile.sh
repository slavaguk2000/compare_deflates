#!/bin/bash

emcc --bind -O3 -s ALLOW_MEMORY_GROWTH=1 -s USE_ZLIB=1 -o archivers.js code/main.cpp code/zlib/archiver.cpp code/zlib/gzip.cpp \
code/libdeflate/archiver.cpp code/libdeflate/lib/deflate_compress.c code/libdeflate/lib/deflate_decompress.c code/libdeflate/lib/aligned_malloc.c \
code/miniz/archiver.cpp \
-msimd128 