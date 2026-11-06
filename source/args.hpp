#pragma once

#include <charconv>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace C1M {

// Parse a whole string as a number. Returns nullopt if any of it is not one.
template <typename T>
std::optional<T> parseNumber(std::string_view text) noexcept {
  T value{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char *const end = text.data() + text.size();
  const auto [parsed, error] = std::from_chars(text.data(), end, value);
  if (error != std::errc{} || parsed != end) {
    return std::nullopt;
  }
  return value;
}

// Parse `text` into `target`. Returns false, leaving `target` alone, if it is
// not a number.
template <typename T>
bool assignNumber(T &target, std::string_view text) noexcept {
  const std::optional<T> value = parseNumber<T>(text);
  if (value) {
    target = *value;
  }
  return value.has_value();
}

using Flag = std::pair<std::string_view, std::string_view>;

// Split `--name value --name value ...` into pairs. Returns nullopt if an
// argument that should be a flag is not one, or the last flag has no value.
inline std::optional<std::vector<Flag>>
parseFlags(std::span<const char *const> args) {
  if (args.size() % 2 != 0) {
    return std::nullopt;
  }
  std::vector<Flag> flags;
  for (size_t i = 0; i < args.size(); i += 2) {
    const std::string_view name = args[i];
    if (!name.starts_with("--")) {
      return std::nullopt;
    }
    flags.emplace_back(name, args[i + 1]);
  }
  return flags;
}

} // namespace C1M
