#include "engine/core/Log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fe {
namespace {
constexpr const char* Labels[] = {"debug", "info", "warning", "error"};
constexpr std::size_t RingLines = 32, RingWidth = 256;

std::mutex& logMutex() {
    static std::mutex mutex;
    return mutex;
}

// Everything the crash handler reads lives in fixed storage: no allocation
// happens between a fault and the report.
struct State {
    std::atomic<int> minimum{FENGINE_DEV ? 0 : 1};
    std::FILE* file = nullptr;
    std::filesystem::path path;
    std::array<std::array<char, RingWidth>, RingLines> ring{};
    std::array<std::size_t, RingLines> ringLength{};
    std::size_t ringNext = 0, ringCount = 0;
    std::array<char, 1024> crashPath{};
    std::shared_ptr<const LogSink> sink; // replaced whole, so a call in flight keeps its copy
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

State& state() {
    static State instance;
    return instance;
}

// Appends to the ring (called with the mutex held).
void remember(State& s, std::string_view line) {
    auto& slot = s.ring[s.ringNext];
    const auto length = std::min(line.size(), RingWidth - 1);
    std::memcpy(slot.data(), line.data(), length);
    slot[length] = '\0';
    s.ringLength[s.ringNext] = length;
    s.ringNext = (s.ringNext + 1) % RingLines;
    s.ringCount = std::min(s.ringCount + 1, RingLines);
}

// Raw, allocation-free write of the ring buffer after a one-line reason.
void writeReport(const char* path, const char* reason) {
    auto& s = state();
#ifdef _WIN32
    const HANDLE file =
        CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const auto put = [&](const char* text, std::size_t length) {
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    };
#else
    const int file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (file < 0) {
        return;
    }
    const auto put = [&](const char* text, std::size_t length) {
        std::size_t done = 0;
        while (done < length) {
            const auto count = write(file, text + done, length - done);
            if (count <= 0) {
                return;
            }
            done += static_cast<std::size_t>(count);
        }
    };
#endif
    put(reason, std::strlen(reason));
    put("\n", 1);
    const std::size_t first = (s.ringNext + RingLines - s.ringCount) % RingLines;
    for (std::size_t i = 0; i < s.ringCount; ++i) {
        const auto index = (first + i) % RingLines;
        put(s.ring[index].data(), s.ringLength[index]);
        put("\n", 1);
    }
#ifdef _WIN32
    FlushFileBuffers(file);
    CloseHandle(file);
#else
    fsync(file);
    close(file);
#endif
}

const char* signalName(int signal) {
    switch (signal) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGFPE:
        return "SIGFPE";
    case SIGILL:
        return "SIGILL";
#ifdef SIGBUS
    case SIGBUS:
        return "SIGBUS";
#endif
    default:
        return "signal";
    }
}

void flushUnlocked(State& s) {
    if (s.file) {
        std::fflush(s.file);
    }
    std::fflush(stderr);
}

extern "C" void onFatalSignal(int signal) {
    auto& s = state();
    // The log file is flushed without the mutex: the faulting thread may hold it.
    flushUnlocked(s);
    char reason[64] = "crash: ";
    std::strncat(reason, signalName(signal), sizeof(reason) - std::strlen(reason) - 1);
    writeReport(s.crashPath.data(), reason);
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

#ifdef _WIN32
LONG WINAPI onStructuredException(EXCEPTION_POINTERS* pointers) {
    auto& s = state();
    flushUnlocked(s);
    char reason[80] = "crash: exception 0x";
    const auto code = pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0u;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(code));
    std::strncat(reason, hex, sizeof(reason) - std::strlen(reason) - 1);
    writeReport(s.crashPath.data(), reason);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
} // namespace

void log(LogLevel level, std::string_view message) {
    auto& s = state();
    if (static_cast<int>(level) < s.minimum.load(std::memory_order_relaxed)) {
        return;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix), "[%8.3f] %-7s ", seconds, Labels[static_cast<int>(level)]);
    std::shared_ptr<const LogSink> sink;
    {
        std::lock_guard lock(logMutex());
        std::fprintf(stderr, "%s%.*s\n", prefix, static_cast<int>(message.size()), message.data());
        if (s.file) {
            std::fprintf(s.file, "%s%.*s\n", prefix, static_cast<int>(message.size()), message.data());
            if (level == LogLevel::Error) {
                std::fflush(s.file);
            }
        }
        std::string line = prefix;
        line.append(message);
        remember(s, line);
        sink = s.sink;
    }
    // Outside the lock, and never from inside itself: a sink that logs gets
    // its record written, not echoed back.
    thread_local bool inSink = false;
    if (sink && !inSink) {
        inSink = true;
        try {
            (*sink)(level, message);
        } catch (...) {}
        inSink = false;
    }
}

void setLogSink(LogSink sink) {
    auto next = sink ? std::make_shared<const LogSink>(std::move(sink)) : nullptr;
    std::lock_guard lock(logMutex());
    state().sink = std::move(next);
}

void setLogLevel(LogLevel minimum) {
    state().minimum.store(static_cast<int>(minimum), std::memory_order_relaxed);
}

LogLevel logLevel() {
    return static_cast<LogLevel>(state().minimum.load(std::memory_order_relaxed));
}

void openLogFile(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    const auto path = folder / "log.txt";
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        auto rotated = path;
        rotated += ".1";
        std::filesystem::remove(rotated, ec);
        std::filesystem::rename(path, rotated, ec);
    }
#ifdef _WIN32
    std::FILE* file = _wfopen(path.c_str(), L"w");
#else
    std::FILE* file = std::fopen(path.c_str(), "w");
#endif
    if (!file) {
        throw std::runtime_error("Cannot create the log file " + path.string());
    }
    std::lock_guard lock(logMutex());
    auto& s = state();
    if (s.file) {
        std::fclose(s.file);
    }
    s.file = file;
    s.path = path;
}

void closeLogFile() {
    std::lock_guard lock(logMutex());
    auto& s = state();
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
    s.path.clear();
}

std::filesystem::path logFilePath() {
    std::lock_guard lock(logMutex());
    return state().path;
}

void flushLog() {
    std::lock_guard lock(logMutex());
    flushUnlocked(state());
}

std::vector<std::string> recentLogLines(std::size_t count) {
    std::lock_guard lock(logMutex());
    auto& s = state();
    count = std::min(count, s.ringCount);
    std::vector<std::string> lines;
    const std::size_t first = (s.ringNext + RingLines - count) % RingLines;
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = (first + i) % RingLines;
        lines.emplace_back(s.ring[index].data(), s.ringLength[index]);
    }
    return lines;
}

std::filesystem::path writeCrashReport(const std::filesystem::path& folder, std::string_view reason) {
    std::filesystem::create_directories(folder);
    const auto path = folder / "crash.txt";
    const auto text = path.string();
    std::string line(reason);
    std::lock_guard lock(logMutex());
    flushUnlocked(state());
    writeReport(text.c_str(), line.c_str());
    return path;
}

void installCrashHandler(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    const auto text = (folder / "crash.txt").string();
    auto& s = state();
    if (text.size() >= s.crashPath.size()) {
        throw std::runtime_error("Crash report path too long: " + text);
    }
    std::memcpy(s.crashPath.data(), text.c_str(), text.size() + 1);
    for (const int signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL
#ifdef SIGBUS
                             ,
                             SIGBUS
#endif
         }) {
        std::signal(signal, onFatalSignal);
    }
#ifdef _WIN32
    SetUnhandledExceptionFilter(onStructuredException);
#endif
}
} // namespace fe
