#include "durable_save.h"
#include "journal_seed.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    try {
        if (argc == 4 && (std::string(argv[1]) == "inspect" || std::string(argv[1]) == "seed")) {
            if (std::string(argv[1]) == "inspect") platinumwt::InspectJournal(argv[2], argv[3]);
            else platinumwt::RefreshJournalSeed(argv[2], argv[3]);
            std::cout << "Journal seed verified\n";
            return 0;
        }
        if (argc != 3 || (std::string(argv[1]) != "prepare" && std::string(argv[1]) != "check")) return 2;
        platinumwt::DurableSave save(argv[2], std::string(argv[1]) == "prepare" ?
            platinumwt::DurableSave::Mode::Recover : platinumwt::DurableSave::Mode::Existing);
        std::cout << "Save store verified\n";
        return 0;
    } catch (const std::exception &) {
        std::cerr << "Durable save unavailable; preserve the save and recovery directory\n";
        return 74;
    }
}
