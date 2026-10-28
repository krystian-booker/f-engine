#include "engine/core/Log.h"
#include <chrono>
#include <cstdio>
#include <mutex>

namespace fe {
void log(LogLevel level, std::string_view message) {
    static std::mutex mutex;
    static const auto start = std::chrono::steady_clock::now();
    constexpr const char* labels[] = {"debug", "info", "warning", "error"};
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard lock(mutex);
    std::fprintf(stderr, "[%8.3f] %-7s %.*s\n", seconds, labels[static_cast<int>(level)],
                 static_cast<int>(message.size()), message.data());
}
} // namespace fe
