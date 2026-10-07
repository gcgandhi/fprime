// ======================================================================
// \title  StateBufferStoreTester.cpp
// \brief  cpp file for StateBufferStore component test harness implementation class
// ======================================================================

#include "StateBufferStoreTester.hpp"

#include "Fw/Types/SerialBuffer.hpp"

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

//! Memory identifier the component under test allocates under
constexpr FwEnumStoreType TEST_MEM_ID = 0;

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
    this->component.configure(TEST_MEM_ID, this->m_allocator);
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
    Fw::PolyType val = value;
    EXPECT_EQ(this->invoke_to_putValue(0, entry, val, validity), SbsStatus::OK);
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
    this->put(ENTRY_A, Fw::PolyType(static_cast<U8>(8)), 1);
    ASSERT_EQ(static_cast<U8>(this->get(ENTRY_A, SbsStatus::OK)), 8u);

    this->put(ENTRY_A, Fw::PolyType(static_cast<I8>(-8)), 2);
    ASSERT_EQ(static_cast<I8>(this->get(ENTRY_A, SbsStatus::OK)), -8);

    this->put(ENTRY_A, Fw::PolyType(static_cast<U16>(16)), 3);
    ASSERT_EQ(static_cast<U16>(this->get(ENTRY_A, SbsStatus::OK)), 16u);

    this->put(ENTRY_A, Fw::PolyType(static_cast<I16>(-16)), 4);
    ASSERT_EQ(static_cast<I16>(this->get(ENTRY_A, SbsStatus::OK)), -16);

    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(32)), 5);
    ASSERT_EQ(static_cast<U32>(this->get(ENTRY_A, SbsStatus::OK)), 32u);

    this->put(ENTRY_A, Fw::PolyType(static_cast<I32>(-32)), 6);
    ASSERT_EQ(static_cast<I32>(this->get(ENTRY_A, SbsStatus::OK)), -32);

    this->put(ENTRY_A, Fw::PolyType(static_cast<F32>(1.5f)), 7);
    ASSERT_EQ(static_cast<F32>(this->get(ENTRY_A, SbsStatus::OK)), 1.5f);

    this->put(ENTRY_A, Fw::PolyType(static_cast<F64>(2.25)), 8);
    ASSERT_EQ(static_cast<F64>(this->get(ENTRY_A, SbsStatus::OK)), 2.25);
}

void StateBufferStoreTester::putWithValidityTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(7)), 10, SbsStatus::INVALID);
    // The stored validity is reported verbatim, not overridden
    (void)this->get(ENTRY_A, SbsStatus::INVALID);
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

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
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

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;
    ASSERT_EQ(this->invoke_to_getHistory(0, ENTRY_A, data, sizeOut), SbsStatus::OK);

    // Only the most recent DEPTH values survive, still oldest first
    for (FwSizeType i = 0; i < DEPTH; i++) {
        const SbsMeasurement record = this->readRecord(data, i);
        ASSERT_EQ(static_cast<U32>(record.get_value()), static_cast<U32>(writes - DEPTH + i + 1));
    }
}

void StateBufferStoreTester::historyShortBufferTest() {
    this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(1)), 1);

    // One byte short of the whole history
    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
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

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
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

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
    Fw::Buffer data(bytes, sizeof bytes);
    FwSizeType sizeOut = 0;
    // Zero means the entry's whole depth
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 0, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, DEPTH * RECORD_SIZE);
}

void StateBufferStoreTester::nHistoryTruncateTest() {
    for (FwSizeType i = 0; i < DEPTH; i++) {
        this->put(ENTRY_A, Fw::PolyType(static_cast<U32>(i + 1)), static_cast<U32>(i + 1));
    }

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
    // Room for one measurement and a fragment of a second
    Fw::Buffer data(bytes, RECORD_SIZE + (RECORD_SIZE / 2));
    FwSizeType sizeOut = 0;
    // Unlike the full-history read, this one truncates rather than failing,
    // and never reports a partial measurement
    ASSERT_EQ(this->invoke_to_getNHistory(0, ENTRY_A, 3, data, sizeOut), SbsStatus::OK);
    ASSERT_EQ(sizeOut, RECORD_SIZE);
}

// ----------------------------------------------------------------------
// Tests: data interface
// ----------------------------------------------------------------------

void StateBufferStoreTester::dataRoundTripTest() {
    U8 stored[] = {'s', 't', 'a', 't', 'e'};
    Fw::Buffer in(stored, sizeof stored);
    this->setTestTime(Fw::Time(TimeBase::TB_WORKSTATION_TIME, 20, 0));
    ASSERT_EQ(this->invoke_to_putData(0, ENTRY_A, in, SbsStatus::OK), SbsStatus::OK);

    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE];
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

    U8 readBack[2];
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

    // Tear every attempt the retry loop makes
    this->component.m_utForceTornReads = StateBufferStoreCfg::MAX_READ_ITERATIONS;
    Fw::PolyType value;
    Fw::Time measTime;
    ASSERT_EQ(this->invoke_to_getValue(0, ENTRY_A, value, measTime, Fw::Time()), SbsStatus::INCOHERENT);

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
    ASSERT_EVENTS_WatermarkReport(0, ENTRY_A, -2.0, 1000000, SbsStatus::OK, 9.0, 2000000, SbsStatus::OK);
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
}

void StateBufferStoreTester::invalidEntryCommandTest() {
    // NUM_ENTRIES is the table size, not an addressable entry. Both commands
    // reject it rather than indexing past the entry table.
    const U32 reportSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, reportSeq,
                                    StateBufferStoreCfg::StateEntry::NUM_ENTRIES);
    ASSERT_EVENTS_SIZE(0);
    ASSERT_CMD_RESPONSE_SIZE(1);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_REPORT_WATERMARKS, reportSeq, Fw::CmdResponse::VALIDATION_ERROR);

    const U32 clearSeq = this->m_cmdSeq++;
    this->sendCmd_CLEAR_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, clearSeq,
                                   StateBufferStoreCfg::StateEntry::NUM_ENTRIES);
    ASSERT_EVENTS_SIZE(0);
    ASSERT_CMD_RESPONSE_SIZE(2);
    ASSERT_CMD_RESPONSE(1, StateBufferStore::OPCODE_CLEAR_WATERMARKS, clearSeq, Fw::CmdResponse::VALIDATION_ERROR);
}

void StateBufferStoreTester::watermarkReportAllTypesTest() {
    // Watermark events carry F64, so every stored type must widen correctly.
    // One entry per type keeps the watermarks from being compared across types.
    struct Case {
        StateBufferStoreCfg::StateEntry::T entry;
        Fw::PolyType value;
        F64 expected;
    };
    const Case cases[] = {
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_00, Fw::PolyType(static_cast<U8>(200)), 200.0},
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_01, Fw::PolyType(static_cast<I8>(-100)), -100.0},
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_02, Fw::PolyType(static_cast<U16>(60000)), 60000.0},
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_03, Fw::PolyType(static_cast<I16>(-30000)), -30000.0},
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_04, Fw::PolyType(static_cast<U32>(4000000000u)), 4000000000.0},
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_05, Fw::PolyType(static_cast<I32>(-2000000000)), -2000000000.0},
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_06, Fw::PolyType(static_cast<U64>(1000000)), 1000000.0},
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_07, Fw::PolyType(static_cast<I64>(-1000000)), -1000000.0},
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_08, Fw::PolyType(static_cast<F32>(0.5f)), 0.5},
        {StateBufferStoreCfg::StateEntry::SBS_ENTRY_09, Fw::PolyType(static_cast<F64>(1.25)), 1.25},
    };

    for (const Case& testCase : cases) {
        this->put(testCase.entry, testCase.value, 1);
        this->clearHistory();
        const U32 cmdSeq = this->m_cmdSeq++;
        this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, cmdSeq, testCase.entry);
        ASSERT_EVENTS_WatermarkReport_SIZE(1);
        // A single stored value is both watermarks
        ASSERT_EVENTS_WatermarkReport(0, testCase.entry, testCase.expected, 1000000, SbsStatus::OK, testCase.expected,
                                      1000000, SbsStatus::OK);
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
    ASSERT_EVENTS_WatermarkReport(0, ENTRY_A, 1.0, 2000000, SbsStatus::OK, 1.0, 2000000, SbsStatus::OK);

    // A cleared watermark holds no type at all, so there is no value to widen
    ASSERT_EQ(this->invoke_to_clearMinMax(0, ENTRY_B), SbsStatus::OK);
    this->clearHistory();
    const U32 clearedSeq = this->m_cmdSeq++;
    this->sendCmd_REPORT_WATERMARKS(StateBufferStoreTester::TEST_INSTANCE_ID, clearedSeq, ENTRY_B);
    ASSERT_EVENTS_WatermarkReport_SIZE(1);
    ASSERT_EVENTS_WatermarkReport(0, ENTRY_B, 0.0, 0, SbsStatus::NOT_WRITTEN, 0.0, 0, SbsStatus::NOT_WRITTEN);
}

void StateBufferStoreTester::dataStatusTest() {
    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE];
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

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
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

    ASSERT_EVENTS_FailedReadCoherentData_SIZE(2);
    ASSERT_EVENTS_FailedReadCoherentData(0, ENTRY_A, StateBufferStore_ReadOperation::GET_HISTORY,
                                         StateBufferStoreCfg::MAX_READ_ITERATIONS);
    ASSERT_EVENTS_FailedReadCoherentData(1, ENTRY_A, StateBufferStore_ReadOperation::GET_N_HISTORY,
                                         StateBufferStoreCfg::MAX_READ_ITERATIONS);
}

void StateBufferStoreTester::historyPartialTest() {
    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
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

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
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

    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE];
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

    U8 bytes[RECORD_SIZE * StateBufferStoreCfg::MAX_HISTORY_DEPTH];
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
    ASSERT_EVENTS_SIZE(0);
    ASSERT_CMD_RESPONSE_SIZE(2);
    ASSERT_CMD_RESPONSE(0, StateBufferStore::OPCODE_REPORT_WATERMARKS, reportSeq, Fw::CmdResponse::EXECUTION_ERROR);
    ASSERT_CMD_RESPONSE(1, StateBufferStore::OPCODE_CLEAR_WATERMARKS, clearSeq, Fw::CmdResponse::EXECUTION_ERROR);

    // The refused store left the entry's data untouched
    U8 readBack[StateBufferStoreCfg::MAX_DATA_SIZE];
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

}  // namespace Svc
