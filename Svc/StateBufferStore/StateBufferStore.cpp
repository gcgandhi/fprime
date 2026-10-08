// ======================================================================
// \title  StateBufferStore.cpp
// \brief  cpp file for StateBufferStore component implementation class
// ======================================================================

#include "Svc/StateBufferStore/StateBufferStore.hpp"

#include <cstring>
#include <new>

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
        Entry& entry = this->m_entries[i];
        entry.history = nullptr;
        entry.data = nullptr;
        entry.coherencyCounter.store(0, std::memory_order_relaxed);
        entry.kind.store(KIND_UNSET, std::memory_order_relaxed);
        entry.depth = 0;
        entry.latest = 0;
        entry.stored = 0;
        entry.index = i;
        // Watermarks start "to be cleared" so that the first put seeds them.
        // Comparison cannot do it: Fw::PolyType's operator< and operator>
        // both return false when the stored types differ, and a
        // default-constructed PolyType holds no type at all.
        entry.clearPending.store(true, std::memory_order_relaxed);
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
        Entry& entry = this->m_entries[i];
        entry.history = reinterpret_cast<Measurement*>(cursor);  // aligned: region starts at the allocation base
        cursor += entry.depth * static_cast<FwSizeType>(sizeof(Measurement));
        for (FwSizeType slot = 0; slot < entry.depth; slot++) {
            (void)new (&entry.history[slot]) Measurement();
        }
    }
    for (FwSizeType i = 0; i < StateBufferStoreCfg::StateEntry::NUM_ENTRIES; i++) {
        Entry& entry = this->m_entries[i];
        entry.data = cursor;
        cursor += entry.depth * StateBufferStoreCfg::MAX_DATA_SIZE;
    }
    FW_ASSERT(cursor <= base + this->m_memSize, static_cast<FwAssertArgType>(cursor - base),
              static_cast<FwAssertArgType>(this->m_memSize));

    // Mapped entries are value entries from the start, so a data put to one
    // is refused even before its channel first arrives
    for (FwSizeType i = 0; i < tlmMappings.getSize(); i++) {
        this->m_entries[static_cast<FwSizeType>(tlmMappings[i].get_entry())].kind.store(KIND_VALUE,
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

bool StateBufferStore::claimKind(Entry& entry, EntryKind kind) {
    U8 current = KIND_UNSET;
    if (entry.kind.compare_exchange_strong(current, static_cast<U8>(kind), std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
        return true;
    }
    // On failure compare_exchange_strong leaves the kind already claimed in current
    return current == static_cast<U8>(kind);
}

void StateBufferStore::beginWrite(Entry& entry) {
    (void)entry.coherencyCounter.fetch_add(1, std::memory_order_relaxed);
    // Keeps the write's stores from becoming visible before the odd count
    std::atomic_thread_fence(std::memory_order_release);
}

void StateBufferStore::endWrite(Entry& entry) {
    (void)entry.coherencyCounter.fetch_add(1, std::memory_order_release);
}

U32 StateBufferStore::beginRead(Entry& entry) {
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
        return entry.coherencyCounter.load(std::memory_order_acquire) + 2;
    }
#endif
    return entry.coherencyCounter.load(std::memory_order_acquire);
}

bool StateBufferStore::endRead(const Entry& entry, U32 before) {
    // Keeps the copy's loads from being satisfied after the counter is resampled
    std::atomic_thread_fence(std::memory_order_acquire);
    const U32 after = entry.coherencyCounter.load(std::memory_order_relaxed);
    // An odd count means the copy began mid-write; a changed one means a write
    // began during it. Either way the bytes may mix two measurements.
    return ((before % 2U) == 0U) and (before == after);
}

void StateBufferStore::storeValue(Entry& entry,
                                  const Fw::PolyType& val,
                                  const Fw::Time& time,
                                  const SbsStatus& validity) {
    StateBufferStore::beginWrite(entry);
    const FwSizeType slot = (entry.latest + 1) % entry.depth;
    entry.history[slot].time = time;
    entry.history[slot].value = val;
    entry.history[slot].validity = validity;
    entry.history[slot].dataSize = 0;
    entry.latest = slot;
    if (entry.stored < entry.depth) {
        entry.stored++;
    }
    StateBufferStore::updateWatermarks(entry, val, time, validity);
    StateBufferStore::endWrite(entry);
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

void StateBufferStore::updateWatermarks(Entry& entry,
                                        const Fw::PolyType& val,
                                        const Fw::Time& time,
                                        const SbsStatus& validity) {
    // The clear, not the comparison, is what seeds the watermarks: see the
    // constructor's note on PolyType comparison across differing types.
    // Consuming it inside the write, and retiring the watermarks it ends,
    // lets readWatermarks report an epoch this put has already closed.
    if (entry.clearPending.exchange(false, std::memory_order_acq_rel)) {
        entry.retiredMin = entry.min;
        entry.retiredMax = entry.max;
        entry.min.value = val;
        entry.min.time = time;
        entry.min.validity = validity;
        entry.max = entry.min;
        return;
    }
    if (val < entry.min.value) {
        entry.min.value = val;
        entry.min.time = time;
        entry.min.validity = validity;
    }
    if (val > entry.max.value) {
        entry.max.value = val;
        entry.max.time = time;
        entry.max.validity = validity;
    }
}

SbsStatus StateBufferStore::readWatermarks(Entry& entry,
                                           bool clear,
                                           StateBufferStore_ReadOperation operation,
                                           SbsMeasurement& min,
                                           SbsMeasurement& max) {
    // Kinds are never unclaimed, so a refused clear can be decided up front
    if (entry.kind.load(std::memory_order_acquire) == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    // A clear that finds one already pending ends an epoch with no puts in it
    const bool alreadyCleared = clear and entry.clearPending.exchange(true, std::memory_order_acq_rel);

    Measurement minCopy;
    Measurement maxCopy;
    bool cleared = false;
    FwSizeType stored = 0;
    U8 kind = KIND_UNSET;
    bool coherent = false;
    for (U32 iteration = 0; iteration < StateBufferStoreCfg::MAX_READ_ITERATIONS; iteration++) {
        const U32 before = this->beginRead(entry);
        const bool pending = entry.clearPending.load(std::memory_order_acquire);
        // Once a put has consumed this read's own clear, the watermarks the
        // clear ended are the ones that put retired
        const bool retired = clear and not pending;
        minCopy = retired ? entry.retiredMin : entry.min;
        maxCopy = retired ? entry.retiredMax : entry.max;
        cleared = clear ? alreadyCleared : pending;
        stored = entry.stored;
        kind = entry.kind.load(std::memory_order_relaxed);
        if (StateBufferStore::endRead(entry, before)) {
            coherent = true;
            break;
        }
    }

    if (not coherent) {
        this->log_FATAL_FailedReadCoherentData(static_cast<StateBufferStoreCfg::StateEntry::T>(entry.index), operation,
                                               StateBufferStoreCfg::MAX_READ_ITERATIONS);
        return SbsStatus::INCOHERENT;
    }
    if (kind == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    reportWatermark(minCopy, cleared, min);
    reportWatermark(maxCopy, cleared, max);
    return (stored > 0) ? SbsStatus::OK : SbsStatus::NOT_WRITTEN;
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
    // out through its own checker. An entry never written holds no type.
    if (value.isU8()) {
        U8 v;
        value.get(v);
        return static_cast<F64>(v);
    }
    if (value.isI8()) {
        I8 v;
        value.get(v);
        return static_cast<F64>(v);
    }
    if (value.isU16()) {
        U16 v;
        value.get(v);
        return static_cast<F64>(v);
    }
    if (value.isI16()) {
        I16 v;
        value.get(v);
        return static_cast<F64>(v);
    }
    if (value.isU32()) {
        U32 v;
        value.get(v);
        return static_cast<F64>(v);
    }
    if (value.isI32()) {
        I32 v;
        value.get(v);
        return static_cast<F64>(v);
    }
    if (value.isU64()) {
        U64 v;
        value.get(v);
        return static_cast<F64>(v);
    }
    if (value.isI64()) {
        I64 v;
        value.get(v);
        return static_cast<F64>(v);
    }
    if (value.isF32()) {
        F32 v;
        value.get(v);
        return static_cast<F64>(v);
    }
    if (value.isF64()) {
        F64 v;
        value.get(v);
        return v;
    }
    if (value.isBool()) {
        bool v;
        value.get(v);
        return v ? 1.0 : 0.0;
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

SbsStatus StateBufferStore::readHistory(Entry& entry,
                                        FwSizeType count,
                                        StateBufferStore_ReadOperation operation,
                                        Fw::Buffer& data,
                                        FwSizeType& sizeOut) {
    FW_ASSERT(count <= entry.depth, static_cast<FwAssertArgType>(count), static_cast<FwAssertArgType>(entry.depth));
    U8* const out = data.getData();
    FW_ASSERT(out != nullptr);

    FwSizeType copied = 0;
    FwSizeType stored = 0;
    U8 kind = KIND_UNSET;
    bool serialized = true;
    bool coherent = false;
    for (U32 iteration = 0; iteration < StateBufferStoreCfg::MAX_READ_ITERATIONS; iteration++) {
        const U32 before = this->beginRead(entry);
        kind = entry.kind.load(std::memory_order_relaxed);
        stored = entry.stored;
        const FwSizeType latest = entry.latest;
        // Only measurements actually stored are reported: an unwritten slot
        // holds an untyped PolyType, which cannot be serialized
        copied = (count < stored) ? count : stored;
        serialized = true;
        // A data entry's slots hold no value to serialize
        if (kind == KIND_VALUE) {
            for (FwSizeType i = 0; i < copied; i++) {
                // Walk oldest to newest: the oldest of copied measurements
                // sits copied-1 slots behind latest, modulo the ring
                const FwSizeType age = copied - 1 - i;
                const Measurement& slot = entry.history[(latest + entry.depth - age) % entry.depth];
                U8* const record = &out[i * HISTORY_RECORD_SIZE];
                // PolyType serializes narrow types in fewer bytes than its
                // maximum; zero the remainder rather than leave caller bytes
                (void)::memset(record, 0, HISTORY_RECORD_SIZE);
                const SbsMeasurement measurement(slot.time, slot.validity, slot.value);
                Fw::ExternalSerializeBuffer serializer(record, HISTORY_RECORD_SIZE);
                // A torn copy may pair a newer count with an unwritten slot,
                // so a failure is only an error once the read proves coherent
                if (serializer.serializeFrom(measurement) != Fw::FW_SERIALIZE_OK) {
                    serialized = false;
                }
            }
        }
        if (StateBufferStore::endRead(entry, before)) {
            coherent = true;
            break;
        }
    }

    if (not coherent) {
        this->log_FATAL_FailedReadCoherentData(static_cast<StateBufferStoreCfg::StateEntry::T>(entry.index), operation,
                                               StateBufferStoreCfg::MAX_READ_ITERATIONS);
        sizeOut = 0;
        return SbsStatus::INCOHERENT;
    }
    if (kind == KIND_DATA) {
        sizeOut = 0;
        return SbsStatus::WRONG_KIND;
    }
    FW_ASSERT(serialized);
    sizeOut = copied * HISTORY_RECORD_SIZE;
    return (stored > 0) ? SbsStatus::OK : SbsStatus::NOT_WRITTEN;
}

// ----------------------------------------------------------------------
// Handler implementations for typed input ports
// ----------------------------------------------------------------------

SbsStatus StateBufferStore::putValue_handler(FwIndexType portNum,
                                             const StateBufferStoreCfg::StateEntry& entry,
                                             Fw::PolyType& val,
                                             const SbsStatus& validity) {
    Entry& record = this->checkedEntry(entry);
    // Timestamp first: the value's time should reflect when it was produced,
    // not how long the store took
    const Fw::Time now = this->getTime();
    // An untyped value cannot be serialized into a history dump
    FW_ASSERT(StateBufferStore::isTyped(val));
    if (not StateBufferStore::claimKind(record, KIND_VALUE)) {
        return SbsStatus::WRONG_KIND;
    }
    StateBufferStore::storeValue(record, val, now, validity);
    return SbsStatus::OK;
}

SbsStatus StateBufferStore::getValue_handler(FwIndexType portNum,
                                             const StateBufferStoreCfg::StateEntry& entry,
                                             Fw::PolyType& val,
                                             Fw::Time& measTime,
                                             const Fw::Time& lastReadTime) {
    Entry& record = this->checkedEntry(entry);

    Measurement measurement;
    FwSizeType stored = 0;
    U8 kind = KIND_UNSET;
    bool coherent = false;
    for (U32 iteration = 0; iteration < StateBufferStoreCfg::MAX_READ_ITERATIONS; iteration++) {
        const U32 before = this->beginRead(record);
        kind = record.kind.load(std::memory_order_relaxed);
        measurement = record.history[record.latest];
        stored = record.stored;
        if (StateBufferStore::endRead(record, before)) {
            coherent = true;
            break;
        }
    }

    if (not coherent) {
        this->log_FATAL_FailedReadCoherentData(entry, StateBufferStore_ReadOperation::GET_VALUE,
                                               StateBufferStoreCfg::MAX_READ_ITERATIONS);
        return SbsStatus::INCOHERENT;
    }
    if (kind == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    val = measurement.value;
    measTime = measurement.time;
    if (stored == 0) {
        return SbsStatus::NOT_WRITTEN;
    }
    // Fw::ZERO_TIME stands in for the heritage NULL: no previous read to compare against
    if ((lastReadTime != Fw::ZERO_TIME) and (measurement.time == lastReadTime)) {
        return SbsStatus::NOT_FRESH;
    }
    return measurement.validity;
}

SbsStatus StateBufferStore::putData_handler(FwIndexType portNum,
                                            const StateBufferStoreCfg::StateEntry& entry,
                                            Fw::Buffer& data,
                                            const SbsStatus& validity) {
    Entry& record = this->checkedEntry(entry);
    const Fw::Time now = this->getTime();
    const FwSizeType size = data.getSize();
    FW_ASSERT(size <= StateBufferStoreCfg::MAX_DATA_SIZE, static_cast<FwAssertArgType>(size));
    FW_ASSERT(data.getData() != nullptr);
    if (not StateBufferStore::claimKind(record, KIND_DATA)) {
        return SbsStatus::WRONG_KIND;
    }

    StateBufferStore::beginWrite(record);
    const FwSizeType slot = (record.latest + 1) % record.depth;
    (void)::memcpy(&record.data[slot * StateBufferStoreCfg::MAX_DATA_SIZE], data.getData(), size);
    record.history[slot].time = now;
    record.history[slot].validity = validity;
    record.history[slot].dataSize = size;
    record.latest = slot;
    if (record.stored < record.depth) {
        record.stored++;
    }
    StateBufferStore::endWrite(record);
    return SbsStatus::OK;
}

SbsStatus StateBufferStore::getData_handler(FwIndexType portNum,
                                            const StateBufferStoreCfg::StateEntry& entry,
                                            Fw::Buffer& data,
                                            Fw::Time& measTime,
                                            const Fw::Time& lastReadTime,
                                            FwSizeType& sizeOut) {
    Entry& record = this->checkedEntry(entry);
    FW_ASSERT(data.getData() != nullptr);

    Measurement measurement;
    U8 scratch[StateBufferStoreCfg::MAX_DATA_SIZE];
    FwSizeType stored = 0;
    U8 kind = KIND_UNSET;
    bool coherent = false;
    for (U32 iteration = 0; iteration < StateBufferStoreCfg::MAX_READ_ITERATIONS; iteration++) {
        const U32 before = this->beginRead(record);
        kind = record.kind.load(std::memory_order_relaxed);
        const FwSizeType index = record.latest;
        measurement = record.history[index];
        // Bounded before the coherency check: a torn size must not overrun scratch
        if (measurement.dataSize > StateBufferStoreCfg::MAX_DATA_SIZE) {
            measurement.dataSize = StateBufferStoreCfg::MAX_DATA_SIZE;
        }
        (void)::memcpy(scratch, &record.data[index * StateBufferStoreCfg::MAX_DATA_SIZE], measurement.dataSize);
        stored = record.stored;
        if (StateBufferStore::endRead(record, before)) {
            coherent = true;
            break;
        }
    }

    if (not coherent) {
        this->log_FATAL_FailedReadCoherentData(entry, StateBufferStore_ReadOperation::GET_DATA,
                                               StateBufferStoreCfg::MAX_READ_ITERATIONS);
        sizeOut = 0;
        return SbsStatus::INCOHERENT;
    }
    if (kind == KIND_VALUE) {
        sizeOut = 0;
        return SbsStatus::WRONG_KIND;
    }
    if (measurement.dataSize > data.getSize()) {
        sizeOut = 0;
        return SbsStatus::INVALID_BUFFER_SIZE;
    }

    (void)::memcpy(data.getData(), scratch, measurement.dataSize);
    sizeOut = measurement.dataSize;
    measTime = measurement.time;

    if (stored == 0) {
        return SbsStatus::NOT_WRITTEN;
    }
    // Fw::ZERO_TIME stands in for the heritage NULL: no previous read to compare against
    if ((lastReadTime != Fw::ZERO_TIME) and (measurement.time == lastReadTime)) {
        return SbsStatus::NOT_FRESH;
    }
    return measurement.validity;
}

SbsStatus StateBufferStore::getMinMax_handler(FwIndexType portNum,
                                              const StateBufferStoreCfg::StateEntry& entry,
                                              SbsMeasurement& min,
                                              SbsMeasurement& max) {
    return this->readWatermarks(this->checkedEntry(entry), false, StateBufferStore_ReadOperation::GET_MIN_MAX, min,
                                max);
}

SbsStatus StateBufferStore::clearMinMax_handler(FwIndexType portNum, const StateBufferStoreCfg::StateEntry& entry) {
    Entry& record = this->checkedEntry(entry);
    if (record.kind.load(std::memory_order_acquire) == KIND_DATA) {
        return SbsStatus::WRONG_KIND;
    }
    record.clearPending.store(true, std::memory_order_release);
    return SbsStatus::OK;
}

SbsStatus StateBufferStore::clearAndGetMinMax_handler(FwIndexType portNum,
                                                      const StateBufferStoreCfg::StateEntry& entry,
                                                      SbsMeasurement& min,
                                                      SbsMeasurement& max) {
    return this->readWatermarks(this->checkedEntry(entry), true, StateBufferStore_ReadOperation::CLEAR_AND_GET_MIN_MAX,
                                min, max);
}

SbsStatus StateBufferStore::getHistory_handler(FwIndexType portNum,
                                               const StateBufferStoreCfg::StateEntry& entry,
                                               Fw::Buffer& data,
                                               FwSizeType& sizeOut) {
    Entry& record = this->checkedEntry(entry);
    const FwSizeType required = record.depth * HISTORY_RECORD_SIZE;
    // Strict: the full-history read either delivers the whole history or
    // nothing, so a caller cannot mistake a partial dump for a complete one
    if (data.getSize() < required) {
        sizeOut = 0;
        return SbsStatus::INVALID_BUFFER_SIZE;
    }
    return this->readHistory(record, record.depth, StateBufferStore_ReadOperation::GET_HISTORY, data, sizeOut);
}

SbsStatus StateBufferStore::getNHistory_handler(FwIndexType portNum,
                                                const StateBufferStoreCfg::StateEntry& entry,
                                                U16 numMeasurements,
                                                Fw::Buffer& data,
                                                FwSizeType& sizeOut) {
    Entry& record = this->checkedEntry(entry);
    FW_ASSERT(numMeasurements <= record.depth, static_cast<FwAssertArgType>(numMeasurements),
              static_cast<FwAssertArgType>(record.depth));

    // Zero requests the entry's whole depth
    FwSizeType count = (numMeasurements == 0) ? record.depth : static_cast<FwSizeType>(numMeasurements);

    // Truncate to whole measurements rather than failing, and report how many
    // bytes were actually written
    const FwSizeType capacity = data.getSize() / HISTORY_RECORD_SIZE;
    if (capacity < count) {
        count = capacity;
    }

    return this->readHistory(record, count, StateBufferStore_ReadOperation::GET_N_HISTORY, data, sizeOut);
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
    const SbsStatus status = this->readWatermarks(this->checkedEntry(entry), false,
                                                  StateBufferStore_ReadOperation::REPORT_WATERMARKS, min, max);
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
    if (this->clearMinMax_handler(0, entry) == SbsStatus::WRONG_KIND) {
        this->log_WARNING_LO_ClearWatermarksRejected(entry, StateBufferStore_WatermarkRejection::DATA_ENTRY);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    this->log_ACTIVITY_HI_WatermarksCleared(entry);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

}  // namespace Svc
