// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/cli.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "power_observatory/attribution.hpp"
#include "power_observatory/crc32c.hpp"
#include "power_observatory/divergence.hpp"
#include "power_observatory/evidence.hpp"
#include "power_observatory/failover.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/flow.hpp"
#include "power_observatory/json_reports.hpp"
#include "power_observatory/persistence.hpp"
#include "power_observatory/policy.hpp"
#include "power_observatory/quality.hpp"
#include "power_observatory/reserve.hpp"
#include "power_observatory/runtime.hpp"
#include "power_observatory/scenario.hpp"
#include "power_observatory/snapshot.hpp"
#include "power_observatory/version.hpp"

namespace po {
namespace {

constexpr std::string_view kSiteFileName = "site.json";
constexpr std::string_view kLogFileName = "evidence.poev";
constexpr std::string_view kScenarioStandard = "standard-2n";

struct Invocation {
  std::string command;
  std::map<std::string, std::string> values;
  std::set<std::string> flags;
};

struct Site {
  std::string store;
  std::string log_path;
  std::string scenario{kScenarioStandard};
  std::string policy{"default"};
  std::uint64_t seed{1};
  Timestamp created_at{};
};

[[nodiscard]] std::string site_path(const std::string& store) { return join_path(store, kSiteFileName); }
[[nodiscard]] std::string log_path_of(const std::string& store) { return join_path(store, kLogFileName); }

[[nodiscard]] Result<Invocation> parse_invocation(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return Error(ReasonCode::InvalidArgument, "no command was supplied");
  }
  Invocation invocation;
  invocation.command = arguments.front();
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string& token = arguments[index];
    if (token.size() < 3 || token.rfind("--", 0) != 0) {
      return Error(ReasonCode::InvalidArgument, "unrecognized argument '" + token + "'");
    }
    const std::size_t separator = token.find('=');
    if (separator != std::string::npos) {
      invocation.values[token.substr(2, separator - 2)] = token.substr(separator + 1);
      continue;
    }
    const std::string key = token.substr(2);
    if (index + 1 < arguments.size() && arguments[index + 1].rfind("--", 0) != 0) {
      invocation.values[key] = arguments[++index];
      continue;
    }
    invocation.flags.insert(key);
  }
  return invocation;
}

[[nodiscard]] std::string value_or(const Invocation& invocation, const std::string& key,
                                   const std::string& fallback) {
  const auto found = invocation.values.find(key);
  return found == invocation.values.end() ? fallback : found->second;
}

[[nodiscard]] bool has_flag(const Invocation& invocation, const std::string& key) {
  return invocation.flags.count(key) != 0 || invocation.values.count(key) != 0;
}

[[nodiscard]] Result<std::uint64_t> unsigned_option(const Invocation& invocation, const std::string& key,
                                                    std::uint64_t fallback) {
  const auto found = invocation.values.find(key);
  if (found == invocation.values.end()) {
    return fallback;
  }
  const Result<QuantityRep> parsed = parse_decimal_scaled(found->second, 1, key);
  if (!parsed) {
    return parsed.error();
  }
  if (parsed.value() < 0) {
    return Error(ReasonCode::InvalidArgument, "option --" + key + " must not be negative");
  }
  return static_cast<std::uint64_t>(parsed.value());
}

[[nodiscard]] bool wants_json(const Invocation& invocation) {
  return !has_flag(invocation, "text");
}

void emit(std::ostream& out, const JsonValue& document, bool pretty) { out << document.dump(pretty); }

[[nodiscard]] Result<Site> read_site(const std::string& store) {
  const std::string path = site_path(store);
  if (!file_exists(path)) {
    return Error(ReasonCode::NotFound,
                 "store '" + store + "' has no " + std::string(kSiteFileName) +
                     "; run 'init --store " + store + "' first");
  }
  const Result<std::vector<std::uint8_t>> bytes = read_whole_file(path);
  if (!bytes) {
    return bytes.error();
  }
  const Result<JsonValue> document =
      JsonValue::parse(std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
  if (!document) {
    return document.error();
  }
  if (document.value().type() != JsonValue::Type::Object) {
    return Error(ReasonCode::SchemaViolation, "site description must be a JSON object");
  }

  Site site;
  site.store = store;
  site.log_path = log_path_of(store);
  const JsonValue* scenario = document.value().find("scenario");
  if (scenario == nullptr || scenario->type() != JsonValue::Type::String) {
    return Error(ReasonCode::SchemaViolation, "site description is missing a string 'scenario' field");
  }
  site.scenario = scenario->as_string();
  if (site.scenario != kScenarioStandard) {
    return Error(ReasonCode::UnsupportedCapability,
                 "site declares scenario '" + site.scenario +
                     "', which this build does not implement; the only declared topology source in this release is '" +
                     std::string(kScenarioStandard) + "'");
  }
  const JsonValue* seed = document.value().find("seed");
  if (seed == nullptr || (seed->type() != JsonValue::Type::Unsigned && seed->type() != JsonValue::Type::Integer)) {
    return Error(ReasonCode::SchemaViolation, "site description is missing an integer 'seed' field");
  }
  site.seed = seed->type() == JsonValue::Type::Unsigned ? seed->as_unsigned()
                                                        : static_cast<std::uint64_t>(seed->as_integer());
  const JsonValue* policy = document.value().find("policy");
  if (policy != nullptr && policy->type() == JsonValue::Type::String) {
    site.policy = policy->as_string();
  }
  return site;
}

[[nodiscard]] Status write_site(const Site& site) {
  const Status created = create_directories(site.store);
  if (!created) {
    return created;
  }
  JsonValue document = JsonValue::object();
  document.set("created_at", JsonValue(site.created_at.to_iso8601()));
  document.set("policy", JsonValue(site.policy));
  document.set("scenario", JsonValue(site.scenario));
  document.set("schema_version", JsonValue(static_cast<std::uint64_t>(kJsonSchemaVersion)));
  document.set("seed", JsonValue(site.seed));
  return write_file_atomically(site_path(site.store), document.dump(true));
}

[[nodiscard]] Result<TopologyModel> topology_for(const Site& site) {
  ScenarioOptions options;
  options.seed = site.seed;
  options.steps = 1;
  const Result<Scenario> scenario = build_standard_scenario(options);
  if (!scenario) {
    return scenario.error();
  }
  return scenario.value().topology;
}

[[nodiscard]] ObservationPolicy policy_for(const Site& site) {
  if (site.policy == "synthetic-lab") {
    return policy_for_synthetic_lab();
  }
  return default_policy();
}

struct LoadedStore {
  EvidenceSet evidence;
  TopologyModel topology;
  ObservationPolicy policy;
  RecoveryReport report;
  Epoch epoch{};
  // The newest delivery instant present in the evidence. A store read offline
  // has no "now" of its own, so the last known state is evaluated as of the
  // instant the newest evidence was delivered, and that instant is published
  // with the answer.
  Timestamp newest_delivery{};
};

[[nodiscard]] Result<LoadedStore> load_store(const Site& site, const StoreOpenOptions& options) {
  Result<EvidenceLog::Loaded> loaded = EvidenceLog::load(site.log_path, options);
  if (!loaded) {
    return loaded.error();
  }
  Result<TopologyModel> topology = topology_for(site);
  if (!topology) {
    return topology.error();
  }
  LoadedStore store;
  store.evidence = std::move(loaded.value().evidence);
  store.report = loaded.value().report;
  store.epoch = loaded.value().epoch;
  store.topology = std::move(topology).value();
  store.policy = policy_for(site);
  for (const Measurement& measurement : store.evidence.measurements()) {
    if (measurement.provenance.received_time > store.newest_delivery) {
      store.newest_delivery = measurement.provenance.received_time;
    }
  }
  return store;
}

// Chooses the instant an offline answer is evaluated as of.
//
// The default is the newest delivery instant in the evidence, because a store
// read by a fresh process has observed nothing and has no better claim to
// "now". Pass --now to evaluate against the wall clock instead, or --as-of to
// name an instant explicitly.
[[nodiscard]] Result<Timestamp> evaluation_instant(const Invocation& invocation, const LoadedStore& store) {
  const std::string explicit_as_of = value_or(invocation, "as-of", "");
  if (!explicit_as_of.empty()) {
    return Timestamp::from_iso8601(explicit_as_of);
  }
  if (has_flag(invocation, "now")) {
    return SystemClock::instance().wall_now();
  }
  if (store.newest_delivery.is_epoch()) {
    return SystemClock::instance().wall_now();
  }
  return store.newest_delivery;
}

[[nodiscard]] Result<Snapshot> snapshot_of(const LoadedStore& store, Timestamp as_of) {
  return Snapshot::build(Revision{1}, store.evidence, store.topology, store.policy, as_of,
                         SystemClock::instance().steady_now());
}

[[nodiscard]] JsonValue results_json(const std::vector<IngestResult>& results) {
  JsonValue array = JsonValue::array();
  for (const IngestResult& result : results) {
    array.push(to_json(result));
  }
  return array;
}

int command_version(const Invocation& invocation, std::ostream& out) {
  if (wants_json(invocation)) {
    JsonValue document = JsonValue::object();
    document.set("build", JsonValue(std::string(build_info())));
    document.set("format_version", JsonValue(static_cast<std::uint64_t>(kEvidenceLogFormatVersion)));
    document.set("json_schema_version", JsonValue(static_cast<std::uint64_t>(kJsonSchemaVersion)));
    document.set("name", JsonValue(std::string(kProjectName)));
    document.set("organization", JsonValue(std::string(kOrganization)));
    document.set("version", JsonValue(std::string(kVersionString)));
    emit(out, document, has_flag(invocation, "pretty"));
    return static_cast<int>(CliExit::Ok);
  }
  out << kProjectName << " " << kVersionString << "\n" << build_info() << "\n";
  return static_cast<int>(CliExit::Ok);
}

struct CheckResult {
  std::string name;
  bool passed{false};
  std::string detail;
};

int command_selfcheck(const Invocation& invocation, std::ostream& out) {
  std::vector<CheckResult> checks;

  auto record = [&checks](std::string name, bool passed, std::string detail) {
    checks.push_back(CheckResult{std::move(name), passed, std::move(detail)});
  };

  record("reason_table_sorted", [&] {
    for (std::size_t index = 1; index < reason_code_count(); ++index) {
      if (static_cast<std::uint16_t>(reason_code_at(index - 1)) >=
          static_cast<std::uint16_t>(reason_code_at(index))) {
        return false;
      }
    }
    return true;
  }(), std::to_string(reason_code_count()) + " reason codes, strictly ascending");

  record("crc32c_vector", Crc32c::compute("123456789") == 0xE3069283u,
         "CRC-32C of \"123456789\" is 0xE3069283");

  {
    QuantityRep result = 0;
    const bool overflow_rejected = !checked_add(std::numeric_limits<QuantityRep>::max(), 1, result);
    record("checked_arithmetic", overflow_rejected, "signed overflow is refused rather than wrapped");
  }

  {
    const Result<Timestamp> parsed = Timestamp::from_iso8601("2026-02-28T12:34:56.789012345Z");
    record("iso8601_round_trip", parsed && parsed.value().to_iso8601() == "2026-02-28T12:34:56.789012345Z",
           "parse and render agree to the nanosecond");
  }

  {
    const Result<JsonValue> document = JsonValue::parse(R"({"b":1,"a":[true,null,"x"]})");
    const bool ordered = document && document.value().dump() == R"({"a":[true,null,"x"],"b":1})";
    record("json_canonical_order", ordered, "object keys are emitted in sorted order");
  }

  {
    ScenarioOptions options;
    options.seed = 11;
    options.steps = 1;
    const Result<Scenario> scenario = build_standard_scenario(options);
    record("scenario_builds", static_cast<bool>(scenario),
           scenario ? std::to_string(scenario.value().topology.size()) + " declared entities"
                    : scenario.error().detail());
  }

  {
    const Timestamp first = SystemClock::instance().wall_now();
    const Timestamp second = SystemClock::instance().wall_now();
    record("wall_clock_monotone", second >= first, "two consecutive wall-clock reads are non-decreasing");
  }

  const bool all_passed = std::all_of(checks.begin(), checks.end(),
                                      [](const CheckResult& check) { return check.passed; });

  if (wants_json(invocation)) {
    JsonValue document = JsonValue::object();
    JsonValue array = JsonValue::array();
    for (const CheckResult& check : checks) {
      JsonValue entry = JsonValue::object();
      entry.set("detail", JsonValue(check.detail));
      entry.set("name", JsonValue(check.name));
      entry.set("passed", JsonValue(check.passed));
      array.push(std::move(entry));
    }
    document.set("checks", std::move(array));
    document.set("passed", JsonValue(all_passed));
    emit(out, document, has_flag(invocation, "pretty"));
  } else {
    for (const CheckResult& check : checks) {
      out << (check.passed ? "ok   " : "FAIL ") << check.name << ": " << check.detail << "\n";
    }
  }
  return all_passed ? static_cast<int>(CliExit::Ok) : static_cast<int>(CliExit::Refused);
}

int command_init(const Invocation& invocation, std::ostream& out, std::ostream& err) {
  const std::string store = value_or(invocation, "store", "");
  if (store.empty()) {
    err << "init requires --store <directory>\n";
    return static_cast<int>(CliExit::Usage);
  }
  const Result<std::uint64_t> seed = unsigned_option(invocation, "seed", 1);
  if (!seed) {
    err << seed.detail() << "\n";
    return static_cast<int>(CliExit::Usage);
  }

  if (file_exists(site_path(store)) && !has_flag(invocation, "force")) {
    err << "store '" << store << "' already describes a site; pass --force to overwrite it\n";
    return static_cast<int>(CliExit::Refused);
  }

  Site site;
  site.store = store;
  site.log_path = log_path_of(store);
  site.seed = seed.value();
  site.created_at = SystemClock::instance().wall_now();
  const std::string policy = value_or(invocation, "policy", "default");
  if (policy != "default" && policy != "synthetic-lab") {
    err << "unknown policy '" << policy << "'; expected 'default' or 'synthetic-lab'\n";
    return static_cast<int>(CliExit::Usage);
  }
  site.policy = policy;

  const Status written = write_site(site);
  if (!written) {
    err << written.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  if (wants_json(invocation)) {
    JsonValue document = JsonValue::object();
    document.set("log_path", JsonValue(site.log_path));
    document.set("policy", JsonValue(site.policy));
    document.set("scenario", JsonValue(site.scenario));
    document.set("seed", JsonValue(site.seed));
    document.set("site_path", JsonValue(site_path(store)));
    document.set("store", JsonValue(store));
    emit(out, document, has_flag(invocation, "pretty"));
  } else {
    out << "initialized site in '" << store << "' with scenario " << site.scenario << " and seed " << site.seed
        << "\n";
  }
  return static_cast<int>(CliExit::Ok);
}

int command_scenario(const Invocation& invocation, std::ostream& out, std::ostream& err) {
  const std::string store = value_or(invocation, "store", "");
  const std::string target = value_or(invocation, "out", "");
  if (target.empty()) {
    err << "scenario requires --out <file>\n";
    return static_cast<int>(CliExit::Usage);
  }

  const Result<std::uint64_t> seed_option = unsigned_option(invocation, "seed", 1);
  const Result<std::uint64_t> steps_option = unsigned_option(invocation, "steps", 1);
  if (!seed_option || !steps_option) {
    err << (!seed_option ? seed_option.detail() : steps_option.detail()) << "\n";
    return static_cast<int>(CliExit::Usage);
  }

  ScenarioOptions options;
  options.seed = seed_option.value();
  options.steps = static_cast<std::size_t>(steps_option.value());
  const Result<std::uint64_t> dropout = unsigned_option(invocation, "dropout-ppm", 0);
  const Result<std::uint64_t> contradiction = unsigned_option(invocation, "contradiction-ppm", 0);
  const Result<std::uint64_t> stale = unsigned_option(invocation, "stale-ppm", 0);
  const Result<std::uint64_t> derate = unsigned_option(invocation, "derate-feed-ppm", 0);
  if (!dropout || !contradiction || !stale || !derate) {
    err << "scenario fault options must be non-negative integers\n";
    return static_cast<int>(CliExit::Usage);
  }
  options.dropout_ppm = static_cast<std::uint32_t>(dropout.value());
  options.contradiction_ppm = static_cast<std::uint32_t>(contradiction.value());
  options.stale_ppm = static_cast<std::uint32_t>(stale.value());
  options.derate_feed_ppm = static_cast<std::uint32_t>(derate.value());
  options.include_power_factor = has_flag(invocation, "power-factor");

  if (!store.empty()) {
    const Result<Site> site = read_site(store);
    if (!site) {
      err << site.detail() << "\n";
      return static_cast<int>(CliExit::Refused);
    }
    options.seed = site.value().seed;
    options.source = "synthetic-lab";
  }
  options.start_time = SystemClock::instance().wall_now();

  const Result<Scenario> scenario = build_standard_scenario(options);
  if (!scenario) {
    err << scenario.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  std::string payload;
  payload.reserve(1 << 20);
  std::size_t measurement_count = 0;
  for (const EvidenceBatch& batch : scenario.value().batches) {
    JsonValue document = JsonValue::object();
    document.set("authority", JsonValue("synthetic"));
    document.set("attempt", JsonValue(batch.attempt.value()));
    document.set("epoch", JsonValue(batch.epoch.value()));
    document.set("first_sequence", JsonValue(batch.first_sequence.value()));
    document.set("generation", JsonValue(batch.generation.value()));
    document.set("mutation", JsonValue(batch.mutation.value()));
    document.set("recorded_at", JsonValue(batch.recorded_at.to_iso8601()));
    document.set("source", JsonValue(batch.source.value()));

    JsonValue measurements = JsonValue::array();
    for (const Measurement& measurement : batch.measurements) {
      ++measurement_count;
      const MeasurementValue& value = measurement.value;
      JsonValue entry = JsonValue::object();
      entry.set("entity", JsonValue(measurement.entity.to_string()));
      entry.set("id", JsonValue(measurement.id.value()));
      entry.set("kind", JsonValue(std::string(po::to_string(measurement.kind()))));
      entry.set("phase", JsonValue(std::string(po::to_string(measurement.phase))));
      entry.set("received_time", JsonValue(measurement.provenance.received_time.to_iso8601()));
      entry.set("sequence", JsonValue(measurement.provenance.sequence.value()));
      entry.set("value", std::visit(
                             [](const auto& typed) { return JsonValue(typed.canonical_value_string()); }, value));
      measurements.push(std::move(entry));
    }
    document.set("measurements", std::move(measurements));
    payload.append(document.dump(false));
    payload.push_back('\n');
  }

  const Status written = write_file_atomically(target, payload);
  if (!written) {
    err << written.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  if (wants_json(invocation)) {
    JsonValue document = JsonValue::object();
    document.set("batches", JsonValue(static_cast<std::uint64_t>(scenario.value().batches.size())));
    document.set("description", JsonValue(scenario.value().description));
    document.set("measurements", JsonValue(static_cast<std::uint64_t>(measurement_count)));
    document.set("out", JsonValue(target));
    document.set("seed", JsonValue(options.seed));
    document.set("steps", JsonValue(static_cast<std::uint64_t>(options.steps)));
    document.set("synthetic", JsonValue(true));
    emit(out, document, has_flag(invocation, "pretty"));
  } else {
    out << "wrote " << scenario.value().batches.size() << " synthetic batch(es), " << measurement_count
        << " measurement(s), to '" << target << "'\n";
  }
  return static_cast<int>(CliExit::Ok);
}

int command_ingest(const Invocation& invocation, std::ostream& out, std::ostream& err) {
  const std::string store = value_or(invocation, "store", "");
  const std::string input = value_or(invocation, "input", "");
  if (store.empty() || input.empty()) {
    err << "ingest requires --store <directory> and --input <file>\n";
    return static_cast<int>(CliExit::Usage);
  }
  const Result<Site> site = read_site(store);
  if (!site) {
    err << site.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  const Result<std::vector<std::uint8_t>> bytes = read_whole_file(input);
  if (!bytes) {
    err << bytes.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  RuntimeOptions runtime_options;
  runtime_options.log_path = site.value().log_path;
  runtime_options.policy = policy_for(site.value());
  const Result<TopologyModel> topology = topology_for(site.value());
  if (!topology) {
    err << topology.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  runtime_options.topology = topology.value();
  runtime_options.auto_start = true;

  const Result<std::unique_ptr<Observatory>> runtime = Observatory::create(runtime_options);
  if (!runtime) {
    err << runtime.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  const std::string_view text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
  std::vector<std::shared_future<IngestResult>> futures;
  std::vector<IngestResult> results;
  std::size_t rejected_at_parse = 0;
  std::string first_parse_error;

  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const std::size_t newline = text.find('\n', cursor);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    const std::string_view line = text.substr(cursor, end - cursor);
    cursor = end + 1;
    if (line.empty()) {
      continue;
    }
    const Result<EvidenceBatch> batch = parse_ingest_batch(line);
    if (!batch) {
      ++rejected_at_parse;
      if (first_parse_error.empty()) {
        first_parse_error = batch.detail();
      }
      continue;
    }
    EvidenceBatch stamped = batch.value();
    stamp_delivery(stamped, SystemClock::instance().wall_now(), SystemClock::instance().steady_now());
    Result<std::shared_future<IngestResult>> future = runtime.value()->submit(std::move(stamped));
    if (!future) {
      ++rejected_at_parse;
      if (first_parse_error.empty()) {
        first_parse_error = future.detail();
      }
      continue;
    }
    futures.push_back(std::move(future).value());
  }

  for (const std::shared_future<IngestResult>& future : futures) {
    results.push_back(future.get());
  }

  const Status stopped = runtime.value()->stop();
  if (!stopped) {
    err << stopped.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  std::size_t committed = 0;
  std::size_t replayed = 0;
  std::size_t rejected = 0;
  for (const IngestResult& result : results) {
    switch (result.disposition) {
      case IngestDisposition::Committed:
        ++committed;
        break;
      case IngestDisposition::IdempotentReplay:
        ++replayed;
        break;
      case IngestDisposition::Rejected:
      case IngestDisposition::Cancelled:
        ++rejected;
        break;
    }
  }

  if (wants_json(invocation)) {
    JsonValue document = JsonValue::object();
    document.set("committed", JsonValue(static_cast<std::uint64_t>(committed)));
    document.set("epoch", JsonValue(runtime.value()->epoch().value()));
    JsonValue array = JsonValue::array();
    for (const IngestResult& result : results) {
      array.push(to_json(result));
    }
    document.set("results", std::move(array));
    document.set("idempotent_replay", JsonValue(static_cast<std::uint64_t>(replayed)));
    document.set("rejected", JsonValue(static_cast<std::uint64_t>(rejected + rejected_at_parse)));
    document.set("recovery", to_json(runtime.value()->recovery()));
    document.set("revision", JsonValue(runtime.value()->current_revision().value()));
    if (!first_parse_error.empty()) {
      document.set("first_document_error", JsonValue(first_parse_error));
    }
    emit(out, document, has_flag(invocation, "pretty"));
  } else {
    out << "committed " << committed << ", idempotent replay " << replayed << ", rejected "
        << (rejected + rejected_at_parse) << "\n";
    if (!first_parse_error.empty()) {
      out << "first document error: " << first_parse_error << "\n";
    }
  }
  return rejected + rejected_at_parse == 0 ? static_cast<int>(CliExit::Ok) : static_cast<int>(CliExit::Refused);
}

int command_verify(const Invocation& invocation, std::ostream& out, std::ostream& err) {
  const std::string store = value_or(invocation, "store", "");
  if (store.empty()) {
    err << "verify requires --store <directory>\n";
    return static_cast<int>(CliExit::Usage);
  }
  const Result<Site> site = read_site(store);
  if (!site) {
    err << site.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  StoreOpenOptions options;
  const Result<RecoveryReport> inspected = EvidenceLog::inspect(site.value().log_path, options);
  if (!inspected) {
    err << inspected.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  const Result<EvidenceLog::Loaded> loaded = EvidenceLog::load(site.value().log_path, options);
  if (!loaded) {
    err << loaded.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  const bool clean = inspected.value().records_rejected == 0 &&
                     inspected.value().records_skipped_replay == 0 &&
                     inspected.value().bytes_discarded == 0;
  if (wants_json(invocation)) {
    JsonValue document = JsonValue::object();
    document.set("clean", JsonValue(clean));
    document.set("evidence_hash", JsonValue(loaded.value().evidence.content_hash()));
    document.set("measurements", JsonValue(static_cast<std::uint64_t>(loaded.value().evidence.size())));
    document.set("inspect", to_json(inspected.value()));
    document.set("load", to_json(loaded.value().report));
    emit(out, document, has_flag(invocation, "pretty"));
  } else {
    out << "clean: " << (clean ? "yes" : "no") << "\n"
        << "records scanned: " << inspected.value().records_scanned << "\n"
        << "records applied: " << inspected.value().records_applied << "\n"
        << "records rejected: " << inspected.value().records_rejected << "\n"
        << "records replayed: " << inspected.value().records_skipped_replay << "\n"
        << "bytes discarded: " << inspected.value().bytes_discarded << "\n"
        << "measurements: " << loaded.value().evidence.size() << "\n";
  }
  return clean ? static_cast<int>(CliExit::Ok) : static_cast<int>(CliExit::Refused);
}

int command_history(const Invocation& invocation, std::ostream& out, std::ostream& err) {
  const std::string store = value_or(invocation, "store", "");
  if (store.empty()) {
    err << "history requires --store <directory>\n";
    return static_cast<int>(CliExit::Usage);
  }
  const Result<Site> site = read_site(store);
  if (!site) {
    err << site.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  const Result<LoadedStore> loaded = load_store(site.value(), StoreOpenOptions{});
  if (!loaded) {
    err << loaded.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  if (wants_json(invocation)) {
    JsonValue document = JsonValue::object();
    document.set("epoch", JsonValue(loaded.value().epoch.value()));
    JsonValue generations = JsonValue::array();
    for (const Generation& generation : loaded.value().evidence.generations()) {
      generations.push(JsonValue(generation.value()));
    }
    document.set("generations", std::move(generations));
    document.set("measurements", JsonValue(static_cast<std::uint64_t>(loaded.value().evidence.size())));
    JsonValue notes = JsonValue::array();
    for (const EvidenceNote& note : loaded.value().evidence.notes()) {
      notes.push(to_json(note));
    }
    document.set("notes", std::move(notes));
    document.set("recovery", to_json(loaded.value().report));
    JsonValue sources = JsonValue::array();
    for (const SourceId& source : loaded.value().evidence.sources()) {
      JsonValue entry = JsonValue::object();
      entry.set("generation", JsonValue(loaded.value().evidence.generation_of(source).value()));
      entry.set("source", JsonValue(source.value()));
      sources.push(std::move(entry));
    }
    document.set("sources", std::move(sources));
    emit(out, document, has_flag(invocation, "pretty"));
  } else {
    out << "epoch: " << loaded.value().epoch.value() << "\n";
    out << "measurements: " << loaded.value().evidence.size() << "\n";
    out << "admitted: " << loaded.value().evidence.admitted_count() << "\n";
    out << "dropped: " << loaded.value().evidence.dropped_count() << "\n";
    for (const EvidenceNote& note : loaded.value().evidence.notes()) {
      out << "  note " << po::to_string(note.code) << ": " << note.detail << "\n";
    }
  }
  return static_cast<int>(CliExit::Ok);
}

int command_divergence(const Invocation& invocation, std::ostream& out, std::ostream& err) {
  const std::string left_store = value_or(invocation, "left-store", "");
  const std::string right_store = value_or(invocation, "right-store", "");
  if (left_store.empty() || right_store.empty()) {
    err << "divergence requires --left-store <directory> and --right-store <directory>\n";
    return static_cast<int>(CliExit::Usage);
  }
  const Result<Site> left_site = read_site(left_store);
  const Result<Site> right_site = read_site(right_store);
  if (!left_site || !right_site) {
    err << (!left_site ? left_site.detail() : right_site.detail()) << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  const Result<LoadedStore> left = load_store(left_site.value(), StoreOpenOptions{});
  const Result<LoadedStore> right = load_store(right_site.value(), StoreOpenOptions{});
  if (!left || !right) {
    err << (!left ? left.detail() : right.detail()) << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  const DivergenceReport report = compare(left.value().evidence, right.value().evidence);
  if (wants_json(invocation)) {
    emit(out, to_json(report), has_flag(invocation, "pretty"));
  } else {
    out << "verdict: " << po::to_string(report.verdict) << "\n"
        << "compared keys: " << report.compared_keys << "\n"
        << "diverged keys: " << report.diverged_keys << "\n";
  }
  return static_cast<int>(CliExit::Ok);
}

[[nodiscard]] std::optional<EntityRef> scope_of(const Invocation& invocation, std::ostream& err, bool& ok) {
  const std::string scope = value_or(invocation, "scope", "");
  if (scope.empty()) {
    ok = true;
    return std::nullopt;
  }
  const Result<EntityRef> parsed = EntityRef::parse(scope);
  if (!parsed) {
    err << parsed.detail() << "\n";
    ok = false;
    return std::nullopt;
  }
  ok = true;
  return parsed.value();
}

int command_report(const Invocation& invocation, std::ostream& out, std::ostream& err) {
  const std::string store = value_or(invocation, "store", "");
  if (store.empty()) {
    err << invocation.command << " requires --store <directory>\n";
    return static_cast<int>(CliExit::Usage);
  }
  const Result<Site> site = read_site(store);
  if (!site) {
    err << site.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  const Result<LoadedStore> loaded = load_store(site.value(), StoreOpenOptions{});
  if (!loaded) {
    err << loaded.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  const Result<Timestamp> as_of = evaluation_instant(invocation, loaded.value());
  if (!as_of) {
    err << as_of.detail() << "\n";
    return static_cast<int>(CliExit::Usage);
  }
  const Result<Snapshot> snapshot = snapshot_of(loaded.value(), as_of.value());
  if (!snapshot) {
    err << snapshot.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  bool scope_ok = true;
  const std::optional<EntityRef> scope = scope_of(invocation, err, scope_ok);
  if (!scope_ok) {
    return static_cast<int>(CliExit::Usage);
  }
  const bool pretty = has_flag(invocation, "pretty");
  const bool json = wants_json(invocation);

  if (invocation.command == "answer") {
    const AnswerReport report = snapshot.value().answer();
    if (json) {
      emit(out, to_json(report), pretty);
    } else {
      out << report.to_text();
    }
    return static_cast<int>(CliExit::Ok);
  }
  if (invocation.command == "flow") {
    const Outcome<FlowReport> report = snapshot.value().flow(FlowQuery{scope, true});
    if (!report) {
      err << po::to_string(report.code()) << ": " << report.detail() << "\n";
      return static_cast<int>(CliExit::Refused);
    }
    if (json) {
      emit(out, to_json(report.value()), pretty);
    } else {
      out << "state: " << po::to_string(report.value().state) << "\n";
      for (const FlowNode& node : report.value().nodes) {
        out << "  " << node.entity.to_string() << " "
            << (node.active_power.has_value() ? node.active_power->to_string() : std::string("unmeasured")) << " "
            << po::to_string(node.direction) << " " << po::to_string(node.state) << "\n";
      }
    }
    return static_cast<int>(CliExit::Ok);
  }
  if (invocation.command == "reserve") {
    if (!scope.has_value()) {
      err << "reserve requires --scope <entity>\n";
      return static_cast<int>(CliExit::Usage);
    }
    const Outcome<ReserveReport> report = snapshot.value().reserve(ReserveQuery{*scope, true});
    if (!report) {
      err << po::to_string(report.code()) << ": " << report.detail() << "\n";
      return static_cast<int>(CliExit::Refused);
    }
    if (json) {
      emit(out, to_json(report.value()), pretty);
    } else {
      out << "scope: " << report.value().scope.to_string() << "\n"
          << "state: " << po::to_string(report.value().state) << "\n"
          << "usable capacity: "
          << (report.value().usable_capacity.has_value() ? report.value().usable_capacity->to_string()
                                                         : std::string("unknown"))
          << "\n"
          << "measured load: "
          << (report.value().measured_load.has_value() ? report.value().measured_load->to_string()
                                                       : std::string("unknown"))
          << "\n"
          << "reserve: "
          << (report.value().reserve.has_value() ? report.value().reserve->to_string() : std::string("unknown"))
          << "\n"
          << "single-failure reserve: "
          << (report.value().single_failure_reserve.has_value()
                  ? report.value().single_failure_reserve->to_string()
                  : std::string("unknown"))
          << "\n";
    }
    return static_cast<int>(CliExit::Ok);
  }
  if (invocation.command == "attribution") {
    const Outcome<AttributionReport> report = snapshot.value().attribution(AttributionQuery{scope, true, true});
    if (!report) {
      err << po::to_string(report.code()) << ": " << report.detail() << "\n";
      return static_cast<int>(CliExit::Refused);
    }
    if (json) {
      emit(out, to_json(report.value()), pretty);
    } else {
      out << "state: " << po::to_string(report.value().state) << "\n"
          << "imbalances: " << report.value().imbalances.size() << "\n"
          << "losses: " << report.value().losses.size() << "\n"
          << "unattributed: " << report.value().unattributed_imbalances << "\n";
    }
    return static_cast<int>(CliExit::Ok);
  }
  if (invocation.command == "quality") {
    const Outcome<QualityReport> report = snapshot.value().quality(QualityQuery{scope});
    if (!report) {
      err << po::to_string(report.code()) << ": " << report.detail() << "\n";
      return static_cast<int>(CliExit::Refused);
    }
    if (json) {
      emit(out, to_json(report.value()), pretty);
    } else {
      out << "state: " << po::to_string(report.value().state) << "\n"
          << "assessed: " << report.value().measurements_assessed << "\n"
          << "skipped (stale): " << report.value().measurements_skipped_stale << "\n";
      for (const QualityFinding& finding : report.value().findings) {
        out << "  " << po::to_string(finding.severity) << " " << po::to_string(finding.code) << " "
            << finding.entity.to_string() << ": " << finding.detail << "\n";
      }
    }
    return static_cast<int>(CliExit::Ok);
  }
  if (invocation.command == "failover") {
    const std::string group = value_or(invocation, "group", "");
    if (group.empty()) {
      err << "failover requires --group <redundancy group id>\n";
      return static_cast<int>(CliExit::Usage);
    }
    const Outcome<FailoverReport> report = snapshot.value().failover(FailoverQuery{RedundancyGroupId(group)});
    if (!report) {
      err << po::to_string(report.code()) << ": " << report.detail() << "\n";
      return static_cast<int>(CliExit::Refused);
    }
    if (json) {
      emit(out, to_json(report.value()), pretty);
    } else {
      out << "group: " << report.value().group.value() << "\n"
          << "readiness: " << po::to_string(report.value().readiness) << "\n";
      for (const FailoverGate& gate : report.value().gates) {
        out << "  " << (gate.passed ? "pass" : (gate.evaluable ? "fail" : "n/a ")) << " "
            << (gate.mandatory ? "[required] " : "[optional] ") << gate.gate << ": " << gate.detail << "\n";
      }
    }
    return static_cast<int>(CliExit::Ok);
  }
  if (invocation.command == "topology") {
    JsonValue document = JsonValue::object();
    JsonValue entities = JsonValue::array();
    for (const EntityRef& entity : snapshot.value().topology().entity_order()) {
      JsonValue entry = JsonValue::object();
      entry.set("authority", JsonValue("configured"));
      entry.set("declared_capacity_w",
                JsonValue(snapshot.value().topology().declared_capacity(entity).canonical_value_string()));
      entry.set("declared_state",
                JsonValue(std::string(po::to_string(snapshot.value().topology().declared_state(entity)))));
      entry.set("entity", JsonValue(entity.to_string()));
      entry.set("kind", JsonValue(std::string(po::to_string(entity.kind()))));
      entry.set("name", JsonValue(snapshot.value().topology().display_name(entity)));
      entities.push(std::move(entry));
    }
    document.set("entities", std::move(entities));
    document.set("note", JsonValue("every element is configured authority: it records what the plant is declared to "
                                   "be, never what it is currently doing"));
    document.set("topology_hash", JsonValue(snapshot.value().metadata().topology_hash));
    if (json) {
      emit(out, document, pretty);
    } else {
      for (const JsonValue& entry : document.find("entities")->as_array()) {
        out << "  " << entry.find("entity")->as_string() << " "
            << entry.find("declared_state")->as_string() << " "
            << entry.find("declared_capacity_w")->as_string() << " W\n";
      }
    }
    return static_cast<int>(CliExit::Ok);
  }
  if (invocation.command == "explain") {
    const AnswerReport report = snapshot.value().answer();
    if (json) {
      emit(out, to_json(report.explanation), pretty);
    } else {
      out << report.explanation.to_text();
    }
    return static_cast<int>(CliExit::Ok);
  }

  err << "unrecognized command '" << invocation.command << "'\n";
  return static_cast<int>(CliExit::Usage);
}

int command_bench(const Invocation& invocation, std::ostream& out, std::ostream& err) {
  const Result<std::uint64_t> iterations = unsigned_option(invocation, "iterations", 200);
  const Result<std::uint64_t> seed = unsigned_option(invocation, "seed", 12345);
  if (!iterations || !seed) {
    err << "bench options must be non-negative integers\n";
    return static_cast<int>(CliExit::Usage);
  }

  ScenarioOptions options;
  options.seed = seed.value();
  options.steps = 1;
  options.include_frequency = true;
  options.include_voltage = true;
  const Result<Scenario> scenario = build_standard_scenario(options);
  if (!scenario) {
    err << scenario.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  Result<EvidenceSet> evidence = EvidenceSet::build(scenario.value().batches.front().measurements);
  if (!evidence) {
    err << evidence.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  ManualClock clock;
  const Result<Snapshot> snapshot = Snapshot::build(Revision{1}, evidence.value(), scenario.value().topology,
                                                    default_policy(), clock.wall_now(), clock.steady_now());
  if (!snapshot) {
    err << snapshot.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }

  const std::uint64_t count = iterations.value();
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t completed = 0;
  std::size_t nodes = 0;
  for (std::uint64_t index = 0; index < count; ++index) {
    const AnswerReport report = snapshot.value().answer();
    nodes += report.groups.size();
    ++completed;
  }
  const auto finished = std::chrono::steady_clock::now();
  const auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();

  if (wants_json(invocation)) {
    JsonValue document = JsonValue::object();
    document.set("completed_answers", JsonValue(completed));
    document.set("elapsed_ns", JsonValue(static_cast<std::int64_t>(elapsed_ns)));
    document.set("evidence_basis", JsonValue("SYNTHETIC"));
    document.set("measurements_per_snapshot",
                 JsonValue(static_cast<std::uint64_t>(evidence.value().size())));
    document.set("methodology", JsonValue("each iteration builds one complete answer to the core question from a "
                                          "single immutable snapshot; the snapshot itself is built once, outside "
                                          "the measured loop, so the numbers measure answer assembly only"));
    document.set("nodes_visited", JsonValue(static_cast<std::uint64_t>(nodes)));
    document.set("real_or_synthetic", JsonValue("SYNTHETIC"));
    if (elapsed_ns > 0) {
      document.set("answers_per_second",
                   JsonValue(static_cast<std::uint64_t>((completed * 1000000000ull) /
                                                        static_cast<std::uint64_t>(elapsed_ns))));
    }
    emit(out, document, has_flag(invocation, "pretty"));
  } else {
    out << "completed answers: " << completed << "\n"
        << "elapsed: " << elapsed_ns << " ns\n"
        << "evidence basis: SYNTHETIC (" << evidence.value().size() << " measurements per snapshot)\n";
  }
  return static_cast<int>(CliExit::Ok);
}

int command_hold(const Invocation& invocation, std::ostream& out, std::ostream& err) {
  const std::string store = value_or(invocation, "store", "");
  if (store.empty()) {
    err << "hold requires --store <directory>\n";
    return static_cast<int>(CliExit::Usage);
  }
  const std::string ready = value_or(invocation, "ready", "");
  const std::string wait_for = value_or(invocation, "wait-for", "");
  if (ready.empty() || wait_for.empty()) {
    err << "hold requires --ready <file> and --wait-for <file>\n";
    return static_cast<int>(CliExit::Usage);
  }
  const Result<Site> site = read_site(store);
  if (!site) {
    err << site.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  StoreOpenOptions options;
  Result<EvidenceLog> log = EvidenceLog::open(site.value().log_path, Epoch{}, options);
  if (!log) {
    err << po::to_string(log.code()) << ": " << log.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  const Status signalled = write_file_atomically(ready, std::string_view("locked\n"));
  if (!signalled) {
    err << signalled.detail() << "\n";
    return static_cast<int>(CliExit::Refused);
  }
  // Bounded so that an abandoned test cannot leave a process behind. The bound
  // is the diagnostic's own safety net, not a test deadline: the caller decides
  // when to release, and the caller always does.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!file_exists(wait_for) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  static_cast<void>(log.value().close());
  if (!file_exists(wait_for)) {
    err << "hold timed out waiting for the release file\n";
    return static_cast<int>(CliExit::Refused);
  }
  out << "released " << site.value().log_path << "\n";
  return static_cast<int>(CliExit::Ok);
}

void print_usage(std::ostream& out) {
  out << kProjectName << " " << kVersionString << "\n\n"
      << "usage: power-observatory <command> [options]\n\n"
      << "commands:\n"
      << "  version                           print version and build information\n"
      << "  selfcheck                         run internal invariants\n"
      << "  init      --store DIR             create a site description\n"
      << "  scenario  --out FILE [--steps N]  write synthetic ingest documents as JSON lines\n"
      << "  ingest    --store DIR --input F   commit ingest documents to the evidence log\n"
      << "  answer    --store DIR             answer the core question\n"
      << "  flow      --store DIR [--scope E] where power is flowing\n"
      << "  reserve   --store DIR --scope E   usable reserve for a scope\n"
      << "  attribution --store DIR [--scope E]  imbalance and conversion loss\n"
      << "  quality   --store DIR [--scope E] power quality findings\n"
      << "  failover  --store DIR --group G   failover readiness gates\n"
      << "  topology  --store DIR             the declared topology\n"
      << "  explain   --store DIR             the reasons behind the current answer\n"
      << "  history   --store DIR             durable history and recovery outcome\n"
      << "  verify    --store DIR             validate the durable log\n"
      << "  divergence --left-store DIR --right-store DIR\n"
      << "  hold      --store DIR --ready F --wait-for F\n"
      << "                                    hold the writer lock for diagnostics\n"
      << "  bench     [--iterations N]        measure completed answer assembly\n\n"
      << "options: --json (default) --text --pretty --seed N --force\n";
}

}  // namespace

int run_cli(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err) {
  if (arguments.empty()) {
    print_usage(err);
    return static_cast<int>(CliExit::Usage);
  }
  if (arguments.front() == "--help" || arguments.front() == "-h" || arguments.front() == "help") {
    print_usage(out);
    return static_cast<int>(CliExit::Ok);
  }
  const Result<Invocation> parsed = parse_invocation(arguments);
  if (!parsed) {
    err << parsed.detail() << "\n";
    print_usage(err);
    return static_cast<int>(CliExit::Usage);
  }
  const Invocation& invocation = parsed.value();
  if (has_flag(invocation, "help")) {
    print_usage(out);
    return static_cast<int>(CliExit::Ok);
  }

  if (invocation.command == "version") {
    return command_version(invocation, out);
  }
  if (invocation.command == "selfcheck") {
    return command_selfcheck(invocation, out);
  }
  if (invocation.command == "init") {
    return command_init(invocation, out, err);
  }
  if (invocation.command == "scenario") {
    return command_scenario(invocation, out, err);
  }
  if (invocation.command == "ingest") {
    return command_ingest(invocation, out, err);
  }
  if (invocation.command == "verify") {
    return command_verify(invocation, out, err);
  }
  if (invocation.command == "history") {
    return command_history(invocation, out, err);
  }
  if (invocation.command == "divergence") {
    return command_divergence(invocation, out, err);
  }
  if (invocation.command == "bench") {
    return command_bench(invocation, out, err);
  }
  if (invocation.command == "hold") {
    return command_hold(invocation, out, err);
  }
  if (invocation.command == "answer" || invocation.command == "flow" || invocation.command == "reserve" ||
      invocation.command == "attribution" || invocation.command == "quality" ||
      invocation.command == "failover" || invocation.command == "topology" || invocation.command == "explain") {
    return command_report(invocation, out, err);
  }

  err << "unrecognized command '" << invocation.command << "'\n";
  print_usage(err);
  return static_cast<int>(CliExit::Usage);
}

}  // namespace po
