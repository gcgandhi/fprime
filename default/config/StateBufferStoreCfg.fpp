# ======================================================================
# FPP file for StateBufferStore configuration
# ======================================================================

module Svc {

  module StateBufferStoreCfg {

    @ Define a set of StateBufferStore entries on a project-specific
    @ basis. Each entry is one stored state: latest value, min/max
    @ watermarks, and a circular measurement history.
    @
    @ NUM_ENTRIES is a sizing counter, not a usable entry: FPP has no
    @ equivalent of the generated C++ NUM_CONSTANTS available for array
    @ sizing, so the count must be an explicit last member. Passing
    @ NUM_ENTRIES to any StateBufferStore port is a programming error and
    @ trips an assertion.
    enum StateEntry: U32 {
      @ Entry 0
      SBS_ENTRY_00
      @ Entry 1
      SBS_ENTRY_01
      @ Entry 2
      SBS_ENTRY_02
      @ Entry 3
      SBS_ENTRY_03
      @ Entry 4
      SBS_ENTRY_04
      @ Entry 5
      SBS_ENTRY_05
      @ Entry 6
      SBS_ENTRY_06
      @ Entry 7
      SBS_ENTRY_07
      @ Entry 8
      SBS_ENTRY_08
      @ Entry 9
      SBS_ENTRY_09
      @ REQUIRED: Counter, leave as last element. Sizes the entry tables.
      NUM_ENTRIES
    }

    @ Upper bound on any entry's history depth. Bounds the history read
    @ interfaces and sizes the worst-case allocation.
    constant MAX_HISTORY_DEPTH = 16

    @ Smallest history depth an entry may be configured with, the floor set
    @ by REQ-STATEBUFFERSTORE-010
    constant MIN_HISTORY_DEPTH = 2

    @ Typical history depth for an entry
    constant DEFAULT_HISTORY_DEPTH = 4

    @ Per-entry history depth, one element per StateEntry. Each element must
    @ be in [MIN_HISTORY_DEPTH, MAX_HISTORY_DEPTH]; StateBufferStore asserts
    @ this at configuration time. The sample sets the last two entries to the
    @ bounds to show that depths are independent.
    array HistoryDepths = [StateEntry.NUM_ENTRIES] FwSizeType \
      default [
        DEFAULT_HISTORY_DEPTH
        DEFAULT_HISTORY_DEPTH
        DEFAULT_HISTORY_DEPTH
        DEFAULT_HISTORY_DEPTH
        DEFAULT_HISTORY_DEPTH
        DEFAULT_HISTORY_DEPTH
        DEFAULT_HISTORY_DEPTH
        DEFAULT_HISTORY_DEPTH
        MIN_HISTORY_DEPTH
        MAX_HISTORY_DEPTH
      ]

    @ Maximum size in bytes of a single string, struct, or byte-array
    @ measurement. Sizes the payload region of every entry's history.
    constant MAX_DATA_SIZE = 256

    @ Number of times a reader retries a torn read before giving up,
    @ emitting a FATAL event, and reporting SbsStatus.INCOHERENT.
    constant MAX_READ_ITERATIONS = 2

  }

}
