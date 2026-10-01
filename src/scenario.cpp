// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/scenario.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace po {
namespace {

constexpr QuantityRep kKilowatt = 1000000;  // milliwatts

}  // namespace

std::uint64_t DeterministicRandom::next() noexcept {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::uint64_t DeterministicRandom::bounded(std::uint64_t bound) noexcept {
  return bound == 0 ? 0 : next() % bound;
}

std::int64_t DeterministicRandom::between(std::int64_t low, std::int64_t high) noexcept {
  if (high <= low) {
    return low;
  }
  const auto span = static_cast<std::uint64_t>(high - low);
  return low + static_cast<std::int64_t>(bounded(span));
}

Result<Scenario> build_scenario(const ScenarioOptions& options) { return build_standard_scenario(options); }

Result<Scenario> build_standard_scenario(const ScenarioOptions& options) {
  if (options.steps == 0) {
    return Error(ReasonCode::InvalidArgument, "scenario requires at least one step");
  }
  if (options.dropout_ppm > 1000000u || options.contradiction_ppm > 1000000u || options.stale_ppm > 1000000u ||
      options.derate_feed_ppm > 1000000u) {
    return Error(ReasonCode::InvalidArgument, "scenario fault fractions must be within 0% and 100%");
  }

  constexpr std::size_t kPdusPerPath = 2;
  constexpr std::size_t kCircuitsPerPdu = 2;

  FeedSet feeds;
  BusSet buses;
  UpsSet ups_units;
  PduSet pdus;
  CircuitSet circuits;
  LoadSet loads;
  RedundancyGroupSet groups;

  const Provenance declared = make_recovered_provenance(SourceId("declared-topology"), AuthorityKind::Configured,
                                                        Generation{1}, Epoch{}, Sequence{1}, std::nullopt,
                                                        Timestamp{});

  const std::size_t paths = 2;
  for (std::size_t path = 0; path < paths; ++path) {
    const bool secondary = path == 1;
    const std::string letter = secondary ? "b" : "a";

    Feed feed;
    feed.id = FeedId("main-" + letter);
    feed.name = "Utility feed " + letter;
    feed.lands_on = BusId("bus-" + letter);
    feed.rated_capacity = Power::from_raw(static_cast<QuantityRep>(1000) * kKilowatt);
    feed.group = RedundancyGroupId("rg-main");
    feed.declared_state = LifecycleState::InService;
    feed.declared_by = declared;
    feeds.push_back(std::move(feed));

    Bus intake;
    intake.id = BusId("bus-" + letter);
    intake.name = "Intake bus " + letter;
    intake.fed_by.push_back(FeedId("main-" + letter));
    intake.rated_capacity = Power::from_raw(static_cast<QuantityRep>(1000) * kKilowatt);
    intake.declared_state = LifecycleState::InService;
    intake.declared_by = declared;
    buses.push_back(std::move(intake));

    Ups unit;
    unit.id = UpsId("ups-" + letter);
    unit.name = "UPS " + letter;
    unit.input = BusId("bus-" + letter);
    unit.outputs.push_back(BusId("bus-" + letter + "-out"));
    unit.rated_capacity = Power::from_raw(static_cast<QuantityRep>(800) * kKilowatt);
    unit.declared_efficiency_ppm = Ratio::from_raw(960000);
    unit.declared_autonomy = seconds(600);
    unit.declared_state = LifecycleState::InService;
    unit.declared_by = declared;
    ups_units.push_back(std::move(unit));

    Bus output;
    output.id = BusId("bus-" + letter + "-out");
    output.name = "UPS output bus " + letter;
    output.rated_capacity = Power::from_raw(static_cast<QuantityRep>(800) * kKilowatt);
    output.declared_state = LifecycleState::InService;
    output.declared_by = declared;
    buses.push_back(std::move(output));

    for (std::size_t index = 0; index < kPdusPerPath; ++index) {
      const std::string pdu_name = "pdu-" + letter + std::to_string(index + 1);
      Pdu pdu;
      pdu.id = PduId(pdu_name);
      pdu.name = "PDU " + pdu_name;
      pdu.input = BusId("bus-" + letter + "-out");
      pdu.rated_capacity = Power::from_raw(static_cast<QuantityRep>(250) * kKilowatt);
      pdu.declared_state = LifecycleState::InService;
      pdu.declared_by = declared;
      pdus.push_back(std::move(pdu));

      for (std::size_t circuit_index = 0; circuit_index < kCircuitsPerPdu; ++circuit_index) {
        const std::string circuit_name = pdu_name + "-c" + std::to_string(circuit_index + 1);
        const std::string load_name = pdu_name + "-load" + std::to_string(circuit_index + 1);

        Load load;
        load.id = LoadId(load_name);
        load.name = "Rack load " + load_name;
        load.circuit = CircuitId(circuit_name);
        load.rated_capacity = Power::from_raw(static_cast<QuantityRep>(60) * kKilowatt);
        load.criticality = path == 0 ? Criticality::MissionCritical : Criticality::High;
        load.declared_state = LifecycleState::InService;
        load.declared_by = declared;
        loads.push_back(std::move(load));

        Circuit circuit;
        circuit.id = CircuitId(circuit_name);
        circuit.name = "Circuit " + circuit_name;
        circuit.pdu = PduId(pdu_name);
        circuit.rated_capacity = Power::from_raw(static_cast<QuantityRep>(63) * kKilowatt);
        circuit.serves = LoadId(load_name);
        circuit.declared_state = LifecycleState::InService;
        circuit.declared_by = declared;
        circuits.push_back(std::move(circuit));
      }
    }
  }

  RedundancyGroup group;
  group.id = RedundancyGroupId("rg-main");
  group.name = "Main utility redundancy group";
  group.topology = RedundancyTopology::TwoN;
  group.members.push_back(FeedId("main-a"));
  group.members.push_back(FeedId("main-b"));
  group.required_live = 1;
  group.declared_transfer_time = milliseconds(80);
  group.declared_state = LifecycleState::InService;
  group.declared_by = declared;
  groups.push_back(std::move(group));

  Result<TopologyModel> topology =
      TopologyModel::build(std::move(feeds), std::move(buses), std::move(ups_units), GeneratorSet{},
                           std::move(pdus), std::move(circuits), std::move(loads), std::move(groups));
  if (!topology) {
    return topology.error();
  }

  Scenario scenario;
  scenario.topology = std::move(topology).value();
  scenario.source = SourceId(options.source);
  scenario.description = "synthetic two-path plant: 2 utility feeds in a 2N redundancy group, one UPS per path, "
                         "2 PDUs per path, 2 circuits per PDU, one rack load per circuit";

  DeterministicRandom random(options.seed);

  // Per-load base demand, in milliwatts.
  std::map<LoadId, QuantityRep> base_demand;
  for (const Load& load : scenario.topology.loads()) {
    base_demand[load.id] = static_cast<QuantityRep>(20 + random.between(0, 30)) * kKilowatt;
  }

  QuantityRep feed_derate = 1000000;
  if (options.derate_feed_ppm > 0) {
    feed_derate = static_cast<QuantityRep>(1000000u - options.derate_feed_ppm);
  }

  MeasurementId next_measurement{1};
  Sequence next_sequence{1};

  for (std::size_t step = 0; step < options.steps; ++step) {
    EvidenceBatch batch;
    batch.source = scenario.source;
    batch.authority = AuthorityKind::Synthetic;
    batch.generation = options.generation;
    batch.epoch = options.epoch;
    batch.first_sequence = next_sequence;
    batch.mutation = MutationId(step + 1);
    batch.attempt = AttemptId(1);

    EvidenceBatch shadow_batch;
    shadow_batch.source = SourceId(std::string(options.source) + "-shadow");
    shadow_batch.authority = AuthorityKind::Synthetic;
    shadow_batch.generation = options.generation;
    shadow_batch.epoch = options.epoch;
    shadow_batch.first_sequence = next_sequence;
    shadow_batch.mutation = MutationId(1000000 + step + 1);
    shadow_batch.attempt = AttemptId(1);

    const Timestamp step_time = Timestamp::from_unix_nanos(
        options.start_time.unix_nanos() + static_cast<QuantityRep>(step) * options.step.raw());
    batch.recorded_at = step_time;
    shadow_batch.recorded_at = step_time;

    std::map<EntityRef, QuantityRep> measured;
    std::map<EntityRef, QuantityRep> losses;

    for (const Load& load : scenario.topology.loads()) {
      const QuantityRep base = base_demand[load.id];
      const QuantityRep jitter = base / 20 == 0 ? 0 : random.between(-(base / 20), base / 20);
      measured[EntityRef::load(load.id)] = base + jitter;
    }

    for (const Circuit& circuit : scenario.topology.circuits()) {
      QuantityRep value = 0;
      if (circuit.serves.has_value()) {
        value = measured[EntityRef::load(*circuit.serves)];
      }
      measured[EntityRef::circuit(circuit.id)] = value;
    }

    for (const Pdu& pdu : scenario.topology.pdus()) {
      QuantityRep total = 0;
      for (const EntityRef& child : scenario.topology.children(EntityRef::pdu(pdu.id))) {
        total += measured[child];
      }
      const QuantityRep loss = total * 15 / 1000;  // 1.5% distribution loss
      losses[EntityRef::pdu(pdu.id)] = loss;
      measured[EntityRef::pdu(pdu.id)] = total + loss;
    }

    const auto sum_children = [&scenario, &measured](const EntityRef& parent) {
      QuantityRep total = 0;
      for (const EntityRef& child : scenario.topology.children(parent)) {
        const auto found = measured.find(child);
        if (found != measured.end()) {
          total += found->second;
        }
      }
      return total;
    };

    for (const Bus& bus : scenario.topology.buses()) {
      if (bus.id.value().find("-out") == std::string::npos) {
        continue;  // Only the UPS output buses are metered at this level.
      }
      measured[EntityRef::bus(bus.id)] = sum_children(EntityRef::bus(bus.id));
    }

    for (const Ups& unit : scenario.topology.ups_units()) {
      const QuantityRep output = sum_children(EntityRef::ups(unit.id));
      const QuantityRep loss = output * 30 / 1000;  // 3% conversion loss
      losses[EntityRef::ups(unit.id)] = loss;
      measured[EntityRef::ups(unit.id)] = output + loss;
      if (unit.input.has_value()) {
        measured[EntityRef::bus(*unit.input)] = measured[EntityRef::ups(unit.id)];
      }
    }

    for (const Feed& feed : scenario.topology.feeds()) {
      QuantityRep value = 0;
      if (feed.lands_on.has_value()) {
        value = measured[EntityRef::bus(*feed.lands_on)];
      }
      if (feed.id == FeedId("main-a")) {
        value = value * feed_derate / 1000000;
      }
      measured[EntityRef::feed(feed.id)] = value;
    }

    for (const EntityRef& entity : scenario.topology.entity_order()) {
      const auto found = measured.find(entity);
      if (found == measured.end()) {
        continue;
      }
      const std::uint32_t roll = static_cast<std::uint32_t>(random.bounded(1000000));
      if (options.dropout_ppm > 0 && roll < options.dropout_ppm) {
        continue;  // Deliberately absent telemetry.
      }

      const QuantityRep value = found->second;

      Timestamp received = step_time;
      if (options.stale_ppm > 0 && random.bounded(1000000) < options.stale_ppm) {
        received = Timestamp::from_unix_nanos(step_time.unix_nanos() - seconds(3600).raw());
      }

      if (options.contradiction_ppm > 0 && random.bounded(1000000) < options.contradiction_ppm) {
        // A second meter on the same entity disagrees with the first by 1%. The
        // disagreement is published by a different source, which is what makes
        // it a contradiction rather than a correction.
        Measurement shadow;
        shadow.id = MeasurementId(1000000 + next_measurement.value());
        shadow.entity = entity;
        shadow.phase = Phase::Total;
        shadow.value = Power::from_raw(value + value / 100);
        shadow.provenance = make_recovered_provenance(
            SourceId(std::string(options.source) + "-shadow"), AuthorityKind::Synthetic, options.generation,
            options.epoch, next_sequence, step_time, received);
        shadow_batch.measurements.push_back(std::move(shadow));
      }

      Measurement measurement;
      measurement.id = next_measurement;
      next_measurement = MeasurementId(next_measurement.value() + 1);
      measurement.entity = entity;
      measurement.phase = Phase::Total;
      measurement.value = Power::from_raw(value);
      measurement.provenance = make_recovered_provenance(
          scenario.source, AuthorityKind::Synthetic, options.generation, options.epoch, next_sequence, step_time,
          received);
      batch.measurements.push_back(std::move(measurement));

      if (options.include_frequency && (entity.kind() == EntityKind::Feed || entity.kind() == EntityKind::Bus)) {
        Measurement frequency;
        frequency.id = next_measurement;
        next_measurement = MeasurementId(next_measurement.value() + 1);
        frequency.entity = entity;
        frequency.phase = Phase::Total;
        const QuantityRep wobble = random.between(-80, 80);  // within +-0.08 Hz of nominal
        frequency.value = Frequency::from_raw(50000 + wobble);
        frequency.provenance = make_recovered_provenance(scenario.source, AuthorityKind::Synthetic,
                                                         options.generation, options.epoch, next_sequence, step_time,
                                                         received);
        batch.measurements.push_back(std::move(frequency));
      }

      if (options.include_voltage && entity.kind() == EntityKind::Bus) {
        for (const Phase phase : {Phase::A, Phase::B, Phase::C}) {
          Measurement voltage;
          voltage.id = next_measurement;
          next_measurement = MeasurementId(next_measurement.value() + 1);
          voltage.entity = entity;
          voltage.phase = phase;
          voltage.value = Voltage::from_raw(230000 + random.between(-3000, 3000));
          voltage.provenance = make_recovered_provenance(scenario.source, AuthorityKind::Synthetic,
                                                         options.generation, options.epoch, next_sequence, step_time,
                                                         received);
          batch.measurements.push_back(std::move(voltage));
        }
      }

      if (options.include_power_factor && entity.kind() == EntityKind::Load) {
        Measurement factor;
        factor.id = next_measurement;
        next_measurement = MeasurementId(next_measurement.value() + 1);
        factor.entity = entity;
        factor.phase = Phase::Total;
        factor.value = Ratio::from_raw(950000 + random.between(-40000, 40000));
        factor.provenance = make_recovered_provenance(scenario.source, AuthorityKind::Synthetic,
                                                      options.generation, options.epoch, next_sequence, step_time,
                                                      received);
        batch.measurements.push_back(std::move(factor));
      }
    }

    next_sequence = Sequence(next_sequence.value() + 1);
    scenario.batches.push_back(std::move(batch));
    if (!shadow_batch.measurements.empty()) {
      scenario.batches.push_back(std::move(shadow_batch));
    }
  }

  return scenario;
}

}  // namespace po
