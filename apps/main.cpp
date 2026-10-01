// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <iostream>
#include <string>
#include <vector>

#include "power_observatory/cli.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return po::run_cli(arguments, std::cout, std::cerr);
}
