// ======================================================================
// \title  StateBufferStore.cpp
// \brief  cpp file for StateBufferStore component implementation class
// ======================================================================

#include "Svc/StateBufferStore/StateBufferStore.hpp"

#include <cmath>
#include <cstring>
#include <new>

#include "Fw/DataStructures/CircularIndex.hpp"
#include "Fw/Types/Assert.hpp"
#include "Fw/Types/SerialBuffer.hpp"

namespace Svc {

namespace {

//! Microseconds in one second
constexpr I64 US_PER_SECOND = 1000000;

//! Bytes a single measurement occupies in a history buffer: one serialized
//! SbsMeasurement, so that ground tools can decode a dump with the FPP type
constexpr FwSizeType HISTORY_RECORD_SIZE = SbsMeasurement::SERIALIZED_SIZE;

//! Deserialize one value of type T into a PolyType
template <typename T>
Fw::SerializeStatus decodeAs(Fw::SerialBuffer& reader, Fw::PolyType& out) {
    T value{};
    const Fw::SerializeStatus status = reader.deserializeTo(value);
    if (status == Fw::FW_SERIALIZE_OK) {
        out = Fw::PolyType(value);
    }
    return status;
}

//! Whether an atomic of unsigned integral width WIDTH can ever be lock-free
//!
//! C++14 lacks std::atomic<T>::is_always_lock_free (C++17), so this is derived
//! from the standard ATOMIC_*_LOCK_FREE macros (0 = never, 1 = sometimes,
//! 2 = always), matched by width so no ABI is assumed, as in
//! Os/Generic/LocklessPriorityQueue.hpp. A never-lock-free width fails the
//! build; a sometimes-lock-free one is caught by the check in configure().
template <FwSizeType WIDTH>
struct AtomicMayBeLockFree {
    static constexpr bool value = ((sizeof(unsigned char) == WIDTH) && (ATOMIC_CHAR_LOCK_FREE != 0)) ||
                                  ((sizeof(unsigned short) == WIDTH) && (ATOMIC_SHORT_LOCK_FREE != 0)) ||
                                  ((sizeof(unsigned int) == WIDTH) && (ATOMIC_INT_LOCK_FREE != 0)) ||
                                  ((sizeof(unsigned long) == WIDTH) && (ATOMIC_LONG_LOCK_FREE != 0)) ||
                                  ((sizeof(unsigned long long) == WIDTH) && (ATOMIC_LLONG_LOCK_FREE != 0));
};

// An atomic emulated with a lock would let a preempted writer block readers,
// defeating the lock-free coherency protocol and risking priority inversion
static_assert(AtomicMayBeLockFree<sizeof(U32)>::value, "std::atomic<U32> is never lock-free on this platform");
static_assert(AtomicMayBeLockFree<sizeof(U8)>::value, "std::atomic<U8> is never lock-free on this platform");
static_assert(ATOMIC_BOOL_LOCK_FREE != 0, "std::atomic<bool> is never lock-free on this platform");

static_assert(StateBufferStoreCfg::MAX_READ_ITERATIONS >= 1,
              "MAX_READ_ITERATIONS counts attempts, including the first, and must be at least 1");
static_assert(StateBufferStoreCfg::MIN_HISTORY_DEPTH >= 2, "REQ-STATEBUFFERSTORE-010 requires a depth of at least 2");
static_assert(StateBufferStoreCfg::MIN_HISTORY_DEPTH <= StateBufferStoreCfg::MAX_HISTORY_DEPTH,
              "MIN_HISTORY_DEPTH exceeds MAX_HISTORY_DEPTH");

//! Whether a PolyType holds a floating-point NaN
bool isNaN(const Fw::PolyType& value) {
    if (value.isF32()) {
        F32 stored = 0.0f;
        value.get(stored);
        return std::isnan(stored);
    }
    if (value.isF64()) {
        F64 stored = 0.0;
        value.get(stored);
        return std::isnan(stored);
    }
    return false;
}

//! Pull a stored value of type T out of a PolyType, widened to F64
template <typename T>
F64 widen(const Fw::PolyType& value) {
    T stored{};
    value.get(stored);
    return static_cast<F64>(stored);
}

}  // namespace

// ----------------------------------------------------------------------
// Component construction and destruction
// ----------------------------------------------------------------------

StateBufferStore::StateBufferStore(const char* const compName)
    : StateBufferStoreComponentBase(compName),
      m_memPtr(nullptr),
      m_memSize(0),
      m_memId(0),
      m_allocator(nullptr),
      m_initialized(false) {
    for (FwSizeType i = 0; i < StateBufferStoreCfg::StateEntry::NUM_ENTRIES; i++) {
        Entry& block = this->m_entries[i];
        block.history = nullptr;
        block.data = nullptr;
        block.coherencyCounter.store(0, std::memory_order_relaxed);
        block.kind.store(KIND_UNSET, std::memory_order_relaxed);
        block.depth = 0;
        block.latest = 0;
        block.stored = 0;
        block.index = i;
        // Watermarks start "to be cleared" so that the first put seeds them.
        // Comparison cannot do it: Fw::PolyType's operator< and operator>
        // both return false when the stored types differ, and a
        // default-constructed PolyType holds no type at all.
        block.clearPending.store(true, std::memory_order_relaxed);
    }
}

StateBufferStore::~StateBufferStore() {
    // Guarded so that destroying a never-configured component is harmless
    if ((this->m_allocator != nullptr) and (this->m_memPtr != nullptr)) {
        this->m_allocator->deallocate(this->m_memId, this->m_memPtr);
    }
}

void StateBufferStore::configure(FwEnumStoreType memId, Fw::MemAllocator& allocator) {
    this->configure(memId, allocator, Fw::ExternalArray<SbsTlmMapping>());
}

void StateBufferStore::configure(FwEnumStoreType memId,
                                 Fw::MemAllocator& allocator,
                                 const Fw::ExternalArray<SbsTlmMapping>& tlmMappings) {
    FW_ASSERT(not this->m_initialized);

    // The authoritative lock-free check where the static_asserts can only say
    // "sometimes": every entry's atomics share these types, so one entry speaks for all
    const Entry& probe = this->m_entries[0];
    FW_ASSERT(probe.coherencyCounter.is_lock_free());
    FW_ASSERT(probe.kind.is_lock_free());
    FW_ASSERT(probe.clearPending.is_lock_free());

    // Validated before allocating, so a bad table fails without a dangling allocation
    for (FwSizeType i = 0; i < tlmMappings.getSize(); i++) {
        const SbsTlmMapping& mapping = tlmMappings[i];
        const FwSizeType index = static_cast<FwSizeType>(mapping.get_entry());
        FW_ASSERT(index < StateBufferStoreCfg::StateEntry::NUM_ENTRIES, static_cast<FwAssertArgType>(index),
                  static_cast<FwAssertArgType>(i));
        // A channel stored twice, or two channels sharing an entry, would
        // interleave unrelated values in one history
        for (FwSizeType j = 0; j < i; j++) {
            FW_ASSERT(tlmMappings[j].get_chanId() != mapping.get_chanId(),
                      static_cast<FwAssertArgType>(mapping.get_chanId()), static_cast<FwAssertArgType>(i));
            FW_ASSERT(tlmMappings[j].get_entry() != mapping.get_entry(), static_cast<FwAssertArgType>(index),
                      static_cast<FwAssertArgType>(i));
        }
    }

    const StateBufferStoreCfg::HistoryDepths depths;

    // Sum the per-entry regions first so the whole history is one allocation:
    // Fw::MemAllocator does not promise that an identifier may be reused
    // across calls, so a component gets one chance to allocate.
    FwSizeType totalHistoryBytes = 0;
    FwSizeType totalPayloadBytes = 0;
    for (FwSizeType i = 0; i < StateBufferStoreCfg::StateEntry::NUM_ENTRIES; i++) {
        const FwSizeType depth = depths[i];
        FW_ASSERT(depth >= StateBufferStoreCfg::MIN_HISTORY_DEPTH, static_cast<FwAssertArgType>(depth),
                  static_cast<FwAssertArgType>(i));
        FW_ASSERT(depth <= StateBufferStoreCfg::MAX_HISTORY_DEPTH, static_cast<FwAssertArgType>(depth),
                  static_cast<FwAssertArgType>(i));
        this->m_entries[i].depth = depth;
        totalHistoryBytes += depth * static_cast<FwSizeType>(sizeof(Measurement));
        totalPayloadBytes += depth * StateBufferStoreCfg::MAX_DATA_SIZE;
    }

    this->m_memSize = totalHistoryBytes + totalPayloadBytes;
    this->m_memId = memId;
    // checkedAllocate asserts rather than returning short. A partial
    // allocation would silently cut the configured depth of an arbitrary
    // subset of entries, so there is no useful degraded mode here.
    this->m_memPtr = allocator.checkedAllocate(memId, this->m_memSize);
    this->m_allocator = &allocator;

    // Carve the allocation up: all Measurement rings first, so that the
    // allocator's alignment covers them, then the byte payloads which need none
    U8* const base = static_cast<U8*>(this->m_memPtr);
    U8* cursor = base;
    for (FwSizeType i = 0; i < StateBufferStoreCfg::StateEntry::NUM_ENTRIES; i++) {
        Entry& block = this->m_entries[i];
        // Aligned: the rings are packed as whole Measurement arrays from the
        // allocation base, ahead of the byte payloads
        block.history = reinterpret_cast<Measurement*>(cursor);
        cursor += block.depth * static_cast<FwSizeType>(sizeof(Measurement));
        for (FwSizeType slotIndex = 0; slotIndex < block.depth; slotIndex++) {
            (void)new (&block.history[slotIndex]) Measurement();
        }
    }
    for (FwSizeType i = 0; i < StateBufferStoreCfg::StateEntry::NUM_ENTRIES; i++) {
        Entry& block = this->m_entries[i];
        block.data = cursor;
        cursor += block.depth * StateBufferStoreCfg::MAX_DATA_SIZE;
    }
    FW_ASSERT(cursor <= base + this->m_memSize, static_cast<FwAssertArgType>(cursor - base),
              static_cast<FwAssertArgType>(this->m_memSize));

    // Mapped entries are claimed for telemetry from the start, so any put to
    // one is refused even before its channel first arrives: tlmIn is the
    // entry's only writer port, which the coherency counter requires (the
    // channel's producer must also emit it from one thread at a time)
    for (FwSizeType i = 0; i < tlmMappings.getSize(); i++) {
        this->m_entries[static_cast<FwSizeType>(tlmMappings[i].get_entry())].kind.store(KIND_TLM,
                                                                                        std::memory_order_relaxed);
    }
    this->m_tlmMappings = tlmMappings;

    this->m_initialized = true;
}

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

StateBufferStore::Entry& StateBufferStore::checkedEntry(const StateBufferStoreCfg::StateEntry& entry) {
    FW_ASSERT(this->m_initialized);
    const FwSizeType index = static_cast<FwSizeType>(entry.e);
    // NUM_ENTRIES is the table size, never a usable entry
    FW_ASSERT(index < StateBufferStoreCfg::StateEntry::NUM_ENTRIES, static_cast<FwAssertArgType>(index));
    return this->m_entries[index];
}

bool StateBufferStore::claimKind(Entry& block, EntryKind kind) {
    // Exact-kind match, not isValueKind: a KIND_TLM entry must fail a
    // KIND_VALUE claim, which is what keeps tlmIn its only writer port
    U8 current = KIND_UNSET;
    if (block.kind.compare_exchange_strong(current, static_cast<U8>(kind), std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
        return true;
    }
    // On failure compare_exchange_strong leaves the kind already claimed in current
    return current == static_cast<U8>(kind);
}

void StateBufferStore::beginWrite(Entry& block) {
    (void)block.coherencyCounter.fetch_add(1, std::memory_order_relaxed);
    // Keeps the write's stores from becoming visible before the odd count
    std::atomic_thread_fence(std::memory_order_release);
}

void StateBufferStore::endWrite(Entry& block) {
    (void)block.coherencyCounter.fetch_add(1, std::memory_order_release);
}

U32 StateBufferStore::beginRead(Entry& block) {
#ifdef BUILD_UT
    if (this->m_utBeforeRead != nullptr) {
        void (*const hook)(void* context) = this->m_utBeforeRead;
        this->m_utBeforeRead = nullptr;
        hook(this->m_utBeforeReadContext);
    }
    if (this->m_utForceTornReads > 0) {
        this->m_utForceTornReads--;
        // Even, so the attempt copies normally, but never equal to the
        // counter, so endRead rejects it
        return block.coherencyCounter.load(std::memory_order_acquire) + 2;
    }
#endif
    return block.coherencyCounter.load(std::memory_order_acquire);
}

bool StateBufferStore::endRead(const Entry& block, U32 before) {
    // Keeps the copy's loads from being satisfied after the counter is resampled
    std::atomic_thread_fence(std::memory_order_acquire);
    const U32 after = block.coherencyCounter.load(std::memory_order_relaxed);
    // An odd count means the copy began mid-write; a changed one means a write
    // began during it. Either way the bytes may mix two measurements.
    return ((before % 2U) == 0U) and (before == after);
}

template <typename Snapshot>
bool StateBufferStore::coherentRead(Entry& block, StateBufferStore_ReadOperation operation, Snapshot& snapshot) {
    for (U32 iteration = 0; iteration < StateBufferStoreCfg::MAX_READ_ITERATIONS; iteration++) {
        const U32 before = this->beginRead(block);
        snapshot.copy(block);
        if (StateBufferStore::endRead(block, before)) {
            return true;
        }
    }
    this->log_WARNING_HI_FailedReadCoherentData(static_cast<StateBufferStoreCfg::StateEntry::T>(block.index), operation,
                                                StateBufferStoreCfg::MAX_READ_ITERATIONS);
    return false;
}

void StateBufferStore::ValueSnapshot::copy(const Entry& block) {
    this->kind = block.kind.load(std::memory_order_relaxed);
    this->measurement = block.history[block.latest];
    this->stored = block.stored;
}

void StateBufferStore::DataSnapshot::copy(const Entry& block) {
    this->kind = block.kind.load(std::memory_order_relaxed);
    const FwSizeType slotIndex = block.latest;
    this->measurement = block.history[slotIndex];
    // Bounded before the coherency check: a torn size must not overrun the payload copy
    if (this->measurement.dataSize > StateBufferStoreCfg::MAX_DATA_SIZE) {
        this->measurement.dataSize = StateBufferStoreCfg::MAX_DATA_SIZE;
    }
    (void)::memcpy(this->payload, &block.data[slotIndex * StateBufferStoreCfg::MAX_DATA_SIZE],
                   this->measurement.dataSize);
    this->stored = block.stored;
}

void StateBufferStore::WatermarkSnapshot::copy(const Entry& block) {
    const bool pending = block.clearPending.load(std::memory_order_acquire);
    // Once a put has consumed this read's own clear, the watermarks the
    // clear ended are the ones that put retired
    const bool retired = this->clear and not pending;
    this->min = retired ? block.retiredMin : block.min;
    this->max = retired ? block.retiredMax : block.max;
    this->cleared = this->clear ? this->alreadyCleared : pending;
    this->stored = block.stored;
    this->kind = block.kind.load(std::memory_order_relaxed);
}

void StateBufferStore::HistorySnapshot::copy(const Entry& block) {
    this->kind = block.kind.load(std::memory_order_relaxed);
    this->stored = block.stored;
    const FwSizeType latest = block.latest;
    // Only measurements actually stored are reported: an unwritten slot
    // holds an untyped PolyType, which cannot be serialized
    this->copied = (this->count < this->stored) ? this->count : this->stored;
    // Reset per attempt: a torn attempt's failure must not survive into a
    // retry that proves coherent, or the caller's FW_ASSERT on it would fire
    this->serialized = true;
    // A data entry's slots hold no value to serialize
    if (not StateBufferStore::isValueKind(this->kind)) {
        return;
    }
    for (FwSizeType i = 0; i < this->copied; i++) {
        // Walk oldest to newest: the oldest of copied measurements
        // sits copied-1 slots behind latest, modulo the ring
        const FwSizeType age = this->copied - 1 - i;
        // Reduced modulo depth on construction, so even a torn latest indexes inside the ring
        const Measurement& slotMeas = block.history[Fw::CircularIndex(block.depth, latest).decrement(age)];
        U8* const recordBytes = &this->records[i * HISTORY_RECORD_SIZE];
        // PolyType serializes narrow types in fewer bytes than its
        // maximum; zero the remainder rather than leave stale bytes
        (void)::memset(recordBytes, 0, HISTORY_RECORD_SIZE);
        const SbsMeasurement measurement(slotMeas.time, slotMeas.validity, slotMeas.value);
        Fw::ExternalSerializeBuffer serializer(recordBytes, HISTORY_RECORD_SIZE);
        // A torn copy may pair a newer count with an unwritten slot,
        // so a failure is only an error once the read proves coherent
        if (serializer.serializeFrom(measurement) != Fw::FW_SERIALIZE_OK) {
            this->serialized = false;
        }
    }
}

FwSizeType StateBufferStore::appendSlot(Entry& block) {
    const FwSizeType slotIndex = Fw::CircularIndex(block.depth, block.latest).increment();
    block.latest = slotIndex;
    if (block.stored < block.depth) {
        block.stored++;
    }
    return slotIndex;
}

SbsStatus StateBufferStore::latestStatus(FwSizeType stored,
                                         const Measurement& measurement,
                                         const Fw::Time& lastReadTime) {
    if (stored == 0) {
        return SbsStatus::NOT_WRITTEN;
    }
    // Fw::ZERO_TIME stands in for the heritage NULL: no previous read to compare against
    if ((lastReadTime != Fw::ZERO_TIME) and (measurement.time == lastReadTime)) {
        return SbsStatus::NOT_FRESH;
    }
    return measurement.validity;
}

bool StateBufferStore::isWriterValidity(const SbsStatus& validity) {
    // Every other status is one the store reports itself, which a reader
    // could not tell apart from a recorded validity
    return (validity == SbsStatus::OK) or (validity == SbsStatus::INVALID);
}

bool StateBufferStore::isValueKind(U8 kind) {
    return (kind == KIND_VALUE) or (kind == KIND_TLM);
}

bool StateBufferStore::lessThan(const Fw::PolyType& lhs, const Fw::PolyType& rhs) {
    // Fw::PolyType::operator< is false for every pair of bools, which would
    // freeze a bool entry's watermarks at their seed
    if (lhs.isBool() and rhs.isBool()) {
        return (not static_cast<bool>(lhs)) and static_cast<bool>(rhs);
    }
    return lhs < rhs;
}

SbsStatus StateBufferStore::clearWatermarks(Entry& block) {
    if (block.kind.load(std::memory_order_acquire) == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    block.clearPending.store(true, std::memory_order_release);
    return SbsStatus::OK;
}

void StateBufferStore::storeValue(Entry& block,
                                  const Fw::PolyType& val,
                                  const Fw::Time& time,
                                  const SbsStatus& validity) {
    StateBufferStore::beginWrite(block);
    const FwSizeType slotIndex = StateBufferStore::appendSlot(block);
    block.history[slotIndex].time = time;
    block.history[slotIndex].value = val;
    block.history[slotIndex].validity = validity;
    block.history[slotIndex].dataSize = 0;
    StateBufferStore::updateWatermarks(block, val, time, validity);
    StateBufferStore::endWrite(block);
}

Fw::SerializeStatus StateBufferStore::decodeTlm(const SbsTlmType& type, Fw::TlmBuffer& val, Fw::PolyType& out) {
    // Read through a separate view so the caller's buffer is left as received
    Fw::SerialBuffer reader(val.getBuffAddr(), val.getSize());
    reader.fill();
    Fw::SerializeStatus status = Fw::FW_SERIALIZE_OK;
    switch (type.e) {
        case SbsTlmType::TYPE_U8:
            status = decodeAs<U8>(reader, out);
            break;
        case SbsTlmType::TYPE_I8:
            status = decodeAs<I8>(reader, out);
            break;
        case SbsTlmType::TYPE_U16:
            status = decodeAs<U16>(reader, out);
            break;
        case SbsTlmType::TYPE_I16:
            status = decodeAs<I16>(reader, out);
            break;
        case SbsTlmType::TYPE_U32:
            status = decodeAs<U32>(reader, out);
            break;
        case SbsTlmType::TYPE_I32:
            status = decodeAs<I32>(reader, out);
            break;
        case SbsTlmType::TYPE_U64:
            status = decodeAs<U64>(reader, out);
            break;
        case SbsTlmType::TYPE_I64:
            status = decodeAs<I64>(reader, out);
            break;
        case SbsTlmType::TYPE_F32:
            status = decodeAs<F32>(reader, out);
            break;
        case SbsTlmType::TYPE_F64:
            status = decodeAs<F64>(reader, out);
            break;
        case SbsTlmType::TYPE_BOOL:
            status = decodeAs<bool>(reader, out);
            break;
        default:
            // The generated enum asserts on construction from an undefined value
            FW_ASSERT(0, static_cast<FwAssertArgType>(type.e));
            break;
    }
    // Leftover bytes mean the channel is wider than its mapping says
    if ((status == Fw::FW_SERIALIZE_OK) and (reader.getDeserializeSizeLeft() != 0)) {
        status = Fw::FW_DESERIALIZE_SIZE_MISMATCH;
    }
    return status;
}

void StateBufferStore::updateWatermarks(Entry& block,
                                        const Fw::PolyType& val,
                                        const Fw::Time& time,
                                        const SbsStatus& validity) {
    // The clear, not the comparison, is what seeds the watermarks: see the
    // constructor's note on PolyType comparison across differing types.
    // Consuming it inside the write, and retiring the watermarks it ends,
    // lets readWatermarks report an epoch this put has already closed.
    // NaN orders against nothing, so as a seed it would freeze both
    // watermarks; it is skipped, leaving any pending clear for the next value.
    if (isNaN(val)) {
        return;
    }
    if (block.clearPending.exchange(false, std::memory_order_acq_rel)) {
        block.retiredMin = block.min;
        block.retiredMax = block.max;
        block.min.value = val;
        block.min.time = time;
        block.min.validity = validity;
        block.max = block.min;
        return;
    }
    if (StateBufferStore::lessThan(val, block.min.value)) {
        block.min.value = val;
        block.min.time = time;
        block.min.validity = validity;
    }
    if (StateBufferStore::lessThan(block.max.value, val)) {
        block.max.value = val;
        block.max.time = time;
        block.max.validity = validity;
    }
}

SbsStatus StateBufferStore::readWatermarks(Entry& block,
                                           StateBufferStore_ReadOperation operation,
                                           SbsMeasurement& min,
                                           SbsMeasurement& max) {
    const bool clear = (operation == StateBufferStore_ReadOperation::CLEAR_AND_GET_MIN_MAX);
    // Kinds are never unclaimed, so a refused clear can be decided up front
    if (block.kind.load(std::memory_order_acquire) == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    // A clear that finds one already pending ends an epoch with no puts in it
    const bool alreadyCleared = clear and block.clearPending.exchange(true, std::memory_order_acq_rel);
    WatermarkSnapshot snapshot(clear, alreadyCleared);

    if (not this->coherentRead(block, operation, snapshot)) {
        if (clear and not snapshot.alreadyCleared) {
            // Clearers are guarded, so only a put can have reset the flag since
            // this read set it. If none has, withdraw the clear so the epoch it
            // would have ended, never reported, is kept; if one has, that put
            // already retired the epoch and the failed exchange changes nothing.
            bool expected = true;
            (void)block.clearPending.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                             std::memory_order_acquire);
        }
        return SbsStatus::INCOHERENT;
    }
    if (snapshot.kind == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    reportWatermark(snapshot.min, snapshot.cleared, min);
    reportWatermark(snapshot.max, snapshot.cleared, max);
    return (snapshot.stored > 0) ? SbsStatus::OK : SbsStatus::NOT_WRITTEN;
}

void StateBufferStore::reportWatermark(const Measurement& source, bool cleared, SbsMeasurement& out) {
    if (cleared) {
        out.set(Fw::Time(), SbsStatus::NOT_WRITTEN, Fw::PolyType());
    } else {
        out.set(source.time, source.validity, source.value);
    }
}

F64 StateBufferStore::toF64(const Fw::PolyType& value) {
    // PolyType has no generic numeric accessor, so each stored type is pulled
    // out through its own checker. A cleared watermark holds no type, and a
    // stored pointer has no numeric value; both report as 0.0.
    if (value.isU8()) {
        return widen<U8>(value);
    }
    if (value.isI8()) {
        return widen<I8>(value);
    }
    if (value.isU16()) {
        return widen<U16>(value);
    }
    if (value.isI16()) {
        return widen<I16>(value);
    }
    if (value.isU32()) {
        return widen<U32>(value);
    }
    if (value.isI32()) {
        return widen<I32>(value);
    }
    if (value.isU64()) {
        return widen<U64>(value);
    }
    if (value.isI64()) {
        return widen<I64>(value);
    }
    if (value.isF32()) {
        return widen<F32>(value);
    }
    if (value.isF64()) {
        return widen<F64>(value);
    }
    if (value.isBool()) {
        return widen<bool>(value);
    }
    return 0.0;
}

bool StateBufferStore::isTyped(const Fw::PolyType& value) {
    return value.isU8() or value.isI8() or value.isU16() or value.isI16() or value.isU32() or value.isI32() or
           value.isU64() or value.isI64() or value.isF32() or value.isF64() or value.isBool() or value.isPtr();
}

I64 StateBufferStore::toMicroseconds(const Fw::Time& time) {
    return (static_cast<I64>(time.getSeconds()) * US_PER_SECOND) + static_cast<I64>(time.getUSeconds());
}

SbsStatus StateBufferStore::readHistory(Entry& block,
                                        FwSizeType count,
                                        StateBufferStore_ReadOperation operation,
                                        Fw::Buffer& data,
                                        FwSizeType& sizeOut) {
    sizeOut = 0;
    FW_ASSERT(count <= block.depth, static_cast<FwAssertArgType>(count), static_cast<FwAssertArgType>(block.depth));
    // Serialized into the snapshot, not the caller's buffer, so a read that
    // never proves coherent copies nothing out
    HistorySnapshot snapshot(count);
    if (not this->coherentRead(block, operation, snapshot)) {
        return SbsStatus::INCOHERENT;
    }
    if (snapshot.kind == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    FW_ASSERT(snapshot.serialized);
    sizeOut = snapshot.copied * HISTORY_RECORD_SIZE;
    // Callers judge the buffer by its size, so an empty Fw::Buffer (as an
    // exhausted BufferManager returns) reaches here only with nothing to copy
    if (sizeOut > 0) {
        U8* const out = data.getData();
        FW_ASSERT(out != nullptr);
        (void)::memcpy(out, snapshot.records, sizeOut);
    }
    return (snapshot.stored > 0) ? SbsStatus::OK : SbsStatus::NOT_WRITTEN;
}

// ----------------------------------------------------------------------
// Handler implementations for typed input ports
// ----------------------------------------------------------------------

SbsStatus StateBufferStore::putValue_handler(FwIndexType portNum,
                                             const StateBufferStoreCfg::StateEntry& entry,
                                             Fw::PolyType& val,
                                             const SbsStatus& validity) {
    Entry& block = this->checkedEntry(entry);
    // Timestamp first: the value's time should reflect when it was produced,
    // not how long the store took
    const Fw::Time now = this->getTime();
    // An untyped value cannot be serialized into a history dump
    FW_ASSERT(StateBufferStore::isTyped(val));
    if (not StateBufferStore::isWriterValidity(validity)) {
        return SbsStatus::INVALID_VALIDITY;
    }
    if (not StateBufferStore::claimKind(block, KIND_VALUE)) {
        return SbsStatus::WRONG_KIND;
    }
    StateBufferStore::storeValue(block, val, now, validity);
    return SbsStatus::OK;
}

SbsStatus StateBufferStore::getValue_handler(FwIndexType portNum,
                                             const StateBufferStoreCfg::StateEntry& entry,
                                             Fw::PolyType& val,
                                             Fw::Time& measTime,
                                             const Fw::Time& lastReadTime) {
    Entry& block = this->checkedEntry(entry);

    ValueSnapshot snapshot;
    if (not this->coherentRead(block, StateBufferStore_ReadOperation::GET_VALUE, snapshot)) {
        return SbsStatus::INCOHERENT;
    }
    if (snapshot.kind == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    val = snapshot.measurement.value;
    measTime = snapshot.measurement.time;
    return StateBufferStore::latestStatus(snapshot.stored, snapshot.measurement, lastReadTime);
}

SbsStatus StateBufferStore::putData_handler(FwIndexType portNum,
                                            const StateBufferStoreCfg::StateEntry& entry,
                                            Fw::Buffer& data,
                                            const SbsStatus& validity) {
    Entry& block = this->checkedEntry(entry);
    const Fw::Time now = this->getTime();
    const FwSizeType size = data.getSize();
    if (not StateBufferStore::isWriterValidity(validity)) {
        return SbsStatus::INVALID_VALIDITY;
    }
    // A value entry is refused as such before its payload is judged, and an
    // oversized payload is refused before it can claim an unset entry
    if (StateBufferStore::isValueKind(block.kind.load(std::memory_order_acquire))) {
        return SbsStatus::WRONG_KIND;
    }
    // A null buffer, such as an exhausted BufferManager returns, holds nothing to store
    if ((data.getData() == nullptr) or (size > StateBufferStoreCfg::MAX_DATA_SIZE)) {
        return SbsStatus::INVALID_BUFFER_SIZE;
    }
    if (not StateBufferStore::claimKind(block, KIND_DATA)) {
        return SbsStatus::WRONG_KIND;
    }

    StateBufferStore::beginWrite(block);
    const FwSizeType slotIndex = StateBufferStore::appendSlot(block);
    (void)::memcpy(&block.data[slotIndex * StateBufferStoreCfg::MAX_DATA_SIZE], data.getData(), size);
    block.history[slotIndex].time = now;
    block.history[slotIndex].validity = validity;
    block.history[slotIndex].dataSize = size;
    StateBufferStore::endWrite(block);
    return SbsStatus::OK;
}

SbsStatus StateBufferStore::getData_handler(FwIndexType portNum,
                                            const StateBufferStoreCfg::StateEntry& entry,
                                            Fw::Buffer& data,
                                            Fw::Time& measTime,
                                            const Fw::Time& lastReadTime,
                                            FwSizeType& sizeOut) {
    sizeOut = 0;
    Entry& block = this->checkedEntry(entry);

    DataSnapshot snapshot;
    if (not this->coherentRead(block, StateBufferStore_ReadOperation::GET_DATA, snapshot)) {
        return SbsStatus::INCOHERENT;
    }
    if (StateBufferStore::isValueKind(snapshot.kind)) {
        return SbsStatus::WRONG_KIND;
    }
    const Measurement& measurement = snapshot.measurement;
    if (measurement.dataSize > data.getSize()) {
        return SbsStatus::INVALID_BUFFER_SIZE;
    }

    // The size check above leaves an empty Fw::Buffer here only with nothing to copy
    if (measurement.dataSize > 0) {
        FW_ASSERT(data.getData() != nullptr);
        (void)::memcpy(data.getData(), snapshot.payload, measurement.dataSize);
    }
    sizeOut = measurement.dataSize;
    measTime = measurement.time;
    return StateBufferStore::latestStatus(snapshot.stored, measurement, lastReadTime);
}

SbsStatus StateBufferStore::getMinMax_handler(FwIndexType portNum,
                                              const StateBufferStoreCfg::StateEntry& entry,
                                              SbsMeasurement& min,
                                              SbsMeasurement& max) {
    return this->readWatermarks(this->checkedEntry(entry), StateBufferStore_ReadOperation::GET_MIN_MAX, min, max);
}

SbsStatus StateBufferStore::clearMinMax_handler(FwIndexType portNum, const StateBufferStoreCfg::StateEntry& entry) {
    return StateBufferStore::clearWatermarks(this->checkedEntry(entry));
}

SbsStatus StateBufferStore::clearAndGetMinMax_handler(FwIndexType portNum,
                                                      const StateBufferStoreCfg::StateEntry& entry,
                                                      SbsMeasurement& min,
                                                      SbsMeasurement& max) {
    return this->readWatermarks(this->checkedEntry(entry), StateBufferStore_ReadOperation::CLEAR_AND_GET_MIN_MAX, min,
                                max);
}

SbsStatus StateBufferStore::getHistory_handler(FwIndexType portNum,
                                               const StateBufferStoreCfg::StateEntry& entry,
                                               Fw::Buffer& data,
                                               FwSizeType& sizeOut) {
    sizeOut = 0;
    Entry& block = this->checkedEntry(entry);
    // Kinds are never unclaimed, so a data entry is refused before its buffer is judged
    if (block.kind.load(std::memory_order_acquire) == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    const FwSizeType required = block.depth * HISTORY_RECORD_SIZE;
    // Strict: the full-history read either delivers the whole history or
    // nothing, so a caller cannot mistake a partial dump for a complete one
    if (data.getSize() < required) {
        return SbsStatus::INVALID_BUFFER_SIZE;
    }
    return this->readHistory(block, block.depth, StateBufferStore_ReadOperation::GET_HISTORY, data, sizeOut);
}

SbsStatus StateBufferStore::getNHistory_handler(FwIndexType portNum,
                                                const StateBufferStoreCfg::StateEntry& entry,
                                                FwSizeType numMeasurements,
                                                Fw::Buffer& data,
                                                FwSizeType& sizeOut) {
    sizeOut = 0;
    Entry& block = this->checkedEntry(entry);
    // Kinds are never unclaimed, so a data entry is refused before its request is judged
    if (block.kind.load(std::memory_order_acquire) == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }

    // Zero requests the entry's whole depth, and so does a request deeper than
    // it: depths differ per entry and no port reports them to the caller
    FwSizeType count = ((numMeasurements == 0) or (numMeasurements > block.depth)) ? block.depth : numMeasurements;

    // Truncate to whole measurements rather than failing, and report how many
    // bytes were actually written
    const FwSizeType capacity = data.getSize() / HISTORY_RECORD_SIZE;
    if (capacity < count) {
        count = capacity;
    }

    const SbsStatus status =
        this->readHistory(block, count, StateBufferStore_ReadOperation::GET_N_HISTORY, data, sizeOut);
    // With measurements to report but no room for one, truncation would
    // otherwise report success with nothing copied. Decided after the read,
    // so a failed or empty read reports that instead; getHistory, which
    // judges its buffer first, reports INVALID_BUFFER_SIZE ahead of them.
    if ((capacity == 0) and (status == SbsStatus::OK)) {
        return SbsStatus::INVALID_BUFFER_SIZE;
    }
    return status;
}

void StateBufferStore::tlmIn_handler(FwIndexType portNum, FwChanIdType id, Fw::Time& timeTag, Fw::TlmBuffer& val) {
    FW_ASSERT(this->m_initialized);
    // A component's telemetry port carries all of its channels, so channels
    // without a mapping are expected here and are not stored
    for (FwSizeType i = 0; i < this->m_tlmMappings.getSize(); i++) {
        const SbsTlmMapping& mapping = this->m_tlmMappings[i];
        if (mapping.get_chanId() == id) {
            Fw::PolyType value;
            if (StateBufferStore::decodeTlm(mapping.get_valueType(), val, value) != Fw::FW_SERIALIZE_OK) {
                this->log_WARNING_HI_TlmDecodeFailed(id, mapping.get_entry(), mapping.get_valueType(), val.getSize());
                return;
            }
            // Telemetry carries its own time tag, which is when the value was sampled
            StateBufferStore::storeValue(this->checkedEntry(mapping.get_entry()), value, timeTag, SbsStatus::OK);
            return;
        }
    }
}

// ----------------------------------------------------------------------
// Handler implementations for commands
// ----------------------------------------------------------------------

void StateBufferStore::REPORT_WATERMARKS_cmdHandler(FwOpcodeType opCode,
                                                    U32 cmdSeq,
                                                    const StateBufferStoreCfg::StateEntry& entry) {
    if (static_cast<FwSizeType>(entry.e) >= StateBufferStoreCfg::StateEntry::NUM_ENTRIES) {
        this->log_WARNING_LO_ReportWatermarksRejected(entry, StateBufferStore_WatermarkRejection::NOT_AN_ENTRY);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR);
        return;
    }

    SbsMeasurement min;
    SbsMeasurement max;
    const SbsStatus status =
        this->readWatermarks(this->checkedEntry(entry), StateBufferStore_ReadOperation::REPORT_WATERMARKS, min, max);
    if (status == SbsStatus::WRONG_KIND) {
        this->log_WARNING_LO_ReportWatermarksRejected(entry, StateBufferStore_WatermarkRejection::DATA_ENTRY);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    // readWatermarks has already emitted FailedReadCoherentData naming this command
    if (status == SbsStatus::INCOHERENT) {
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }

    this->log_ACTIVITY_HI_WatermarkReport(entry, StateBufferStore::toF64(min.get_value()),
                                          StateBufferStore::toMicroseconds(min.get_time()), min.get_validity(),
                                          StateBufferStore::toF64(max.get_value()),
                                          StateBufferStore::toMicroseconds(max.get_time()), max.get_validity());
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void StateBufferStore::CLEAR_WATERMARKS_cmdHandler(FwOpcodeType opCode,
                                                   U32 cmdSeq,
                                                   const StateBufferStoreCfg::StateEntry& entry) {
    if (static_cast<FwSizeType>(entry.e) >= StateBufferStoreCfg::StateEntry::NUM_ENTRIES) {
        this->log_WARNING_LO_ClearWatermarksRejected(entry, StateBufferStore_WatermarkRejection::NOT_AN_ENTRY);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR);
        return;
    }
    if (StateBufferStore::clearWatermarks(this->checkedEntry(entry)) == SbsStatus::WRONG_KIND) {
        this->log_WARNING_LO_ClearWatermarksRejected(entry, StateBufferStore_WatermarkRejection::DATA_ENTRY);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    this->log_ACTIVITY_HI_WatermarksCleared(entry);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void StateBufferStore::RESET_THROTTLES_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) {
    this->log_WARNING_HI_FailedReadCoherentData_ThrottleClear();
    this->log_WARNING_HI_TlmDecodeFailed_ThrottleClear();
    this->log_ACTIVITY_HI_ThrottlesReset();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

}  // namespace Svc
