# smalltalk-a-vm

C17 runtime for [Smalltalk/A](docs/architecture/smalltalk-a-vision-architecture-v3.md) —
a modern Smalltalk system with BEAM-class actor concurrency, a live image,
and a native macOS IDE.

This repository contains the C runtime only.
The Swift IDE lives in `smalltalk-a-ide` (separate repository).

## Build

    cmake -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build

## Test

    cd build && ctest --output-on-failure

## Environment

- macOS Tahoe · Xcode 26.3 · Apple clang 17 · CMake 4.2.3 · arm64
- C17, `-Wall -Wextra -Wpedantic -Werror`

## License

TBD before public release. Candidates: MIT/Apache 2.0 or LGPL.
