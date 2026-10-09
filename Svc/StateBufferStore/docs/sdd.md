# Svc::StateBufferStore Component

## 1. Introduction

`Svc::StateBufferStore` stores measurements produced across the flight software
and provides read access to them: the latest value, the minimum and maximum
values recorded since they were last cleared ("watermarks"), and a per-entry
circular history of recent measurements. It decouples measurement producers from consumers — a producer
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
exactly one of the two interfaces, fixed by its first store or, for an entry
mapped to a telemetry channel, by `configure()`.

## 2. Requirements

The "Heritage" column traces each requirement to the Level 6 requirement it
derives from in the heritage module's specification.

| Requirement | Heritage | Description | Verification |
|---|---|---|---|
| REQ-STATEBUFFERSTORE-001 | HER-0004 | `Svc::StateBufferStore` shall provide a synchronous interface to store a primitive value for a single entry, recording a caller-supplied validity of `OK` or `INVALID` with it and refusing any other validity with `WRONG_KIND`, storing nothing. | Unit Test |
| REQ-STATEBUFFERSTORE-002 | HER-0005, .0006 | `Svc::StateBufferStore` shall provide a synchronous interface to store a structure or fixed-length string for a single entry, recording a caller-supplied validity of `OK` or `INVALID` with it and refusing any other validity with `WRONG_KIND`, storing nothing. | Unit Test |
| REQ-STATEBUFFERSTORE-003 | HER-0007 | `Svc::StateBufferStore` shall provide a synchronous interface to get the latest primitive measurement for an entry, reporting its value, time, and validity. | Unit Test |
| REQ-STATEBUFFERSTORE-004 | HER-0008, .0009 | `Svc::StateBufferStore` shall provide a synchronous interface to get the latest structure or string measurement for an entry, and shall report `INVALID_BUFFER_SIZE` without copying when the supplied buffer is too small. | Unit Test |
| REQ-STATEBUFFERSTORE-005 | HER-0011 | `Svc::StateBufferStore` shall record the minimum and maximum value stored for each entry, each with the time and validity in force when it was recorded, and shall provide interfaces to report them over a port and as an event. | Unit Test |
| REQ-STATEBUFFERSTORE-006 | HER-0014 | `Svc::StateBufferStore` shall provide an interface to clear an entry's watermarks, after which the next stored value becomes both watermarks. | Unit Test |
| REQ-STATEBUFFERSTORE-007 | HER-0013 | `Svc::StateBufferStore` shall provide an interface that reports an entry's watermarks and then clears them in one operation. | Unit Test |
| REQ-STATEBUFFERSTORE-008 | HER-0010 | `Svc::StateBufferStore` shall provide a synchronous interface to get a value entry's stored measurement history in chronological order as serialized `SbsMeasurement` records, and shall report `INVALID_BUFFER_SIZE` without copying when the supplied buffer cannot hold the entry's full depth. | Unit Test |
| REQ-STATEBUFFERSTORE-009 | HER-0100 | `Svc::StateBufferStore` shall provide a synchronous interface to get a value entry's most recent N stored measurements in chronological order, where N of zero or N exceeding the entry's depth requests the entry's full depth, and shall copy as many whole measurements as the supplied buffer holds when it cannot hold N, reporting `INVALID_BUFFER_SIZE` without copying when it cannot hold one. | Unit Test |
| REQ-STATEBUFFERSTORE-010 | HER-0017, .0018 | `Svc::StateBufferStore` shall store, for each entry independently, a configuration-specified history depth of at least two measurements, in memory obtained once from an `Fw::MemAllocator`, and shall overwrite the oldest measurement when the history is full. | Unit Test |
| REQ-STATEBUFFERSTORE-011 | HER-0020, .0021, .0022 | `Svc::StateBufferStore` shall store measurements of the primitive types `U8`, `I8`, `U16`, `I16`, `U32`, `I32`, `F32`, and `F64`, and shall store structures and fixed-length strings up to a configured maximum size, reporting `INVALID_BUFFER_SIZE` and storing nothing for a larger one. `U64`, `I64`, and `bool` are also accepted, as `Fw::PolyType` carries them. | Unit Test |
| REQ-STATEBUFFERSTORE-012 | HER-0023 | `Svc::StateBufferStore` shall time tag every measurement stored through `putValue` or `putData` with the time obtained from its time port at the moment of storage. | Unit Test |
| REQ-STATEBUFFERSTORE-013 | derived | `Svc::StateBufferStore` shall report `NOT_WRITTEN` for an entry never stored to, and `NOT_FRESH` when the measurement has not changed since the caller's previous read; a caller supplying no previous read time (`Fw::ZERO_TIME`) shall not be checked for freshness. | Unit Test |
| REQ-STATEBUFFERSTORE-014 | derived (#5067) | `Svc::StateBufferStore` shall detect a measurement, watermark, or history read concurrently with a write, retry the read up to a configured number of attempts, and on exhaustion emit a warning-high event identifying the read and report `INCOHERENT` without copying out the read data. | Unit Test |
| REQ-STATEBUFFERSTORE-015 | derived | `Svc::StateBufferStore` shall perform every store and read operation without dynamic memory allocation after configuration and in bounded time. | Inspection |
| REQ-STATEBUFFERSTORE-016 | derived | `Svc::StateBufferStore` shall fix each entry as a value or data entry on its first store, or as a value entry written only through `tlmIn` when `configure()` maps a telemetry channel to it, and shall report `WRONG_KIND`, storing and copying nothing, for any operation of the other kind on it and for any put to a telemetry-mapped entry. | Unit Test |
| REQ-STATEBUFFERSTORE-017 | derived (#5067) | `Svc::StateBufferStore` shall accept telemetry on an `Fw.Tlm` port, storing each channel named in its configured mapping table in the mapped entry, decoded as the mapped primitive type and time tagged with the sender's time tag; shall ignore unmapped channels; and shall emit a warning event, storing nothing, when a mapped value does not decode as its type. | Unit Test |

Heritage requirements HER-0001, .0002, and .0003 govern shared-memory
allocation across space partitions and initialization order between them.
Space partitioning is out of scope for this component, so they are not ported.

## 3. Design

### 3.1 Component diagram

```
                          +-------------------------+
   putValue          ---->|                         |<---- timeCaller
   getValue          <--->|                         |
   putData           ---->|                         |----> eventOut
   getData           <--->|                         |
   getMinMax         <--->|  Svc::StateBufferStore  |<---- cmdIn
   getHistory        <--->|                         |----> cmdResponseOut
   getNHistory       <--->|                         |----> cmdRegOut
   tlmIn             ---->|                         |----> textEventOut
                          |                         |
   (privileged)           |                         |
   clearMinMax       <--->|                         |
   clearAndGetMinMax <--->|                         |
                          +-------------------------+
```

### 3.2 Port kinds and concurrency

The component is `passive`, so each handler runs on its caller's thread with no
queue. This matches the heritage module, which was a synchronous library with no
thread of its own. The store and read ports are `sync` and take no lock. Only
the clearers — `clearMinMax`, `clearAndGetMinMax`, and the `CLEAR_WATERMARKS`
command — are `guarded`, serializing clears against one another on the
component mutex (§3.3) without ever blocking a put or a read.

Readers and writers are coordinated by a per-entry **coherency counter** rather
than by guarded ports, following #5067's preference. The counter works as a
sequence lock covering everything the entry's writer touches: history ring,
payloads, latest index, stored count, and watermarks. A writer increments it
before writing, making it odd, and again afterwards, making it even. A reader
samples it before and after its copy, and accepts the copy only if the first
sample was even and the two are equal; otherwise a write overlapped the copy and
the bytes may mix two measurements. The reader re-reads the latest index on
every attempt, so a retry follows the newest measurement. After
`MAX_READ_ITERATIONS` attempts, none of them coherent, the component emits the
warning-high `FailedReadCoherentData` event (throttled, §3.11), naming the read operation, and
reports `INCOHERENT` without copying anything out. Every read copies into a
scratch copy on the reader's stack and hands it to the caller only once the read
proves coherent.

The counter admits one writer per entry. A telemetry-mapped entry refuses
puts, so `tlmIn` is its only writer; for every other entry a single writer is an
assumption (§6).

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

`Fw::PolyType` orders no pair of bools either — its `operator<` is false for
every pair — so the component orders bool values itself, `false` before `true`.

Watermarks are written inside the writer's coherency window, so watermark reads
are protected exactly as measurement reads are.

`clearAndGetMinMax` reports precisely the watermarks its clear ends. It sets the
flag first, then reads. If a put consumes the flag in between, that put has
already reseeded the watermarks, so a put that consumes the flag first moves the
watermarks it displaces to a retired pair, and the read reports those. Without
this, a put landing between the read and the clear would have its value neither
reported nor kept. Clears are serialized on the component mutex, because a second
clear landing inside a `clearAndGetMinMax` could overwrite the retired pair it is
about to report.

If its read never proves coherent, `clearAndGetMinMax` reports `INCOHERENT` and
withdraws its clear, provided no put has consumed it, so the watermarks it
failed to report are kept for the next read. If a put has already consumed the
clear, that put retired the epoch, and the epoch's watermarks are lost.

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

`REPORT_WATERMARKS` and `CLEAR_WATERMARKS` reject an entry argument naming the
`NUM_ENTRIES` sizing counter (`VALIDATION_ERROR`) or a data entry
(`EXECUTION_ERROR`), emitting the warning-low `ReportWatermarksRejected` or
`ClearWatermarksRejected` event with the reason. A `REPORT_WATERMARKS` whose
read stays torn fails with `EXECUTION_ERROR`; the `FailedReadCoherentData`
event it emits names the command.

### 3.6 Status reporting

`SbsStatus` preserves the heritage status enumeration's values 0-5 so that ported
requirements and ground tooling keep their meaning, and adds `INCOHERENT` and
`WRONG_KIND`.

Three heritage behaviors were deliberately changed:

- **`INCOHERENT` is new, and the event is a warning.** The heritage `get`
  emitted a FATAL event on retry exhaustion and then returned the possibly-torn
  data with its stored validity, so a caller that ignored events could consume
  bad data silently. This component reports `INCOHERENT` and copies nothing out,
  so the caller can act on it. The event is warning high rather than FATAL: a
  deployment whose event manager forwards FATAL events to `Svc::FatalHandler`
  would end the software on the event, leaving `INCOHERENT` unreachable.
- **Entry kinds are refused, not asserted.** The heritage `put` asserted that a
  value matched its entry's configured type. Here an entry is fixed as a value
  or data entry by its first store, or as a value entry by `configure()` when
  a telemetry channel is mapped to it, and every operation of the other kind —
  including the puts, which therefore return `SbsStatus` — reports `WRONG_KIND`
  and changes nothing. Watermarks and history are value-entry operations; a data
  entry's history is kept for coherency but is not readable. A
  telemetry-mapped entry also refuses `putValue`, keeping `tlmIn` its only
  writer.
- **`UNINITIALIZED` is unreachable.** The heritage module checked a global
  initialization flag on every call. Here, calling any port before
  `configure()` is a programming error and trips an assertion, which is the
  F Prime-idiomatic response. The value is retained in the enumeration for
  requirement traceability.

Two further heritage differences follow from the design rather than from a
decision to diverge:

- **`NOT_WRITTEN` comes from a stored count, not a zero timestamp.** The
  heritage `get` treated a zero timestamp as "never written". This component
  counts the measurements each entry has stored, which also tells history reads
  how many records exist. A measurement legitimately stamped with zero time —
  for instance, when `timeCaller` is unconnected — is therefore still reported
  as written.
- **Full-history reads are chronological.** The heritage `get_history` dumped
  the ring in raw slot order, leaving the caller to find the oldest slot.
  `getHistory` returns measurements oldest first, the same order as
  `getNHistory` and the heritage `get_n_history_measurements`, so the two
  history reads differ only in how they treat a short buffer.

### 3.7 Event reporting of values

FPP cannot carry `Fw::PolyType` or `Fw::Time` in an event — neither is a
displayable type. The `WatermarkReport` event therefore widens values to `F64`
and reduces times to microseconds, following the `I64` microsecond convention in
`Svc::TimeConverter`. Integer magnitudes above 2^53 are reported imprecisely by
the event; the `getMinMax` port reports them exactly, and is the interface to
use when precision matters.

### 3.8 Telemetry

`tlmIn` is a standard `Fw.Tlm` port, so a component's autocoded telemetry
output can be connected to it directly, alongside or instead of a telemetry
channel database. The topology passes `configure()` a table of `SbsTlmMapping`
rows, each naming a channel ID, the entry that stores it, and the `SbsTlmType`
its value is serialized as. A future autocoder would generate this table from
the deployment's channels (§7); until then it is written by hand, which is why
it belongs to the deployment rather than to `StateBufferStoreCfg.fpp`: channel
IDs are assigned per deployment.

A received channel found in the table is decoded into an `Fw::PolyType` of its
mapped type and stored exactly as `putValue` stores a value, so it gains
watermarks and history. Two things differ from `putValue`:

- The measurement is time tagged with the telemetry's own time tag, not with
  `timeCaller`, because the tag records when the value was sampled. This is the
  client-supplied timestamp #5067 describes; `putValue` remains
  component-stamped.
- The validity recorded is always `OK`, as telemetry carries none.

A channel not in the table is ignored without comment: a component's telemetry
port carries all of its channels, and the store holds only those mapped. A
mapped value that fails to decode — too short, or with bytes left over — means
the table disagrees with the channel's definition; it is not stored, and the
throttled `TlmDecodeFailed` warning reports it; `RESET_THROTTLES` (§3.10)
re-enables it once throttled. Lookup is a linear scan of the
table, bounded by its size.

`configure()` asserts that no channel and no entry appears twice in the table,
since either would interleave unrelated values in one history, and fixes every
mapped entry as a value entry written only by `tlmIn`, so any put to it is
refused even before its channel first arrives. The table must outlive the
component.

### 3.9 Ports

| Port | Kind | Data Type | Description |
|---|---|---|---|
| `putValue` | sync input | `Svc.SbsPut` | Stores a primitive measurement |
| `getValue` | sync input | `Svc.SbsGet` | Gets the latest primitive measurement |
| `putData` | sync input | `Svc.SbsPutData` | Stores a string, structure, or byte-array measurement |
| `getData` | sync input | `Svc.SbsGetData` | Gets the latest string, structure, or byte-array measurement |
| `getMinMax` | sync input | `Svc.SbsGetMinMax` | Gets an entry's watermarks |
| `getHistory` | sync input | `Svc.SbsGetHistory` | Gets an entry's full history |
| `getNHistory` | sync input | `Svc.SbsGetNHistory` | Gets an entry's most recent N measurements |
| `tlmIn` | sync input | `Fw.Tlm` | Receives telemetry, storing mapped channels (§3.8) |
| `clearMinMax` | guarded input | `Svc.SbsClearMinMax` | Clears an entry's watermarks (privileged, §3.5) |
| `clearAndGetMinMax` | guarded input | `Svc.SbsClearAndGetMinMax` | Gets an entry's watermarks, then clears them (privileged, §3.5) |
| `cmdIn` | command recv | `Fw.Cmd` | Receives commands |
| `cmdRegOut` | command reg | `Fw.CmdReg` | Registers commands |
| `cmdResponseOut` | command resp | `Fw.CmdResponse` | Sends command responses |
| `eventOut` | event | `Fw.Log` | Emits events |
| `textEventOut` | text event | `Fw.LogText` | Emits text events |
| `timeCaller` | time get | `Fw.Time` | Gets the time stamped on `putValue` and `putData` measurements |

### 3.10 Commands

| Command | Opcode | Kind | Argument | Description | Failure |
|---|---|---|---|---|---|
| `REPORT_WATERMARKS` | 0x00 | sync | `entry: StateBufferStoreCfg.StateEntry` | Reports an entry's watermarks with `WatermarkReport` | `VALIDATION_ERROR` for `NUM_ENTRIES`; `EXECUTION_ERROR` for a data entry or a torn read |
| `CLEAR_WATERMARKS` | 0x01 | guarded | `entry: StateBufferStoreCfg.StateEntry` | Clears an entry's watermarks and reports it with `WatermarksCleared` | `VALIDATION_ERROR` for `NUM_ENTRIES`; `EXECUTION_ERROR` for a data entry |
| `RESET_THROTTLES` | 0x02 | sync | none | Resets the throttles of `FailedReadCoherentData` and `TlmDecodeFailed` and reports it with `ThrottlesReset` | none |

Both throttled warnings stay silent after their tenth occurrence until
`RESET_THROTTLES` is sent, so an operator who has acted on one can see whether
the condition recurs.

### 3.11 Events

| Event | Severity | Throttle | Description |
|---|---|---|---|
| `FailedReadCoherentData` | warning high | 10 | A read found no coherent copy within `MAX_READ_ITERATIONS` attempts and reported `INCOHERENT` |
| `WatermarkReport` | activity high | none | Report of an entry's watermarks (§3.7) |
| `WatermarksCleared` | activity high | none | An entry's watermarks were cleared by command |
| `TlmDecodeFailed` | warning high | 10 | A mapped telemetry value did not decode as its mapping's type and was not stored |
| `ReportWatermarksRejected` | warning low | none | A `REPORT_WATERMARKS` was rejected, with the reason |
| `ClearWatermarksRejected` | warning low | none | A `CLEAR_WATERMARKS` was rejected, with the reason |
| `ThrottlesReset` | activity high | none | The throttled warnings' throttles were reset by command |

## 4. Configuration

`config/StateBufferStoreCfg.fpp` defines:

| Name | Meaning |
|---|---|
| `StateEntry` | The set of stored entries. `NUM_ENTRIES` is a required last member used to size the entry tables; it is not an addressable entry, and commands naming it are rejected. |
| `HistoryDepths` | Per-entry history depth, one element per entry. Each must be in `[MIN_HISTORY_DEPTH, MAX_HISTORY_DEPTH]`, asserted by `configure()`. The sample sets its last two entries to the two bounds. |
| `MIN_HISTORY_DEPTH` | Lower bound on any entry's depth: 2, per REQ-STATEBUFFERSTORE-010. |
| `MAX_HISTORY_DEPTH` | Upper bound on any entry's depth. |
| `DEFAULT_HISTORY_DEPTH` | Typical depth, used by the sample for most entries. |
| `MAX_DATA_SIZE` | Maximum size of a string, structure, or byte-array measurement. |
| `MAX_READ_ITERATIONS` | Read attempts, including the first, before reporting `INCOHERENT`. |

History reads return only measurements actually stored: before an entry's ring
fills, a read returns fewer records than its depth. Each record is a serialized
`SbsMeasurement`, `SbsMeasurement::SERIALIZED_SIZE` bytes long; narrow values
serialize in fewer bytes, and the remainder of the record is zeroed.

Memory used is
`sum over entries of depth * (sizeof(Measurement) + MAX_DATA_SIZE)`. Because
`MAX_DATA_SIZE` multiplies by depth for *every* entry, including entries that
only ever hold primitives, projects storing few structures should keep
`MAX_DATA_SIZE` tight.

Reads also use the caller's stack for their scratch copy: `getData` takes
`MAX_DATA_SIZE` bytes, and `getHistory` and `getNHistory` take
`MAX_HISTORY_DEPTH * SbsMeasurement::SERIALIZED_SIZE` bytes. Size the stacks of
reading threads for these, especially after raising `MAX_DATA_SIZE`.

## 5. Usage

Instantiate the component, connect its ports, then call `configure()` once
before any port is invoked, with an allocator that outlives the component:

```c++
Svc::StateBufferStore stateBufferStore("stateBufferStore");
Fw::MallocAllocator stateBufferStoreAllocator;

// after port connections are made
stateBufferStore.configure(0, stateBufferStoreAllocator);
```

To also store telemetry, pass a mapping table that outlives the component:

```c++
Svc::SbsTlmMapping stateBufferStoreTlm[] = {
    Svc::SbsTlmMapping(myChannelId, Svc::StateBufferStoreCfg::StateEntry::SBS_ENTRY_00,
                       Svc::SbsTlmType::TYPE_F32),
};

stateBufferStore.configure(0, stateBufferStoreAllocator,
                           Fw::ExternalArray<Svc::SbsTlmMapping>(stateBufferStoreTlm,
                                                                 FW_NUM_ARRAY_ELEMENTS(stateBufferStoreTlm)));
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
`lastReadTime` is optional, as in the heritage interface, which accepted `NULL`:
passing `Fw::ZERO_TIME` skips the freshness check, so a consumer that does not
track freshness never sees `NOT_FRESH`. A measurement stamped with
`Fw::ZERO_TIME` itself (time base `TB_NONE`, as an unconnected `timeCaller`
produces) can therefore never be reported `NOT_FRESH`. Zero seconds in any other
time base is an ordinary timestamp: `Fw::Time` comparison treats differing time
bases as incomparable, so such a stamp passed back as `lastReadTime` is checked
for freshness like any other.
Freshness is judged by timestamp, so two stores within one tick of the time
source look identical to a reader, and a component whose `timeCaller` is
unconnected stamps every measurement with zero time.

## 6. Assumptions and limitations

- **One writer per entry.** As in the heritage design, where an entry is owned
  by the subsystem producing it. The component enforces this for
  telemetry-mapped entries, which refuse puts so that `tlmIn` is their only
  writer. For every other entry it is an assumption: concurrent `putValue` or
  `putData` callers on one entry are not supported, since overlapping writes can
  leave the counter even mid-write and let a reader accept a torn copy.
  Concurrent readers are safe.
- **A torn read is detected, not prevented.** The counter tells a reader that a
  write intervened; it cannot stop one, so a reader racing a fast writer can
  exhaust its attempts and report `INCOHERENT`. Because the counter covers the
  whole entry, a writer lapping the ring during a read is detected too.
  Attempts run back to back, so a reader that preempts its writer mid-write —
  a higher-priority reader under a strict-priority scheduler — fails every
  attempt. Give readers no higher priority than the writers of the entries they
  read, or treat `INCOHERENT` as a status to retry later.
- **Clears are serialized.** `clearMinMax`, `clearAndGetMinMax`, and
  `CLEAR_WATERMARKS` are guarded, so a clearer waits only for another clearer,
  never for a put or a read.
- **Type changes do not re-seed watermarks** — see §3.3.
- **`configure()` is a new lifecycle requirement** that the heritage library did
  not have, since it allocated statically. A component whose `configure()` was
  never called asserts on first port use rather than reading null pointers.

## 7. Deferred work

- An autocoder generating entry identifiers, the telemetry mapping table of
  §3.8, and history offsets, as proposed in #5067. Configuration and the
  mapping table are written by hand for now, as `Svc::PolyDb`'s configuration
  is.
- Clearing the watermarks of all entries at once. #5067 has watermarks
  "cleared in response to a command or input port, either for individual
  entries or for all entries in the database"; `clearMinMax`,
  `clearAndGetMinMax`, and `CLEAR_WATERMARKS` each take one entry, so clearing
  every entry takes one call per entry.
- Data product reporting: dumping all watermarks, or an entry's full history,
  as a data product rather than over a port.
- Encoding access permissions in entry identifiers (#5067), which would let the
  component enforce §3.5's privilege tier itself rather than relying on wiring.
- Topology integration. No deployment in this repository instantiates this
  component (nor `Svc::PolyDb`), so there is no integration test suite.

## 8. Unit test coverage

The unit tests cover 97% of the lines of `StateBufferStore.cpp` (483 of 494)
and every function. gcovr counts 55.5% of branches across the component's
source and header (962 of 1734); most of the remainder are the failure branches
of `FW_ASSERT`s and of code generated by the compiler, in line with comparable
components such as `Svc::TimeConverter`.

The uncovered lines are:

- Paths reached only when a writer interleaves with a reader on another thread,
  which a single-threaded test cannot arrange: clamping a torn payload size,
  a history serialization that fails on a torn copy, and an entry whose kind is
  claimed as data during a watermark or history read, or as a value entry
  between a data put's kind check and its claim. The retry and
  `INCOHERENT` paths themselves are covered through a test-only hook that
  forces torn reads.
- The `default` case of the telemetry type switch, unreachable because the
  generated `SbsTlmType` asserts on construction from an undefined value.
- The closing lines of `getMinMax_handler` and `clearAndGetMinMax_handler`,
  which gcov reports as unexecuted although both handlers are exercised.

The assertion paths — use before `configure()`, configuring twice, an untyped
value, and a malformed telemetry table — are verified by death tests. Death
tests run in a child process that exits through the assertion, so they do not
contribute to the coverage figures. A short allocation is not tested: the
component requests memory with `Fw::MemAllocator::checkedAllocate`, which
asserts inside `Fw` before the component sees the result.

## 9. Change log

| Date | Description |
|---|---|
| 2026-10-05 | Initial version, ported from a heritage state buffer store module |
| 2026-10-07 | Sequence-lock coherency covering watermarks and whole history dumps; entry kinds; `SbsMeasurement` history records; explicit allocator in `configure()` |
| 2026-10-07 | `tlmIn` telemetry port with a `configure()` mapping table; per-entry depth bounds in the sample configuration; coverage section |
| 2026-10-09 | Review fixes: `FailedReadCoherentData` is warning high; guarded clearers; telemetry-mapped entries refuse puts; torn reads copy nothing out and a torn clear-and-get keeps its watermarks; bool watermarks; `WRONG_KIND` checked first on history reads; ports, commands, and events sections |
| 2026-10-09 | `getNHistory` returns the entry's full depth for a request deeper than it, rather than asserting |
| 2026-10-09 | `getNHistory` reports `INVALID_BUFFER_SIZE` when its buffer cannot hold one measurement |
| 2026-10-09 | `FailedReadCoherentData` throttled at 10; `RESET_THROTTLES` command and `ThrottlesReset` event |
| 2026-10-09 | `getNHistory`'s `numMeasurements` is `FwSizeType`, matching entry depths |
| 2026-10-09 | Puts accept only `OK` or `INVALID` validity, refusing any other with `WRONG_KIND` |
| 2026-10-09 | `putData` reports `INVALID_BUFFER_SIZE` for data larger than `MAX_DATA_SIZE`, rather than asserting |
