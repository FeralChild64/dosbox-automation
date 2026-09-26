# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

"""TTF output's screen font check across text mode changes.

TTF output switches itself off when a program loads its own font, and
back on when the font returns to the ROM one. A change between 25 and 50
lines schedules the VGA resize half a frame later. Before the fix for
ada-rsn9, a frame end inside that half frame ran the check on the new
font at the old character height: a false "altered", then "restored" in
the same millisecond.

tests/files/ttf/ttfphase.com places the switches at a known point in the
frame. Late in the frame a frame end always falls inside the pending
resize; at the frame top it never does. The glyph test is the control: a
real custom font must still be detected, so a check that never fires
cannot pass the others.
"""

import shutil
import time
from pathlib import Path

import pytest
import requests

FIXTURES = Path(__file__).resolve().parents[1] / "files" / "ttf"

ALTERED = "Screen font altered"
RESTORED = "Screen font restored"

# Rows minus one, as the BIOS keeps it at 0040:0084
ROWS_25 = 24

PHASE_DONE = "ttfphase: 8 round trips"


def start_ttf_instance(dosbox_e2e, conf_dir):
    for name in ("ttfphase.com", "fonta.com"):
        shutil.copy2(FIXTURES / name, conf_dir / name)
    instance = dosbox_e2e(
        autoexec_lines=[f"mount d {conf_dir}"],
        conf_dir=conf_dir,
        settings={"ttf_output": "true", "keyboard_layout": "us"},
    )
    instance.client.wait_shell(timeout=15)
    # Same settle time as the other typing tests before the first input
    time.sleep(2.1)
    return instance


def run_script(instance, source, name, timeout=120.0):
    client = instance.client
    try:
        assert client.script_load(source, name=name).status_code == 200
        assert client.script_start().status_code == 200
        data = client.wait_script_done(timeout=timeout)
    except requests.exceptions.ConnectionError:
        exit_code = instance.proc.poll()
        if exit_code is None:
            raise
        pytest.fail(f"Engine exited with {exit_code} during '{name}':\n"
                    f"{instance.capture_thread.get_output()[-3000:]}")
    assert data["state"] == "completed", (
        f"Script did not complete: state={data['state']}, "
        f"error={data.get('error', '')}"
    )
    return data.get("output", {})


def wait_for_log(instance, text, count, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if instance.capture_thread.get_output().count(text) >= count:
            return
        time.sleep(0.1)
    log = instance.capture_thread.get_output()
    pytest.fail(f"'{text}' seen {log.count(text)} times, expected {count}:\n"
                f"{log[-3000:]}")


LUA_HELPERS = """
local function run(cmd, rows_minus_one)
    dosbox.type(cmd)
    dosbox.key('KBD_enter', true)
    dosbox.key('KBD_enter', false)
    for _ = 1, 600 do
        if dosbox.mem_read_byte(0x40, 0x84) == rows_minus_one then
            -- let several frames end at the new geometry
            dosbox.wait_frames(10)
            return
        end
        dosbox.wait_frames(1)
    end
    dosbox.abort('rows never became ' .. (rows_minus_one + 1) .. ' after ' .. cmd)
end
"""


@pytest.mark.parametrize("phase_arg", ["", " e"], ids=["late", "early"])
def test_line_count_changes_do_not_report_a_font_change(dosbox_e2e, tmp_path,
                                                        phase_arg):
    instance = start_ttf_instance(dosbox_e2e, tmp_path)

    output = run_script(instance, f"""
dosbox.type('d:\\\\ttfphase{phase_arg}')
dosbox.key('KBD_enter', true)
dosbox.key('KBD_enter', false)
dosbox.output.done = dosbox.wait_for_text('{PHASE_DONE}', 1200)
dosbox.output.rows = dosbox.mem_read_byte(0x40, 0x84) + 1
""", name="ttf-line-counts")
    assert output.get("done") is True, "ttfphase did not finish"
    assert output.get("rows") == 25

    log = instance.capture_thread.get_output()
    assert log.count(ALTERED) == 0, log[-3000:]
    assert log.count(RESTORED) == 0, log[-3000:]


def test_custom_glyph_is_still_detected(dosbox_e2e, tmp_path):
    instance = start_ttf_instance(dosbox_e2e, tmp_path)

    # One script: the server accepts a script load every 2 s at most
    run_script(instance, LUA_HELPERS + f"""
dosbox.type('d:\\\\fonta')
dosbox.key('KBD_enter', true)
dosbox.key('KBD_enter', false)
dosbox.wait_frames(30)
run('mode co80', {ROWS_25})
""", name="ttf-custom-glyph")
    wait_for_log(instance, ALTERED, 1)
    wait_for_log(instance, RESTORED, 1)

    log = instance.capture_thread.get_output()
    assert log.count(ALTERED) == 1, log[-3000:]
    assert log.index(ALTERED) < log.index(RESTORED), log[-3000:]
