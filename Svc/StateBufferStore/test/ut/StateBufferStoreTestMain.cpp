// ======================================================================
// \title  StateBufferStoreTestMain.cpp
// \brief  cpp file for StateBufferStore component test main function
// ======================================================================

#include "Fw/Test/UnitTest.hpp"
#include "StateBufferStoreTester.hpp"

TEST(Nominal, PutGet) {
    COMMENT("A stored primitive is returned with its timestamp and validity");
    REQUIREMENT("REQ-STATEBUFFERSTORE-001");
    REQUIREMENT("REQ-STATEBUFFERSTORE-003");
    REQUIREMENT("REQ-STATEBUFFERSTORE-012");
    Svc::StateBufferStoreTester tester;
    tester.putGetTest();
}

TEST(Nominal, AllTypes) {
    COMMENT("Every supported primitive type round trips through one entry");
    REQUIREMENT("REQ-STATEBUFFERSTORE-011");
    Svc::StateBufferStoreTester tester;
    tester.allTypesTest();
}

TEST(Nominal, PutWithValidity) {
    COMMENT("A put carrying a validity reports that validity on read");
    REQUIREMENT("REQ-STATEBUFFERSTORE-001");
    Svc::StateBufferStoreTester tester;
    tester.putWithValidityTest();
}

TEST(OffNominal, PutRefusesStoreStatus) {
    COMMENT("A put whose validity is a store-generated status is refused and stores nothing");
    REQUIREMENT("REQ-STATEBUFFERSTORE-001");
    REQUIREMENT("REQ-STATEBUFFERSTORE-002");
    Svc::StateBufferStoreTester tester;
    tester.putRefusesStoreStatusTest();
}

TEST(OffNominal, NotWritten) {
    COMMENT("An entry never written reports NOT_WRITTEN");
    REQUIREMENT("REQ-STATEBUFFERSTORE-013");
    Svc::StateBufferStoreTester tester;
    tester.notWrittenTest();
}

TEST(OffNominal, NotFresh) {
    COMMENT("A read repeating the previous measurement time reports NOT_FRESH");
    REQUIREMENT("REQ-STATEBUFFERSTORE-013");
    Svc::StateBufferStoreTester tester;
    tester.notFreshTest();
}

TEST(Nominal, LastReadTimeOptional) {
    COMMENT("A read with no previous read time is never reported NOT_FRESH");
    REQUIREMENT("REQ-STATEBUFFERSTORE-013");
    Svc::StateBufferStoreTester tester;
    tester.lastReadTimeOptionalTest();
}

TEST(Nominal, EntryIsolation) {
    COMMENT("Entries are independent of one another");
    REQUIREMENT("REQ-STATEBUFFERSTORE-010");
    Svc::StateBufferStoreTester tester;
    tester.entryIsolationTest();
}

TEST(Nominal, WatermarkSeed) {
    COMMENT("The first stored value seeds both watermarks");
    REQUIREMENT("REQ-STATEBUFFERSTORE-005");
    Svc::StateBufferStoreTester tester;
    tester.watermarkSeedTest();
}

TEST(Nominal, WatermarkTracking) {
    COMMENT("Watermarks track the extremes across many puts");
    REQUIREMENT("REQ-STATEBUFFERSTORE-005");
    Svc::StateBufferStoreTester tester;
    tester.watermarkTrackingTest();
}

TEST(Nominal, WatermarkClear) {
    COMMENT("Clearing watermarks reseeds them on the next put");
    REQUIREMENT("REQ-STATEBUFFERSTORE-006");
    Svc::StateBufferStoreTester tester;
    tester.watermarkClearTest();
}

TEST(Nominal, ClearAndGet) {
    COMMENT("Clear-and-get reports the watermarks it then clears");
    REQUIREMENT("REQ-STATEBUFFERSTORE-007");
    Svc::StateBufferStoreTester tester;
    tester.clearAndGetTest();
}

TEST(Nominal, History) {
    COMMENT("Full history is returned oldest first");
    REQUIREMENT("REQ-STATEBUFFERSTORE-008");
    Svc::StateBufferStoreTester tester;
    tester.historyTest();
}

TEST(Nominal, HistoryWrap) {
    COMMENT("History wraps once more values are stored than the configured depth");
    REQUIREMENT("REQ-STATEBUFFERSTORE-008");
    REQUIREMENT("REQ-STATEBUFFERSTORE-010");
    Svc::StateBufferStoreTester tester;
    tester.historyWrapTest();
}

TEST(OffNominal, HistoryShortBuffer) {
    COMMENT("A buffer too small for the full history is rejected");
    REQUIREMENT("REQ-STATEBUFFERSTORE-008");
    Svc::StateBufferStoreTester tester;
    tester.historyShortBufferTest();
}

TEST(Nominal, NHistory) {
    COMMENT("An N-measurement read returns the latest N, oldest first");
    REQUIREMENT("REQ-STATEBUFFERSTORE-009");
    Svc::StateBufferStoreTester tester;
    tester.nHistoryTest();
}

TEST(Nominal, NHistoryZero) {
    COMMENT("An N-measurement read of zero returns the entry's full depth");
    REQUIREMENT("REQ-STATEBUFFERSTORE-009");
    Svc::StateBufferStoreTester tester;
    tester.nHistoryZeroTest();
}

TEST(OffNominal, NHistoryTruncate) {
    COMMENT("An N-measurement read truncates to whole measurements");
    REQUIREMENT("REQ-STATEBUFFERSTORE-009");
    Svc::StateBufferStoreTester tester;
    tester.nHistoryTruncateTest();
}

TEST(Nominal, DataRoundTrip) {
    COMMENT("A string or struct value round trips through the data interface");
    REQUIREMENT("REQ-STATEBUFFERSTORE-002");
    REQUIREMENT("REQ-STATEBUFFERSTORE-004");
    Svc::StateBufferStoreTester tester;
    tester.dataRoundTripTest();
}

TEST(OffNominal, DataShortBuffer) {
    COMMENT("A data read into a buffer too small is rejected");
    REQUIREMENT("REQ-STATEBUFFERSTORE-004");
    Svc::StateBufferStoreTester tester;
    tester.dataShortBufferTest();
}

TEST(Nominal, TornReadRecovers) {
    COMMENT("A torn read that resolves on retry reports the measurement normally");
    REQUIREMENT("REQ-STATEBUFFERSTORE-014");
    Svc::StateBufferStoreTester tester;
    tester.tornReadRecoversTest();
}

TEST(OffNominal, TornReadExhausted) {
    COMMENT("A read torn on every attempt reports INCOHERENT and a warning event");
    REQUIREMENT("REQ-STATEBUFFERSTORE-014");
    Svc::StateBufferStoreTester tester;
    tester.tornReadExhaustedTest();
}

TEST(Nominal, ReportWatermarksCommand) {
    COMMENT("REPORT_WATERMARKS emits the watermark report event");
    REQUIREMENT("REQ-STATEBUFFERSTORE-005");
    Svc::StateBufferStoreTester tester;
    tester.reportWatermarksCommandTest();
}

TEST(Nominal, ClearWatermarksCommand) {
    COMMENT("CLEAR_WATERMARKS clears the watermarks and reports doing so");
    REQUIREMENT("REQ-STATEBUFFERSTORE-006");
    Svc::StateBufferStoreTester tester;
    tester.clearWatermarksCommandTest();
}

TEST(OffNominal, InvalidEntryCommand) {
    COMMENT("Commands naming the sizing counter are rejected");
    REQUIREMENT("REQ-STATEBUFFERSTORE-010");
    Svc::StateBufferStoreTester tester;
    tester.invalidEntryCommandTest();
}

TEST(Nominal, WatermarkReportAllTypes) {
    COMMENT("Watermark reporting widens every stored numeric type to F64");
    REQUIREMENT("REQ-STATEBUFFERSTORE-005");
    REQUIREMENT("REQ-STATEBUFFERSTORE-011");
    Svc::StateBufferStoreTester tester;
    tester.watermarkReportAllTypesTest();
}

TEST(OffNominal, DataStatus) {
    COMMENT("A data entry reports NOT_WRITTEN and NOT_FRESH like a primitive entry");
    REQUIREMENT("REQ-STATEBUFFERSTORE-013");
    REQUIREMENT("REQ-STATEBUFFERSTORE-014");
    Svc::StateBufferStoreTester tester;
    tester.dataStatusTest();
}

TEST(OffNominal, HistoryTornRead) {
    COMMENT("A history read torn on every attempt reports INCOHERENT and copies nothing");
    REQUIREMENT("REQ-STATEBUFFERSTORE-014");
    Svc::StateBufferStoreTester tester;
    tester.historyTornReadTest();
}

TEST(Nominal, HistoryPartial) {
    COMMENT("A history read before the ring fills returns only stored measurements");
    REQUIREMENT("REQ-STATEBUFFERSTORE-008");
    REQUIREMENT("REQ-STATEBUFFERSTORE-009");
    Svc::StateBufferStoreTester tester;
    tester.historyPartialTest();
}

TEST(Nominal, HistoryRecordFormat) {
    COMMENT("History records are serialized SbsMeasurements with zeroed padding");
    REQUIREMENT("REQ-STATEBUFFERSTORE-008");
    Svc::StateBufferStoreTester tester;
    tester.historyRecordFormatTest();
}

TEST(OffNominal, ValueEntryRefusesData) {
    COMMENT("A value entry refuses the data interface");
    REQUIREMENT("REQ-STATEBUFFERSTORE-016");
    Svc::StateBufferStoreTester tester;
    tester.valueEntryRefusesDataTest();
}

TEST(OffNominal, DataEntryRefusesValue) {
    COMMENT("A data entry refuses the value, watermark, and history interfaces");
    REQUIREMENT("REQ-STATEBUFFERSTORE-016");
    Svc::StateBufferStoreTester tester;
    tester.dataEntryRefusesValueTest();
}

TEST(Nominal, ClearAndGetRacingPut) {
    COMMENT("Clear-and-get reports the watermarks it ended even if a put consumes the clear first");
    REQUIREMENT("REQ-STATEBUFFERSTORE-007");
    Svc::StateBufferStoreTester tester;
    tester.clearAndGetRacingPutTest();
}

TEST(OffNominal, WatermarkTornRead) {
    COMMENT("Watermark reads torn on every attempt report INCOHERENT and a warning event");
    REQUIREMENT("REQ-STATEBUFFERSTORE-014");
    Svc::StateBufferStoreTester tester;
    tester.watermarkTornReadTest();
}

TEST(Nominal, BoolWatermark) {
    COMMENT("Bool watermarks order false before true");
    REQUIREMENT("REQ-STATEBUFFERSTORE-005");
    Svc::StateBufferStoreTester tester;
    tester.boolWatermarkTest();
}

TEST(OffNominal, DataEntryKindCheckedFirst) {
    COMMENT("History reads of a data entry report WRONG_KIND before judging the request");
    REQUIREMENT("REQ-STATEBUFFERSTORE-016");
    Svc::StateBufferStoreTester tester;
    tester.dataEntryKindCheckedFirstTest();
}

TEST(Nominal, PerEntryDepth) {
    COMMENT("Each entry keeps its own configured depth, including the bounds");
    REQUIREMENT("REQ-STATEBUFFERSTORE-010");
    Svc::StateBufferStoreTester tester;
    tester.perEntryDepthTest();
}

TEST(Nominal, WatermarkTypeMismatch) {
    COMMENT("A value of a different type than the watermarks leaves them unchanged");
    REQUIREMENT("REQ-STATEBUFFERSTORE-005");
    REQUIREMENT("REQ-STATEBUFFERSTORE-006");
    Svc::StateBufferStoreTester tester;
    tester.watermarkTypeMismatchTest();
}

TEST(Nominal, TlmStore) {
    COMMENT("Mapped telemetry is decoded and stored with the sender's time tag");
    REQUIREMENT("REQ-STATEBUFFERSTORE-017");
    Svc::StateBufferStoreTester tester;
    tester.tlmStoreTest();
}

TEST(Nominal, TlmUnmapped) {
    COMMENT("Unmapped telemetry is ignored");
    REQUIREMENT("REQ-STATEBUFFERSTORE-017");
    Svc::StateBufferStoreTester tester;
    tester.tlmUnmappedTest();
}

TEST(Nominal, TlmAllTypes) {
    COMMENT("Telemetry of every mapping type decodes to a PolyType of that type");
    REQUIREMENT("REQ-STATEBUFFERSTORE-017");
    Svc::StateBufferStoreTester tester;
    tester.tlmAllTypesTest();
}

TEST(OffNominal, TlmDecodeFailed) {
    COMMENT("Telemetry that does not decode as its mapping's type is reported, not stored");
    REQUIREMENT("REQ-STATEBUFFERSTORE-017");
    Svc::StateBufferStoreTester tester;
    tester.tlmDecodeFailedTest();
}

TEST(OffNominal, ThrottleReset) {
    COMMENT("Throttled warnings stop at their throttle and resume after RESET_THROTTLES");
    REQUIREMENT("REQ-STATEBUFFERSTORE-014");
    REQUIREMENT("REQ-STATEBUFFERSTORE-017");
    Svc::StateBufferStoreTester tester;
    tester.throttleResetTest();
}

TEST(OffNominal, TlmEntryRefusesData) {
    COMMENT("An entry mapped to telemetry refuses data puts before any telemetry arrives");
    REQUIREMENT("REQ-STATEBUFFERSTORE-016");
    REQUIREMENT("REQ-STATEBUFFERSTORE-017");
    Svc::StateBufferStoreTester tester;
    tester.tlmEntryRefusesDataTest();
}

TEST(OffNominal, TlmEntryRefusesValue) {
    COMMENT("A telemetry-mapped entry refuses value puts and data reads");
    REQUIREMENT("REQ-STATEBUFFERSTORE-016");
    REQUIREMENT("REQ-STATEBUFFERSTORE-017");
    Svc::StateBufferStoreTester tester;
    tester.tlmEntryRefusesValueTest();
}

TEST(Death, Unconfigured) {
    COMMENT("Using the component before configure() asserts");
    REQUIREMENT("REQ-STATEBUFFERSTORE-010");
    Svc::StateBufferStoreTester tester;
    tester.unconfiguredDeathTest();
}

TEST(Death, ConfigureTwice) {
    COMMENT("Configuring twice asserts");
    REQUIREMENT("REQ-STATEBUFFERSTORE-010");
    Svc::StateBufferStoreTester tester;
    tester.configureTwiceDeathTest();
}

TEST(OffNominal, NHistoryNoRoom) {
    COMMENT("An N-measurement read into a buffer too small for one measurement reports INVALID_BUFFER_SIZE");
    REQUIREMENT("REQ-STATEBUFFERSTORE-009");
    Svc::StateBufferStoreTester tester;
    tester.nHistoryNoRoomTest();
}

TEST(Nominal, NHistoryTooDeep) {
    COMMENT("An N-measurement read deeper than the entry returns its full depth");
    REQUIREMENT("REQ-STATEBUFFERSTORE-009");
    Svc::StateBufferStoreTester tester;
    tester.nHistoryTooDeepTest();
}

TEST(OffNominal, OversizedPutData) {
    COMMENT("A data put larger than MAX_DATA_SIZE reports INVALID_BUFFER_SIZE and stores nothing");
    REQUIREMENT("REQ-STATEBUFFERSTORE-011");
    REQUIREMENT("REQ-STATEBUFFERSTORE-016");
    Svc::StateBufferStoreTester tester;
    tester.oversizedPutDataTest();
}

TEST(Death, UntypedPut) {
    COMMENT("A put of an untyped PolyType asserts");
    REQUIREMENT("REQ-STATEBUFFERSTORE-001");
    Svc::StateBufferStoreTester tester;
    tester.untypedPutDeathTest();
}

TEST(Death, TlmMapping) {
    COMMENT("A malformed telemetry mapping table asserts at configuration");
    REQUIREMENT("REQ-STATEBUFFERSTORE-017");
    Svc::StateBufferStoreTester tester;
    tester.tlmMappingDeathTest();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
