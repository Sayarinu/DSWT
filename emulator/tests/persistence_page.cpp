#include "wonder_trade/persistence.h"
#include "persistence_page.h"

#include <cstring>
#include <iostream>

// persistence-page CODE...: print each four-letter game code's page address (0: none).
int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strlen(argv[i]) != 4) return 2;
        std::cout << argv[i] << ' ' << dswt::PersistencePageAddress(argv[i]) << '\n';
    }
    return 0;
}
