#####
# StateBufferStorePorts:
#
# Types and ports used to store measurements in, and retrieve measurements,
# watermarks, and measurement history from the Svc::StateBufferStore
#####

module Svc {

    @ Validity of a stored measurement, or the result of a store operation.
    @
    @ Values 0-5 are preserved from the heritage status enumeration so that ported
    @ requirements and ground tooling keep their meaning. INCOHERENT,
    @ WRONG_KIND, and INVALID_VALIDITY are new.
    enum SbsStatus: U8 {
        OK = 0                   @< measurement value is okay
        UNINITIALIZED = 1        @< retained for heritage trace; unreachable, see sdd.md
        NOT_WRITTEN = 2          @< measurement has never been written
        INVALID = 3              @< measurement marked invalid by the writer
        NOT_FRESH = 4            @< measurement unchanged since the caller's last read
        INVALID_BUFFER_SIZE = 5  @< caller buffer too small for the read, or put data null or larger than MAX_DATA_SIZE
        INCOHERENT = 6           @< no coherent read within MAX_READ_ITERATIONS
        WRONG_KIND = 7           @< entry holds the other kind of measurement (value vs. data), or only tlmIn may write it
        INVALID_VALIDITY = 8     @< a put's validity is not OK or INVALID
    }

    @ A stored measurement: a value, when it was written, and its validity
    struct SbsMeasurement {
        $time: Fw.Time              @< time the measurement was written
        validity: SbsStatus         @< validity recorded by the writer
        value: Fw.PolyType          @< the measurement value
    }

    @ How a telemetry channel's serialized value is decoded for storage
    enum SbsTlmType: U8 {
        TYPE_U8    @< U8 value
        TYPE_I8    @< I8 value
        TYPE_U16   @< U16 value
        TYPE_I16   @< I16 value
        TYPE_U32   @< U32 value
        TYPE_I32   @< I32 value
        TYPE_U64   @< U64 value
        TYPE_I64   @< I64 value
        TYPE_F32   @< F32 value
        TYPE_F64   @< F64 value
        TYPE_BOOL  @< bool value
    }

    @ Maps one telemetry channel to the entry that stores it.
    @ A table of these is passed to StateBufferStore::configure; a future
    @ autocoder would generate it from the deployment's channels.
    struct SbsTlmMapping {
        chanId: FwChanIdType                    @< telemetry channel ID
        $entry: StateBufferStoreCfg.StateEntry  @< entry storing the channel
        valueType: SbsTlmType                   @< how the channel's value is serialized
    }

    @ Store a primitive measurement, timestamped by the component.
    @ Reports WRONG_KIND and stores nothing if the entry holds data or is
    @ mapped to a telemetry channel, which only tlmIn may write, and
    @ INVALID_VALIDITY and stores nothing if validity is not OK or INVALID
    port SbsPut(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to write
        val: Fw.PolyType                        @< value to store
        validity: SbsStatus                     @< validity to record with the value: OK or INVALID
    ) -> SbsStatus

    @ Get the latest primitive measurement for an entry
    port SbsGet(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to read
        ref val: Fw.PolyType                    @< populated with the latest value
        ref measTime: Fw.Time                   @< populated with the measurement time
        lastReadTime: Fw.Time                   @< caller's previous read time, or Fw::ZERO_TIME for none; drives NOT_FRESH
    ) -> SbsStatus

    @ Get the minimum and maximum measurements recorded for an entry
    port SbsGetMinMax(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to read
        ref min: SbsMeasurement                 @< populated with the minimum
        ref max: SbsMeasurement                 @< populated with the maximum
    ) -> SbsStatus

    @ Clear the minimum and maximum measurements for an entry.
    @ Privileged: see the access-control note in sdd.md
    port SbsClearMinMax(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to clear
    ) -> SbsStatus

    @ Get the minimum and maximum measurements for an entry, then clear them.
    @ Privileged: see the access-control note in sdd.md
    port SbsClearAndGetMinMax(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to read and clear
        ref min: SbsMeasurement                 @< populated with the minimum
        ref max: SbsMeasurement                 @< populated with the maximum
    ) -> SbsStatus

    @ Get an entry's full measurement history, oldest first, as serialized
    @ SbsMeasurement records of SbsMeasurement::SERIALIZED_SIZE bytes each.
    @ Only measurements actually stored are returned. Reports
    @ INVALID_BUFFER_SIZE and copies nothing if data is too small to hold the
    @ entry's whole depth. Value entries only
    port SbsGetHistory(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to read
        ref data: Fw.Buffer                     @< filled with serialized measurements
        ref sizeOut: FwSizeType                 @< bytes written to data
    ) -> SbsStatus

    @ Get an entry's most recent numMeasurements measurements, oldest first,
    @ in the getHistory record format. numMeasurements == 0 requests the
    @ entry's full depth, as does a numMeasurements deeper than the entry. A
    @ buffer too small for the request is filled with as many whole
    @ measurements as fit; one too small for a single measurement reports
    @ INVALID_BUFFER_SIZE and copies nothing, unless the read itself fails or
    @ finds the entry never written, which is reported instead. Value entries only
    port SbsGetNHistory(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to read
        numMeasurements: FwSizeType             @< how many to read; 0 means full depth
        ref data: Fw.Buffer                     @< filled with serialized measurements
        ref sizeOut: FwSizeType                 @< bytes written to data
    ) -> SbsStatus

    @ Store a string, struct, or byte-array measurement, timestamped by the
    @ component. Reports WRONG_KIND and stores nothing if the entry holds values,
    @ INVALID_VALIDITY and stores nothing if validity is not OK or INVALID,
    @ and INVALID_BUFFER_SIZE and stores nothing if data is null or larger than
    @ StateBufferStoreCfg.MAX_DATA_SIZE
    port SbsPutData(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to write
        ref data: Fw.Buffer                     @< bytes to store
        validity: SbsStatus                     @< validity to record with the value: OK or INVALID
    ) -> SbsStatus

    @ Get the latest string, struct, or byte-array measurement for an entry
    port SbsGetData(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to read
        ref data: Fw.Buffer                     @< filled with the latest value
        ref measTime: Fw.Time                   @< populated with the measurement time
        lastReadTime: Fw.Time                   @< caller's previous read time, or Fw::ZERO_TIME for none; drives NOT_FRESH
        ref sizeOut: FwSizeType                 @< bytes written to data
    ) -> SbsStatus

}
