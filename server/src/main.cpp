#include "platinumwt/server/server.h"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
  try {
    platinumwt::server::RunServer(platinumwt::server::LoadServerConfig());
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
