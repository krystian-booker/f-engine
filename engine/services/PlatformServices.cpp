#include "engine/services/PlatformServices.h"
#include "engine/core/Log.h"
#include <cmath>
#include <stdexcept>

namespace fe {
namespace {
void checkName(const std::string& name, const char* what) {
    if (name.empty() || name.size() > 128) {
        throw std::invalid_argument(std::string(what) + " names are 1 to 128 characters");
    }
}
} // namespace

LocalPlatformServices::LocalPlatformServices(std::filesystem::path file,
                                             std::chrono::milliseconds writeInterval)
    : file_(std::move(file)), writeInterval_(writeInterval),
      data_({{"unlocked", Json::object()}, {"stats", Json::object()}}) {
    std::error_code ec;
    if (file_.empty() || !std::filesystem::exists(file_, ec)) {
        return;
    }
    try {
        const auto stored = readJsonOrBackup(file_);
        for (const char* key : {"unlocked", "stats"}) {
            if (stored.contains(key) && stored[key].is_object()) {
                data_[key] = stored[key];
            }
        }
    } catch (const std::exception& error) {
        log(LogLevel::Error, std::string("Platform data unreadable, starting empty: ") + error.what());
    }
}

LocalPlatformServices::~LocalPlatformServices() {
    if (dirty_) {
        try {
            write();
        } catch (const std::exception& error) {
            log(LogLevel::Error, std::string("Cannot write platform data: ") + error.what());
        }
    }
}

void LocalPlatformServices::unlock(const std::string& id) {
    checkName(id, "Unlock");
    if (unlocked(id)) {
        return;
    }
    data_["unlocked"][id] = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    write();
}

bool LocalPlatformServices::unlocked(const std::string& id) const {
    return data_["unlocked"].contains(id);
}

void LocalPlatformServices::setStat(const std::string& name, double value) {
    checkName(name, "Stat");
    if (!std::isfinite(value)) {
        throw std::invalid_argument("Stat '" + name + "' must be a finite number");
    }
    auto& stats = data_["stats"];
    // A stored value that is not a number (an edited or foreign file) counts
    // as 0 and is overwritten.
    const auto found = stats.find(name);
    if (found != stats.end() && found->is_number() && found->get<double>() == value) {
        return;
    }
    stats[name] = value;
    dirty_ = true;
}

std::optional<double> LocalPlatformServices::stat(const std::string& name) const {
    const auto& stats = data_["stats"];
    const auto found = stats.find(name);
    if (found == stats.end() || !found->is_number()) {
        return std::nullopt;
    }
    return found->get<double>();
}

void LocalPlatformServices::update() {
    if (dirty_ && std::chrono::steady_clock::now() - written_ >= writeInterval_) {
        write();
    }
}

void LocalPlatformServices::flush() {
    if (dirty_) {
        write();
    }
}

void LocalPlatformServices::write() {
    dirty_ = false;
    written_ = std::chrono::steady_clock::now();
    if (file_.empty()) {
        return;
    }
    std::filesystem::create_directories(file_.parent_path());
    writeJson(file_, data_, true);
}
} // namespace fe
