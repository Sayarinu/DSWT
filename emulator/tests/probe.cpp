#include "durable_save.h"

#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

// probe SAVE IMAGE...: commit each image, then checkpoint. With
// DSWT_PROBE_NO_CHECKPOINT set, exit straight after the commits instead, as a
// killed emulator would, leaving them only in the write log.
int main(int argc, char **argv) {
    try {
        if (argc < 3) return 2;
        if (std::string(argv[1]) == "--supports-host") {
            dswt::DurableSave save(argv[2]);
            std::cout << (save.SupportsHostDurability() ? "SUPPORTED\n" : "UNSUPPORTED\n");
            return 0;
        }
        std::vector<std::vector<std::uint8_t>> images;
        for (int i = 2; i < argc; ++i) {
            std::ifstream input(argv[i], std::ios::binary);
            images.emplace_back(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            if (images.back().size() != 524288) return 2;
        }
        dswt::DurableSave save(argv[1]);
        ::setenv("DSWT_FAULT_ARMED", "1", 1); // Only this test binary arms the syscall shim.
        for (const auto &bytes : images) save.Commit(bytes.data(), bytes.size());
        if (std::getenv("DSWT_PROBE_NO_CHECKPOINT")) {
            std::cout << "COMMITTED" << std::endl;
            ::_exit(0);
        }
        save.Checkpoint();
        std::cout << "COMMITTED\n";
        return 0;
    } catch (const std::exception &) {
        std::cerr << "FAILED\n";
        return 74;
    }
}
