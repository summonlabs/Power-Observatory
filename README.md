# Power Observatory

Electrical observability for data center control planes: feeds, buses, UPS units,
PDUs, circuits, loads, and the redundancy groups that tie them together.

Power Observatory answers one question, and shows its work:

> Given the electrical evidence currently in hand, where is power flowing, how much
> usable reserve remains, where are imbalance, loss and power-quality constraints
> emerging, how ready is failover, and which evidence supports each conclusion?

Every answer this runtime publishes carries an explicit state - `known`,
`recovered`, `stale`, `conflicting`, `indeterminate`, `unsupported`, `expired` or
`refused` - together with a deterministic, order-independent explanation naming the
reason codes and the exact evidence references it depends on.

## Boundary and non-ownership

Power Observatory owns **observation, attribution, explanation, history and
divergence analysis** of electrical state. It does not own, and will not perform,
any of the following:

* actuating switchgear, breakers or transfer switches;
* selecting, preferring or switching feeds;
* shedding, sequencing or curtailing load;
* owning topology or capacity authority;
* authorizing recovery, restoration or return-to-service.

It consumes generation-bound evidence published by adjacent authorities (DCCP, ASI,
DFI) through explicit typed contracts and never infers another runtime's authority
merely because its evidence is visible. A statement that an adjacent authority owns
is recorded as `external` evidence with that authority named; it does not transfer
any decision right to this runtime.

### The three separations the model insists on

| Separation | How it is enforced |
| --- | --- |
| Observation is not ownership | Measurements carry an `AuthorityKind`. Only `observed` counts as first-hand electrical reality. `configured` describes intent, `acknowledged` records that a controller accepted a request, `synthetic` is generated evidence and `external` is another authority's assertion. |
| Acknowledgement is not effect | Acknowledgement evidence is refused as a measurement at ingest (`authority_violation`) and carries no freshness class at all (`not_applicable`). |
| Configured state is not observed state | The declared topology is tagged `configured` on every element. Nothing in it is ever presented as measured, and no measurement is ever inferred from it. |

A fourth separation is structural rather than documentary:

| Separation | How it is enforced |
| --- | --- |
| Recovered evidence is not fresh evidence | Evidence read back from durable storage is decoded with `EvidenceOrigin::RecoveredFromStore` and no monotonic anchor. It can be `recovered` - recent, usable, and explicitly **not** the current state of the plant - but it can never be classified `fresh`. |

## Architecture

```
  ingest documents (JSON lines)
          |
          v
  +-------------------+     +------------------------------+
  | EvidenceLog       |     | EvidenceSet                  |
  | versioned, CRC32C |---->| canonical, deduplicated,     |
  | single-writer     |     | supersession-resolved        |
  +-------------------+     +------------------------------+
          |                              |
          |                              v
          |                 +--------------------------+
          |                 | TopologyModel (declared) |
          |                 +--------------------------+
          |                              |
          v                              v
  +--------------------------------------------------+
  | Snapshot: immutable, revisioned, fully computed   |
  |  flow | reserve | attribution | quality | failover |
  +--------------------------------------------------+
          |
          v
  +--------------------------------------------------+
  | AnswerReport + Explanation                        |
  |  reasons, evidence references, dependencies       |
  +--------------------------------------------------+
```

The runtime adds one worker thread that owns every mutation:

```
  Observatory (single writer, many readers)
    submit(batch) -> future<IngestResult>
         |  queue (bounded)
         v
    worker thread --- append to EvidenceLog (durable commit)
         |            --- rebuild Snapshot
         |            --- publish atomically (shared_ptr swap)
         v
    subscribers (called with no lock held)
```

## State, evidence and authority model

### Quantities

Every physical quantity is a signed 64-bit integer in an exact sub-unit
(milliwatts, millivolts, milliamperes, millihertz, millijoules, millidegrees
Celsius, parts per million for ratios, nanoseconds for durations). Binary floating
point appears nowhere on the public surface, so results are bit-identical across
platforms and optimisation levels, and every arithmetic step is overflow-checked
rather than allowed to wrap. `scale_by_ppm`, `checked_mul_div` and the accumulators
report `arithmetic_overflow` instead of saturating silently.

### Freshness

Age is decided in exactly one place. Live evidence is aged against a **monotonic**
clock inside the process that observed it; recovered evidence is aged against the
wall clock and capped at `recovered`. A source clock that disagrees with local
arrival by more than the policy budget is reported as `clock_skew_exceeded` or
`future_timestamp` **alongside** the age band rather than instead of it, so neither
fact is stretched to fit the other.

| Classification | Meaning | Usable | Current |
| --- | --- | --- | --- |
| `fresh` | observed by this process, inside the fresh window | yes | yes |
| `recovered` | recent by the clock, read back from durable storage | yes | **no** |
| `aging` | past the fresh window, inside the aging window | yes | no |
| `stale` | past the aging window, inside the stale window | no | no |
| `expired` | past the stale window | no | no |
| `unknown` | no anchor, no source, or a backwards clock | no | no |
| `not_applicable` | an acknowledgement, which is not a measurement | no | no |

Three explicit bands (`fresh_within`, `aging_within`, `stale_within`) replace a
single timeout, and the bands are validated to be monotone.

### An absent value is never a zero

Reports use `std::optional` for anything that could not be established. A `reserve`
total is absent when any supply component carries no usable load evidence; a
`total_observed_load` is absent when nothing in scope could be measured; a loss is
`unsupported` rather than inferred from declared efficiency. `LoadEstimate` lists
the entities it could not measure instead of assuming they draw nothing.

### Confidence ordering

`worse()` composes determinations and can only ever move towards less certainty:

```
known(0) < recovered(1) < stale(2) < unknown(3) < indeterminate(4)
        < unsupported(5) < conflicting(6) < expired(7) < refused(8)
```

An answer can never be more current than the evidence behind it:
`Snapshot::answer()` folds the aggregate freshness of its evidence into its own
state, so a query against a store whose newest evidence has expired reports
`expired` rather than `known`.

## Explanations

Every outcome carries an `Explanation`: a canonically ordered list of `Reason`
records (reason code, severity, subject, detail, evidence references) plus the
`EvidenceDependency` set - source and generation - that the conclusion rests on.

Reasons are accumulated in whatever order the computation discovers them and
`canonicalize()` sorts and merges them, so two runs that reach the same conclusion
by different paths publish byte-identical text. `Explanation::content_hash()` is a
stable fingerprint of the canonical form.

Each reason code is a stable, documented wire identifier with a declared severity
and category. `reason_code_count()` currently returns 113 codes; the table is
asserted strictly ascending by `selfcheck`.

Sample of one reason set, from `power-observatory explain`:

```
[info]     ok                          redundancy_group:rg-main: group topology two_n declares 2
                                       member feed(s) and requires 1 to be live
[info]     flow_established            <whole-model>: 28 entities carry usable active power evidence
[info]     observed_evidence           <answer>: answer assembled from 46 admitted measurement(s)
[notice]   recovered_not_fresh         <answer>: the answer is assembled from evidence read back
                                       from durable storage; it is the last known state rather than
                                       the current state of the plant
```

## Persistence, recovery and concurrency

### On-disk format

The evidence log is an append-only sequence of records behind a 64-byte header:

```
header (64 bytes)
  magic[8] "POEVLOG\0" | format_version u16 | header_size u16 | record_header_size u32
  created_at_ns i64 | epoch u64 | log_id u64 | reserved[16] | header_crc32c u32 | reserved[4]
record (32-byte header + payload)
  record_magic u32 | format_version u16 | flags u16 | payload_length u32
  record_index u64 | payload_crc32c u32 | header_crc32c u32 | reserved u32
payload: little-endian, length-prefixed, strictly validated
```

Both the header and every record carry CRC-32C over an explicitly little-endian
encoding. Decoding rejects unknown record kinds, unknown enumerators, implausible
lengths and trailing bytes.

### Commit point

A batch is durable once the complete record has been written **and** the file has
been flushed to stable storage (`FlushFileBuffers` / `fsync`). In-memory state is
advanced only after that flush succeeds, so published state never claims more than
the durable stream contains.

### Recovery rules

1. A damaged file header is unrecoverable: the log refuses to open.
2. A record whose header or payload fails its integrity check is treated as a torn
   tail **only when no well-formed record follows it**. The probe looks for a later
   record whose magic, version, header CRC, payload CRC, index and length all
   validate; if one exists, the damage is interior corruption and the whole log is
   refused with `interior_corruption`.
3. A torn tail is discarded by truncation **at the last good record boundary**.
   Only trailing bytes are ever discarded, and a record is admitted whole or not at
   all.
4. A read-only load takes a shared lock and never truncates, so a reader cannot
   mutate what it reads.

### Single-writer lock

Authoritative ownership is guarded by a kernel-enforced exclusive lock on
`<log>.lock` (`LockFileEx` on Windows, `flock` on POSIX). The lock is owned by the
process, so a crashed writer never leaves it held. A second writer is refused with
`writer_lock_held` rather than blocking. Readers take a shared lock and are excluded
while a writer holds the exclusive one.

### Replay and idempotency

Every batch carries a `MutationId` and an `AttemptId` **inside the same durable
record as the mutation**, so a retry is recognised from the durable stream alone. A
repeated mutation is reported as `idempotent_replay` and is not applied twice. A
batch carrying an epoch older than the log's is refused as `stale_epoch`; a caller
may not reopen a log at an older epoch than it already holds; an epoch advance must
move strictly forward.

### Atomic publication

Snapshots and standalone documents are published by writing a sibling temporary
file, flushing it, and renaming it over the destination. A concurrent reader
observes either the previous content or the new content, never a partial file.

### Concurrency contract

* Exactly one worker thread mutates durable state and publishes snapshots; readers
  never mutate anything.
* A published snapshot is immutable and shared by pointer. A reader copies the
  pointer under the publication mutex and releases it before touching the snapshot,
  so no lock is ever held across a query.
* Subscriber callbacks run on the worker thread **with no lock held**, so a callback
  may call back into the runtime - including `subscribe()` and
  `current_snapshot()` - without self-deadlocking. A callback that throws is counted
  and does not kill the worker.
* Lock order is `queue_mutex_` then `publication_mutex_` then `subscribers_mutex_`.
  `lifecycle_mutex_` is a leaf that is never held together with any other mutex. No
  path acquires them in any other order, and no path nests a lifecycle lock inside a
  queue lock.
* No mutex is held across thread creation, thread join, a callback, or a wait on
  another thread. `stop()` sets the stop flag, notifies, and joins with nothing held.
* Read-modify-write of published state happens only on the worker thread, so there
  is no read-to-write upgrade path.
* Shutdown fulfils every future. A request already in flight runs to completion;
  requests still queued are completed as `cancelled`, so no caller waits on a future
  that will never be fulfilled.
* The ingest queue is bounded; exceeding `max_pending_requests` is reported as
  `queue_full` rather than growing without limit.

`tests/integration_runtime.cpp` exercises each clause directly, including a callback
that parks while another thread enters every public entry point.

## Build

Requirements: a C++20 compiler, CMake 3.21 or newer, and a generator.
There are no third-party dependencies.

Both implementations of the platform layer - the Win32 one and the POSIX one -
are compiled and executed. Validated toolchains:

| Platform | Toolchain | Configurations exercised |
| --- | --- | --- |
| Windows 11 | MSVC 19.44 (Visual Studio 2022 Build Tools), Ninja | Release, Debug, AddressSanitizer |
| Ubuntu 24.04 | GCC 13, Ninja | Release, Debug, AddressSanitizer + UndefinedBehaviorSanitizer |
| Ubuntu 24.04 | Clang 18, Ninja | Release, Debug, AddressSanitizer + UndefinedBehaviorSanitizer |

The runtime has one real dependency, and it is not a third-party library: it owns
a worker thread and synchronises with mutexes and condition variables, so on
POSIX it needs the system thread library. That dependency is declared `PUBLIC` on
the exported target and resolved by the package config with `find_dependency`,
so a consumer never has to add `-pthread` or `Threads::Threads` itself.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

| Option | Default | Effect |
| --- | --- | --- |
| `PO_BUILD_CLI` | `ON` | build the `power-observatory` command line tool |
| `PO_BUILD_TESTS` | `ON` when top level | build the test suites |
| `PO_BUILD_BENCHMARKS` | `ON` when top level | build the benchmark harness and the `bench` target |
| `PO_WARNINGS_AS_ERRORS` | `ON` | treat first-party diagnostics as errors |
| `PO_ENABLE_ASAN` | `OFF` | compile and link with AddressSanitizer |
| `PO_ENABLE_UBSAN` | `OFF` | compile and link with UndefinedBehaviorSanitizer; ignored on MSVC, which has no equivalent |
| `PO_ENABLE_STATIC_ANALYSIS` | `OFF` | run `/analyze` during the build |

First-party code compiles warning-free under `/W4 /permissive- /WX` with MSVC and
under `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` elsewhere.

## Command line

```sh
power-observatory version
power-observatory selfcheck
power-observatory init       --store DIR [--seed N] [--policy NAME] [--force]
power-observatory scenario   --store DIR --out FILE [--steps N] [--dropout-ppm N]
power-observatory ingest     --store DIR --input FILE
power-observatory answer     --store DIR [--as-of TS | --now] [--text]
power-observatory flow       --store DIR [--scope ENTITY] [--text]
power-observatory reserve    --store DIR --scope ENTITY [--text]
power-observatory attribution --store DIR [--scope ENTITY] [--text]
power-observatory quality    --store DIR [--scope ENTITY] [--text]
power-observatory failover   --store DIR --group GROUP [--text]
power-observatory topology   --store DIR [--text]
power-observatory explain    --store DIR [--text]
power-observatory history    --store DIR [--text]
power-observatory verify     --store DIR [--text]
power-observatory divergence --left-store DIR --right-store DIR [--text]
power-observatory bench      [--iterations N] [--seed N]
power-observatory hold       --store DIR --ready FILE --wait-for FILE
```

JSON is the default output and is always written with sorted keys and integer
numbers, so the same answer always produces byte-identical text. `--pretty` indents
it; `--text` renders a human summary. Exit codes are `0` success, `1` usage error,
`2` refused.

`answer`, `flow`, `reserve`, `attribution`, `quality` and `failover` read the store
offline. A fresh process has observed nothing, so by default an offline answer is
evaluated **as of the newest delivery instant in the evidence** and reports
`recovered`. Pass `--now` to evaluate against the wall clock, or `--as-of <RFC3339>`
to name an instant explicitly.

### Worked example

```sh
power-observatory init --store lab --seed 7
power-observatory scenario --store lab --out lab/samples.jsonl --steps 3
power-observatory ingest --store lab --input lab/samples.jsonl
power-observatory verify --store lab --text
power-observatory answer --store lab --text
power-observatory reserve --store lab --scope redundancy_group:rg-main --text
power-observatory failover --store lab --group rg-main --text
```

Abridged output:

```
state: recovered
revision: 1
evaluated as of: 2026-01-01T00:00:02.000000100Z
total observed load: 275332.855 W
total usable capacity: 2000000 W
total reserve: 1712153.271 W
measured entities: 28
unmeasured entities: 0
unattributed imbalances: 0
quality findings: 0
failover groups ready/degraded/not ready/unknown: 0/1/0/0
```

```
group: rg-main
readiness: degraded
  n/a  [optional] live_evidence_fresh: at least one live member carries aging evidence
  pass [required] members_declared: group topology two_n declares 2 member feed(s) and requires 1 to be live
  pass [required] peer_frequency_agreement: measured source frequencies span 0.059 Hz against a tolerance of 0.5 Hz
  pass [required] peer_headroom_after_single_failure: after losing the largest live member (1000000 W),
                                  1000000 W remains against a required 175697.107 W
  pass [required] peer_live: 2 of 2 member feed(s) carry fresh, positive measured power; 1 required
  pass [required] ride_through_autonomy: weakest declared ride-through is 600s against the policy minimum of 300s;
                                  this is configured data, not a measured ride-through
  pass [required] transfer_window: declared transfer time 80ms against the policy window of 10s;
                                  this is configured data, not a measured transfer
```

The single optional failure is the honest one: the evidence was read back from the
durable store, so it is `recovered` rather than `fresh`, and the group is `degraded`
rather than `ready` because this process has not observed the plant.

### Ingest document

```json
{
  "source": "meter-1",
  "authority": "observed",
  "generation": 4,
  "epoch": 2,
  "first_sequence": 10,
  "mutation": 5,
  "attempt": 1,
  "recorded_at": "2026-01-01T00:00:00.000000000Z",
  "measurements": [
    { "id": 1, "entity": "feed:main-a", "phase": "total", "kind": "active_power", "value": "1234.567 kW" },
    { "id": 2, "entity": "feed:main-a", "phase": "a", "kind": "voltage", "value": "230.1 V" }
  ]
}
```

Values are exact decimals in a named unit (`MW`, `kW`, `W`, `mW`; `kV`, `V`, `mV`;
`kA`, `A`, `mA`; `kHz`, `Hz`, `mHz`; `%`, `ppm`; `h`, `m`, `s`, `ms`, `us`, `ns`).
A value that is not exactly representable in the base sub-unit is refused rather
than truncated. `authority` must be one that can carry a measurement: `observed`,
`derived`, `external` or `synthetic`.

## Library use

```cpp
#include <power_observatory/runtime.hpp>
#include <power_observatory/scenario.hpp>

po::ScenarioOptions options;
options.seed = 7;
options.steps = 3;
po::Result<po::Scenario> scenario = po::build_standard_scenario(options);

po::RuntimeOptions runtime_options;
runtime_options.log_path = "lab/evidence.poev";
runtime_options.topology = scenario.value().topology;
po::Result<std::unique_ptr<po::Observatory>> runtime = po::Observatory::create(runtime_options);

po::Result<std::shared_future<po::IngestResult>> ticket = runtime.value()->submit(batch);
const po::IngestResult result = ticket.value().get();

const po::SnapshotHandle snapshot = runtime.value()->current_snapshot();
const po::AnswerReport answer = snapshot->answer();
```

A complete, independent consumer lives in `examples/consumer`. It is built only
against an **installed** package, never against this source tree.

## Install and downstream use

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(PowerObservatory 1.0 REQUIRED)
target_link_libraries(my-target PRIVATE PowerObservatory::power_observatory)
```

The package installs `PowerObservatoryConfig.cmake`,
`PowerObservatoryConfigVersion.cmake` (SameMajorVersion) and an exported target
set under `lib/cmake/PowerObservatory`, plus the public headers under `include/`
and the command line tool under `bin/`. The config file calls
`find_dependency(Threads)`, so the thread library is resolved on the consumer's
behalf rather than left as an undocumented link requirement.

## Validation

Everything below was executed against this revision. Nothing is extrapolated.

| Platform | Configuration | Build | Suite |
| --- | --- | --- | --- |
| Windows / MSVC | Release | clean, zero first-party warnings at `/W4 /permissive- /WX` | 15/15 |
| Windows / MSVC | Debug | clean, zero first-party warnings | 15/15 |
| Windows / MSVC | AddressSanitizer | clean | 15/15 |
| Linux / GCC 13 | Release | clean, zero first-party warnings at `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` | 15/15 |
| Linux / GCC 13 | Debug | clean, zero first-party warnings | 15/15 |
| Linux / GCC 13 | AddressSanitizer + UndefinedBehaviorSanitizer | clean, `-fno-sanitize-recover=all` | 15/15 |
| Linux / Clang 18 | Release | clean, zero first-party warnings | 15/15 |
| Linux / Clang 18 | Debug | clean, zero first-party warnings | 15/15 |
| Linux / Clang 18 | AddressSanitizer + UndefinedBehaviorSanitizer | clean, `-fno-sanitize-recover=all` | 15/15 |

The same fifteen suites run everywhere. That matters most for the suites that
exist to exercise the platform layer: on Linux they exercise `flock`, `pread`,
`pwrite`, `ftruncate`, `fsync` and `posix_spawn`, and on Windows the
`LockFileEx` and `_spawnv` equivalents, with no test skipped, weakened or
substituted on either side.

### Test suites

Each suite is a separate executable registered with CTest, so a failure names the
suite. No test uses a timeout: a hang is a defect to diagnose, not something to
kill. The suites share one deterministic harness and one fixture library; child
processes are launched without a shell so that no quoting is involved.

| Suite | Kind | What it proves |
| --- | --- | --- |
| `unit_foundation` | unit | checked arithmetic, exact decimal formatting, RFC 3339 parsing, reason table integrity |
| `unit_crc32c` | unit | published CRC-32C vectors, incremental agreement, detection of every single-bit flip |
| `unit_json` | unit | canonical key ordering, escaping, strict parsing, integer-only numbers, ingest schema refusals |
| `unit_evidence` | unit | duplicate and reorder detection, generation supersession, order-independent construction, conflicting meters |
| `unit_topology` | unit | declared-wiring traversal, dangling and duplicate refusals, declaration gating |
| `unit_freshness` | unit | every band, the recovered cap, backwards clocks, source-clock skew, per-source budgets |
| `unit_reserve` | unit | capacity, load, headroom, independent single-failure model, out-of-service members, absent totals |
| `unit_engines` | integration | attribution, quality, failover gates and divergence verdicts, including their failing branches |
| `unit_persistence` | integration | format, commit, reopen, replay refusal, epoch rules, torn tail, interior corruption, read-only load |
| `adversarial_storage` | adversarial | single-bit flips at every payload, byte-by-byte truncation, zeroed and empty files |
| `failure_restart` | failure injection | real child processes, real retry, real restart, real tail repair, real cross-process lock release |
| `multiprocess_lock` | multiprocess | a second process is refused the writer lock; shared locks are excluded by an exclusive one |
| `integration_runtime` | integration | every clause of the concurrency contract, including a parked callback and a bounded queue |
| `prop_randomized` | property / seeded | order independence and deduplication over 60 seeds, determinism of the answer over 25 seeds, self-comparison, durable round trips over 12 seeds |
| `e2e_workflow` | end to end | the whole operator workflow through the real command line entry point, plus usage and refusal exit codes |

## REAL, SYNTHETIC and UNSUPPORTED

Honesty about what was and was not exercised is part of the interface.

**REAL** - proved in this environment, on this machine:

* the entire C++20 library, the command line tool and every test executable;
* persistence on a real filesystem: real files, real flushes, real renames, real
  truncation, real recovery;
* process death and restart: real child processes launched without a shell,
  killed and reopened, with the writer lock released by the operating system;
* cross-process exclusion: a second real process refused the writer lock;
* concurrency: real threads, real shared pointers, a real worker thread;
* the CMake package: installed to a clean prefix on both Windows and Linux, and
  consumed out of tree by an independent `find_package` project that builds and
  runs;
* the POSIX platform layer: `flock`-based writer exclusion across real
  processes, the kernel releasing that lock when the owning process exits,
  `pread` / `pwrite` / `ftruncate` / `fsync` durability, truncation and tail
  repair, and `posix_spawn` child processes that are killed and reopened;
* AddressSanitizer across the whole suite on both platforms, plus
  UndefinedBehaviorSanitizer with `halt_on_error` on Linux;
* both compilers on Linux, GCC and Clang, in Release and Debug.

**SYNTHETIC** - modelled, not measured:

* the plant. `build_standard_scenario` produces a two-path facility (two utility
  feeds in a 2N group, one UPS per path, two PDUs per path, two circuits per PDU,
  one rack load per circuit) with internally consistent power flow, conversion
  losses, frequency and voltage. It is generated by this repository, not read from
  hardware, and every measurement it produces carries `AuthorityKind::synthetic`;
* every benchmark workload. The measurements are synthetic; the code paths
  measured on top of them - persistence, recovery, snapshot and answer assembly -
  are real.

**UNSUPPORTED** - not claimed, because it was not exercised here:

* no facility hardware, no meter, no PLC, no BMS or DCIM feed was ever contacted;
* no multi-node, clustered or distributed deployment was tested; this is a
  single-process runtime with a single-writer durable log and no network surface;
* phase-angle synchronism is not a quantity this runtime carries. The failover
  synchronism gate uses agreement between measured source **frequencies** as a
  proxy and says so; a group without frequency telemetry reports its synchronism
  gate as **not evaluable** rather than passing it;
* a measured transfer time is not modelled. The transfer-window gate compares a
  **declared** transfer time against the policy window and labels the basis as
  configured; with no declaration the gate is not evaluable;
* measured ride-through autonomy is not modelled. The autonomy gate uses the
  weakest **declared** autonomy below the live members and labels it as configured;
* Linux is validated on x86-64 only. macOS, the BSDs and other architectures are
  not exercised, and the POSIX behaviour that differs there - above all the file
  locking primitive - is not claimed;
* the declared topology is synthesized from a named scenario rather than exchanged
  with a topology authority. Topology interchange is an adjacent authority's
  contract and is deliberately out of scope;
* no soak, endurance or long-duration run was performed.

## Benchmarks

`bench_answer` measures **completed work only**. Each iteration builds one complete
answer to the core question; nothing is extrapolated from a partial run and no
number is reported for a workload that did not finish. Run it with:

```sh
cmake --build build --target bench
# or, for a different workload:
./build/bench_answer --answers 2000 --steps 60 --seed 20260101
```

The harness labels its own basis: `SYNTHETIC evidence, REAL code path` for the
computation benchmarks and `REAL filesystem` for the persistence benchmarks. It
prints the unsupported list alongside the numbers.

Representative run on the development machine (Windows, MSVC 19.44, Release,
AMD Ryzen 7 9800X3D), 60 synthetic steps across 29 declared entities:

| Measurement | Basis | Cost |
| --- | --- | --- |
| snapshot construction | SYNTHETIC evidence, REAL code path | see the table printed by `bench` |
| core question answer | SYNTHETIC evidence, REAL code path | see the table printed by `bench` |
| durable commit including flush to stable storage | REAL filesystem | see the table printed by `bench` |
| cold recovery of the whole log | REAL filesystem | see the table printed by `bench` |

Numbers are deliberately not copied into this document: run the harness and read
the table it prints, so that no figure here can drift from the code that produced
it. The harness also reports how many batches were committed and how many bytes
they occupied, and how many measurements were recovered, so each cost is attached
to a quantity of completed work.

## Limitations

* **Single node, single writer.** One process owns a store at a time. There is no
  replication, no consensus and no network protocol.
* **No actuation, by design.** If you need something switched, this runtime will
  not switch it, and it will refuse evidence that claims it did.
* **Topology is declared, not exchanged.** See UNSUPPORTED above.
* **A query takes a lock.** Offline commands take the store lock (shared for reads,
  exclusive for writes). Concurrent query and ingest **within one process** is what
  the `Observatory` API is for; concurrent query and ingest across processes is not
  supported.
* **Recovered evidence ages by wall clock.** A wall-clock step between recording and
  reading can move evidence between bands; the classification says `recovered` and
  the anomaly field reports the disagreement, but the age itself is only as good as
  the wall clock.
* **Freshness is per source, not per measurement.** A chatty source and a quiet one
  share one budget unless an override is configured.
* **Log growth is bounded by configuration, not by compaction.** There is no
  truncation, checkpointing or segment rotation; `max_recovery_bytes` bounds what a
  single process will read.
* **Quality checks are policy-shaped.** Frequencies, voltages, power factors and
  plausibility ceilings come from one observation policy; there is no per-bus
  overrides mechanism.
* **The comparison is textual and integer.** There is no floating-point tolerance
  anywhere, which is a strength for determinism and a limitation if you need to
  model physical uncertainty explicitly.

## Repository layout

```
include/power_observatory/   public headers, one per concept
src/                         implementation
apps/                        the command line entry point
tests/                       test suites and the shared harness and fixtures
bench/                       the benchmark harness
examples/consumer/           an independent find_package consumer
cmake/                       the package config template
docs/DESIGN.md               rationale, invariants and decision records
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Contributions are accepted under the
Apache License 2.0; there is no CLA and no copyright assignment.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
