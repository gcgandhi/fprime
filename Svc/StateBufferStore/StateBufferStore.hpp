// ======================================================================
// \title  StateBufferStore.hpp
// \brief  hpp file for StateBufferStore component implementation class
// ======================================================================

#ifndef Svc_StateBufferStore_HPP
#define Svc_StateBufferStore_HPP

#include <atomic>

#include "Fw/Types/MemAllocator.hpp"
#include "Svc/StateBufferStore/StateBufferStoreComponentAc.hpp"
#include "config/FppConstantsAc.hpp"
#include "config/HistoryDepthsArrayAc.hpp"

namespace Svc {

class StateBufferStore final : public StateBufferStoreComponentBase {
  public:
    // ----------------------------------------------------------------------
    // Component construction and destruction
    // ----------------------------------------------------------------------

    //! Construct StateBufferStore object
    explicit StateBufferStore(const char* const compName  //!< The component name
    );

    //! Destroy StateBufferStore object, releasing the measurement history
    ~StateBufferStore();

    //! Allocate the measurement history
    //!
    //! Must be called once, after construction and before any port is invoked.
    void configure(FwEnumStoreType memId,       //!< Identifier used when dealing with the Fw::MemAllocator
                   Fw::MemAllocator& allocator  //!< Allocator for the history. MUST outlive the component.
    );

  private:
    // ----------------------------------------------------------------------
    // Types
    // ----------------------------------------------------------------------

    //! Which interface an entry is used through, fixed by its first store
    enum EntryKind : U8 {
        KIND_UNSET = 0,  //!< never stored to
        KIND_VALUE = 1,  //!< stored through putValue
        KIND_DATA = 2,   //!< stored through putData
    };

    //! A stored measurement, as held internally
    struct Measurement {
        Fw::Time time;                                //!< when the value was written
        Fw::PolyType value;                           //!< the value, for value entries
        SbsStatus validity = SbsStatus::NOT_WRITTEN;  //!< validity recorded by the writer
        FwSizeType dataSize = 0;                      //!< bytes used in the payload slot, for data entries
    };

    //! Per-entry control block
    //!
    //! history and data point into the single allocation owned by m_memPtr.
    //! Every field other than the atomics is shared between the writer and
    //! readers without a lock, and is valid to a reader only if the
    //! coherency counter was even and unchanged across its copy.
    struct Entry {
        Measurement* history;               //!< ring of depth measurements
        U8* data;                           //!< depth payload slots of MAX_DATA_SIZE bytes
        std::atomic<U32> coherencyCounter;  //!< odd while a write is in progress
        std::atomic<U8> kind;               //!< EntryKind, claimed by the first store
        std::atomic<bool> clearPending;     //!< when set, the next put starts new watermarks
        FwSizeType depth;                   //!< configured history depth
        FwSizeType latest;                  //!< index of the most recent measurement
        FwSizeType stored;                  //!< measurements stored, saturating at depth
        Measurement min;                    //!< minimum value recorded
        Measurement max;                    //!< maximum value recorded
        Measurement retiredMin;             //!< minimum displaced by the most recent clear
        Measurement retiredMax;             //!< maximum displaced by the most recent clear
        FwSizeType index;                   //!< this entry's own index, for event reporting
    };

    // ----------------------------------------------------------------------
    // Handler implementations for typed input ports
    // ----------------------------------------------------------------------

    //! Handler implementation for putValue
    Svc::SbsStatus putValue_handler(FwIndexType portNum,                                //!< The port number
                                    const Svc::StateBufferStoreCfg::StateEntry& entry,  //!< The entry to write
                                    Fw::PolyType& val,                                  //!< Value to store
                                    const Svc::SbsStatus& validity                      //!< Validity to record
                                    ) override;

    //! Handler implementation for getValue
    Svc::SbsStatus getValue_handler(FwIndexType portNum,
                                    const Svc::StateBufferStoreCfg::StateEntry& entry,
                                    Fw::PolyType& val,
                                    Fw::Time& measTime,
                                    const Fw::Time& lastReadTime) override;

    //! Handler implementation for putData
    Svc::SbsStatus putData_handler(FwIndexType portNum,
                                   const Svc::StateBufferStoreCfg::StateEntry& entry,
                                   Fw::Buffer& data,
                                   const Svc::SbsStatus& validity) override;

    //! Handler implementation for getData
    Svc::SbsStatus getData_handler(FwIndexType portNum,
                                   const Svc::StateBufferStoreCfg::StateEntry& entry,
                                   Fw::Buffer& data,
                                   Fw::Time& measTime,
                                   const Fw::Time& lastReadTime,
                                   FwSizeType& sizeOut) override;

    //! Handler implementation for getMinMax
    Svc::SbsStatus getMinMax_handler(FwIndexType portNum,
                                     const Svc::StateBufferStoreCfg::StateEntry& entry,
                                     Svc::SbsMeasurement& min,
                                     Svc::SbsMeasurement& max) override;

    //! Handler implementation for clearMinMax
    Svc::SbsStatus clearMinMax_handler(FwIndexType portNum, const Svc::StateBufferStoreCfg::StateEntry& entry) override;

    //! Handler implementation for clearAndGetMinMax
    Svc::SbsStatus clearAndGetMinMax_handler(FwIndexType portNum,
                                             const Svc::StateBufferStoreCfg::StateEntry& entry,
                                             Svc::SbsMeasurement& min,
                                             Svc::SbsMeasurement& max) override;

    //! Handler implementation for getHistory
    Svc::SbsStatus getHistory_handler(FwIndexType portNum,
                                      const Svc::StateBufferStoreCfg::StateEntry& entry,
                                      Fw::Buffer& data,
                                      FwSizeType& sizeOut) override;

    //! Handler implementation for getNHistory
    Svc::SbsStatus getNHistory_handler(FwIndexType portNum,
                                       const Svc::StateBufferStoreCfg::StateEntry& entry,
                                       U16 numMeasurements,
                                       Fw::Buffer& data,
                                       FwSizeType& sizeOut) override;

    // ----------------------------------------------------------------------
    // Handler implementations for commands
    // ----------------------------------------------------------------------

    //! Handler implementation for command REPORT_WATERMARKS
    void REPORT_WATERMARKS_cmdHandler(FwOpcodeType opCode,
                                      U32 cmdSeq,
                                      const Svc::StateBufferStoreCfg::StateEntry& entry) override;

    //! Handler implementation for command CLEAR_WATERMARKS
    void CLEAR_WATERMARKS_cmdHandler(FwOpcodeType opCode,
                                     U32 cmdSeq,
                                     const Svc::StateBufferStoreCfg::StateEntry& entry) override;

    // ----------------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------------

    //! Validate an entry index and return its control block
    Entry& checkedEntry(const Svc::StateBufferStoreCfg::StateEntry& entry);

    //! Fix an entry's kind on its first store
    //!
    //! Returns true when the entry is, or has just become, of the given kind.
    static bool claimKind(Entry& entry, EntryKind kind);

    //! Mark an entry as being written, making its counter odd
    static void beginWrite(Entry& entry);

    //! Mark an entry's write complete, making its counter even again
    static void endWrite(Entry& entry);

    //! Sample the coherency counter at the start of a read attempt
    //!
    //! Not static so that unit tests can force a torn read: nothing in a
    //! single-threaded test can interleave a write. See docs/sdd.md.
    U32 beginRead(Entry& entry);

    //! Report whether the read attempt begun with before saw a coherent entry
    static bool endRead(const Entry& entry, U32 before);

    //! Update an entry's watermarks with a newly stored value, within a write
    static void updateWatermarks(Entry& entry,
                                 const Fw::PolyType& val,
                                 const Fw::Time& time,
                                 const SbsStatus& validity);

    //! Read an entry's watermarks coherently, optionally clearing them
    //!
    //! With clear set, the watermarks reported are exactly those that the
    //! clear ends, even if a put consumes the clear before they are read.
    SbsStatus readWatermarks(Entry& entry,
                             bool clear,
                             StateBufferStore_ReadOperation operation,
                             Svc::SbsMeasurement& min,
                             Svc::SbsMeasurement& max);

    //! Fill an SbsMeasurement from an internal measurement, honoring a clear
    static void reportWatermark(const Measurement& source, bool cleared, Svc::SbsMeasurement& out);

    //! Widen a stored value to F64 for event reporting
    //!
    //! Events cannot carry an Fw::PolyType, so watermark reporting narrows to
    //! F64. Integer magnitudes above 2^53 are reported imprecisely; the port
    //! interface reports them exactly.
    static F64 toF64(const Fw::PolyType& value);

    //! Whether a PolyType holds a value of any type
    static bool isTyped(const Fw::PolyType& value);

    //! Reduce a time to microseconds for event reporting
    static I64 toMicroseconds(const Fw::Time& time);

    //! Coherently serialize the most recent count measurements of an entry, oldest first
    SbsStatus readHistory(Entry& entry,
                          FwSizeType count,
                          StateBufferStore_ReadOperation operation,
                          Fw::Buffer& data,
                          FwSizeType& sizeOut);

    // ----------------------------------------------------------------------
    // Member variables
    // ----------------------------------------------------------------------

    //! Per-entry control blocks
    Entry m_entries[Svc::StateBufferStoreCfg::StateEntry::NUM_ENTRIES];

    //! Base of the single history allocation
    void* m_memPtr;

    //! Size of the history allocation, as actually provided
    FwSizeType m_memSize;

    //! Identifier the history was allocated under, retained for deallocation
    FwEnumStoreType m_memId;

    //! Allocator that provided m_memPtr, retained for deallocation
    Fw::MemAllocator* m_allocator;

    //! Whether configure() has run
    bool m_initialized;

#ifdef BUILD_UT
  public:
    //! Test-only hook forcing the next this many read attempts to report a
    //! torn entry, exercising the retry and INCOHERENT paths
    U32 m_utForceTornReads = 0;

    //! Test-only hook run once at the start of the next read attempt, letting
    //! a test store a value between a clear and the read that follows it
    void (*m_utBeforeRead)(void* context) = nullptr;

    //! Context passed to m_utBeforeRead
    void* m_utBeforeReadContext = nullptr;
#endif
};

}  // namespace Svc

#endif
