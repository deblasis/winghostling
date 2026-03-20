# Ghostling - Minimal libghostty Terminal

Ghostling is a demo project meant to highlight a minimum
functional terminal built on libghostty. This can then be used by
other people interested in libghostty to build out their own, higher level,
more functional terminals.

> [!IMPORTANT]
>
> The Ghostling terminal isn't meant to be a full featured, daily use
> terminal. It is a minimal viable terminal based on libghostty.

## Building

Requires CMake 3.11+ and a C compiler. Raylib is fetched automatically
via CMake's FetchContent if not already installed.

```sh
cmake -B build -G Ninja
cmake --build build
./build/ghostling
```

For a release (optimized) build:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

After the initial configure, you only need to run the build step:

```sh
cmake --build build
```
