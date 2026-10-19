// ======================================================================
// \title  StateBufferStoreTester.cpp
// \brief  cpp file for StateBufferStore component test harness implementation class
// ======================================================================

#include "StateBufferStoreTester.hpp"

#include <cmath>
#include <limits>

#include "Fw/Types/SerialBuffer.hpp"
#include "Fw/Types/String.hpp"

namespace Svc {

namespace {

//! Entries used by the tests
constexpr StateBufferStoreCfg::StateEntry::T ENTRY_A = StateBufferStoreCfg::StateEntry::SBS_ENTRY_00;
constexpr StateBufferStoreCfg::StateEntry::T ENTRY_B = StateBufferStoreCfg::StateEntry::SBS_ENTRY_01;

//! Bytes one serialized history measurement occupies
constexpr FwSizeType RECORD_SIZE = SbsMeasurement::SERIALIZED_SIZE;

//! History depth every entry carries in the default configuration
constexpr FwSizeType DEPTH = StateBufferStoreCfg::DEFAULT_HISTORY_DEPTH;

//! Value stored by putDuringRead
constexpr I32 RACING_PUT_VALUE = 500;

//! A kind byte no EntryKind uses, as a memory upset might leave it
constexpr U8 CORRUPT_KIND = 9;

//! StateBufferStore's KIND_VALUE and KIND_DATA, which are private to it, as
//! an upset could flip one into the other
constexpr U8 VALUE_KIND = 1;
constexpr U8 DATA_KIND = 2;

//! Memory identifier the component under test allocates under
constexpr FwEnumStoreType TEST_MEM_ID = 0;

//! Entries the sample configuration sets to the depth bounds
constexpr StateBufferStoreCfg::StateEntry::T ENTRY_MIN_DEPTH = StateBufferStoreCfg::StateEntry::SBS_ENTRY_08;
constexpr StateBufferStoreCfg::StateEntry::T ENTRY_MAX_DEPTH = StateBufferStoreCfg::StateEntry::SBS_ENTRY_09;

//! Telemetry channels in the test mapping table, and one outside it
constexpr FwChanIdType TLM_CHAN_U32 = 0x100;
constexpr FwChanIdType TLM_CHAN_F32 = 0x101;
constexpr FwChanIdType TLM_CHAN_BOOL = 0x102;
constexpr FwChanIdType TLM_CHAN_STRUCT = 0x103;
constexpr FwChanIdType TLM_CHAN_STRING = 0x104;
constexpr FwChanIdType TLM_CHAN_UNMAPPED = 0x1FF;

//! Entries the test telemetry channels are stored in
constexpr StateBufferStoreCfg::StateEntry::T ENTRY_TLM_STRING = StateBufferStoreCfg::StateEntry::SBS_ENTRY_03;
constexpr StateBufferStoreCfg::StateEntry::T ENTRY_TLM_STRUCT = StateBufferStoreCfg::StateEntry::SBS_ENTRY_04;
constexpr StateBufferStoreCfg::StateEntry::T ENTRY_TLM_U32 = StateBufferStoreCfg::StateEntry::SBS_ENTRY_05;
constexpr StateBufferStoreCfg::StateEntry::T ENTRY_TLM_F32 = StateBufferStoreCfg::StateEntry::SBS_ENTRY_06;
constexpr StateBufferStoreCfg::StateEntry::T ENTRY_TLM_BOOL = StateBufferStoreCfg::StateEntry::SBS_ENTRY_07;

//! Telemetry mapping table given to the component under test
SbsTlmMapping TLM_MAPPINGS[] = {
    SbsTlmMapping(TLM_CHAN_U32, ENTRY_TLM_U32, SbsTlmType::TYPE_U32),
    SbsTlmMapping(TLM_CHAN_F32, ENTRY_TLM_F32, SbsTlmType::TYPE_F32),
    SbsTlmMapping(TLM_CHAN_BOOL, ENTRY_TLM_BOOL, SbsTlmType::TYPE_BOOL),
    SbsTlmMapping(TLM_CHAN_STRUCT, ENTRY_TLM_STRUCT, SbsTlmType::TYPE_DATA),
    SbsTlmMapping(TLM_CHAN_STRING, ENTRY_TLM_STRING, SbsTlmType::TYPE_DATA),
};

//! Read from a component that was never configured
void readUnconfigured() {
    StateBufferStore fresh("fresh");
    fresh.init(StateBufferStoreTester::TEST_INSTANCE_ID);
    Fw::PolyType value;
    Fw::Time measTime;
    (void)fresh.get_getValue_InputPort(0)->invoke(ENTRY_A, value, measTime, Fw::Time());
}

//! Store one telemetry value in a fresh component mapping it as the given
//! type, and return what that component then reports for the entry
template <typename T>
Fw::PolyType storeTlmAs(SbsTlmType::T type, T sample) {
    Fw::MallocAllocator allocator;
    StateBufferStore fresh("fresh");
    fresh.init(StateBufferStoreTester::TEST_INSTANCE_ID);
    SbsTlmMapping mapping[] = {SbsTlmMapping(TLM_CHAN_U32, ENTRY_A, type)};
    fresh.configure(TEST_MEM_ID, allocator, Fw::ExternalArray<SbsTlmMapping>(mapping, FW_NUM_ARRAY_ELEMENTS(mapping)));

    Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);
    Fw::TlmBuffer buffer;
    EXPECT_EQ(buffer.serializeFrom(sample), Fw::FW_SERIALIZE_OK);
    fresh.get_tlmIn_InputPort(0)->invoke(TLM_CHAN_U32, tag, buffer);

    Fw::PolyType value;
    Fw::Time measTime;
    EXPECT_EQ(fresh.get_getValue_InputPort(0)->invoke(ENTRY_A, value, measTime, Fw::Time()), SbsStatus::OK);
    return value;
}

//! Whether two PolyTypes hold the same type and value
//!
//! PolyType::operator== compares type as well as value, but is false for
//! every pair of floats, so those are compared by value here.
bool sameTypeAndValue(const Fw::PolyType& lhs, const Fw::PolyType& rhs) {
    Fw::PolyType left = lhs;
    Fw::PolyType right = rhs;
    if (left.isF32() and right.isF32()) {
        return static_cast<F32>(left) == static_cast<F32>(right);
    }
    if (left.isF64() and right.isF64()) {
        return static_cast<F64>(left) == static_cast<F64>(right);
    }
    return left == right;
}

//! Check that telemetry mapped as the given type reads back as a PolyType of that type and value
template <typename T>
void checkTlmType(SbsTlmType::T type, T sample) {
    EXPECT_TRUE(sameTypeAndValue(storeTlmAs(type, sample), Fw::PolyType(sample)))
        << "mapping type " << static_cast<U32>(type);
}

//! Configure a fresh component with the given telemetry mappings
void configureWithMappings(SbsTlmMapping* mappings, FwSizeType count) {
    Fw::MallocAllocator allocator;
    StateBufferStore fresh("fresh");
    fresh.init(StateBufferStoreTester::TEST_INSTANCE_ID);
    fresh.configure(TEST_MEM_ID, allocator, Fw::ExternalArray<SbsTlmMapping>(mappings, count));
}

}  // namespace

// ----------------------------------------------------------------------
// Construction and destruction
// ----------------------------------------------------------------------

StateBufferStoreTester::StateBufferStoreTester()
    : StateBufferStoreGTestBase("StateBufferStoreTester", StateBufferStoreTester::MAX_HISTORY_SIZE),
      component("StateBufferStore"),
      m_cmdSeq(0) {
    this->initComponents();
    this->connectPorts();
    this->component.configure(TEST_MEM_ID, this->m_allocator,
                              Fw::ExternalArray<SbsTlmMapping>(TLM_MAPPINGS, FW_NUM_ARRAY_ELEMENTS(TLM_MAPPINGS)));
}

StateBufferStoreTester::~StateBufferStoreTester() {}

// ----------------------------------------------------------------------
// Helper functions
// ----------------------------------------------------------------------

void StateBufferStoreTester::put(StateBufferStoreCfg::StateEntry::T entry,
                                 const Fw::PolyType& value,
                                 U32 seconds,
                                 SbsStatus validity) {
    this->setTestTime(Fw::Time(TimeBase::TB_WORKSTATION_TIME, seconds, 0));
    EXPECT_EQ(this->invoke_to_putValue(0, entry, value, validity), SbsStatus::OK);
}

Fw::PolyType StateBufferStoreTester::get(StateBufferStoreCfg::StateEntry::T entry,
                                         SbsStatus expected,
                                         const Fw::Time& lastReadTime) {
    Fw::PolyType value;
    Fw::Time measTime;
    const SbsStatus actual = this->invoke_to_getValue(0, entry, value, measTime, lastReadTime);
    EXPECT_EQ(actual, expected);
    return value;
}

SbsMeasurement StateBufferStoreTester::readRecord(const Fw::Buffer& data, FwSizeType index) {
    Fw::SerialBuffer deserializer(&data.getData()[index * RECORD_SIZE], RECORD_SIZE);
    deserializer.fill();
    SbsMeasurement record;
    EXPECT_EQ(deserializer.deserializeTo(record), Fw::FW_SERIALIZE_OK);
    return record;
}

void StateBufferStoreTester::putDuringRead(void* context) {
    StateBufferStoreTester* const tester = static_cast<StateBufferStoreTester*>(context);
    tester->put(ENTRY_A, Fw::PolyType(RACING_PUT_VALUE), 99);
}

void StateBufferStoreTester::corruptIndexDuringRead(void* context) {
    StateBufferStoreTester* const tester = static_cast<StateBufferStoreTester*>(context);
    tester->component.utEntry(ENTRY_A).index = StateBufferStoreCfg::StateEntry::NUM_ENTRIES;
}

void StateBufferStoreTester::corruptKindDuringRead(void* context) {
    StateBufferStoreTester* const tester = static_cast<StateBufferStoreTester*>(context);
    tester->component.utEntry(ENTRY_A).kind.store(CORRUPT_KIND);
}

void StateBufferStoreTester::flipToDataKindDuringRead(void* context) {
    StateBufferStoreTester* const tester = static_cast<StateBufferStoreTester*>(context);
    tester->component.utEntry(ENTRY_A).kind.store(DATA_KIND);
}

void StateBufferStoreTester::flipToValueKindDuringRead(void* context) {
    StateBufferStoreTester* const tester = static_cast<StateBufferStoreTester*>(context);
    tester->component.utEntry(ENTRY_B).kind.store(VALUE_KIND);
}

void StateBufferStoreTester::useCorrupted(Corruption corruption) {
    auto& blockA = this->component.utEntry(ENTRY_A);
    auto& blockB = this->component.utEntry(ENTRY_B);
    switch (corruption) {
        case Corruption::DEPTH_ZERO:
            blockA.depth = 0;
            break;
        case Corruption::DEPTH_ABOVE_MAX:
            blockA.depth = StateBufferStoreCfg::MAX_HISTORY_DEPTH + 1;
            break;
        case Corruption::INDEX:
            blockA.index = static_cast<FwSizeType>(ENTRY_B);
            break;
        case Corruption::HISTORY_NULL:
            blockA.history = nullptr;
            break;
        case Corruption::DATA_NULL:
            blockA.data = nullptr;
            break;
        case Corruption::LATEST_VALUE_READ:
            // Stored first: a read of an entry never stored to reads nothing
            this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);
            blockA.latest = blockA.depth;
            break;
        case Corruption::LATEST_DATA_READ: {
            U8 stored[] = {'d'};
            Fw::Buffer in(stored, sizeof stored);
            (void)this->invoke_to_putData(0, ENTRY_B, in, SbsStatus::OK);
            blockB.latest = blockB.depth;
            U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
            Fw::Buffer out(readBack, sizeof readBack);
            Fw::Time measTime;
            FwSizeType sizeOut = 0;
            (void)this->invoke_to_getData(0, ENTRY_B, out, measTime, Fw::Time(), sizeOut);
            return;
        }
        case Corruption::STORED:
            blockA.stored = blockA.depth + 1;
            this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);
            return;
        case Corruption::LATEST_PUT:
            blockA.latest = blockA.depth;
            this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);
            return;
        case Corruption::INDEX_DURING_READ:
            // checkedEntry() sees a sound index; the hook corrupts it once the
            // read is under way, and forced torn reads carry it to the warning.
            // Stored first: a read of an entry never stored to reads nothing
            this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);
            this->component.m_utBeforeRead = &StateBufferStoreTester::corruptIndexDuringRead;
            this->component.m_utBeforeReadContext = this;
            this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
            break;
        case Corruption::KIND:
            blockA.kind.store(CORRUPT_KIND);
            break;
        case Corruption::TLM_VALIDITY: {
            this->component.utEntry(ENTRY_TLM_U32).tlmValidity.store(static_cast<U8>(SbsStatus::NOT_FRESH));
            Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);
            Fw::TlmBuffer u32Value;
            (void)u32Value.serializeFrom(static_cast<U32>(1));
            this->invoke_to_tlmIn(0, TLM_CHAN_U32, tag, u32Value);
            return;
        }
        case Corruption::KIND_DURING_READ: {
            // checkedEntry() sees a sound kind; the hook corrupts it before
            // the snapshot copies it
            this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);
            this->component.m_utBeforeRead = &StateBufferStoreTester::corruptKindDuringRead;
            this->component.m_utBeforeReadContext = this;
            U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
            Fw::Buffer data(bytes, sizeof bytes);
            FwSizeType sizeOut = 0;
            (void)this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut);
            return;
        }
        case Corruption::KIND_FLIP_VALUE_READ:
        case Corruption::KIND_FLIP_WATERMARK_READ:
        case Corruption::KIND_FLIP_HISTORY_READ: {
            // The read's kind check sees a value entry; the hook flips it to a
            // valid but different kind before the snapshot copies it
            this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);
            this->component.m_utBeforeRead = &StateBufferStoreTester::flipToDataKindDuringRead;
            this->component.m_utBeforeReadContext = this;
            if (corruption == Corruption::KIND_FLIP_VALUE_READ) {
                break;
            }
            if (corruption == Corruption::KIND_FLIP_WATERMARK_READ) {
                SbsMeasurement min;
                SbsMeasurement max;
                (void)this->invoke_to_getMinMax(0, ENTRY_A, min, max);
                return;
            }
            U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
            Fw::Buffer data(bytes, sizeof bytes);
            FwSizeType sizeOut = 0;
            (void)this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut);
            return;
        }
        case Corruption::KIND_FLIP_DATA_READ: {
            U8 stored[] = {'d'};
            Fw::Buffer in(stored, sizeof stored);
            (void)this->invoke_to_putData(0, ENTRY_B, in, SbsStatus::OK);
            this->component.m_utBeforeRead = &StateBufferStoreTester::flipToValueKindDuringRead;
            this->component.m_utBeforeReadContext = this;
            U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
            Fw::Buffer out(readBack, sizeof readBack);
            Fw::Time measTime;
            FwSizeType sizeOut = 0;
            (void)this->invoke_to_getData(0, ENTRY_B, out, measTime, Fw::Time(), sizeOut);
            return;
        }
    }
    (void)this->get(ENTRY_A, SbsStatus::OK);
}

// ----------------------------------------------------------------------
// Tests: put and get
// ----------------------------------------------------------------------

void StateBufferStoreTester::putGetTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(42)), 100);

    Fw::PolyType value;
    Fw::Time measTime;
    const SbsStatus status = this->invoke_to_getValue(0, ENTRY_A, value, measTime, Fw::Time());
    ASSERT_EQ(status, SbsStatus::OK);
    ASSERT_EQ(static_cast<U32>(value), 42u);
    ASSERT_EQ(measTime.getSeconds(), 100u);
}

void StateBufferStoreTester::allTypesTest() {
    // Each put replaces the entry's type; the store is type-agnostic
    const Fw::PolyType samples[] = {
        Fw::PolyType(static_cast<U8>(8)),
        Fw::PolyType(static_cast<I8>(-8)),
        Fw::PolyType(static_cast<U16>(16)),
        Fw::PolyType(static_cast<I16>(-16)),
        Fw::PolyType(static_cast<U32>(32)),
        Fw::PolyType(static_cast<I32>(-32)),
        Fw::PolyType(static_cast<U64>(1) << 40),
        Fw::PolyType(-(static_cast<I64>(1) << 40)),
        Fw::PolyType(static_cast<F32>(1.5f)),
        Fw::PolyType(static_cast<F64>(2.25)),
        Fw::PolyType(true),
    };
    U32 seconds = 1;
    for (const Fw::PolyType& sample : samples) {
        this->put(ENTRY_A, sample, seconds++);
        ASSERT_TRUE(sameTypeAndValue(this->get(ENTRY_A, SbsStatus::OK), sample))
            << "sample stored at " << (seconds - 1) << " s";
    }
}

void StateBufferStoreTester::putWithValidityTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(7)), 10, SbsStatus::INVALID);
    // The stored validity is reported verbatim, not overridden
    (void)this->get(ENTRY_A, SbsStatus::INVALID);
}

void StateBufferStoreTester::putRefusesStoreStatusTest() {
    // Statuses the store reports itself are not validities a writer may record
    const SbsStatus refused[] = {SbsStatus::UNINITIALIZED,       SbsStatus::NOT_WRITTEN, SbsStatus::NOT_FRESH,
                                 SbsStatus::INVALID_BUFFER_SIZE, SbsStatus::INCOHERENT,  SbsStatus::WRONG_KIND,
                                 SbsStatus::INVALID_VALIDITY};
    U8 stored[] = {'v'};
    Fw::Buffer in(stored, sizeof stored);
    for (const SbsStatus& validity : refused) {
        Fw::PolyType val(static_cast<U32>(1));
        ASSERT_EQ(this->invoke_to_putValue(0, ENTRY_A, val, validity), SbsStatus::INVALID_VALIDITY);
        ASSERT_EQ(this->invoke_to_putData(0, ENTRY_B, in, validity), SbsStatus::INVALID_VALIDITY);
    }

    // Nothing was stored and neither entry was claimed, so either kind still fits
    (void)this->get(ENTRY_A, SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, in, SbsStatus::OK), SbsStatus::OK);
    this->put(ENTRY_B, Fw::PolyType(static_cast<U32>(2)), 1);
}

void StateBufferStoreTester::notWrittenTest() {
    Fw::PolyType value;
    Fw::Time measTime;
    const SbsStatus status = this->invoke_to_getValue(0, ENTRY_A, value, measTime, Fw::Time());
    ASSERT_EQ(status, SbsStatus::NOT_WRITTEN);
}

void StateBufferStoreTester::notFreshTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(5)), 50);

    Fw::PolyType value;
    Fw::Time measTime;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, value, measTime, Fw::Time()), SbsStatus::OK);

    // Repeating the measurement time as the caller's last-read time means the
    // value has not changed since that read
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, value, measTime, measTime), SbsStatus::NOT_FRESH);

    // A newer put makes it fresh again
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(6)), 51);
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, value, measTime, Fw::Time(TimeBase::TB_WORKSTATION_TIME, 50, 0)),
              SbsStatus::OK);
}

void StateBufferStoreTester::lastReadTimeOptionalTest() {
    // A measurement stamped with zero time, as from an unconnected time
    // source, is where a missing previous read could be mistaken for a match
    this->setTestTime(Fw::ZERO_TIME);
    Fw::PolyType first(static_cast<U32>(8));
    ASSERT_EQ(this->invoke_to_putValue(0, ENTRY_A, first, SbsStatus::OK), SbsStatus::OK);
    Fw::PolyType second(static_cast<U32>(9));
    ASSERT_EQ(this->invoke_to_putValue(0, ENTRY_A, second, SbsStatus::OK), SbsStatus::OK);

    Fw::PolyType value;
    Fw::Time measTime;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, value, measTime, Fw::ZERO_TIME), SbsStatus::OK);
    ASSERT_EQ(measTime, Fw::ZERO_TIME);
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, value, measTime, Fw::ZERO_TIME), SbsStatus::OK);

    U8 stored[] = {'z'};
    Fw::Buffer in(stored, sizeof stored);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_B, in, SbsStatus::OK), SbsStatus::OK);
    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_B, out, measTime, Fw::ZERO_TIME, sizeOut), SbsStatus::OK);
}

void StateBufferStoreTester::entryIsolationTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 10);
    this->put(ENTRY_B, Fw::PolyType(static_cast<U32>(2)), 11);

    ASSERT_EQ(static_cast<U32>(this->get(ENTRY_A, SbsStatus::OK)), 1u);
    ASSERT_EQ(static_cast<U32>(this->get(ENTRY_B, SbsStatus::OK)), 2u);

    // An untouched entry is unaffected
    Fw::PolyType value;
    Fw::Time measTime;
    ASSERT_EQ(this->invoke_to_getValue(0, StateBufferStoreCfg::StateEntry::SBS_ENTRY_02, value, measTime, Fw::Time()),
              SbsStatus::NOT_WRITTEN);
}

// ----------------------------------------------------------------------
// Tests: watermarks
// ----------------------------------------------------------------------

void StateBufferStoreTester::watermarkSeedTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(5)), 10);

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);

    // A single value is simultaneously the minimum and the maximum. This is
    // the case comparison alone cannot produce, since PolyType comparisons
    // against an untyped watermark are always false.
    ASSERT_EQ(static_cast<I32>(min.get_value()), 5);
    ASSERT_EQ(static_cast<I32>(max.get_value()), 5);
    ASSERT_EQ(min.get_time().getSeconds(), 10u);
    ASSERT_EQ(max.get_time().getSeconds(), 10u);
}

void StateBufferStoreTester::watermarkTrackingTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(10)), 1);
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(-5)), 2);
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(30)), 3);
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(7)), 4);

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_EQ(static_cast<I32>(min.get_value()), -5);
    ASSERT_EQ(static_cast<I32>(max.get_value()), 30);
    // Watermarks carry the time of the extreme, not the latest put
    ASSERT_EQ(min.get_time().getSeconds(), 2u);
    ASSERT_EQ(max.get_time().getSeconds(), 3u);
}

void StateBufferStoreTester::watermarkClearTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(10)), 1);
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(-5)), 2);

    ASSERT_EQ(this->invoke_to_clearMinMax(0, ENTRY_A), SbsStatus::OK);

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    // While cleared, watermarks read back as never written
    ASSERT_EQ(min.get_validity(), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(max.get_validity(), SbsStatus::NOT_WRITTEN);

    // The next put reseeds both from that single value, discarding the old
    // extremes rather than comparing against them
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(100)), 3);
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_EQ(static_cast<I32>(min.get_value()), 100);
    ASSERT_EQ(static_cast<I32>(max.get_value()), 100);
}

void StateBufferStoreTester::clearAndGetTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(3)), 1);
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(90)), 2);

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_clearAndGetMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    // The reported values are the ones in force before the clear
    ASSERT_EQ(static_cast<I32>(min.get_value()), 3);
    ASSERT_EQ(static_cast<I32>(max.get_value()), 90);

    // and they are cleared afterwards
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_EQ(min.get_validity(), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(max.get_validity(), SbsStatus::NOT_WRITTEN);

    // A second clear-and-get with no put between ends an empty epoch
    ASSERT_EQ(this->invoke_to_clearAndGetMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_EQ(min.get_validity(), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(max.get_validity(), SbsStatus::NOT_WRITTEN);
}

// ----------------------------------------------------------------------
// Tests: history
// ----------------------------------------------------------------------

void StateBufferStoreTester::historyTest() {
    for (FwSizeType i = 0; i < DEPTH; i++) {
        this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(i + 1)), static_cast<U32>(i + 1));
    }

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, DEPTH * RECORD_SIZE);

    // Oldest first
    for (FwSizeType i = 0; i < DEPTH; i++) {
        const SbsMeasurement record = this->readRecord(data, i);
        ASSERT_EQ(static_cast<U32>(record.get_value()), static_cast<U32>(i + 1));
        ASSERT_EQ(record.get_time().getSeconds(), static_cast<U32>(i + 1));
        ASSERT_EQ(record.get_validity(), SbsStatus::OK);
    }
}

void StateBufferStoreTester::historyWrapTest() {
    // Store one and a half rings so the oldest values are overwritten
    const FwSizeType writes = DEPTH + (DEPTH / 2);
    for (FwSizeType i = 0; i < writes; i++) {
        this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(i + 1)), static_cast<U32>(i + 1));
    }

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, DEPTH * RECORD_SIZE);

    // Only the most recent DEPTH values survive, still oldest first
    for (FwSizeType i = 0; i < DEPTH; i++) {
        const SbsMeasurement record = this->readRecord(data, i);
        ASSERT_EQ(static_cast<U32>(record.get_value()), static_cast<U32>(writes - DEPTH + i + 1));
    }
}

void StateBufferStoreTester::historyShortBufferTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);

    // One byte short of the whole history
    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, (DEPTH * RECORD_SIZE) - 1);
    FwSizeType sizeOut = 0;
    // The full-history read is all or nothing
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut), SbsStatus::INVALID_BUFFER_SIZE);
    ASSERT_EQ(sizeOut, 0u);
}

void StateBufferStoreTester::nHistoryTest() {
    for (FwSizeType i = 0; i < DEPTH; i++) {
        this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(i + 1)), static_cast<U32>(i + 1));
    }

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 2, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, 2u * RECORD_SIZE);

    // The latest two, oldest of the pair first
    ASSERT_EQ(static_cast<U32>(this->readRecord(data, 0).get_value()), static_cast<U32>(DEPTH - 1));
    ASSERT_EQ(static_cast<U32>(this->readRecord(data, 1).get_value()), static_cast<U32>(DEPTH));
}

void StateBufferStoreTester::nHistoryZeroTest() {
    for (FwSizeType i = 0; i < DEPTH; i++) {
        this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(i + 1)), static_cast<U32>(i + 1));
    }

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;
    // Zero means the entry's whole depth
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 0, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, DEPTH * RECORD_SIZE);
    for (FwSizeType i = 0; i < DEPTH; i++) {
        ASSERT_EQ(static_cast<U32>(this->readRecord(data, i).get_value()), static_cast<U32>(i + 1));
    }
}

void StateBufferStoreTester::nHistoryTruncateTest() {
    for (FwSizeType i = 0; i < DEPTH; i++) {
        this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(i + 1)), static_cast<U32>(i + 1));
    }

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    // Room for one measurement and a fragment of a second
    Fw::Buffer data(bytes, RECORD_SIZE + (RECORD_SIZE / 2));
    FwSizeType sizeOut = 0;
    // Unlike the full-history read, this one truncates rather than failing,
    // and never reports a partial measurement
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 3, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, RECORD_SIZE);
    // The one that fits is the most recent, not the oldest of the three requested
    ASSERT_EQ(static_cast<U32>(this->readRecord(data, 0).get_value()), static_cast<U32>(DEPTH));
}

void StateBufferStoreTester::nHistoryNoRoomTest() {
    U8 bytes[RECORD_SIZE] = {};
    // Room for a fragment of one measurement, never a whole one
    Fw::Buffer data(bytes, RECORD_SIZE - 1);
    FwSizeType sizeOut = 1;

    // Nothing stored, so nothing is lost: the empty read is reported as such
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 1, data, sizeOut), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(sizeOut, 0u);

    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(7)), 1);
    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 1, data, sizeOut), SbsStatus::INVALID_BUFFER_SIZE);
    ASSERT_EQ(sizeOut, 0u);
}

void StateBufferStoreTester::nHistoryTooDeepTest() {
    // Overfill so the ring has wrapped: values 1..DEPTH+1, oldest held is 2
    for (FwSizeType i = 0; i <= DEPTH; i++) {
        this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(i + 1)), static_cast<U32>(i + 1));
    }

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    const FwSizeType requests[] = {DEPTH + 1, StateBufferStoreCfg::MAX_HISTORY_DEPTH};
    for (const FwSizeType request : requests) {
        FwSizeType sizeOut = 0;
        ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, request, data, sizeOut), SbsStatus::OK);
        ASSERT_EQ(sizeOut, DEPTH * RECORD_SIZE);
        ASSERT_EQ(static_cast<U32>(this->readRecord(data, 0).get_value()), 2u);
        ASSERT_EQ(static_cast<U32>(this->readRecord(data, DEPTH - 1).get_value()), static_cast<U32>(DEPTH + 1));
    }
}

// ----------------------------------------------------------------------
// Tests: data interface
// ----------------------------------------------------------------------

void StateBufferStoreTester::dataRoundTripTest() {
    U8 stored[] = {'s', 't', 'a', 't', 'e'};
    Fw::Buffer in(stored, sizeof stored);
    this->setTestTime(Fw::Time(TimeBase::TB_WORKSTATION_TIME, 20, 0));
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, in, SbsStatus::OK), SbsStatus::OK);

    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_A, out, measTime, Fw::Time(), sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, sizeof stored);
    ASSERT_EQ(::memcmp(readBack, stored, sizeof stored), 0);
    ASSERT_EQ(measTime.getSeconds(), 20u);
}

void StateBufferStoreTester::dataShortBufferTest() {
    U8 stored[] = {'a', 'b', 'c', 'd'};
    Fw::Buffer in(stored, sizeof stored);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, in, SbsStatus::OK), SbsStatus::OK);

    U8 readBack[2] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_A, out, measTime, Fw::Time(), sizeOut), SbsStatus::INVALID_BUFFER_SIZE);
    ASSERT_EQ(sizeOut, 0u);
}

// ----------------------------------------------------------------------
// Tests: coherency
// ----------------------------------------------------------------------

void StateBufferStoreTester::tornReadRecoversTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(77)), 5);

    // Force exactly one torn attempt; the retry sees a stable counter
    this->component.m_utForceTornReads = 1;
    Fw::PolyType value;
    Fw::Time measTime;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, value, measTime, Fw::Time()), SbsStatus::OK);
    ASSERT_EQ(static_cast<U32>(value), 77u);
    // Recovering within the retry limit is not an anomaly, so nothing is logged
    ASSERT_EVENTS_SIZE(0);
}

void StateBufferStoreTester::tornReadExhaustedTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(77)), 5);

    // Tear every attempt the retry loop makes. Sentinels show that nothing
    // is copied out of a read that never proves coherent.
    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    const Fw::PolyType sentinelValue(static_cast<U32>(0xDEAD));
    const Fw::Time sentinelTime(TimeBase::TB_WORKSTATION_TIME, 123, 0);
    Fw::PolyType value = sentinelValue;
    Fw::Time measTime = sentinelTime;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, value, measTime, Fw::Time()), SbsStatus::INCOHERENT);
    ASSERT_TRUE(sameTypeAndValue(value, sentinelValue));
    ASSERT_EQ(measTime, sentinelTime);

    ASSERT_EVENTS_SIZE(1);
    ASSERT_EVENTS_FailedReadCoherentData_SIZE(1);
    ASSERT_EVENTS_FailedReadCoherentData(0, ENTRY_A, StateBufferStore_ReadOperation::GET_VALUE,
                                         StateBufferStoreCfg::MAX_READ_ITERATIONS);
}

// ----------------------------------------------------------------------
// Tests: commands
// ----------------------------------------------------------------------

void StateBufferStoreTester::reportWatermarksCommandTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(-2)), 1);
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(9)), 2);
    this->clearHistory();

    const U32 cmdSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, cmdSeq, ENTRY_A);

    ASSERT_EVENTS_SIZE(1);
    ASSERT_EVENTS_WatermarkReport_SIZE(1);
    ASSERT_EVENTS_WatermarkReport(0, ENTRY_A, -2.0, 1000000, SbsStatus::OK, 9.0, 2000000, SbsStatus::OK, SbsStatus::OK);
    ASSERT_CMD_RESPONSE_SIZE(1);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_REPORT_WATERMARKS, cmdSeq, Fw::CmdResponse::OK);
}

void StateBufferStoreTester::reportWatermarksNotWrittenTest() {
    // Never written: zeros with NOT_WRITTEN validity, and the command succeeds
    U32 cmdSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, cmdSeq, ENTRY_A);
    ASSERT_EVENTS_SIZE(1);
    ASSERT_EVENTS_WatermarkReport(0, ENTRY_A, 0.0, 0, SbsStatus::NOT_WRITTEN, 0.0, 0, SbsStatus::NOT_WRITTEN,
                                  SbsStatus::OK);
    ASSERT_CMD_RESPONSE_SIZE(1);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_REPORT_WATERMARKS, cmdSeq, Fw::CmdResponse::OK);

    // Written, then cleared with no put since: the same report
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(4)), 1);
    ASSERT_EQ(this->invoke_to_clearMinMax(0, ENTRY_A), SbsStatus::OK);
    this->clearHistory();
    cmdSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, cmdSeq, ENTRY_A);
    ASSERT_EVENTS_SIZE(1);
    ASSERT_EVENTS_WatermarkReport(0, ENTRY_A, 0.0, 0, SbsStatus::NOT_WRITTEN, 0.0, 0, SbsStatus::NOT_WRITTEN,
                                  SbsStatus::OK);
    ASSERT_CMD_RESPONSE_SIZE(1);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_REPORT_WATERMARKS, cmdSeq, Fw::CmdResponse::OK);
}

void StateBufferStoreTester::clearWatermarksCommandTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(4)), 1);
    this->clearHistory();

    const U32 cmdSeq = this->m_cmdSeq++;
    this->sendCmd_CLEAR_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, cmdSeq, ENTRY_A);

    ASSERT_EVENTS_SIZE(1);
    ASSERT_EVENTS_WatermarksCleared_SIZE(1);
    ASSERT_EVENTS_WatermarksCleared(0, ENTRY_A);
    ASSERT_CMD_RESPONSE_SIZE(1);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_CLEAR_WATERMARKS, cmdSeq, Fw::CmdResponse::OK);

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_EQ(min.get_validity(), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(max.get_validity(), SbsStatus::NOT_WRITTEN);
}

void StateBufferStoreTester::invalidEntryCommandTest() {
    // NUM_ENTRIES is the table size, not an addressable entry. Both commands
    // reject it rather than indexing past the entry table.
    const U32 reportSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, reportSeq,
                                    StateBufferStoreCfg::StateEntry::NUM_ENTRIES);
    ASSERT_EVENTS_SIZE(1);
    ASSERT_EVENTS_ReportWatermarksRejected_SIZE(1);
    ASSERT_EVENTS_ReportWatermarksRejected(0, StateBufferStoreCfg::StateEntry::NUM_ENTRIES,
                                           StateBufferStore_WatermarkRejection::NOT_AN_ENTRY);
    ASSERT_CMD_RESPONSE_SIZE(1);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_REPORT_WATERMARKS, reportSeq, Fw::CmdResponse::VALIDATION_ERROR);

    const U32 clearSeq = this->m_cmdSeq++;
    this->sendCmd_CLEAR_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, clearSeq,
                                   StateBufferStoreCfg::StateEntry::NUM_ENTRIES);
    ASSERT_EVENTS_SIZE(2);
    ASSERT_EVENTS_ClearWatermarksRejected_SIZE(1);
    ASSERT_EVENTS_ClearWatermarksRejected(0, StateBufferStoreCfg::StateEntry::NUM_ENTRIES,
                                          StateBufferStore_WatermarkRejection::NOT_AN_ENTRY);
    ASSERT_CMD_RESPONSE_SIZE(2);
    ASSERT_CMD_RESPONSE(1, StateBufferStore::OPCODE_CLEAR_WATERMARKS, clearSeq, Fw::CmdResponse::VALIDATION_ERROR);
}

void StateBufferStoreTester::watermarkReportAllTypesTest() {
    // Watermark events carry F64, so every stored type must widen correctly.
    // Clearing before each put keeps the watermarks from being compared
    // across types; telemetry-mapped entries refuse puts, so one entry is reused.
    struct Case {
        Fw::PolyType value;
        F64 expected;
    };
    const Case cases[] = {
        {Fw::PolyType(static_cast<U8>(200)), 200.0},
        {Fw::PolyType(static_cast<I8>(-100)), -100.0},
        {Fw::PolyType(static_cast<U16>(60000)), 60000.0},
        {Fw::PolyType(static_cast<I16>(-30000)), -30000.0},
        {Fw::PolyType(static_cast<U32>(4000000000u)), 4000000000.0},
        {Fw::PolyType(static_cast<I32>(-2000000000)), -2000000000.0},
        {Fw::PolyType(static_cast<U64>(1000000)), 1000000.0},
        {Fw::PolyType(static_cast<I64>(-1000000)), -1000000.0},
        {Fw::PolyType(static_cast<F32>(0.5f)), 0.5},
        {Fw::PolyType(static_cast<F64>(1.25)), 1.25},
    };

    for (const Case& testCase : cases) {
        ASSERT_EQ(this->invoke_to_clearMinMax(0, ENTRY_A), SbsStatus::OK);
        this->put(ENTRY_A, testCase.value, 1);
        this->clearHistory();
        const U32 cmdSeq = this->m_cmdSeq++;
        this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, cmdSeq, ENTRY_A);
        ASSERT_EVENTS_WatermarkReport_SIZE(1);
        // A single stored value is both watermarks
        ASSERT_EVENTS_WatermarkReport(0, ENTRY_A, testCase.expected, 1000000, SbsStatus::OK, testCase.expected, 1000000,
                                      SbsStatus::OK, SbsStatus::OK);
    }

    // bool reports as 1 or 0. Clear first: the entries above already hold a
    // numeric watermark, and a comparison across differing PolyType types
    // never updates one.
    ASSERT_EQ(this->invoke_to_clearMinMax(0, ENTRY_A), SbsStatus::OK);
    this->put(ENTRY_A, Fw::PolyType(true), 2);
    this->clearHistory();
    const U32 boolSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, boolSeq, ENTRY_A);
    ASSERT_EVENTS_WatermarkReport_SIZE(1);
    ASSERT_EVENTS_WatermarkReport(0, ENTRY_A, 1.0, 2000000, SbsStatus::OK, 1.0, 2000000, SbsStatus::OK, SbsStatus::OK);

    // A cleared watermark holds no type at all, so there is no value to widen
    ASSERT_EQ(this->invoke_to_clearMinMax(0, ENTRY_B), SbsStatus::OK);
    this->clearHistory();
    const U32 clearedSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, clearedSeq, ENTRY_B);
    ASSERT_EVENTS_WatermarkReport_SIZE(1);
    ASSERT_EVENTS_WatermarkReport(0, ENTRY_B, 0.0, 0, SbsStatus::NOT_WRITTEN, 0.0, 0, SbsStatus::NOT_WRITTEN,
                                  SbsStatus::OK);
}

void StateBufferStoreTester::oversizedPutDataTest() {
    U8 oversized[StateBufferStoreCfg::MAX_DATA_SIZE + 1] = {};
    Fw::Buffer in(oversized, sizeof oversized);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, in, SbsStatus::OK), SbsStatus::INVALID_BUFFER_SIZE);

    // The refused put stored nothing and left the entry unclaimed, so a value put still succeeds
    (void)this->get(ENTRY_A, SbsStatus::NOT_WRITTEN);
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(5)), 1);

    // A value entry is refused as such, whatever the payload's size
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, in, SbsStatus::OK), SbsStatus::WRONG_KIND);
}

void StateBufferStoreTester::emptyBufferTest() {
    // An exhausted BufferManager hands out Fw::Buffer(): null data, size zero
    Fw::Buffer empty;
    FwSizeType sizeOut = 1;

    // Nothing to read yet, so nothing is lost
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 1, empty, sizeOut), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(sizeOut, 0u);

    // A null put stores nothing and claims nothing, so the entry still takes a value
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, empty, SbsStatus::OK), SbsStatus::INVALID_BUFFER_SIZE);
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(3)), 1);

    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, empty, sizeOut), SbsStatus::INVALID_BUFFER_SIZE);
    ASSERT_EQ(sizeOut, 0u);
    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 1, empty, sizeOut), SbsStatus::INVALID_BUFFER_SIZE);
    ASSERT_EQ(sizeOut, 0u);

    U8 stored[] = {'e'};
    Fw::Buffer in(stored, sizeof stored);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_B, in, SbsStatus::OK), SbsStatus::OK);
    Fw::Time measTime;
    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_B, empty, measTime, Fw::Time(), sizeOut),
              SbsStatus::INVALID_BUFFER_SIZE);
    ASSERT_EQ(sizeOut, 0u);

    // A null pointer with a nonzero size passes every size check, so the
    // reads must refuse it themselves rather than assert
    Fw::Buffer nullSized(nullptr, StateBufferStoreCfg::MAX_DATA_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH);
    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_B, nullSized, measTime, Fw::Time(), sizeOut),
              SbsStatus::INVALID_BUFFER_SIZE);
    ASSERT_EQ(sizeOut, 0u);
    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, nullSized, sizeOut), SbsStatus::INVALID_BUFFER_SIZE);
    ASSERT_EQ(sizeOut, 0u);
    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 1, nullSized, sizeOut), SbsStatus::INVALID_BUFFER_SIZE);
    ASSERT_EQ(sizeOut, 0u);
}

void StateBufferStoreTester::dataStatusTest() {
    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    FwSizeType sizeOut = 0;

    // Never written
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_A, out, measTime, Fw::Time(), sizeOut), SbsStatus::NOT_WRITTEN);

    U8 stored[] = {'x', 'y'};
    Fw::Buffer in(stored, sizeof stored);
    this->setTestTime(Fw::Time(TimeBase::TB_WORKSTATION_TIME, 30, 0));
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, in, SbsStatus::OK), SbsStatus::OK);

    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_A, out, measTime, Fw::Time(), sizeOut), SbsStatus::OK);
    // Reading again with the measurement's own time reports staleness
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_A, out, measTime, measTime, sizeOut), SbsStatus::NOT_FRESH);

    // A read torn on every attempt reports INCOHERENT
    this->clearHistory();
    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_A, out, measTime, Fw::Time(), sizeOut), SbsStatus::INCOHERENT);
    ASSERT_EQ(sizeOut, 0u);
    ASSERT_EVENTS_FailedReadCoherentData_SIZE(1);
    ASSERT_EVENTS_FailedReadCoherentData(0, ENTRY_A, StateBufferStore_ReadOperation::GET_DATA,
                                         StateBufferStoreCfg::MAX_READ_ITERATIONS);
}

void StateBufferStoreTester::historyTornReadTest() {
    for (FwSizeType i = 0; i < DEPTH; i++) {
        this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(i + 1)), static_cast<U32>(i + 1));
    }
    this->clearHistory();

    // A sentinel shows whether a torn read wrote into the caller's buffer
    constexpr U8 SENTINEL = 0xA5;
    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    (void)::memset(bytes, SENTINEL, sizeof bytes);
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;

    // The whole dump is one coherent read, so tearing it reports the dump as
    // a whole rather than passing a torn measurement off as OK
    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut), SbsStatus::INCOHERENT);
    ASSERT_EQ(sizeOut, 0u);

    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 2, data, sizeOut), SbsStatus::INCOHERENT);
    ASSERT_EQ(sizeOut, 0u);

    for (FwSizeType i = 0; i < sizeof bytes; i++) {
        ASSERT_EQ(bytes[i], SENTINEL) << "byte " << i;
    }

    ASSERT_EVENTS_FailedReadCoherentData_SIZE(2);
    ASSERT_EVENTS_FailedReadCoherentData(0, ENTRY_A, StateBufferStore_ReadOperation::GET_HISTORY,
                                         StateBufferStoreCfg::MAX_READ_ITERATIONS);
    ASSERT_EVENTS_FailedReadCoherentData(1, ENTRY_A, StateBufferStore_ReadOperation::GET_N_HISTORY,
                                         StateBufferStoreCfg::MAX_READ_ITERATIONS);
}

void StateBufferStoreTester::historyPartialTest() {
    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;

    // Never written: nothing to report
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(sizeOut, 0u);

    // Fewer measurements than the depth: unwritten slots are not reported
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(2)), 2);
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, 2u * RECORD_SIZE);
    ASSERT_EQ(static_cast<U32>(this->readRecord(data, 0).get_value()), 1u);
    ASSERT_EQ(static_cast<U32>(this->readRecord(data, 1).get_value()), 2u);

    // An N read asking for more than was stored gets only what was stored
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 3, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, 2u * RECORD_SIZE);
}

void StateBufferStoreTester::historyRecordFormatTest() {
    // A distinctive validity catches a record whose fields are out of order
    this->put(ENTRY_A, Fw::PolyType(static_cast<U8>(0x5A)), 7, SbsStatus::INVALID);

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    ::memset(bytes, 0xAA, sizeof bytes);
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 1, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, RECORD_SIZE);

    const SbsMeasurement record = this->readRecord(data, 0);
    ASSERT_EQ(record.get_time().getSeconds(), 7u);
    ASSERT_EQ(record.get_validity(), SbsStatus::INVALID);
    ASSERT_EQ(static_cast<U8>(record.get_value()), 0x5A);

    // A U8 serializes shorter than the widest PolyType; the rest of the
    // record is zeroed, not left holding the caller's bytes
    const FwSizeType used =
        Fw::Time::SERIALIZED_SIZE + SbsStatus::SERIALIZED_SIZE + sizeof(FwEnumStoreType) + sizeof(U8);
    for (FwSizeType i = used; i < RECORD_SIZE; i++) {
        ASSERT_EQ(bytes[i], 0u) << "byte " << i;
    }
}

// ----------------------------------------------------------------------
// Tests: entry kinds
// ----------------------------------------------------------------------

void StateBufferStoreTester::valueEntryRefusesDataTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(11)), 1);

    U8 stored[] = {'n', 'o'};
    Fw::Buffer in(stored, sizeof stored);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, in, SbsStatus::OK), SbsStatus::WRONG_KIND);

    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    FwSizeType sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_A, out, measTime, Fw::Time(), sizeOut), SbsStatus::WRONG_KIND);
    ASSERT_EQ(sizeOut, 0u);

    // The refused store left the entry's value untouched
    ASSERT_EQ(static_cast<U32>(this->get(ENTRY_A, SbsStatus::OK)), 11u);
}

void StateBufferStoreTester::dataEntryRefusesValueTest() {
    U8 stored[] = {'d', 'a', 't', 'a'};
    Fw::Buffer in(stored, sizeof stored);
    this->setTestTime(Fw::Time(TimeBase::TB_WORKSTATION_TIME, 40, 0));
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_B, in, SbsStatus::OK), SbsStatus::OK);

    Fw::PolyType val(static_cast<U32>(3));
    ASSERT_EQ(this->invoke_to_putValue(0, ENTRY_B, val, SbsStatus::OK), SbsStatus::WRONG_KIND);
    (void)this->get(ENTRY_B, SbsStatus::WRONG_KIND);

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_B, min, max), SbsStatus::WRONG_KIND);
    ASSERT_EQ(this->invoke_to_clearMinMax(0, ENTRY_B), SbsStatus::WRONG_KIND);
    ASSERT_EQ(this->invoke_to_clearAndGetMinMax(0, ENTRY_B, min, max), SbsStatus::WRONG_KIND);

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_B, data, sizeOut), SbsStatus::WRONG_KIND);
    ASSERT_EQ(sizeOut, 0u);
    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_B, 0, data, sizeOut), SbsStatus::WRONG_KIND);
    ASSERT_EQ(sizeOut, 0u);

    const U32 reportSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, reportSeq, ENTRY_B);
    const U32 clearSeq = this->m_cmdSeq++;
    this->sendCmd_CLEAR_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, clearSeq, ENTRY_B);
    ASSERT_EVENTS_SIZE(2);
    ASSERT_EVENTS_ReportWatermarksRejected_SIZE(1);
    ASSERT_EVENTS_ReportWatermarksRejected(0, ENTRY_B, StateBufferStore_WatermarkRejection::DATA_ENTRY);
    ASSERT_EVENTS_ClearWatermarksRejected_SIZE(1);
    ASSERT_EVENTS_ClearWatermarksRejected(0, ENTRY_B, StateBufferStore_WatermarkRejection::DATA_ENTRY);
    ASSERT_CMD_RESPONSE_SIZE(2);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_REPORT_WATERMARKS, reportSeq, Fw::CmdResponse::EXECUTION_ERROR);
    ASSERT_CMD_RESPONSE(1, StateBufferStore::OPCODE_CLEAR_WATERMARKS, clearSeq, Fw::CmdResponse::EXECUTION_ERROR);

    // The refused store left the entry's data untouched
    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_B, out, measTime, Fw::Time(), sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, sizeof stored);
    ASSERT_EQ(::memcmp(readBack, stored, sizeof stored), 0);
}

// ----------------------------------------------------------------------
// Tests: watermark coherency
// ----------------------------------------------------------------------

void StateBufferStoreTester::clearAndGetRacingPutTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(3)), 1);
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(90)), 2);

    // A put lands after the clear but before the watermarks are read, so it
    // consumes the clear and reseeds them
    this->component.m_utBeforeRead = &StateBufferStoreTester::putDuringRead;
    this->component.m_utBeforeReadContext = this;
    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_clearAndGetMinMax(0, ENTRY_A, min, max), SbsStatus::OK);

    // The epoch the clear ended is still what gets reported
    ASSERT_EQ(static_cast<I32>(min.get_value()), 3);
    ASSERT_EQ(static_cast<I32>(max.get_value()), 90);

    // and the racing put starts the new one
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_EQ(static_cast<I32>(min.get_value()), RACING_PUT_VALUE);
    ASSERT_EQ(static_cast<I32>(max.get_value()), RACING_PUT_VALUE);
}

void StateBufferStoreTester::watermarkTornReadTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(1)), 1);
    this->clearHistory();

    SbsMeasurement min;
    SbsMeasurement max;
    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::INCOHERENT);

    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    ASSERT_EQ(this->invoke_to_clearAndGetMinMax(0, ENTRY_A, min, max), SbsStatus::INCOHERENT);

    // The torn clear-and-get withdrew its clear, so the watermarks it failed
    // to report are still held
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_EQ(static_cast<I32>(min.get_value()), 1);
    ASSERT_EQ(static_cast<I32>(max.get_value()), 1);

    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    const U32 cmdSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, cmdSeq, ENTRY_A);
    ASSERT_CMD_RESPONSE_SIZE(1);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_REPORT_WATERMARKS, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);

    ASSERT_EVENTS_SIZE(3);
    ASSERT_EVENTS_FailedReadCoherentData_SIZE(3);
    ASSERT_EVENTS_FailedReadCoherentData(0, ENTRY_A, StateBufferStore_ReadOperation::GET_MIN_MAX,
                                         StateBufferStoreCfg::MAX_READ_ITERATIONS);
    ASSERT_EVENTS_FailedReadCoherentData(1, ENTRY_A, StateBufferStore_ReadOperation::CLEAR_AND_GET_MIN_MAX,
                                         StateBufferStoreCfg::MAX_READ_ITERATIONS);
    ASSERT_EVENTS_FailedReadCoherentData(2, ENTRY_A, StateBufferStore_ReadOperation::REPORT_WATERMARKS,
                                         StateBufferStoreCfg::MAX_READ_ITERATIONS);
}

void StateBufferStoreTester::boolWatermarkTest() {
    this->put(ENTRY_A, Fw::PolyType(false), 1);
    this->put(ENTRY_A, Fw::PolyType(true), 2);
    this->put(ENTRY_A, Fw::PolyType(false), 3);

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_FALSE(static_cast<bool>(min.get_value()));
    ASSERT_EQ(min.get_time().getSeconds(), 1u);
    ASSERT_TRUE(static_cast<bool>(max.get_value()));
    ASSERT_EQ(max.get_time().getSeconds(), 2u);
}

void StateBufferStoreTester::nanWatermarkTest() {
    // A NaN first value must not seed the watermarks: the finite values after it do
    const F32 nan = std::numeric_limits<F32>::quiet_NaN();
    this->put(ENTRY_A, Fw::PolyType(nan), 1);
    this->put(ENTRY_A, Fw::PolyType(static_cast<F32>(3.0f)), 2);
    this->put(ENTRY_A, Fw::PolyType(static_cast<F32>(1.0f)), 3);
    // Nor does a later NaN disturb them
    this->put(ENTRY_A, Fw::PolyType(nan), 4);

    // It is still the latest value
    ASSERT_TRUE(std::isnan(static_cast<F32>(this->get(ENTRY_A, SbsStatus::OK))));

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    Fw::PolyType minValue = min.get_value();
    Fw::PolyType maxValue = max.get_value();
    ASSERT_EQ(static_cast<F32>(minValue), 1.0f);
    ASSERT_EQ(min.get_time().getSeconds(), 3u);
    ASSERT_EQ(static_cast<F32>(maxValue), 3.0f);
    ASSERT_EQ(max.get_time().getSeconds(), 2u);
}

void StateBufferStoreTester::dataEntryKindCheckedFirstTest() {
    U8 stored[] = {'d'};
    Fw::Buffer in(stored, sizeof stored);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_B, in, SbsStatus::OK), SbsStatus::OK);

    // A data entry is refused as such, whatever else is wrong with the request
    U8 tooSmall[1] = {};
    Fw::Buffer shortBuffer(tooSmall, sizeof tooSmall);
    FwSizeType sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_B, shortBuffer, sizeOut), SbsStatus::WRONG_KIND);
    ASSERT_EQ(sizeOut, 0u);
}

void StateBufferStoreTester::wrongKindLatestReadNotTornTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(5)), 1);
    U8 stored[] = {'d'};
    Fw::Buffer in(stored, sizeof stored);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_B, in, SbsStatus::OK), SbsStatus::OK);
    this->clearHistory();

    // Every attempt would be torn, as if racing the entry's writer; the kind
    // check comes first, so the wrong kind is reported and nothing is read
    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    FwSizeType sizeOut = 1;
    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_A, out, measTime, Fw::Time(), sizeOut), SbsStatus::WRONG_KIND);
    ASSERT_EQ(sizeOut, 0u);
    ASSERT_EQ(this->component.m_utForceTornReads, StateBufferStoreCfg::MAX_READ_ITERATIONS);

    Fw::PolyType val;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_B, val, measTime, Fw::Time()), SbsStatus::WRONG_KIND);
    ASSERT_EQ(this->component.m_utForceTornReads, StateBufferStoreCfg::MAX_READ_ITERATIONS);
    this->component.m_utForceTornReads = 0;

    ASSERT_EVENTS_SIZE(0);
}

void StateBufferStoreTester::unwrittenReadsSkipReadTest() {
    // ENTRY_A is never stored to. Every attempt would be torn, as if racing a
    // writer; an unclaimed entry is reported from its kind, without a read
    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    const Fw::Time stale(TimeBase::TB_WORKSTATION_TIME, 5, 0);

    // Outputs are reset as a read of the empty slot would leave them
    Fw::PolyType val(static_cast<U32>(7));
    Fw::Time measTime = stale;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, val, measTime, Fw::Time()), SbsStatus::NOT_WRITTEN);
    ASSERT_FALSE(val.isU32());
    ASSERT_EQ(measTime, Fw::ZERO_TIME);

    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    FwSizeType sizeOut = 1;
    measTime = stale;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_A, out, measTime, Fw::Time(), sizeOut), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(sizeOut, 0u);
    ASSERT_EQ(measTime, Fw::ZERO_TIME);

    // Watermarks report zeroed with NOT_WRITTEN validity, cleared or not
    SbsMeasurement min(stale, SbsStatus::OK, Fw::PolyType(static_cast<U32>(1)));
    SbsMeasurement max(stale, SbsStatus::OK, Fw::PolyType(static_cast<U32>(1)));
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(min.get_validity(), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(min.get_time(), Fw::ZERO_TIME);
    ASSERT_EQ(max.get_validity(), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(max.get_time(), Fw::ZERO_TIME);
    min.set(stale, SbsStatus::OK, Fw::PolyType(static_cast<U32>(1)));
    max.set(stale, SbsStatus::OK, Fw::PolyType(static_cast<U32>(1)));
    ASSERT_EQ(this->invoke_to_clearAndGetMinMax(0, ENTRY_A, min, max), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(min.get_validity(), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(max.get_validity(), SbsStatus::NOT_WRITTEN);

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(sizeOut, 0u);
    // The full-history read still judges its buffer first
    Fw::Buffer oneRecord(bytes, RECORD_SIZE);
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, oneRecord, sizeOut), SbsStatus::INVALID_BUFFER_SIZE);
    sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 1, data, sizeOut), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(sizeOut, 0u);
    Fw::Buffer noRecord(bytes, RECORD_SIZE - 1);
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 1, noRecord, sizeOut), SbsStatus::NOT_WRITTEN);

    // No read was attempted
    ASSERT_EQ(this->component.m_utForceTornReads, StateBufferStoreCfg::MAX_READ_ITERATIONS);
    this->component.m_utForceTornReads = 0;
    ASSERT_EVENTS_SIZE(0);

    // Still unclaimed, so either kind may yet claim it
    U8 stored[] = {'d'};
    Fw::Buffer in(stored, sizeof stored);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, in, SbsStatus::OK), SbsStatus::OK);
}

// ----------------------------------------------------------------------
// Tests: configuration
// ----------------------------------------------------------------------

void StateBufferStoreTester::perEntryDepthTest() {
    const StateBufferStoreCfg::HistoryDepths depths;
    // The sample configuration sets these entries to the depth bounds; the
    // test is only meaningful while the bounds differ. ENTRY_A keeps the
    // default depth, which may equal either bound.
    ASSERT_EQ(depths[ENTRY_MIN_DEPTH], static_cast<FwSizeType>(StateBufferStoreCfg::MIN_HISTORY_DEPTH));
    ASSERT_EQ(depths[ENTRY_MAX_DEPTH], static_cast<FwSizeType>(StateBufferStoreCfg::MAX_HISTORY_DEPTH));
    ASSERT_NE(depths[ENTRY_MIN_DEPTH], depths[ENTRY_MAX_DEPTH]);

    const StateBufferStoreCfg::StateEntry::T entries[] = {ENTRY_MIN_DEPTH, ENTRY_A, ENTRY_MAX_DEPTH};
    for (const StateBufferStoreCfg::StateEntry::T entry : entries) {
        const FwSizeType depth = depths[entry];
        // One more than the depth, so each ring wraps exactly once
        for (FwSizeType i = 0; i <= depth; i++) {
            this->put(entry, Fw::PolyType(static_cast<U32>(i + 1)), static_cast<U32>(i + 1));
        }

        U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
        FwSizeType sizeOut = 0;
        Fw::Buffer tooSmall(bytes, (depth * RECORD_SIZE) - 1);
        ASSERT_EQ(this->invoke_to_getHistory(0, entry, tooSmall, sizeOut), SbsStatus::INVALID_BUFFER_SIZE);

        // A buffer of exactly this entry's depth holds its whole history,
        // which has dropped only the first value
        Fw::Buffer exact(bytes, depth * RECORD_SIZE);
        ASSERT_EQ(this->invoke_to_getHistory(0, entry, exact, sizeOut), SbsStatus::OK);
        ASSERT_EQ(sizeOut, depth * RECORD_SIZE);
        ASSERT_EQ(static_cast<U32>(this->readRecord(exact, 0).get_value()), 2u);
        ASSERT_EQ(static_cast<U32>(this->readRecord(exact, depth - 1).get_value()), static_cast<U32>(depth + 1));
    }
}

void StateBufferStoreTester::watermarkTypeMismatchTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(5)), 1);
    // Larger and differently typed: PolyType cannot compare across types, so
    // neither watermark moves
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(100)), 2);

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_TRUE(min.get_value().isI32());
    ASSERT_TRUE(max.get_value().isI32());
    ASSERT_EQ(static_cast<I32>(min.get_value()), 5);
    ASSERT_EQ(static_cast<I32>(max.get_value()), 5);

    // The value itself was still stored
    ASSERT_EQ(static_cast<U32>(this->get(ENTRY_A, SbsStatus::OK)), 100u);

    // Clearing is how an entry's watermarks adopt a new type
    ASSERT_EQ(this->invoke_to_clearMinMax(0, ENTRY_A), SbsStatus::OK);
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(7)), 3);
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_A, min, max), SbsStatus::OK);
    ASSERT_EQ(static_cast<U32>(min.get_value()), 7u);
    ASSERT_EQ(static_cast<U32>(max.get_value()), 7u);
}

// ----------------------------------------------------------------------
// Tests: telemetry
// ----------------------------------------------------------------------

void StateBufferStoreTester::tlmStoreTest() {
    // The component's own time differs from the time tags, to show that the
    // tags are what get stored
    this->setTestTime(Fw::Time(TimeBase::TB_WORKSTATION_TIME, 5, 0));

    Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);
    Fw::TlmBuffer u32Value;
    ASSERT_EQ(u32Value.serializeFrom(static_cast<U32>(1234)), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_U32, tag, u32Value);

    Fw::PolyType value;
    Fw::Time measTime;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_TLM_U32, value, measTime, Fw::Time()), SbsStatus::OK);
    ASSERT_TRUE(value.isU32());
    ASSERT_EQ(static_cast<U32>(value), 1234u);
    ASSERT_EQ(measTime.getSeconds(), 70u);

    // A second sample feeds the watermarks and history like any put
    Fw::Time laterTag(TimeBase::TB_WORKSTATION_TIME, 71, 0);
    Fw::TlmBuffer lowerValue;
    ASSERT_EQ(lowerValue.serializeFrom(static_cast<U32>(10)), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_U32, laterTag, lowerValue);

    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_TLM_U32, min, max), SbsStatus::OK);
    ASSERT_EQ(static_cast<U32>(min.get_value()), 10u);
    ASSERT_EQ(min.get_time().getSeconds(), 71u);
    ASSERT_EQ(static_cast<U32>(max.get_value()), 1234u);
    ASSERT_EQ(max.get_time().getSeconds(), 70u);

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_TLM_U32, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, 2u * RECORD_SIZE);

    // Each mapping decodes as its own type
    Fw::TlmBuffer f32Value;
    ASSERT_EQ(f32Value.serializeFrom(static_cast<F32>(2.5f)), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_F32, tag, f32Value);
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_TLM_F32, value, measTime, Fw::Time()), SbsStatus::OK);
    ASSERT_TRUE(value.isF32());
    ASSERT_EQ(static_cast<F32>(value), 2.5f);

    Fw::TlmBuffer boolValue;
    ASSERT_EQ(boolValue.serializeFrom(true), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_BOOL, tag, boolValue);
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_TLM_BOOL, value, measTime, Fw::Time()), SbsStatus::OK);
    ASSERT_TRUE(value.isBool());
    ASSERT_TRUE(static_cast<bool>(value));

    ASSERT_EVENTS_SIZE(0);
}

void StateBufferStoreTester::tlmUnmappedTest() {
    Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);
    Fw::TlmBuffer u32Value;
    ASSERT_EQ(u32Value.serializeFrom(static_cast<U32>(1)), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_UNMAPPED, tag, u32Value);

    // Nothing stored anywhere, and not an anomaly
    ASSERT_EVENTS_SIZE(0);
    (void)this->get(ENTRY_TLM_U32, SbsStatus::NOT_WRITTEN);
    (void)this->get(ENTRY_TLM_F32, SbsStatus::NOT_WRITTEN);
    (void)this->get(ENTRY_TLM_BOOL, SbsStatus::NOT_WRITTEN);

    // A component configured without a mapping table stores no telemetry
    Fw::MallocAllocator allocator;
    StateBufferStore unmapped("unmapped");
    unmapped.init(StateBufferStoreTester::TEST_INSTANCE_ID);
    unmapped.configure(TEST_MEM_ID, allocator);
    unmapped.get_tlmIn_InputPort(0)->invoke(TLM_CHAN_U32, tag, u32Value);
    Fw::PolyType value;
    Fw::Time measTime;
    ASSERT_EQ(unmapped.get_getValue_InputPort(0)->invoke(ENTRY_TLM_U32, value, measTime, Fw::Time()),
              SbsStatus::NOT_WRITTEN);
}

void StateBufferStoreTester::tlmAllTypesTest() {
    checkTlmType(SbsTlmType::TYPE_U8, static_cast<U8>(200));
    checkTlmType(SbsTlmType::TYPE_I8, static_cast<I8>(-100));
    checkTlmType(SbsTlmType::TYPE_U16, static_cast<U16>(60000));
    checkTlmType(SbsTlmType::TYPE_I16, static_cast<I16>(-30000));
    checkTlmType(SbsTlmType::TYPE_U32, static_cast<U32>(4000000000u));
    checkTlmType(SbsTlmType::TYPE_I32, static_cast<I32>(-2000000000));
    checkTlmType(SbsTlmType::TYPE_U64, static_cast<U64>(1) << 40);
    checkTlmType(SbsTlmType::TYPE_I64, -(static_cast<I64>(1) << 40));
    checkTlmType(SbsTlmType::TYPE_F32, static_cast<F32>(-0.75f));
    checkTlmType(SbsTlmType::TYPE_F64, static_cast<F64>(1.0e100));
    checkTlmType(SbsTlmType::TYPE_BOOL, false);
}

void StateBufferStoreTester::tlmDecodeFailedTest() {
    Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);

    // Too short for the mapped U32
    Fw::TlmBuffer narrow;
    ASSERT_EQ(narrow.serializeFrom(static_cast<U16>(1)), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_U32, tag, narrow);

    // Bytes left over after the mapped U32
    Fw::TlmBuffer wide;
    ASSERT_EQ(wide.serializeFrom(static_cast<U32>(1)), Fw::FW_SERIALIZE_OK);
    ASSERT_EQ(wide.serializeFrom(static_cast<U8>(2)), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_U32, tag, wide);

    ASSERT_EVENTS_SIZE(2);
    ASSERT_EVENTS_TlmDecodeFailed_SIZE(2);
    ASSERT_EVENTS_TlmDecodeFailed(0, TLM_CHAN_U32, ENTRY_TLM_U32, SbsTlmType::TYPE_U32, sizeof(U16));
    ASSERT_EVENTS_TlmDecodeFailed(1, TLM_CHAN_U32, ENTRY_TLM_U32, SbsTlmType::TYPE_U32, sizeof(U32) + sizeof(U8));
    (void)this->get(ENTRY_TLM_U32, SbsStatus::NOT_WRITTEN);
}

void StateBufferStoreTester::throttleResetTest() {
    constexpr FwIndexType READ_THROTTLE = StateBufferStoreComponentBase::EVENTID_FAILEDREADCOHERENTDATA_THROTTLE;
    constexpr FwIndexType DECODE_THROTTLE = StateBufferStoreComponentBase::EVENTID_TLMDECODEFAILED_THROTTLE;
    constexpr FwIndexType TOO_LARGE_THROTTLE = StateBufferStoreComponentBase::EVENTID_TLMDATATOOLARGE_THROTTLE;
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);
    Fw::TlmBuffer narrow;
    ASSERT_EQ(narrow.serializeFrom(static_cast<U16>(1)), Fw::FW_SERIALIZE_OK);
    // Only built when it fits a telemetry buffer; otherwise TlmDataTooLarge is unreachable
    const bool canOversize = StateBufferStoreCfg::MAX_DATA_SIZE < FW_TLM_BUFFER_MAX_SIZE;
    Fw::TlmBuffer oversized;
    for (FwSizeType i = 0; canOversize and (i <= StateBufferStoreCfg::MAX_DATA_SIZE); i++) {
        ASSERT_EQ(oversized.serializeFrom(static_cast<U8>(0)), Fw::FW_SERIALIZE_OK);
    }
    Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);
    this->clearHistory();

    // One past each throttle: the extra occurrence is suppressed, not stored
    Fw::PolyType val;
    Fw::Time measTime;
    for (FwIndexType i = 0; i <= READ_THROTTLE; i++) {
        this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
        ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, val, measTime, Fw::Time()), SbsStatus::INCOHERENT);
    }
    for (FwIndexType i = 0; i <= DECODE_THROTTLE; i++) {
        this->invoke_to_tlmIn(0, TLM_CHAN_U32, tag, narrow);
    }
    for (FwIndexType i = 0; canOversize and (i <= TOO_LARGE_THROTTLE); i++) {
        this->invoke_to_tlmIn(0, TLM_CHAN_STRUCT, tag, oversized);
    }
    ASSERT_EVENTS_FailedReadCoherentData_SIZE(READ_THROTTLE);
    ASSERT_EVENTS_TlmDecodeFailed_SIZE(DECODE_THROTTLE);
    ASSERT_EVENTS_TlmDataTooLarge_SIZE(canOversize ? TOO_LARGE_THROTTLE : 0);
    (void)this->get(ENTRY_TLM_U32, SbsStatus::NOT_WRITTEN);

    this->clearHistory();
    const U32 cmdSeq = this->m_cmdSeq++;
    this->sendCmd_RESET_THROTTLES(StateBufferStoreTester::TEST_INSTANCE_ID, cmdSeq);
    ASSERT_CMD_RESPONSE_SIZE(1);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_RESET_THROTTLES, cmdSeq, Fw::CmdResponse::OK);
    ASSERT_EVENTS_SIZE(1);
    ASSERT_EVENTS_ThrottlesReset_SIZE(1);

    // Both warnings resume after the reset
    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, val, measTime, Fw::Time()), SbsStatus::INCOHERENT);
    this->invoke_to_tlmIn(0, TLM_CHAN_U32, tag, narrow);
    if (canOversize) {
        this->invoke_to_tlmIn(0, TLM_CHAN_STRUCT, tag, oversized);
    }
    ASSERT_EVENTS_FailedReadCoherentData_SIZE(1);
    ASSERT_EVENTS_TlmDecodeFailed_SIZE(1);
    ASSERT_EVENTS_TlmDataTooLarge_SIZE(canOversize ? 1 : 0);
}

void StateBufferStoreTester::tlmEntryRefusesDataTest() {
    U8 stored[] = {'t', 'l', 'm'};
    Fw::Buffer in(stored, sizeof stored);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_TLM_U32, in, SbsStatus::OK), SbsStatus::WRONG_KIND);
}

void StateBufferStoreTester::tlmEntryRefusesValueTest() {
    // tlmIn is a mapped entry's only writer, as the coherency counter requires
    Fw::PolyType val(static_cast<U32>(9));
    ASSERT_EQ(this->invoke_to_putValue(0, ENTRY_TLM_U32, val, SbsStatus::OK), SbsStatus::WRONG_KIND);
    (void)this->get(ENTRY_TLM_U32, SbsStatus::NOT_WRITTEN);

    // A mapped entry is a value entry to readers
    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    FwSizeType sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_TLM_U32, out, measTime, Fw::Time(), sizeOut), SbsStatus::WRONG_KIND);
    ASSERT_EQ(sizeOut, 0u);
}

void StateBufferStoreTester::tlmDataStoreTest() {
    this->setTestTime(Fw::Time(TimeBase::TB_WORKSTATION_TIME, 5, 0));
    Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);

    // Any FPP struct serves; SbsMeasurement is one this module already defines
    const SbsMeasurement sent(Fw::Time(TimeBase::TB_WORKSTATION_TIME, 3, 4), SbsStatus::INVALID,
                              Fw::PolyType(static_cast<I16>(-7)));
    Fw::TlmBuffer structValue;
    ASSERT_EQ(structValue.serializeFrom(sent), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_STRUCT, tag, structValue);

    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_TLM_STRUCT, out, measTime, Fw::Time(), sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, structValue.getSize());
    ASSERT_EQ(measTime.getSeconds(), 70u);

    // The stored bytes are the channel's serialization, decoded by its own type
    Fw::SerialBuffer reader(readBack, sizeOut);
    reader.fill();
    SbsMeasurement received;
    ASSERT_EQ(reader.deserializeTo(received), Fw::FW_SERIALIZE_OK);
    ASSERT_EQ(received.get_time(), sent.get_time());
    ASSERT_EQ(received.get_validity(), SbsStatus::INVALID);
    ASSERT_TRUE(received.get_value().isI16());
    ASSERT_EQ(static_cast<I16>(received.get_value()), -7);

    const Fw::String sentString("state");
    Fw::TlmBuffer stringValue;
    ASSERT_EQ(stringValue.serializeFrom(sentString), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_STRING, tag, stringValue);
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_TLM_STRING, out, measTime, Fw::Time(), sizeOut), SbsStatus::OK);
    Fw::SerialBuffer stringReader(readBack, sizeOut);
    stringReader.fill();
    Fw::String receivedString;
    ASSERT_EQ(stringReader.deserializeTo(receivedString), Fw::FW_SERIALIZE_OK);
    ASSERT_EQ(receivedString, sentString);

    ASSERT_EVENTS_SIZE(0);
}

void StateBufferStoreTester::tlmValidityTest() {
    Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);
    Fw::TlmBuffer first;
    ASSERT_EQ(first.serializeFrom(static_cast<U32>(5)), Fw::FW_SERIALIZE_OK);
    ASSERT_EQ(this->invoke_to_setTlmValidity(0, ENTRY_TLM_U32, SbsStatus::INVALID), SbsStatus::OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_U32, tag, first);

    Fw::PolyType value;
    Fw::Time measTime;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_TLM_U32, value, measTime, Fw::Time()), SbsStatus::INVALID);
    ASSERT_EQ(static_cast<U32>(value), 5u);
    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_TLM_U32, min, max), SbsStatus::OK);
    ASSERT_EQ(min.get_validity(), SbsStatus::INVALID);
    ASSERT_EQ(max.get_validity(), SbsStatus::INVALID);

    // The validity in force is reported from the ground, so one left INVALID is visible
    const U32 cmdSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, cmdSeq, ENTRY_TLM_U32);
    ASSERT_EVENTS_WatermarkReport_SIZE(1);
    ASSERT_EVENTS_WatermarkReport(0, ENTRY_TLM_U32, 5.0, 70000000, SbsStatus::INVALID, 5.0, 70000000,
                                  SbsStatus::INVALID, SbsStatus::INVALID);
    this->clearHistory();

    // In force until set again: the next sample is OK, the earlier one keeps INVALID
    ASSERT_EQ(this->invoke_to_setTlmValidity(0, ENTRY_TLM_U32, SbsStatus::OK), SbsStatus::OK);
    Fw::Time laterTag(TimeBase::TB_WORKSTATION_TIME, 71, 0);
    Fw::TlmBuffer second;
    ASSERT_EQ(second.serializeFrom(static_cast<U32>(6)), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_U32, laterTag, second);
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_TLM_U32, value, measTime, Fw::Time()), SbsStatus::OK);

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_TLM_U32, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, 2u * RECORD_SIZE);
    ASSERT_EQ(this->readRecord(data, 0).get_validity(), SbsStatus::INVALID);
    ASSERT_EQ(this->readRecord(data, 1).get_validity(), SbsStatus::OK);

    // A TYPE_DATA entry records it the same way
    ASSERT_EQ(this->invoke_to_setTlmValidity(0, ENTRY_TLM_STRUCT, SbsStatus::INVALID), SbsStatus::OK);
    const SbsMeasurement sent(Fw::Time(TimeBase::TB_WORKSTATION_TIME, 3, 4), SbsStatus::OK,
                              Fw::PolyType(static_cast<I16>(-7)));
    Fw::TlmBuffer structValue;
    ASSERT_EQ(structValue.serializeFrom(sent), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_STRUCT, tag, structValue);
    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_TLM_STRUCT, out, measTime, Fw::Time(), sizeOut), SbsStatus::INVALID);
    ASSERT_EQ(sizeOut, structValue.getSize());

    ASSERT_EVENTS_SIZE(0);
}

void StateBufferStoreTester::tlmValidityRefusedTest() {
    // Entries not mapped to telemetry carry validity with each put
    ASSERT_EQ(this->invoke_to_setTlmValidity(0, ENTRY_A, SbsStatus::INVALID), SbsStatus::WRONG_KIND);

    // A status the store reports itself is refused and leaves OK in force
    ASSERT_EQ(this->invoke_to_setTlmValidity(0, ENTRY_TLM_U32, SbsStatus::NOT_FRESH), SbsStatus::INVALID_VALIDITY);
    Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);
    Fw::TlmBuffer u32Value;
    ASSERT_EQ(u32Value.serializeFrom(static_cast<U32>(1)), Fw::FW_SERIALIZE_OK);
    this->invoke_to_tlmIn(0, TLM_CHAN_U32, tag, u32Value);
    Fw::PolyType value;
    Fw::Time measTime;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_TLM_U32, value, measTime, Fw::Time()), SbsStatus::OK);
}

void StateBufferStoreTester::tlmDataOversizeTest() {
    if (StateBufferStoreCfg::MAX_DATA_SIZE >= FW_TLM_BUFFER_MAX_SIZE) {
        GTEST_SKIP() << "MAX_DATA_SIZE holds any telemetry buffer, so no TYPE_DATA value can be oversized";
    }
    Fw::Time tag(TimeBase::TB_WORKSTATION_TIME, 70, 0);
    Fw::TlmBuffer largest;
    for (FwSizeType i = 0; i < StateBufferStoreCfg::MAX_DATA_SIZE; i++) {
        ASSERT_EQ(largest.serializeFrom(static_cast<U8>(i)), Fw::FW_SERIALIZE_OK);
    }
    Fw::TlmBuffer oversized = largest;
    ASSERT_EQ(oversized.serializeFrom(static_cast<U8>(0)), Fw::FW_SERIALIZE_OK);

    this->invoke_to_tlmIn(0, TLM_CHAN_STRUCT, tag, oversized);
    ASSERT_EVENTS_SIZE(1);
    ASSERT_EVENTS_TlmDataTooLarge_SIZE(1);
    ASSERT_EVENTS_TlmDataTooLarge(0, TLM_CHAN_STRUCT, ENTRY_TLM_STRUCT, StateBufferStoreCfg::MAX_DATA_SIZE + 1,
                                  StateBufferStoreCfg::MAX_DATA_SIZE);

    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    FwSizeType sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_TLM_STRUCT, out, measTime, Fw::Time(), sizeOut), SbsStatus::NOT_WRITTEN);
    ASSERT_EQ(sizeOut, 0u);

    // Exactly MAX_DATA_SIZE fits
    this->clearHistory();
    this->invoke_to_tlmIn(0, TLM_CHAN_STRUCT, tag, largest);
    ASSERT_EVENTS_SIZE(0);
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_TLM_STRUCT, out, measTime, Fw::Time(), sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, StateBufferStoreCfg::MAX_DATA_SIZE);
    ASSERT_EQ(readBack[StateBufferStoreCfg::MAX_DATA_SIZE - 1],
              static_cast<U8>(StateBufferStoreCfg::MAX_DATA_SIZE - 1));
}

void StateBufferStoreTester::tlmDataEntryRefusesTest() {
    // Refused as a telemetry entry even with a null buffer, before any telemetry arrives
    Fw::Buffer empty;
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_TLM_STRUCT, empty, SbsStatus::OK), SbsStatus::WRONG_KIND);
    U8 stored[] = {'t', 'l', 'm'};
    Fw::Buffer in(stored, sizeof stored);
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_TLM_STRUCT, in, SbsStatus::OK), SbsStatus::WRONG_KIND);
    Fw::PolyType val(static_cast<U32>(9));
    ASSERT_EQ(this->invoke_to_putValue(0, ENTRY_TLM_STRUCT, val, SbsStatus::OK), SbsStatus::WRONG_KIND);

    // A TYPE_DATA entry is a data entry to readers
    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE] = {};
    Fw::Buffer out(readBack, sizeof readBack);
    Fw::Time measTime;
    FwSizeType sizeOut = 1;
    ASSERT_EQ(this->invoke_to_getData(0, ENTRY_TLM_STRUCT, out, measTime, Fw::Time(), sizeOut), SbsStatus::NOT_WRITTEN);
    (void)this->get(ENTRY_TLM_STRUCT, SbsStatus::WRONG_KIND);
    SbsMeasurement min;
    SbsMeasurement max;
    ASSERT_EQ(this->invoke_to_getMinMax(0, ENTRY_TLM_STRUCT, min, max), SbsStatus::WRONG_KIND);
    ASSERT_EQ(this->invoke_to_clearMinMax(0, ENTRY_TLM_STRUCT), SbsStatus::WRONG_KIND);
    ASSERT_EQ(this->invoke_to_clearAndGetMinMax(0, ENTRY_TLM_STRUCT, min, max), SbsStatus::WRONG_KIND);
    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH] = {};
    Fw::Buffer data(bytes, sizeof bytes);
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_TLM_STRUCT, data, sizeOut), SbsStatus::WRONG_KIND);
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_TLM_STRUCT, 0, data, sizeOut), SbsStatus::WRONG_KIND);

    const U32 reportSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, reportSeq, ENTRY_TLM_STRUCT);
    const U32 clearSeq = this->m_cmdSeq++;
    this->sendCmd_CLEAR_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, clearSeq, ENTRY_TLM_STRUCT);
    ASSERT_EVENTS_SIZE(2);
    ASSERT_EVENTS_ReportWatermarksRejected(0, ENTRY_TLM_STRUCT, StateBufferStore_WatermarkRejection::DATA_ENTRY);
    ASSERT_EVENTS_ClearWatermarksRejected(0, ENTRY_TLM_STRUCT, StateBufferStore_WatermarkRejection::DATA_ENTRY);
    ASSERT_CMD_RESPONSE_SIZE(2);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_REPORT_WATERMARKS, reportSeq, Fw::CmdResponse::EXECUTION_ERROR);
    ASSERT_CMD_RESPONSE(1, StateBufferStore::OPCODE_CLEAR_WATERMARKS, clearSeq, Fw::CmdResponse::EXECUTION_ERROR);
}

// ----------------------------------------------------------------------
// Tests: programming errors
// ----------------------------------------------------------------------

void StateBufferStoreTester::unconfiguredDeathTest() {
    ASSERT_DEATH_IF_SUPPORTED(readUnconfigured(), "StateBufferStore.cpp");
}

void StateBufferStoreTester::configureTwiceDeathTest() {
    ASSERT_DEATH_IF_SUPPORTED(this->component.configure(TEST_MEM_ID, this->m_allocator), "StateBufferStore.cpp");
}

void StateBufferStoreTester::untypedPutDeathTest() {
    Fw::PolyType untyped;
    ASSERT_DEATH_IF_SUPPORTED(this->invoke_to_putValue(0, ENTRY_A, untyped, SbsStatus::OK), "StateBufferStore.cpp");
}

void StateBufferStoreTester::tlmMappingDeathTest() {
    SbsTlmMapping duplicateChannel[] = {
        SbsTlmMapping(TLM_CHAN_U32, StateBufferStoreCfg::StateEntry::SBS_ENTRY_00, SbsTlmType::TYPE_U32),
        SbsTlmMapping(TLM_CHAN_U32, StateBufferStoreCfg::StateEntry::SBS_ENTRY_01, SbsTlmType::TYPE_U32),
    };
    ASSERT_DEATH_IF_SUPPORTED(configureWithMappings(duplicateChannel, FW_NUM_ARRAY_ELEMENTS(duplicateChannel)),
                              "StateBufferStore.cpp");

    SbsTlmMapping duplicateEntry[] = {
        SbsTlmMapping(TLM_CHAN_U32, StateBufferStoreCfg::StateEntry::SBS_ENTRY_00, SbsTlmType::TYPE_U32),
        SbsTlmMapping(TLM_CHAN_F32, StateBufferStoreCfg::StateEntry::SBS_ENTRY_00, SbsTlmType::TYPE_F32),
    };
    ASSERT_DEATH_IF_SUPPORTED(configureWithMappings(duplicateEntry, FW_NUM_ARRAY_ELEMENTS(duplicateEntry)),
                              "StateBufferStore.cpp");

    SbsTlmMapping sizingCounter[] = {
        SbsTlmMapping(TLM_CHAN_U32, StateBufferStoreCfg::StateEntry::NUM_ENTRIES, SbsTlmType::TYPE_U32),
    };
    ASSERT_DEATH_IF_SUPPORTED(configureWithMappings(sizingCounter, FW_NUM_ARRAY_ELEMENTS(sizingCounter)),
                              "StateBufferStore.cpp");

    // A well-formed table configures cleanly
    configureWithMappings(TLM_MAPPINGS, FW_NUM_ARRAY_ELEMENTS(TLM_MAPPINGS));
}

void StateBufferStoreTester::corruptedEntryDeathTest() {
    // Each corruption happens inside the death statement, in the child, so
    // this tester's own component is never left corrupted
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::DEPTH_ZERO), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::DEPTH_ABOVE_MAX), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::INDEX), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::HISTORY_NULL), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::DATA_NULL), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::LATEST_VALUE_READ), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::LATEST_DATA_READ), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::STORED), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::LATEST_PUT), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::INDEX_DURING_READ), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::KIND), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::KIND_DURING_READ), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::KIND_FLIP_VALUE_READ), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::KIND_FLIP_DATA_READ), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::KIND_FLIP_WATERMARK_READ), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::KIND_FLIP_HISTORY_READ), "StateBufferStore.cpp");
    ASSERT_DEATH_IF_SUPPORTED(this->useCorrupted(Corruption::TLM_VALIDITY), "StateBufferStore.cpp");

    // Uncorrupted, the same entries still work
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);
    (void)this->get(ENTRY_A, SbsStatus::OK);
}

}  // namespace Svc
