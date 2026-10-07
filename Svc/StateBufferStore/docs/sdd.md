# Svc::StateBufferStore Component

## 1. Introduction

`Svc::StateBufferStore` stores measurements produced across the flight software
and provides read access to them: the latest value, the minimum and maximum
values ever recorded ("watermarks"), and a per-entry circular history of recent
measurements. It decouples measurement producers from consumers — a producer
stores a value without knowing who reads it, and a consumer reads without
knowing who produced it.

The component is a port of a heritage flight software state buffer store
module, and implements the component proposed in F Prime discussion
[#5067](https://github.com/nasa/fprime/discussions/5067): a more feature-rich
`Svc::PolyDb` carrying per-entry status, watermarks, and configurable history
depth. `Svc::PolyDb` stores only the latest value of each entry; projects
needing watermarks or history should use this component instead.

Entries are declared in project configuration
(`config/StateBufferStoreCfg.fpp`) as an enumeration, and each entry's history
depth is set independently. Values are carried as `Fw::PolyType`, so a single
set of ports serves all supported primitive types; strings, structures, and
byte arrays use a parallel `Fw::Buffer` interface. Each entry is used through
exactly one of the two interfaces, fixed by its first store.

## 2. Requirements

The "Heritage" column traces each requirement to the Level 6 requirement it
derives from in the heritage module's specification.

| Requirement | Heritage | Description | Verification |
|---|---|---|---|
| REQ-STATEBUFFERSTORE-001 | HER-0004 | `Svc::StateBufferStore` shall provide a synchronous interface to store a primitive value for a single entry, recording a caller-supplied validity with it. | Unit Test |
| REQ-STATEBUFFERSTORE-002 | HER-0005, .0006 | `Svc::StateBufferStore` shall provide a synchronous interface to store a structure or fixed-length string for a single entry, recording a caller-supplied validity with it. | Unit Test |
| REQ-STATEBUFFERSTORE-003 | HER-0007 | `Svc::StateBufferStore` shall provide a synchronous interface to get the latest primitive measurement for an entry, reporting its value, time, and validity. | Unit Test |
| REQ-STATEBUFFERSTORE-004 | HER-0008, .0009 | `Svc::StateBufferStore` shall provide a synchronous interface to get the latest structure or string measurement for an entry, and shall report `INVALID_BUFFER_SIZE` without copying when the supplied buffer is too small. | Unit Test |
| REQ-STATEBUFFERSTORE-005 | HER-0011 | `Svc::StateBufferStore` shall record the minimum and maximum value stored for each entry, each with the time and validity in force when it was recorded, and shall provide interfaces to report them over a port and as an event. | Unit Test |
| REQ-STATEBUFFERSTORE-006 | HER-0014 | `Svc::StateBufferStore` shall provide an interface to clear an entry's watermarks, after which the next stored value becomes both watermarks. | Unit Test |
| REQ-STATEBUFFERSTORE-007 | HER-0013 | `Svc::StateBufferStore` shall provide an interface that reports an entry's watermarks and then clears them in one operation. | Unit Test |
| REQ-STATEBUFFERSTORE-008 | HER-0010 | `Svc::StateBufferStore` shall provide a synchronous interface to get a value entry's stored measurement history in chronological order as serialized `SbsMeasurement` records, and shall report `INVALID_BUFFER_SIZE` without copying when the supplied buffer cannot hold the entry's full depth. | Unit Test |
| REQ-STATEBUFFERSTORE-009 | HER-0100 | `Svc::StateBufferStore` shall provide a synchronous interface to get a value entry's most recent N stored measurements in chronological order, where N of zero requests the entry's full depth, and shall copy as many whole measurements as the supplied buffer holds when it cannot hold N. | Unit Test |
| REQ-STATEBUFFERSTORE-010 | HER-0017, .0018 | `Svc::StateBufferStore` shall store, for each entry independently, a configuration-specified history depth of at least two measurements, in memory obtained once from an `Fw::MemAllocator`, and shall overwrite the oldest measurement when the history is full. | Unit Test |
| REQ-STATEBUFFERSTORE-011 | HER-0020, .0021, .0022 | `Svc::StateBufferStore` shall store measurements of the primitive types `U8`, `I8`, `U16`, `I16`, `U32`, `I32`, `F32`, and `F64`, and shall store structures and fixed-length strings up to a configured maximum size. `U64`, `I64`, and `bool` are also accepted, as `Fw::PolyType` carries them. | Unit Test |
| REQ-STATEBUFFERSTORE-012 | HER-0023 | `Svc::StateBufferStore` shall time tag every stored measurement with the time obtained from its time port at the moment of storage. | Unit Test |
| REQ-STATEBUFFERSTORE-013 | derived | `Svc::StateBufferStore` shall report `NOT_WRITTEN` for an entry never stored to, and `NOT_FRESH` when the measurement has not changed since the caller's previous read. | Unit Test |
| REQ-STATEBUFFERSTORE-014 | derived (#5067) | `Svc::StateBufferStore` shall detect a measurement, watermark, or history read concurrently with a write, retry the read up to a configured number of attempts, and on exhaustion emit a FATAL event identifying the read and report `INCOHERENT` without copying out the read data. | Unit Test |
| REQ-STATEBUFFERSTORE-015 | derived | `Svc::StateBufferStore` shall perform every store and read operation without dynamic memory allocation after configuration and in bounded time. | Inspection |
| REQ-STATEBUFFERSTORE-016 | derived | `Svc::StateBufferStore` shall fix each entry as a value or data entry on its first store, and shall report `WRONG_KIND`, storing and copying nothing, for any operation of the other kind on it. | Unit Test |

Heritage requirements HER-0001, .0002, and .0003 govern shared-memory
allocation across space partitions and initialization order between them.
Space partitioning is out of scope for this component, so they are not ported.

## 3. Design

### 3.1 Component diagram

```
                   +----------------------------+
   putValue  ----->|                            |
   getValue  <---->|                            |
   putData   ----->|                            |
   getData   <---->|    Svc::StateBufferStore   |<----- timeCaller
   getMinMax <---->|                            |
   getHistory <--->|                            |-----> eventOut
   getNHistory <-->|                            |
                   |  (privileged)              |<----- cmdIn
   clearMinMax ---->|                           |-----> cmdResponseOut
   clearAndGetMinMax <->|                       |
                   +----------------------------+
```

### 3.2 Port kinds and concurrency

The component is `passive` and every input port is `sync`, so each handler runs
on its caller's thread with no mutex and no queue. This matches the heritage
module, which was a synchronous library with no thread of its own.

Readers and writers are coordinated by a per-entry **coherency counter** rather
than by guarded ports, following #5067's preference. The counter works as a
sequence lock covering everything the entry's writer touches: history ring,
payloads, latest index, stored count, and watermarks. A writer increments it
before writing, making it odd, and again afterwards, making it even. A reader
samples it before and after its copy, and accepts the copy only if the first
sample was even and the two are equal; otherwise a write overlapped the copy and
the bytes may mix two measurements. The reader re-reads the latest index on
every attempt, so a retry follows the newest measurement. After
`MAX_READ_ITERATIONS` failed attempts the component emits the FATAL
`FailedReadCoherentData` event, naming the read operation, and reports
`INCOHERENT` without copying anything out.

A history read is a single coherent read of the whole dump, not one per
measurement, so a dump never mixes measurements from before and after a write.

The counter is `std::atomic<U32>`. The writer's increments and the reader's
samples use the fence placement of a standard sequence lock (a release fence
after the first increment, an acquire fence before the second sample), the
ordering that keeps the protected stores and loads inside the counter
increments on weakly ordered processors. The heritage module used `volatile` reads and a
platform atomic-increment routine; `volatile` is not a substitute for atomics in
C++, so this is a deliberate upgrade. The counter wraps after 2^32 increments,
which is harmless: only equality between two adjacent samples matters.

The protected fields themselves are ordinary, non-atomic memory, as in any
sequence lock. A reader may therefore copy bytes while they are being written;
the counter guarantees such a copy is discarded, and every use of copied data
that could fault (the payload length, serializing a value) is bounded or
deferred until the copy is known to be coherent.

### 3.3 Watermarks and `Fw::PolyType` comparison

Each entry carries a clear-pending flag. A put that finds it set reseeds both
watermarks from its own value; otherwise each watermark updates when the new
value beats it. The flag is not an optimization — it is the only way a watermark
can ever be seeded. `Fw::PolyType::operator<` and `operator>` both return
`false` when the two operands hold different types, and a default-constructed
`Fw::PolyType` holds no type at all, so a comparison against an unseeded
watermark never succeeds. The flag therefore starts set, and a cleared
watermark reads back as a zeroed measurement with `NOT_WRITTEN` validity.

Watermarks are written inside the writer's coherency window, so watermark reads
are protected exactly as measurement reads are.

`clearAndGetMinMax` reports precisely the watermarks its clear ends. It sets the
flag first, then reads. If a put consumes the flag in between, that put has
already reseeded the watermarks, so a put that consumes the flag first moves the
watermarks it displaces to a retired pair, and the read reports those. Without
this, a put landing between the read and the clear would have its value neither
reported nor kept.

A consequence worth noting: storing a value of a *different* type than an
entry's existing watermarks will not update them, because the comparison cannot
succeed. Clear the watermarks when deliberately changing an entry's type.

### 3.4 Memory

`configure(memId, allocator)` makes a single request of the supplied
`Fw::MemAllocator` covering every entry's history ring and payload region, then
carves it into per-entry pointers, following `Svc::ComQueue` and
`Svc::BufferManager`. One request is used because `Fw::MemAllocator` does not
guarantee that an identifier may be reused across calls. The allocator must
outlive the component, which returns the memory on destruction.

The request uses `checkedAllocate`, which asserts rather than returning a short
allocation. A partial allocation would silently reduce the configured history
depth of an arbitrary subset of entries, violating
REQ-STATEBUFFERSTORE-010, and there is no useful degraded mode — unlike
`Svc::DpCatalog`, which can meaningfully run with fewer file slots.

Watermarks, the coherency counter, and the entry kind live in statically
allocated control blocks, not in the allocation, so the coherency protocol does
not depend on allocation having succeeded. The heritage module stored watermarks as two extra
ring slots; that was an artifact of its shared-memory offset arithmetic and
carries no semantic weight here.

### 3.5 Access control

The heritage module exposed watermark clearing through a separate "protected"
API available only to the component responsible for telemetry, so
that an arbitrary caller could not discard watermarks another subsystem was
accumulating. F Prime has no in-component equivalent of that privilege tier, so
`clearMinMax` and `clearAndGetMinMax` are ordinary ports and **access is
controlled by topology wiring**: connect them only to the component responsible
for reporting and resetting watermarks. The `CLEAR_WATERMARKS` command provides
the same capability to ground.

### 3.6 Status reporting

`SbsStatus` preserves the heritage status enumeration's values 0-5 so that ported
requirements and ground tooling keep their meaning, and adds `INCOHERENT` and
`WRONG_KIND`.

Three heritage behaviors were deliberately changed:

- **`INCOHERENT` is new.** The heritage `get` emitted the FATAL event on retry
  exhaustion and then returned the possibly-torn data with its stored validity,
  so a caller that ignored events could consume bad data silently. This
  component reports `INCOHERENT` and copies nothing out, so the caller can act
  on it. Note that in a deployment whose event manager forwards FATAL events to
  `Svc::FatalHandler`, the event itself ends the software; `INCOHERENT` matters
  in deployments that do not.
- **Entry kinds are refused, not asserted.** The heritage `put` asserted that a
  value matched its entry's configured type. Here an entry is fixed as a value
  or data entry by its first store, and every operation of the other kind —
  including the puts, which therefore return `SbsStatus` — reports `WRONG_KIND`
  and changes nothing. Watermarks and history are value-entry operations; a data
  entry's history is kept for coherency but is not readable.
- **`UNINITIALIZED` is unreachable.** The heritage module checked a global
  initialization flag on every call. Here, calling any port before
  `configure()` is a programming error and trips an assertion, which is the
  F Prime-idiomatic response. The value is retained in the enumeration for
  requirement traceability.

### 3.7 Event reporting of values

FPP cannot carry `Fw::PolyType` or `Fw::Time` in an event — neither is a
displayable type. The `WatermarkReport` event therefore widens values to `F64`
and reduces times to microseconds, following the `I64` microsecond convention in
`Svc::TimeConverter`. Integer magnitudes above 2^53 are reported imprecisely by
the event; the `getMinMax` port reports them exactly, and is the interface to
use when precision matters.

## 4. Configuration

`config/StateBufferStoreCfg.fpp` defines:

| Name | Meaning |
|---|---|
| `StateEntry` | The set of stored entries. `NUM_ENTRIES` is a required last member used to size the entry tables; it is not an addressable entry, and commands naming it are rejected. |
| `HistoryDepths` | Per-entry history depth. Each must be in `[2, MAX_HISTORY_DEPTH]`, asserted by `configure()`. |
| `MAX_HISTORY_DEPTH` | Upper bound on any entry's depth. |
| `DEFAULT_HISTORY_DEPTH` | Depth applied to entries not individually overridden. |
| `MAX_DATA_SIZE` | Maximum size of a string, structure, or byte-array measurement. |
| `MAX_READ_ITERATIONS` | Read attempts before reporting `INCOHERENT`. |

History reads return only measurements actually stored: before an entry's ring
fills, a read returns fewer records than its depth. Each record is a serialized
`SbsMeasurement`, `SbsMeasurement::SERIALIZED_SIZE` bytes long; narrow values
serialize in fewer bytes, and the remainder of the record is zeroed.

Memory used is
`sum over entries of depth * (sizeof(Measurement) + MAX_DATA_SIZE)`. Because
`MAX_DATA_SIZE` multiplies by depth for *every* entry, including entries that
only ever hold primitives, projects storing few structures should keep
`MAX_DATA_SIZE` tight.

## 5. Usage

Instantiate the component, connect its ports, then call `configure()` once
before any port is invoked, with an allocator that outlives the component:

```c++
Svc::StateBufferStore stateBufferStore("stateBufferStore");
Fw::MallocAllocator stateBufferStoreAllocator;

// after port connections are made
stateBufferStore.configure(0, stateBufferStoreAllocator);
```

A producer declares an output port of type `Svc.SbsPut` and a consumer one of
type `Svc.SbsGet`; the call names below follow whatever those ports are named in
the using component's FPP:

```fpp
output port storeValue: Svc.SbsPut
output port loadValue: Svc.SbsGet
```

```c++
Fw::PolyType value(static_cast<U32>(42));
const Svc::SbsStatus stored =
    this->storeValue_out(0, Svc::StateBufferStoreCfg::StateEntry::SBS_ENTRY_00, value, Svc::SbsStatus::OK);

Fw::PolyType readBack;
Fw::Time measTime;
const Svc::SbsStatus status =
    this->loadValue_out(0, Svc::StateBufferStoreCfg::StateEntry::SBS_ENTRY_00, readBack, measTime, this->m_lastRead);
```

Retaining the previous `measTime` as `lastReadTime` on the next call is what
produces `NOT_FRESH`, letting a consumer skip values it has already processed.
Freshness is judged by timestamp, so two stores within one tick of the time
source look identical to a reader, and a component whose `timeCaller` is
unconnected stamps every measurement with zero time.

## 6. Assumptions and limitations

- **One writer per entry.** As in the heritage design, where an entry is owned
  by the subsystem producing it. Concurrent writers to the same entry are not
  supported. Concurrent readers are safe, and a reader may safely race the
  single writer.
- **A torn read is detected, not prevented.** The counter tells a reader that a
  write intervened; it cannot stop one, so a reader racing a fast writer can
  exhaust its retries and report `INCOHERENT`. Because the counter covers the
  whole entry, a writer lapping the ring during a read is detected too.
- **One privileged clearer.** `clearMinMax`, `clearAndGetMinMax`, and
  `CLEAR_WATERMARKS` should not race one another on the same entry: a second
  clear landing inside a `clearAndGetMinMax` can overwrite the retired
  watermarks it is about to report.
- **Type changes do not re-seed watermarks** — see §3.3.
- **`configure()` is a new lifecycle requirement** that the heritage library did
  not have, since it allocated statically. A component whose `configure()` was
  never called asserts on first port use rather than reading null pointers.

## 7. Deferred work

- An autocoder generating entry identifiers, a telemetry-ID-to-entry-ID table,
  and history offsets, as proposed in #5067. Configuration is written by hand
  for now, as `Svc::PolyDb`'s is.
- Data product reporting: dumping all watermarks, or an entry's full history,
  as a data product rather than over a port.
- Encoding access permissions in entry identifiers (#5067), which would let the
  component enforce §3.5's privilege tier itself rather than relying on wiring.
- Topology integration. No deployment in this repository instantiates this
  component (nor `Svc::PolyDb`), so there is no integration test suite.

## 8. Change log

| Date | Description |
|---|---|
| 2026-10-05 | Initial version, ported from a heritage state buffer store module |
| 2026-10-07 | Sequence-lock coherency covering watermarks and whole history dumps; entry kinds; `SbsMeasurement` history records; explicit allocator in `configure()` |
