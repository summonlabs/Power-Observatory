// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace po {

// Exit codes are part of the command line contract.
enum class CliExit : int {
  Ok = 0,
  Usage = 1,
  Refused = 2,
};

// Runs one command. Never throws; every failure is reported on the stream and
// as an exit code.
[[nodiscard]] int run_cli(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err);

}  // namespace po
