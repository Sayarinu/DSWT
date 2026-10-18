#include "durable_save.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    try {
        if (argc != 3 || (std::string(argv[1]) != "prepare" && std::string(argv[1]) != "check" && std::string(argv[1]) != "refresh")) return 2;
        platinumwt::DurableSave save(argv[2], std::string(argv[1]) == "prepare" ?
            platinumwt::DurableSave::Mode::Recover : platinumwt::DurableSave::Mode::Existing);
        if (std::string(argv[1]) == "refresh") {
            std::vector<std::uint8_t> data(524288);
            std::cin.read(reinterpret_cast<char *>(data.data()), data.size());
            if (std::cin.gcount() != static_cast<std::streamsize>(data.size()) || std::cin.peek() != std::char_traits<char>::eof()) return 2;
            save.Commit(data.data(), data.size());
        }
        std::cout << "Save store verified\n";
        return 0;
    } catch (const std::exception &) {
        std::cerr << "Durable save unavailable; preserve the save and recovery directory\n";
        return 74;
    }
}
