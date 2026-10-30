#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fe {
enum class LogLevel { Debug, Info, Warning, Error };
// Thread-safe. Every message below the minimum level is dropped; the rest go
// to stderr and, once openLogFile ran, to the log file, which is flushed after
// every error. The last few lines are kept for crash reports (recentLogLines).
void log(LogLevel level, std::string_view message);
// The minimum level written: Debug in development builds, Info otherwise,
// until a call changes it.
void setLogLevel(LogLevel minimum);
[[nodiscard]] LogLevel logLevel();
// A hook called for every record that passes the level filter, with the
// level and the message (no time prefix), from the logging thread after the
// record was written; telemetry and crash reporters. It runs outside the log
// lock, so it may take its time, and a record it logs itself is written but
// not passed back to it. A null sink removes the hook.
using LogSink = std::function<void(LogLevel, std::string_view)>;
void setLogSink(LogSink sink);
// Writes to <folder>/log.txt from now on. An existing log.txt is rotated once
// to log.txt.1 (replacing the previous .1). Throws when the file cannot be
// created; the stderr sink is unaffected.
void openLogFile(const std::filesystem::path& folder);
void closeLogFile();
[[nodiscard]] std::filesystem::path logFilePath();
void flushLog();
// The most recent lines written, oldest first (at most `count`, up to 32).
[[nodiscard]] std::vector<std::string> recentLogLines(std::size_t count = 32);
// Installs handlers for fatal signals (and structured exceptions on Windows)
// that flush the log and write <folder>/crash.txt: one line naming the signal
// followed by the last 32 log lines. No stack walking; the handler uses only
// pre-allocated buffers and raw file writes, then lets the default action run.
void installCrashHandler(const std::filesystem::path& folder);
// Writes <folder>/crash.txt as the handler would, for `reason`; returns the
// file written. Tests the crash report without crashing.
std::filesystem::path writeCrashReport(const std::filesystem::path& folder, std::string_view reason);
} // namespace fe
