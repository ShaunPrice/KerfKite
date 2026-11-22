#pragma once
// Reading an environment variable in the same way with every compiler (MSVC deprecates std::getenv).

#include <cstdlib>
#include <string>

namespace kerfkite {

// The value of the environment variable `name`, or an empty string when it is not set.
inline std::string environment(const char *name) {
#if defined(_MSC_VER)
    char *value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::string();
    std::string text(value);
    std::free(value);
    return text;
#else
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

} // namespace kerfkite
