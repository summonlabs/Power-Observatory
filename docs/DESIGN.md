# Power Observatory - design rationale and decision records

This document records *why* the runtime is shaped the way it is. The public
contract is described in [README.md](../README.md); this file is for the reasoning
behind it, and for the decisions that would otherwise look arbitrary.

## 1. Integers, not floating point

Every physical quantity is a signed 64-bit integer in an exact sub-unit. Rationale:

* **Determinism is a contract, not a nicety.** Two runs that reach the same
  conclusion must publish byte-identical explanations. Binary floating point cannot
  promise that across platforms, optimisation levels or compilers, and the
  explanation model is only useful if it is reproducible.
* **Checked arithmetic is possible.** Integer overflow can be detected exactly.
  Floating-point saturation cannot: an infinity propagates silently through a
  reserve total and arrives at the operator looking like a very large number.
* **Exactness survives the round trip.** A value parsed from a document, stored,
  and read back is the same value, bit for bit.

The cost is that a value which is not exactly representable in the base sub-unit is
refused instead of rounded. `0.0001 W` is a refusal, not 0 W. That is the intended
trade: a silent rounding error in an electrical total is worse than a refusal that
names the problem.

`checked_mul_div` multiplies and divides without a 128-bit type by decomposing
`a = q*d + r`, which keeps every partial product inside the representable range
whenever the final result is representable. The quotient is therefore the truncated
*true* quotient rather than an approximation.

## 2. Four separations that the type system enforces

### Observation is not ownership

`AuthorityKind` is carried by every measurement. Only `Observed` is first-hand
electrical reality. `Configured` describes intent, `Acknowledged` records that a
controller accepted a request, `Synthetic` is generated, `External` is another
authority's assertion.

The runtime has no API that actuates anything, so 'owning' another authority is not
a policy it could violate by accident. What it could do by accident is *report*
another authority's assertion as its own observation. Carrying the authority in the
evidence makes that impossible to do silently.

### Acknowledgement is not effect

This is enforced twice, in two different places, on purpose:

* at ingest, `carries_measurement()` refuses an acknowledgement as a measurement
  (`authority_violation`);
* at freshness, an acknowledgement has no age band at all (`not_applicable`), because
  'how old is the news that a controller said yes' is not a meaningful question about
  the plant.

### Configured state is not observed state

The topology model is tagged `configured` on every element, and the engines treat it
as *structure* only. It tells them which entity is wired to which; it never supplies
a value that a measurement should have supplied. Where a declared value is the only
one available - declared transfer time, declared ride-through autonomy - the gate
that consumes it says so in its own detail text, and the gate is *not evaluable*
when the declaration is absent rather than passing by default.

### Recovered evidence is not fresh evidence

`EvidenceOrigin` and the monotonic anchor make this structural. Evidence decoded
from durable storage is rebuilt through `make_recovered_provenance`, which sets
`origin = RecoveredFromStore` and `has_monotonic_anchor = false`. Freshness can then
only be measured against the wall clock, and the wall-clock path *cannot* return
`Fresh`: a recent recovery becomes `Recovered`, which is usable and explicitly not
the current state of the plant.

The first draft of this collapsed recovered-but-recent evidence to `Stale`. That was
wrong in a way that mattered: it made every offline query return nothing at all,
because `Stale` is not usable. `Recovered` was added as its own class so that 'this
is recent' and 'this process did not observe it' are both true at once without either
being stretched.

## 3. An absent value is never a zero

`std::optional` is used for anything that could not be established. The temptation to
default a missing measurement to zero is strong and always wrong: it turns 'we do not
know what this branch draws' into 'this branch draws nothing', and a reserve total
computed from it looks healthy.

`LoadEstimate` therefore returns both a total *and* the list of entities it could not
measure. A reserve total is published only when every supply component has usable
load evidence; otherwise the partial total is published as an explicitly labelled
lower bound and the reserve itself is absent.

`worse()` composes determinations and can only move towards less certainty. An answer
can never be more current than the evidence behind it, which is why
`Snapshot::answer()` folds the aggregate freshness of its evidence into its own state.
This was a real defect found by the test suite: before the fix, a query against a
store whose newest evidence had expired still reported `known`.

## 4. Deterministic explanations

Reasons are accumulated in discovery order and canonicalised afterwards. Canonical
ordering is a total order on (code, severity, subject, detail, evidence), so two runs
that discover the same reasons in different orders publish identical text.

`Explanation::content_hash()` is computed over a canonicalised copy rather than over
the stored order, so a fingerprint is stable regardless of how the explanation was
assembled. The same reasoning applies to `EvidenceSet::content_hash()`: construction
canonicalises, so the hash of a set does not depend on arrival order.

The content hash deliberately covers the provenance origin. Live and recovered views
of the same evidence therefore hash differently - which is correct, because they are
different claims about the world, and a hash that could not tell them apart would be
the wrong hash.

## 5. Persistence decisions

### Append-only, with a CRC on the header *and* the payload

Two checksums rather than one because they answer different questions. The header CRC
decides whether the record's own framing can be trusted; without it, a damaged length
field would make the scan read an arbitrary span of bytes. The payload CRC decides
whether the content can be trusted.

### Torn tail versus interior corruption

The hard part of append-only recovery is that a damaged record at the end of the file
and a damaged record in the middle look identical from the damage alone. The rule is:
a damaged record is a torn tail **only if no well-formed record follows it**. The
probe searches for a later record whose magic, version, header CRC, payload CRC, index
and length all validate. If one exists, the damage is interior and the whole log is
refused.

The probe initially required the following record to have exactly the next expected
index. That is wrong for the *first* record: when record 0's payload is damaged, the
next expected index is 0, but record 1 has index 1, so the probe found nothing and the
log was silently truncated. The probe now accepts any later index at or above the
expected one. A test that flips a byte in each payload in turn is what caught it.

### A record is admitted whole or not at all

Truncation always lands on a record boundary. A partially-written record is discarded
in full, from its header, rather than partially admitted. This is why the discarded
byte count reported by recovery is the whole incomplete record, not the number of
bytes that were trimmed away.

### Read-only loads never repair

`EvidenceLog::load` takes a shared lock, reports a torn tail, and leaves it in place. A
reader that repairs what it reads is a writer wearing a reader's name.

### Identity travels with the mutation

`MutationId` and `AttemptId` live inside the same durable record as the measurements
they describe, so a retry can be recognised from the durable stream alone. If the
identity lived in a side table, a crash between the two writes would either lose the
mutation or apply it twice.

### The lock is owned by the process, not by the object

`LockFileEx` / `flock` release on process death, including abrupt termination. That is
what makes the lock usable as a crash-safe ownership guard: a writer that dies does
not leave a store that can never be opened again.

## 6. Concurrency decisions

### One writer, by construction

There is no API that lets a caller mutate published state. Ingest goes through a
bounded queue to the single worker; publication is a `shared_ptr` swap performed only
by that worker. This removes read-to-write upgrade as a category rather than managing
it.

### Callbacks run with no lock held

This is the decision that costs the most care and buys the most. A subscriber that
runs under the publication lock would deadlock the moment it called
`current_snapshot()`. The runtime copies the subscriber list under
`subscribers_mutex_`, releases it, and only then invokes the callbacks, so a callback
may re-enter any public entry point. `tests/integration_runtime.cpp` parks a callback
on a condition variable and enters every public entry point from another thread; if
any lock were held across the callback, that test would hang instead of passing.

A callback that throws is counted and swallowed. A hostile subscriber must not be able
to stop the plant's evidence from being recorded.

### Shutdown fulfils every future

Every queued request holds a promise. On stop, the worker finishes the request in
flight and completes the rest as `Cancelled`. No caller is ever left waiting on a
future that will never be fulfilled - which would be a hang in *their* code, caused by
ours.

### Lock order and what is never held across

`queue_mutex_` then `publication_mutex_` then `subscribers_mutex_`; `lifecycle_mutex_`
is a leaf and is never held together with anything. No mutex is held across thread
creation, thread join, a callback, or a wait on another thread. These are stated in
`runtime.hpp` because a reader of the code should be able to check them by
inspection, not by experiment.

## 7. Testing decisions

### No timeouts, anywhere

A hang is a defect. A test harness that kills a hung test converts the most valuable
signal in the suite - 'something deadlocks' - into a colour change. There is no
timeout in the harness and none in CTest.

### Children are launched without a shell

Both the source tree and the scratch directory can contain spaces. Passing a command
line through `cmd.exe` means trusting a shell to reassemble paths, and a quoting bug
then looks like a product bug. `_spawnv` / `posix_spawn` hand the program and its
arguments to the operating system directly.

### Deterministic synchronisation, not sleeps

Where a test needs a second process to be in a particular state, the child signals
that state by creating a file and reports its own readiness. The poll observes; it
never decides whether the behaviour is correct.

### Debug is a test configuration, not an afterthought

The first Debug run aborted two suites with exit code 3 and no output. The cause was
`std::find(left.sources().begin(), left.sources().end(), source)` where `sources()`
returns by value: two different temporaries, so a pair of iterators from two different
containers. It is undefined behaviour that happened to work in Release. Debug's
checked iterators refused it.

That is the whole argument for running every configuration. The harness now also
flushes per case and routes CRT assertion output to stderr, so the next abort of this
kind names itself immediately instead of vanishing into a lost buffer.

## 8. Bugs the suite found, and what each one taught

| Defect | Found by | Fix |
| --- | --- | --- |
| Same-sequence measurements in one batch were treated as duplicates, discarding all but one | the end-to-end pipeline producing one measurement from a 46-measurement batch | duplicate detection is keyed on (source, sequence, entity, kind, phase), because a batch shares one sequence across every measurement it carries |
| The declared-wiring traversal stopped at a bus and never reached the UPS behind it | failover autonomy never found a declared ride-through | `children(Bus)` now includes UPS units whose declared input it is |
| Conversion loss read the shared input bus instead of the node's own meter | loss attribution reported the whole bus as one PDU's input | input candidates are ordered by preference and the first usable one is used, not summed |
| Divergence verdicts were inverted | a test that advanced a generation and expected `advanced` | a left side that dominates is the newer one, so the right has gone backwards |
| `Snapshot::answer()` ignored the freshness of its own evidence | a query at a year-old instant reported `known` | the answer folds the aggregate freshness into its own state |
| A damaged *first* record was mistaken for a torn tail and silently truncated | byte-by-byte truncation and per-payload bit-flip tests | the interior-corruption probe accepts any later valid record at or above the expected index |
| `std::find` over two temporaries from one by-value accessor | the Debug configuration's checked iterators | the source lists are materialised once |
| A scenario generator emitted moved-from entities | the CLI refusing its own generated documents | the phase loop constructs each measurement independently |

## 9. What was deliberately not built

* **No actuation API.** Adding one would make 'observation is not ownership' a
  policy rather than a property.
* **No network surface.** A protocol would need authentication, authorisation and
  replay protection, and none of that is observable electrical state.
* **No topology interchange format.** Topology authority belongs to an adjacent
  runtime; inventing a format here would be claiming it.
* **No compaction or segment rotation.** Bounded by configuration instead, and stated
  as a limitation rather than implied to be solved.
* **No fabricated phase-angle synchronism.** The failover gate uses frequency
  agreement because that is what the evidence channels actually carry, and reports
  the phase-angle question as unsupported rather than answered.
