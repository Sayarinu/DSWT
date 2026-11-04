// Waits for the barrier to attest one position. Exit 0: covered. Exit 1: not within the time limit.
#include "platinumwt/server/wal_archive_barrier.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  platinumwt::server::WalArchiveBarrier archive("", std::chrono::milliseconds(50), std::chrono::milliseconds(200));
  const auto barrier = archive.Barrier();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::stoul(argv[2]));
  const std::uint64_t position = std::stoull(argv[1]);
  while (std::chrono::steady_clock::now() < deadline) {
    if (barrier(position)) return 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return 1;
}
