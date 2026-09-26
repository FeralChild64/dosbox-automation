# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

"""VIEW.COM, the text and hex viewer (ada-dswu), copied to D: with its test files.

VIEW_COM names the binary; the default is the copy on the bundled Y: drive.
Attributes are read from video memory, because screen_text carries only characters.
"""

import os
import secrets
import shutil
import time
from pathlib import Path

import pytest

from conftest import WORKSPACE

VIEW_COM = Path(os.environ.get(
    "VIEW_COM",
    Path(__file__).resolve().parents[2] / "resources" / "drives" / "y" / "dos" / "view.com"))

COLOUR_TEXT = 0xB8000
MONO_TEXT = 0xB0000


def write_test_files(target: Path):
    (target / "SHORT.TXT").write_bytes(
        b"VIEW test file, short.\r\n"
        b"Col\tafter one tab\t\tafter two more\r\n"
        + b"0" * 80 + b"\r\n"
        b"Last line without a line end.")
    (target / "BIG.TXT").write_bytes(b"".join(
        b"Line %05d of 01100. The quick brown fox jumps over the lazy dog.\r\n" % i
        for i in range(1, 1101)))
    long_lines = b"Short line before the long ones.\r\n"
    for i in range(1, 4):
        long_lines += b"Long line %d: " % i + b"".join(
            b"col%04d " % (j * 8) for j in range(30)) + b"\r\n"
    long_lines += b"".join(b"x%07d" % (j * 8) for j in range(625))
    long_lines += b"\r\nShort line after the long ones.\r\n"
    (target / "LONG.TXT").write_bytes(long_lines)
    (target / "LF.TXT").write_bytes(b"first\nsecond\nthird\n")
    (target / "CTRLZ.TXT").write_bytes(b"visible line\r\n\x1ahidden after Ctrl-Z\r\n")
    (target / "BIN.DAT").write_bytes(bytes(range(256)) * 2)


@pytest.fixture
def view_env(dosbox_e2e):
    """Start an engine with the test files and VIEW.COM on D:."""
    if not VIEW_COM.is_file():
        pytest.fail(f"VIEW.COM missing at {VIEW_COM}")

    def _start(settings=None, extra_lines=()):
        work_dir = WORKSPACE / f"view-{secrets.token_hex(4)}"
        work_dir.mkdir(parents=True)
        write_test_files(work_dir)
        shutil.copy2(VIEW_COM, work_dir / "VIEW.COM")
        inst = dosbox_e2e(
            work_dir=work_dir, settings=settings,
            autoexec_lines=[f"mount d {work_dir}", "d:", *extra_lines, "ECHO MARKER"])
        wait_text(inst.client, "\nMARKER\n")
        return inst.client

    return _start


def screen(client) -> str:
    response = client.screen_text()
    assert response.status_code == 200
    # rows come padded to the screen width; stripped, "\nX\n" means a line of just X
    return "\n".join(line.rstrip() for line in response.json()["text"].split("\n"))


def wait_text(client, needle, timeout=15.0, absent=False):
    deadline = time.monotonic() + timeout
    text = ""
    while time.monotonic() < deadline:
        text = screen(client)
        if (needle not in text) if absent else (needle in text):
            return text
        time.sleep(0.2)
    state = "still there" if absent else "not found"
    raise AssertionError(f"{needle!r} {state}:\n{text}")


def wait_for(check, what, timeout=10.0):
    """Poll check() for things the status line does not signal, like attributes."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {what}")


def rows(client):
    return screen(client).split("\n")


def send(post, what, timeout=5.0):
    """The engine answers 409 while an earlier timed sequence still has events pending."""
    deadline = time.monotonic() + timeout
    while True:
        response = post()
        if response.status_code != 409 or time.monotonic() > deadline:
            break
        time.sleep(0.05)
    assert response.status_code == 200, f"{what}: {response.status_code} {response.text}"


def type_text(client, text):
    send(lambda: client.type_string(text), f"typing {text!r}")


def key(client, name, *modifiers):
    events = [{"type": "key", "key": m, "pressed": True} for m in modifiers]
    events += [{"type": "key", "key": name, "pressed": True},
               {"type": "key", "key": name, "pressed": False}]
    events += [{"type": "key", "key": m, "pressed": False} for m in reversed(modifiers)]
    send(lambda: client.input_sequence(events), f"key {name}")


def run_view(client, args, expect):
    """VIEW draws its status line last, so expect on it means a complete screen."""
    type_text(client, f"view {args}\n")
    return wait_text(client, expect)


def quit_view(client, quit_key="KBD_q"):
    """The restore runs top to bottom; the key bar on the last row goes last."""
    key(client, quit_key)
    return wait_text(client, "10Quit", absent=True)


def attrs(client, base, row, first, count, cols=80):
    response = client.memory_read(base + (row * cols + first) * 2, count * 2)
    assert response.status_code == 200
    return list(response.content[1::2])


def cell_chars(client, base, row, first, count, cols=80):
    response = client.memory_read(base + (row * cols + first) * 2, count * 2)
    assert response.status_code == 200
    return bytes(response.content[0::2])


def test_short_file_shows_and_q_restores_the_screen(view_env):
    client = view_env()
    text = run_view(client, "short.txt", "Line 001 / 004")

    assert "VIEW test file, short." in text
    assert "Col     after one tab           after two more" in text
    assert "File: short.txt" in text
    assert " 1Help" in text and " 3Quit" in text and "10Quit" in text

    text = quit_view(client)
    assert "\nMARKER\n" in text
    assert "VIEW.COM 1.0 - simple text viewer" in text


@pytest.mark.parametrize("quit_key", ["KBD_esc", "KBD_f3", "KBD_f10"])
def test_other_quit_keys(view_env, quit_key):
    client = view_env()
    run_view(client, "short.txt", "Line 001 / 004")
    assert "\nMARKER\n" in quit_view(client, quit_key)


def test_big_file_pages_back_from_the_end(view_env):
    client = view_env()
    run_view(client, "big.txt", "Line 0001 / 1100")

    key(client, "KBD_end")
    wait_text(client, "Line 1078 / 1100")
    assert rows(client)[1].startswith("Line 01078 of 01100.")
    assert rows(client)[23].startswith("Line 01100 of 01100.")

    key(client, "KBD_pageup")
    key(client, "KBD_pageup")
    wait_text(client, "Line 1032 / 1100")
    assert rows(client)[1].startswith("Line 01032 of 01100.")


def test_long_line_marker_and_sideways_scroll(view_env):
    client = view_env()
    run_view(client, "long.txt", "Line 001 / 006")

    assert cell_chars(client, COLOUR_TEXT, 2, 79, 1) == b"\xaf"
    assert attrs(client, COLOUR_TEXT, 2, 79, 1) == [0x0B]
    assert cell_chars(client, COLOUR_TEXT, 1, 79, 1) == b" "

    for _ in range(3):
        key(client, "KBD_right")
    wait_text(client, "Col 24")
    assert rows(client)[2].startswith("0008 col0016 col0024")

    key(client, "KBD_home")
    wait_text(client, "Col 24", absent=True)
    assert rows(client)[2].startswith("Long line 1: col0000")


def test_binary_opens_in_hex_past_its_ctrl_z(view_env):
    client = view_env()
    text = run_view(client, "bin.dat", "Byte 000 / 512")

    lines = text.split("\n")
    assert lines[1].startswith("00000000  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F")
    assert lines[2].startswith("00000010  10 11 12 13 14 15 16 17 18 19 1A 1B")
    assert lines[3][59:75] == " !\"#$%&'()*+,-./"


def test_h_toggles_hex_on_a_text_file(view_env):
    client = view_env()
    run_view(client, "short.txt", "Line 001 / 004")

    key(client, "KBD_h")
    text = wait_text(client, "Byte 000 / 170")
    assert "00000000  56 49 45 57" in text

    key(client, "KBD_h")
    wait_text(client, "Line 001 / 004")


def test_hex_option_forces_hex(view_env):
    client = view_env()
    run_view(client, "short.txt /h", "Byte 000 / 170")


def test_search_highlights_and_navigates(view_env):
    client = view_env()
    run_view(client, "big.txt", "Line 0001 / 1100")

    type_text(client, "/")
    wait_text(client, "Search:")
    type_text(client, "quick\n")
    # the first search starts below the top line, so line 2 holds the current hit
    wait_for(lambda: attrs(client, COLOUR_TEXT, 2, 25, 5) == [0x4F] * 5, "first hit")
    assert attrs(client, COLOUR_TEXT, 1, 25, 5) == [0x0E] * 5
    assert attrs(client, COLOUR_TEXT, 2, 24, 1) == [0x07]

    type_text(client, "n")
    wait_for(lambda: attrs(client, COLOUR_TEXT, 3, 25, 5) == [0x4F] * 5, "current hit on row 3")
    assert attrs(client, COLOUR_TEXT, 2, 25, 5) == [0x0E] * 5

    type_text(client, "P")
    wait_text(client, "Line 1078 / 1100")
    assert attrs(client, COLOUR_TEXT, 23, 25, 5) == [0x4F] * 5

    type_text(client, "x")
    wait_for(lambda: attrs(client, COLOUR_TEXT, 23, 25, 5) == [0x07] * 5, "highlight cleared")


def test_hex_search_takes_bytes(view_env):
    client = view_env()
    run_view(client, "bin.dat", "Byte 000 / 512")

    type_text(client, "/")
    wait_text(client, "Search:")
    type_text(client, "4d 4e\n")
    # 4Dh sits at offset 4Dh: row 4 of the hex view, byte 13
    wait_for(lambda: attrs(client, COLOUR_TEXT, 5, 10 + 3 * 13, 2) == [0x4F, 0x4F], "hex hit")


def test_not_found_keeps_the_position(view_env):
    client = view_env()
    run_view(client, "big.txt", "Line 0001 / 1100")
    key(client, "KBD_pagedown")
    wait_text(client, "Line 0024 / 1100")

    type_text(client, "/")
    wait_text(client, "Search:")
    type_text(client, "zzzz\n")
    # keys queue in the BIOS buffer, so Down is handled only after the search returned
    key(client, "KBD_down")
    wait_text(client, "Line 0025 / 1100")


def test_ctrl_l_with_a_missing_file_keeps_the_current_one(view_env):
    client = view_env()
    run_view(client, "short.txt", "Line 001 / 004")

    key(client, "KBD_l", "KBD_leftctrl")
    wait_text(client, "Load:")
    type_text(client, "nosuch.txt\n")
    wait_text(client, "Cannot open nosuch.txt")
    key(client, "KBD_space")
    text = wait_text(client, "File: short.txt")
    assert "VIEW test file, short." in text


def test_ctrl_l_refuses_a_device(view_env):
    client = view_env()
    run_view(client, "short.txt", "Line 001 / 004")

    key(client, "KBD_l", "KBD_leftctrl")
    wait_text(client, "Load:")
    type_text(client, "con\n")
    wait_text(client, "Cannot open con")


def test_ctrl_l_opens_another_file(view_env):
    client = view_env()
    run_view(client, "short.txt", "Line 001 / 004")

    key(client, "KBD_l", "KBD_leftctrl")
    wait_text(client, "Load:")
    type_text(client, "lf.txt\n")
    text = wait_text(client, "Line 001 / 003")
    assert "File: lf.txt" in text


def test_help_and_back(view_env):
    client = view_env()
    run_view(client, "short.txt", "Line 001 / 004")

    key(client, "KBD_f1")
    wait_text(client, "VIEW 1.0 help - any key returns")
    wait_text(client, "SJ-ACC-0650")
    key(client, "KBD_space")
    wait_text(client, "Line 001 / 004")


def test_ctrl_z_ends_the_text(view_env):
    client = view_env()
    text = run_view(client, "ctrlz.txt", "Line 001 / 001")
    assert "visible line" in text
    assert "hidden after" not in text


def test_fifty_lines(view_env):
    client = view_env(extra_lines=["MODE CON LINES=50"])
    run_view(client, "big.txt", "Line 0001 / 1100")
    assert cell_chars(client, COLOUR_TEXT, 49, 0, 6) == b" 1Help"
    key(client, "KBD_end")
    wait_text(client, "Line 1053 / 1100")


def test_hercules_uses_the_mono_table(view_env):
    client = view_env(settings={"machine": "hercules"})
    run_view(client, "big.txt", "Line 0001 / 1100")

    assert attrs(client, MONO_TEXT, 0, 0, 1) == [0x70]
    assert attrs(client, MONO_TEXT, 1, 0, 1) == [0x07]
    assert attrs(client, MONO_TEXT, 24, 0, 2) == [0x07, 0x07]
    assert attrs(client, MONO_TEXT, 24, 2, 1) == [0x70]


def test_ega_takes_its_rows_from_the_bios(view_env):
    # 43 lines: the 25-row fallback would put the key bar and End elsewhere
    client = view_env(settings={"machine": "ega"}, extra_lines=["MODE CON LINES=43"])
    run_view(client, "big.txt", "Line 0001 / 1100")

    assert attrs(client, COLOUR_TEXT, 0, 0, 1) == [0x1F]
    assert cell_chars(client, COLOUR_TEXT, 42, 0, 6) == b" 1Help"
    key(client, "KBD_end")
    wait_text(client, "Line 1060 / 1100")


def test_cga_uses_the_colour_table(view_env):
    client = view_env(settings={"machine": "cga"})
    run_view(client, "big.txt", "Line 0001 / 1100")

    assert attrs(client, COLOUR_TEXT, 0, 0, 1) == [0x1F]
    assert attrs(client, COLOUR_TEXT, 0, 6, 1) == [0x1E]
    assert attrs(client, COLOUR_TEXT, 24, 2, 1) == [0x30]


@pytest.mark.parametrize("args, code", [
    ("", 1), ("/x f", 1), ("nosuch.txt", 2), ("con", 2), ("/?", 0), ("short.txt /h /m /?", 0),
])
def test_exit_codes(view_env, args, code):
    client = view_env()
    type_text(client, f"view {args}\n")
    for level in (3, 2, 1):
        type_text(client, f"if errorlevel {level} echo EL{level}\n")
    type_text(client, "echo DONE\n")
    text = wait_text(client, "\nDONE\n", timeout=30)
    shown = [level for level in (3, 2, 1) if f"\nEL{level}" in text]
    assert (max(shown) if shown else 0) == code, text


def test_a_cancelled_prompt_keeps_the_search(view_env):
    client = view_env()
    run_view(client, "big.txt", "Line 0001 / 1100")
    type_text(client, "/")
    wait_text(client, "Search:")
    type_text(client, "quick\n")
    wait_for(lambda: attrs(client, COLOUR_TEXT, 2, 25, 5) == [0x4F] * 5, "first hit")

    type_text(client, "/")
    wait_text(client, "Search:")
    key(client, "KBD_esc")
    wait_text(client, "Search:", absent=True)
    type_text(client, "n")
    wait_for(lambda: attrs(client, COLOUR_TEXT, 3, 25, 5) == [0x4F] * 5, "next hit after Esc")


def test_a_hit_under_the_right_edge_scrolls_into_view(view_env):
    client = view_env()
    run_view(client, "long.txt", "Line 001 / 006")
    type_text(client, "/")
    wait_text(client, "Search:")
    # "col0064" starts at column 77 of the long line: its tail would sit under the marker
    type_text(client, "col0064\n")
    text = wait_text(client, "Col 72")
    assert text.split("\n")[2].startswith("0056 col0064")

