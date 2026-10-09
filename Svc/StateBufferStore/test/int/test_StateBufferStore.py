"""test_StateBufferStore.py:

Reusable integration tests for Svc.StateBufferStore's ground interface: the
REPORT_WATERMARKS, CLEAR_WATERMARKS, and RESET_THROTTLES commands and the
events they emit.

No deployment in this repository instantiates the component yet, so these
tests only run once a topology does. Map it in the deployment's
int_config.json (see docs/user-manual/gds/reusable-integration-tests.md):

    "Svc.StateBufferStore": "<instance mnemonic>",
    "Svc.StateBufferStore.quietEntry": "<value entry nothing writes during the test>",
    "Svc.StateBufferStore.dataEntry": "<entry already stored through putData before the tests run>"

Tests that need quietEntry or dataEntry are skipped when it is not mapped. An
entry's kind is fixed by its first store, so dataEntry must have been written
through putData before these tests run, or the watermark commands accept it.
"""

import pytest

COMPONENT = "Svc.StateBufferStore"
QUIET_ENTRY_KEY = COMPONENT + ".quietEntry"
DATA_ENTRY_KEY = COMPONENT + ".dataEntry"
NOT_AN_ENTRY = "NUM_ENTRIES"
# Seconds to wait for each command's events over the GDS link
TIMEOUT = 5


def _config_value(fprime_test_api, key, default=None):
    """Read an optional value from the deployment configuration

    get_mnemonic returns the key itself when the configuration lacks it.
    """
    value = fprime_test_api.get_mnemonic(key)
    return default if value == key else value


def _sbs(fprime_test_api):
    return fprime_test_api.get_mnemonic(COMPONENT)


def _completed(fprime_test_api, command):
    cmd_disp = fprime_test_api.get_mnemonic("Svc.CommandDispatcher")
    opcode = fprime_test_api.translate_command_name(command)
    return fprime_test_api.get_event_pred(cmd_disp + ".OpCodeCompleted", [opcode])


def _failed(fprime_test_api, command, response):
    cmd_disp = fprime_test_api.get_mnemonic("Svc.CommandDispatcher")
    opcode = fprime_test_api.translate_command_name(command)
    return fprime_test_api.get_event_pred(
        cmd_disp + ".OpCodeError", [opcode, response]
    )


def test_reset_throttles(fprime_test_api):
    """RESET_THROTTLES completes and reports ThrottlesReset

    Covers: REQ-STATEBUFFERSTORE-018
    """
    sbs = _sbs(fprime_test_api)
    command = sbs + ".RESET_THROTTLES"
    fprime_test_api.send_and_assert_event(
        command,
        [],
        [
            fprime_test_api.get_event_pred(sbs + ".ThrottlesReset"),
            _completed(fprime_test_api, command),
        ],
        timeout=TIMEOUT,
    )


def test_clear_then_report_watermarks(fprime_test_api):
    """A cleared entry reports its watermarks as never written

    CLEAR_WATERMARKS reports WatermarksCleared; a following REPORT_WATERMARKS
    reports both watermarks zeroed with NOT_WRITTEN validity, since nothing
    stores to the quiet entry in between. Ground cannot store a value, so this
    exercises the command interfaces only; recording and reseeding are unit
    tested.

    Covers: REQ-STATEBUFFERSTORE-005 (event report interface),
    REQ-STATEBUFFERSTORE-006 (clear command interface)
    """
    entry = _config_value(fprime_test_api, QUIET_ENTRY_KEY)
    if entry is None:
        pytest.skip(QUIET_ENTRY_KEY + " is not mapped in the deployment configuration")

    sbs = _sbs(fprime_test_api)

    clear = sbs + ".CLEAR_WATERMARKS"
    fprime_test_api.send_and_assert_event(
        clear,
        [entry],
        [
            fprime_test_api.get_event_pred(sbs + ".WatermarksCleared", [entry]),
            _completed(fprime_test_api, clear),
        ],
        timeout=TIMEOUT,
    )

    report = sbs + ".REPORT_WATERMARKS"
    fprime_test_api.send_and_assert_event(
        report,
        [entry],
        [
            fprime_test_api.get_event_pred(
                sbs + ".WatermarkReport",
                [entry, 0.0, 0, "NOT_WRITTEN", 0.0, 0, "NOT_WRITTEN"],
            ),
            _completed(fprime_test_api, report),
        ],
        timeout=TIMEOUT,
    )


@pytest.mark.parametrize(
    "command_name, rejected_event",
    [
        ("REPORT_WATERMARKS", "ReportWatermarksRejected"),
        ("CLEAR_WATERMARKS", "ClearWatermarksRejected"),
    ],
)
def test_not_an_entry_rejected(fprime_test_api, command_name, rejected_event):
    """Naming the NUM_ENTRIES sizing counter is rejected with VALIDATION_ERROR

    Covers: REQ-STATEBUFFERSTORE-019
    """
    sbs = _sbs(fprime_test_api)
    command = sbs + "." + command_name
    fprime_test_api.send_and_assert_event(
        command,
        [NOT_AN_ENTRY],
        [
            fprime_test_api.get_event_pred(
                sbs + "." + rejected_event, [NOT_AN_ENTRY, "NOT_AN_ENTRY"]
            ),
            _failed(fprime_test_api, command, "VALIDATION_ERROR"),
        ],
        timeout=TIMEOUT,
    )


@pytest.mark.parametrize(
    "command_name, rejected_event",
    [
        ("REPORT_WATERMARKS", "ReportWatermarksRejected"),
        ("CLEAR_WATERMARKS", "ClearWatermarksRejected"),
    ],
)
def test_data_entry_rejected(fprime_test_api, command_name, rejected_event):
    """A data entry has no watermarks, so watermark commands fail with EXECUTION_ERROR

    Covers: REQ-STATEBUFFERSTORE-016
    """
    entry = _config_value(fprime_test_api, DATA_ENTRY_KEY)
    if entry is None:
        pytest.skip(DATA_ENTRY_KEY + " is not mapped in the deployment configuration")

    sbs = _sbs(fprime_test_api)
    command = sbs + "." + command_name
    fprime_test_api.send_and_assert_event(
        command,
        [entry],
        [
            fprime_test_api.get_event_pred(
                sbs + "." + rejected_event, [entry, "DATA_ENTRY"]
            ),
            _failed(fprime_test_api, command, "EXECUTION_ERROR"),
        ],
        timeout=TIMEOUT,
    )
