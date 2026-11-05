#include "dswt/server/server.h"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
  try {
    dswt::server::RunServer(dswt::server::LoadServerConfig());
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
