# cpp-starter

A C++23 project template with CMake, Google Test, clang-tidy, cppcheck, clang-format, ASAN/TSAN, and GitHub Actions CI.

## Using This Template

1. Click **Use this template** on GitHub (or `gh repo create --template jacobfreund/cpp-starter`)
2. Clone your new repo
3. Rename `cpp-starter` → your project name in:
   - `CMakeLists.txt` (`project(...)` and the `DEVELOPER_MODE` option)
   - `CMakePresets.json` (the `cpp-starter_DEVELOPER_MODE` cache variable)
   - `source/CMakeLists.txt` (the `cpp-starter_lib` target)
   - `test/CMakeLists.txt` and `benchmark/CMakeLists.txt` (link targets)
4. Replace `source/add.hpp`, `test/source/add_test.cpp`, and `benchmark/source/benchmark.cpp` with your code

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
cpp-starter/
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
