#include "engine/runtime/Headless.h"
#include <exception>
#include <iostream>

// Window-less entry point for CI and tools: --test <game>, --check <game>,
// --write-api <folder>.
int main(int argc, char** argv) {
    try {
        const int code = fe::runHeadlessCommand(argc, argv);
        if (code >= 0) {
            return code;
        }
        std::cerr << "usage: fengine_headless (--test <game> [--filter text] | --check <game> | --write-api "
                     "<folder>)\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "fengine_headless: " << error.what() << "\n";
        return 1;
    }
}
