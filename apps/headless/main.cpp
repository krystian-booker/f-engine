#include "engine/core/Paths.h"
#include "engine/runtime/Headless.h"
#include "engine/runtime/ReplayHarness.h"
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>

namespace {
void usage() {
    std::cerr << "<game> is a game folder, or its .pak archive\n"
                 "usage: fengine_headless --test <game> [--filter text]\n"
                 "       fengine_headless --check <game>\n"
                 "       fengine_headless --write-api <folder>\n"
                 "       fengine_headless <game> --replay <file> [--seed n] [--user-data dir]\n";
}

// <game> --replay <file>: runs a replay without a window (captures skipped).
int replay(int argc, char** argv) {
    std::filesystem::path game, file, userData;
    std::optional<std::uint64_t> seed;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string(arg) + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--replay") {
            file = value();
        } else if (arg == "--seed") {
            seed = std::stoull(value());
        } else if (arg == "--user-data") {
            userData = value();
        } else if (!arg.starts_with("--") && game.empty()) {
            game = std::string(arg);
        } else {
            throw std::runtime_error("Unknown option " + std::string(arg));
        }
    }
    if (game.empty() || file.empty()) {
        usage();
        return 2;
    }
    return fe::runHeadlessReplay(fe::openGame(game), fe::findLibraryRoot(argv[0]), file, userData, seed);
}
} // namespace

// Window-less entry point for CI and tools.
int main(int argc, char** argv) {
    try {
        const int code = fe::runHeadlessCommand(argc, argv);
        return code >= 0 ? code : replay(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "fengine_headless: " << error.what() << "\n";
        return 1;
    }
}
