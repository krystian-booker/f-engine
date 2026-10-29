#pragma once
#include "engine/core/Json.h"
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace fe {
// Storefront services scripts reach through fe.platform: unlockable
// milestones and numeric stats. A port or game module that talks to a store
// (Steam, a console SDK) implements this interface and hands it to the engine
// (EngineOptions::platform, RuntimeOptions::platform); without one the engine
// keeps them in the user-data folder.
class PlatformServices {
public:
    virtual ~PlatformServices() = default;
    // Backend name for scripts and logs, e.g. "local" or "steam".
    [[nodiscard]] virtual std::string name() const = 0;
    virtual void unlock(const std::string& id) = 0;
    [[nodiscard]] virtual bool unlocked(const std::string& id) const = 0;
    virtual void setStat(const std::string& name, double value) = 0;
    [[nodiscard]] virtual std::optional<double> stat(const std::string& name) const = 0;
    // Called once per tick (store callbacks, batched writes).
    virtual void update() {}
};

// The default backend: <user data>/platform.json holds {"unlocked": {id:
// seconds since epoch}, "stats": {name: value}}. Unlocks are written at once;
// stat changes at most once a second and when the backend is destroyed. An
// empty file path keeps everything in memory.
class LocalPlatformServices final : public PlatformServices {
public:
    explicit LocalPlatformServices(std::filesystem::path file);
    ~LocalPlatformServices() override;
    LocalPlatformServices(const LocalPlatformServices&) = delete;
    LocalPlatformServices& operator=(const LocalPlatformServices&) = delete;

    [[nodiscard]] std::string name() const override { return "local"; }
    void unlock(const std::string& id) override;
    [[nodiscard]] bool unlocked(const std::string& id) const override;
    void setStat(const std::string& name, double value) override;
    [[nodiscard]] std::optional<double> stat(const std::string& name) const override;
    void update() override;

private:
    void write();
    std::filesystem::path file_;
    Json data_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point written_;
};
} // namespace fe
