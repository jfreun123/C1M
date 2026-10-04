// starter
#include "add.hpp"

// std
#include <cstdint>
#include <exception>
#include <iostream>

int main() {
  try {
    constexpr int kIterations = 1'000'000;
    std::uint64_t result = 0;
    for (int i = 0; i < kIterations; ++i) {
      result += static_cast<std::uint64_t>(starter::add(i, i + 1));
    }
    std::cout << "iterations=" << kIterations << " result=" << result << "\n";
  } catch (const std::exception &e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
