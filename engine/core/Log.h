#pragma once
#include <string_view>

namespace fe {
enum class LogLevel { Debug, Info, Warning, Error };
void log(LogLevel level, std::string_view message);
} // namespace fe
