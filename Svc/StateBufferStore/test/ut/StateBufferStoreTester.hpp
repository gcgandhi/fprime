// ======================================================================
// \title  StateBufferStoreTester.hpp
// \brief  hpp file for StateBufferStore component test harness implementation class
// ======================================================================

#ifndef Svc_StateBufferStoreTester_HPP
#define Svc_StateBufferStoreTester_HPP

#include "Fw/Types/MallocAllocator.hpp"
#include "Svc/StateBufferStore/StateBufferStore.hpp"
#include "Svc/StateBufferStore/StateBufferStoreGTestBase.hpp"

namespace Svc {

class StateBufferStoreTester : public StateBufferStoreGTestBase {
  public:
    // ----------------------------------------------------------------------
    // Constants
    // ----------------------------------------------------------------------

    //! Maximum size of histories storing events, telemetry, and port outputs
    static const FwSizeType MAX_HISTORY_SIZE = 32;

    //! Instance ID supplied to the component instance under test
    static const FwEnumStoreType TEST_INSTANCE_ID = 0;

  public:
    // ----------------------------------------------------------------------
    // Construction and destruction
    // ----------------------------------------------------------------------

    //! Construct object StateBufferStoreTester
    StateBufferStoreTester();

    //! Destroy object StateBufferStoreTester
    ~StateBufferStoreTester();

  public:
    // ----------------------------------------------------------------------
    // Tests
    // ----------------------------------------------------------------------

    //! A stored primitive is returned with its timestamp and validity
    void putGetTest();

    //! Every supported primitive type round trips through one entry
    void allTypesTest();

    //! A put carrying a validity reports that validity on read
    void putWithValidityTest();

    //! A put whose validity is a store-generated status is refused and stores nothing
    void putRefusesStoreStatusTest();

    //! An entry never written reports NOT_WRITTEN
    void notWrittenTest();

    //! A read repeating the previous measurement time reports NOT_FRESH
    void notFreshTest();

    //! A read with no previous read time is never reported NOT_FRESH
    void lastReadTimeOptionalTest();

    //! Entries are independent of one another
    void entryIsolationTest();

    //! The first stored value seeds both watermarks
    void watermarkSeedTest();

    //! Watermarks track the extremes across many puts
    void watermarkTrackingTest();

    //! Clearing watermarks reseeds them on the next put
    void watermarkClearTest();

    //! Clear-and-get reports the watermarks it then clears
    void clearAndGetTest();

    //! Full history is returned oldest first
    void historyTest();

    //! History wraps once more values are stored than the configured depth
    void historyWrapTest();

    //! A buffer too small for the full history is rejected
    void historyShortBufferTest();

    //! An N-measurement read returns the latest N, oldest first
    void nHistoryTest();

    //! An N-measurement read of zero returns the entry's full depth
    void nHistoryZeroTest();

    //! An N-measurement read truncates to whole measurements
    void nHistoryTruncateTest();

    //! An N-measurement read into a buffer too small for one measurement reports INVALID_BUFFER_SIZE
    void nHistoryNoRoomTest();

    //! An N-measurement read deeper than the entry returns its full depth
    void nHistoryTooDeepTest();

    //! A string or struct value round trips through the data interface
    void dataRoundTripTest();

    //! A data read into a buffer too small is rejected
    void dataShortBufferTest();

    //! A torn read that resolves on retry reports the measurement normally
    void tornReadRecoversTest();

    //! A read torn on every attempt reports INCOHERENT and a warning event
    void tornReadExhaustedTest();

    //! REPORT_WATERMARKS emits the watermark report event
    void reportWatermarksCommandTest();

    //! CLEAR_WATERMARKS clears the watermarks and reports doing so
    void clearWatermarksCommandTest();

    //! Commands naming the sizing counter are rejected
    void invalidEntryCommandTest();

    //! Watermark reporting widens every stored numeric type to F64
    void watermarkReportAllTypesTest();

    //! A data entry reports NOT_WRITTEN and NOT_FRESH like a primitive entry
    void dataStatusTest();

    //! An empty Fw::Buffer is judged by its size on every port, never asserted on
    void emptyBufferTest();

    //! A data put larger than MAX_DATA_SIZE reports INVALID_BUFFER_SIZE and stores nothing
    void oversizedPutDataTest();

    //! A history read torn on every attempt reports INCOHERENT and copies nothing
    void historyTornReadTest();

    //! A history read before the ring fills returns only stored measurements
    void historyPartialTest();

    //! History records are serialized SbsMeasurements with zeroed padding
    void historyRecordFormatTest();

    //! A value entry refuses the data interface
    void valueEntryRefusesDataTest();

    //! A data entry refuses the value, watermark, and history interfaces
    void dataEntryRefusesValueTest();

    //! Clear-and-get reports the watermarks it ended even if a put consumes
    //! the clear before they are read
    void clearAndGetRacingPutTest();

    //! Watermark reads torn on every attempt report INCOHERENT and a warning event
    void watermarkTornReadTest();

    //! Bool watermarks order false before true
    void boolWatermarkTest();

    //! A floating-point NaN never becomes a watermark
    void nanWatermarkTest();

    //! A full-history read of a data entry reports WRONG_KIND before judging its buffer
    void dataEntryKindCheckedFirstTest();

    //! Each entry keeps its own configured depth, including the bounds
    void perEntryDepthTest();

    //! A value of a different type than the watermarks leaves them unchanged
    void watermarkTypeMismatchTest();

    //! Mapped telemetry is decoded and stored with the sender's time tag
    void tlmStoreTest();

    //! Unmapped telemetry is ignored
    void tlmUnmappedTest();

    //! Telemetry of every mapping type decodes to a PolyType of that type
    void tlmAllTypesTest();

    //! Telemetry that does not decode as its mapping's type is reported, not stored
    void tlmDecodeFailedTest();

    //! Throttled warnings stop at their throttle and resume after RESET_THROTTLES
    void throttleResetTest();

    //! An entry mapped to telemetry refuses data puts before any telemetry arrives
    void tlmEntryRefusesDataTest();

    //! A telemetry-mapped entry refuses value puts and data reads
    void tlmEntryRefusesValueTest();

    //! Using the component before configure() asserts
    void unconfiguredDeathTest();

    //! Configuring twice asserts
    void configureTwiceDeathTest();

    //! A put of an untyped PolyType asserts
    void untypedPutDeathTest();

    //! A malformed telemetry mapping table asserts at configuration
    void tlmMappingDeathTest();

  private:
    // ----------------------------------------------------------------------
    // Helper functions
    // ----------------------------------------------------------------------

    //! Set the component's time and store a value in an entry
    void put(StateBufferStoreCfg::StateEntry::T entry,
             const Fw::PolyType& value,
             U32 seconds,
             SbsStatus validity = SbsStatus::OK);

    //! Read an entry, asserting the reported status
    Fw::PolyType get(StateBufferStoreCfg::StateEntry::T entry,
                     SbsStatus expected,
                     const Fw::Time& lastReadTime = Fw::Time());

    //! Deserialize the index'th measurement out of a history buffer
    static SbsMeasurement readRecord(const Fw::Buffer& data, FwSizeType index);

    //! Store a value in ENTRY_A from inside a read, via the component's test hook
    static void putDuringRead(void* context);

    //! Connect ports
    void connectPorts();

    //! Initialize components
    void initComponents();

    // ----------------------------------------------------------------------
    // Member variables
    // ----------------------------------------------------------------------

    //! Allocator for the component's history. Declared before the component
    //! so that it outlives the component's deallocation.
    Fw::MallocAllocator m_allocator;

    //! The component under test
    StateBufferStore component;

    //! Next command sequence number
    U32 m_cmdSeq;
};

}  // namespace Svc

#endif
