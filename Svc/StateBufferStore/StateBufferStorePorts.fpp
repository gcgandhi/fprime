#####
# StateBufferStorePorts:
#
# Types and ports used to store measurements in, and retrieve measurements,
# watermarks, and measurement history from, Svc::StateBufferStore
#####

module Svc {

    @ Validity of a stored measurement, or the result of a store operation.
    @
    @ Values 0-5 are preserved from the heritage status enumeration so that ported
    @ requirements and ground tooling keep their meaning. INCOHERENT and
    @ WRONG_KIND are new.
    enum SbsStatus: U8 {
        OK = 0                   @< measurement value is okay
        UNINITIALIZED = 1        @< retained for heritage trace; unreachable, see sdd.md
        NOT_WRITTEN = 2          @< measurement has never been written
        INVALID = 3              @< measurement marked invalid by the writer
        NOT_FRESH = 4            @< measurement unchanged since the caller's last read
        INVALID_BUFFER_SIZE = 5  @< caller buffer too small for the full history
        INCOHERENT = 6           @< no coherent read within MAX_READ_ITERATIONS
        WRONG_KIND = 7           @< entry holds the other kind of measurement (value vs. data)
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
    @ Reports WRONG_KIND and stores nothing if the entry holds data
    port SbsPut(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to write
        ref val: Fw.PolyType                    @< value to store
        validity: SbsStatus                     @< validity to record with the value
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
    @ entry's full depth. A buffer too small for the request is filled with as
    @ many whole measurements as fit. Value entries only
    port SbsGetNHistory(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to read
        numMeasurements: U16                    @< how many to read; 0 means full depth
        ref data: Fw.Buffer                     @< filled with serialized measurements
        ref sizeOut: FwSizeType                 @< bytes written to data
    ) -> SbsStatus

    @ Store a string, struct, or byte-array measurement, timestamped by the
    @ component. Reports WRONG_KIND and stores nothing if the entry holds values
    port SbsPutData(
        $entry: StateBufferStoreCfg.StateEntry  @< the entry to write
        ref data: Fw.Buffer                     @< bytes to store
        validity: SbsStatus                     @< validity to record with the value
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
