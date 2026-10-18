#include "platinumwt/server/ledger_store.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc < 2 || (std::string(argv[1]) == "migrate"
                       ? argc != 3
                       : std::string(argv[1]) != "verify" || argc != 2)) {
    std::cerr << "Usage: platinumwt-ledger migrate DIRECTORY | verify\n";
    return 2;
  }
  try {
    const char *info = std::getenv("PLATINUMWT_DATABASE_URL");
    platinumwt::server::LedgerStore ledger(info ? info : "");
    if (std::string(argv[1]) == "migrate") {
      ledger.Migrate(argv[2]);
      std::cout << "Ledger migrations verified and applied\n";
    } else {
      const auto count = ledger.VerifyReplay();
      std::cout << "Verified journal events: " << count << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
