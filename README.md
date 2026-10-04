# C1M

Working from the C10k problem up to one million concurrent connections, to learn networking.  Built from [cpp-starter](https://github.com/jfreun123/cpp-starter).

## Prerequisites

Install on macOS (Homebrew):
```bash
brew install cmake llvm cppcheck ninja
# llvm is keg-only; symlink clang-tidy into PATH:
ln -sf /usr/local/opt/llvm/bin/clang-tidy /usr/local/bin/clang-tidy
```

Install on Ubuntu/Debian:
```bash
sudo apt-get install cmake clang clang-tidy cppcheck ninja-build
```

## Building and Testing

```bash
# Configure (Debug build with static analysis)
cmake --preset=dev

# Build
cmake --build --preset=dev

# Run tests
ctest --preset=dev
```

## Sanitizers

```bash
# AddressSanitizer
cmake --preset=asan && cmake --build --preset=asan && ctest --preset=asan

# ThreadSanitizer
cmake --preset=tsan && cmake --build --preset=tsan && ctest --preset=tsan
```

## Benchmark

```bash
cmake --preset=bench
cmake --build --preset=bench
./build-bench/benchmark/benchmark
```

## Code Formatting

```bash
# Check formatting
cmake --build build -t format-check

# Fix formatting
cmake --build build -t format-fix
```

## Project Structure

```
C1M/
├── source/                  # Header-only library
│   ├── CMakeLists.txt
│   └── add.hpp              # Replace with your headers
├── test/                    # Unit tests (Google Test)
│   ├── CMakeLists.txt
│   └── source/
│       └── add_test.cpp
├── benchmark/               # Benchmarks
│   ├── CMakeLists.txt
│   └── source/
│       └── benchmark.cpp
├── cmake/                   # CMake modules
│   ├── dev-mode.cmake
│   ├── gtest.cmake
│   └── lint-targets.cmake
├── .clang-tidy
├── .clang-format
├── CMakeLists.txt
└── CMakePresets.json        # dev, asan, tsan, bench presets
```
