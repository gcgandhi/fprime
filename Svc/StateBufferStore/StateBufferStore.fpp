module Svc {

    @ Stores the latest value, min/max watermarks, and a circular measurement
    @ history for each configured state entry
    passive component StateBufferStore {

        @ The read operation that failed to obtain a coherent copy
        enum ReadOperation : U8 {
            GET_VALUE              @< getValue port
            GET_DATA               @< getData port
            GET_MIN_MAX            @< getMinMax port
            CLEAR_AND_GET_MIN_MAX  @< clearAndGetMinMax port
            GET_HISTORY            @< getHistory port
            GET_N_HISTORY          @< getNHistory port
            REPORT_WATERMARKS      @< REPORT_WATERMARKS command
        }

        @ Why a watermark command was rejected
        enum WatermarkRejection : U8 {
            NOT_AN_ENTRY  @< the entry named is the NUM_ENTRIES sizing counter
            DATA_ENTRY    @< the entry holds data, which has no watermarks
        }

        # ----------------------------------------------------------------------
        # Store interface
        #
        # Synchronous and unguarded: handlers run on the caller's thread. Reader
        # and writer are coordinated by a per-entry coherency counter rather
        # than a mutex, preserving the heritage design. The counter allows one
        # writer per entry; a telemetry-mapped entry refuses puts, so tlmIn is
        # its only writer port, and each mapped channel must reach tlmIn from
        # one thread at a time.
        # ----------------------------------------------------------------------

        @ Port storing a primitive measurement
        sync input port putValue: SbsPut

        @ Port getting the latest primitive measurement
        sync input port getValue: SbsGet

        @ Port storing a string, struct, or byte-array measurement
        sync input port putData: SbsPutData

        @ Port getting the latest string, struct, or byte-array measurement
        sync input port getData: SbsGetData

        @ Port getting an entry's minimum and maximum measurements
        sync input port getMinMax: SbsGetMinMax

        @ Port getting an entry's full measurement history
        sync input port getHistory: SbsGetHistory

        @ Port getting an entry's most recent N measurements
        sync input port getNHistory: SbsGetNHistory

        @ Port receiving telemetry. Channels in the mapping table given to
        @ configure() are decoded and stored in their mapped entry, stamped
        @ with the sender's time tag; all other channels are ignored
        sync input port tlmIn: Fw.Tlm

        # ----------------------------------------------------------------------
        # Privileged store interface
        #
        # The heritage module exposed these through a separate "protected" API
        # available only to the component responsible for telemetry. F Prime has no
        # in-component equivalent, so access is controlled by topology wiring.
        # See the access-control note in docs/sdd.md.
        #
        # Guarded, as is CLEAR_WATERMARKS, so that clears are serialized on the
        # component mutex: a second clear landing inside clearAndGetMinMax would
        # overwrite the retired watermarks it is about to report. Puts and
        # plain reads take no lock.
        # ----------------------------------------------------------------------

        @ Port clearing an entry's minimum and maximum measurements
        guarded input port clearMinMax: SbsClearMinMax

        @ Port getting an entry's minimum and maximum measurements, then clearing them
        guarded input port clearAndGetMinMax: SbsClearAndGetMinMax

        # ----------------------------------------------------------------------
        # Framework ports
        # ----------------------------------------------------------------------

        @ Port for receiving commands
        command recv port cmdIn

        @ Port for sending command registration requests
        command reg port cmdRegOut

        @ Port for sending command responses
        command resp port cmdResponseOut

        @ Event port
        event port eventOut

        @ Text event port
        text event port textEventOut

        @ Time get port
        time get port timeCaller

        # ----------------------------------------------------------------------
        # Commands
        # ----------------------------------------------------------------------

        @ Report an entry's minimum and maximum measurements as an event
        sync command REPORT_WATERMARKS(
            $entry: StateBufferStoreCfg.StateEntry  @< the entry to report
        ) opcode 0x00

        @ Clear an entry's minimum and maximum measurements
        guarded command CLEAR_WATERMARKS(
            $entry: StateBufferStoreCfg.StateEntry  @< the entry to clear
        ) opcode 0x01

        @ Reset the throttles of FailedReadCoherentData and TlmDecodeFailed, so
        @ those warnings resume after the operator has acted on them
        sync command RESET_THROTTLES opcode 0x02

        # ----------------------------------------------------------------------
        # Events
        # ----------------------------------------------------------------------

        @ A coherent read could not be obtained within the configured number
        @ of attempts, so the read reported INCOHERENT and copied nothing out
        event FailedReadCoherentData(
            $entry: StateBufferStoreCfg.StateEntry  @< the entry being read
            operation: ReadOperation                @< the read that failed
            iterations: U32                         @< attempts made
        ) \
        severity warning high \
        id 0x00 \
        format "Failed to read coherent data for entry {} in {} after {} iterations" \
        throttle 10

        @ Report of an entry's minimum and maximum measurements.
        @
        @ Values are widened to F64 and times reduced to microseconds because
        @ FPP events cannot carry Fw::PolyType or Fw::Time. Integer values
        @ above 2^53 lose precision here; the port interface reports them
        @ exactly.
        event WatermarkReport(
            $entry: StateBufferStoreCfg.StateEntry  @< the entry reported
            minValue: F64                           @< minimum value recorded
            minTime_us: I64                         @< when the minimum was recorded
            minValidity: SbsStatus                  @< validity of the minimum
            maxValue: F64                           @< maximum value recorded
            maxTime_us: I64                         @< when the maximum was recorded
            maxValidity: SbsStatus                  @< validity of the maximum
        ) \
        severity activity high \
        id 0x01 \
        format "Entry {} watermarks: min {} at {} us ({}), max {} at {} us ({})"

        @ An entry's minimum and maximum measurements were cleared
        event WatermarksCleared(
            $entry: StateBufferStoreCfg.StateEntry  @< the entry cleared
        ) \
        severity activity high \
        id 0x02 \
        format "Cleared watermarks for entry {}"

        @ A mapped telemetry value did not decode as its mapping's type, so it
        @ was not stored. Indicates a mapping table that disagrees with the
        @ channel's definition.
        event TlmDecodeFailed(
            chanId: FwChanIdType                    @< the channel received
            $entry: StateBufferStoreCfg.StateEntry  @< the entry it maps to
            valueType: SbsTlmType                   @< the type it was decoded as
            numBytes: FwSizeType                    @< bytes received
        ) \
        severity warning high \
        id 0x03 \
        format "Telemetry channel {} for entry {} did not decode as {} from {} bytes; not stored" \
        throttle 10

        @ A REPORT_WATERMARKS command was rejected without reporting
        event ReportWatermarksRejected(
            $entry: StateBufferStoreCfg.StateEntry  @< the entry named
            reason: WatermarkRejection              @< why it was rejected
        ) \
        severity warning low \
        id 0x04 \
        format "REPORT_WATERMARKS for entry {} rejected: {}"

        @ A CLEAR_WATERMARKS command was rejected without clearing
        event ClearWatermarksRejected(
            $entry: StateBufferStoreCfg.StateEntry  @< the entry named
            reason: WatermarkRejection              @< why it was rejected
        ) \
        severity warning low \
        id 0x05 \
        format "CLEAR_WATERMARKS for entry {} rejected: {}"

        @ The throttles of FailedReadCoherentData and TlmDecodeFailed were reset
        event ThrottlesReset \
        severity activity high \
        id 0x06 \
        format "Event throttles reset"

    }

}
