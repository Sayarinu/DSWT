#include "durable_save.h"

#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    try {
        if (argc != 3) return 2;
        if (std::string(argv[1]) == "--supports-host") {
            platinumwt::DurableSave save(argv[2]);
            std::cout << (save.SupportsHostDurability() ? "SUPPORTED\n" : "UNSUPPORTED\n");
            return 0;
        }
        std::ifstream input(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        if (bytes.size() != 524288) return 2;
        platinumwt::DurableSave save(argv[1]);
        ::setenv("PWT_FAULT_ARMED", "1", 1); // Only this test binary arms the syscall shim.
        save.Commit(bytes.data(), bytes.size());
        std::cout << "COMMITTED\n";
        return 0;
    } catch (const std::exception &) {
        std::cerr << "FAILED\n";
        return 74;
    }
}
