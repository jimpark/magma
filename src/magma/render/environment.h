#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

namespace magma::render {

// MSVC rejects std::getenv, so this wraps the portable form.
[[nodiscard]] inline auto environment_equals(char const *name, std::string_view expected) -> bool {
#if defined(_WIN32)
  char *raw = nullptr;
  auto size = std::size_t{0};
  if (_dupenv_s(&raw, &size, name) != 0 || raw == nullptr) {
    return false;
  }
  auto const value = std::string{raw};
  std::free(raw);
  return value == expected;
#else
  auto const *const value = std::getenv(name);
  return value != nullptr && std::string_view{value} == expected;
#endif
}

} // namespace magma::render
