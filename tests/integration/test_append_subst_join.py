# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

"""APPEND, SUBST and JOIN driven by batch lines against Commander Keen 4.

The fixture installs the unmodified shareware package (tests/files/disks/
commander-keen-4-shareware/4keen.zip) through its own INSTALL.EXE. KEEN4_ARCHIVE
may point at one of the other known Keen 4 archives instead."""

import hashlib
import os
import shutil
import time
import zipfile
from pathlib import Path

import pytest
from conftest import WORKSPACE, start_dosbox_instance

KEEN4_PACKAGE = (Path(__file__).resolve().parents[1] / "files" / "disks"
                 / "commander-keen-4-shareware" / "4keen.zip")

# sha256 of the archive -> (how to get the game, folder inside the archive)
KNOWN_ARCHIVES = {
    # classicdosgames.com 4keen.zip: Apogee's v1.4 shareware package
    "f7c63feb8bbec817646d95aa26313df1654c01eda8f7ccc5cfd0c0bc0b5e305e": ("install", ""),
    # dosgames.com DOSBOX_KEEN4.ZIP: v1.4 already installed, 2018 repack
    "af49c2967969e89a8a4da8e5bf7b8bbc570522b81295696e7f5c1da37996b3c0": ("extract", ""),
    # archive.org msdos_Commander_Keen_4: an earlier release, game files only
    "8029ad83871c67585b187aa679e57a057fe8fd0097dc4186b558f08b9367bc4e": ("extract", "CKeen4"),
}

KEEN4E_V14_SHA256 = "bdbb46746e02c0dddcba9b24fa464c26cc7e3514a3b3931267c8c17b9fb9c405"

SETTINGS = {"keyboard_layout": "us", "cpu_cycles": "12000"}


def sha256_of(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def extract_with_dates(archive, target):
    """zipfile drops the stored times; DOS programs and our checks want them."""
    with zipfile.ZipFile(archive) as zf:
        for info in zf.infolist():
            path = Path(zf.extract(info, target))
            stamp = time.mktime((*info.date_time, 0, 0, -1))
            os.utime(path, (stamp, stamp))


def screen(client):
    r = client.screen_text()
    assert r.status_code == 200, r.text
    return r.json()


def wait_for_text(client, needle, timeout=30.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = screen(client).get("text", "")
        if needle in text:
            return text
        time.sleep(0.5)
    raise TimeoutError(f"{needle!r} not on screen:\n{screen(client).get('text', '')}")


def press_enter(client):
    for _ in range(20):
        r = client.type_string("\n")
        if r.status_code != 409:
            assert r.status_code == 200, r.text
            return
        time.sleep(0.25)
    raise TimeoutError("input queue stayed busy")


def install_from_package(package, work):
    src = work / "src"
    target = work / "c"
    src.mkdir(parents=True)
    target.mkdir()
    extract_with_dates(package, src)
    inst = start_dosbox_instance(
        work, [f"mount c {target}", f"mount d {src}", "d:", "install"],
        conf_dir=work, settings=dict(SETTINGS))
    try:
        client = inst.client
        wait_for_text(client, "Distributed by Apogee Software")
        press_enter(client)
        wait_for_text(client, "Install Commander Keen")
        press_enter(client)
        wait_for_text(client, "C:\\KEEN")
        press_enter(client)
        wait_for_text(client, "Thanks for installing", timeout=120)
        for _ in range(5):
            if client.program_state().json().get("is_shell"):
                break
            press_enter(client)
            time.sleep(1.0)
        client.wait_shell(timeout=15)
    finally:
        inst.shutdown()
    game = target / "KEEN"
    assert sha256_of(game / "KEEN4E.EXE") == KEEN4E_V14_SHA256
    return game


@pytest.fixture(scope="module")
def keen4_dir():
    archive = Path(os.environ.get("KEEN4_ARCHIVE", KEEN4_PACKAGE))
    if not archive.is_file():
        pytest.skip(f"Keen 4 archive not found: {archive}")
    digest = sha256_of(archive)
    if digest not in KNOWN_ARCHIVES:
        pytest.fail(f"unknown Keen 4 archive {archive} (sha256 {digest})")
    how, folder = KNOWN_ARCHIVES[digest]

    work = WORKSPACE / f"keen4-{digest[:12]}"
    if work.exists():
        shutil.rmtree(work)
    if how == "install":
        return install_from_package(archive, work)
    extract_with_dates(archive, work)
    return work / folder if folder else work


def start_with_batch(dosbox_e2e, tmp_path, keen4_dir, lines):
    drive_c = tmp_path / "c"
    shutil.copytree(keen4_dir, drive_c / "KEEN")
    (drive_c / "RUN").mkdir()
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {drive_c}", "c:", "cd \\RUN", "cls", *lines],
        conf_dir=tmp_path, settings=dict(SETTINGS))
    return inst.client, drive_c


def wait_for_keen_graphics(client, timeout=20.0):
    """Keen 4 opens its data, waits on a text screen for a key, then draws."""
    wait_for_text(client, "Ready - Press a Key", timeout=timeout)
    press_enter(client)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = screen(client)
        if not state.get("is_text_mode", True):
            return state
        time.sleep(0.5)
    pytest.fail(f"Keen never left text mode:\n{screen(client).get('text', '')}")


def shell_screen(client):
    client.wait_shell(timeout=20)
    time.sleep(1.0)
    return screen(client).get("text", "")


def test_keen_without_append_cannot_find_its_data(dosbox_e2e, tmp_path, keen4_dir):
    client, _ = start_with_batch(dosbox_e2e, tmp_path, keen4_dir, ["C:\\KEEN\\KEEN4E"])
    # v1.4 without EGAGRAPH.CK4 at hand tries to rebuild it from installer parts.
    assert "Combining EGA1.CK4 and EGA2.CK4" in shell_screen(client)


def test_append_lets_keen_open_its_data_from_another_directory(
        dosbox_e2e, tmp_path, keen4_dir):
    client, _ = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir, ["append C:\\KEEN", "C:\\KEEN\\KEEN4E"])
    wait_for_keen_graphics(client)


def test_append_x_finds_and_runs_keen_by_name(dosbox_e2e, tmp_path, keen4_dir):
    client, drive_c = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir,
        ["append /x", "append C:\\KEEN",
         "if exist KEEN4E.EXE echo found>EXIST.TXT", "KEEN4E"])
    wait_for_keen_graphics(client)
    assert (drive_c / "RUN" / "EXIST.TXT").read_text().strip() == "found"


def test_current_directory_extensions_beat_the_appended_list(
        dosbox_e2e, tmp_path, keen4_dir):
    # COMMAND.COM finds NAME.* in the current directory before APPEND /x
    # is asked, so a .BAT here beats a .COM in an appended directory.
    client, drive_c = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir,
        ["append /x", "append C:\\KEEN", "HELLO"])
    assert "Illegal command" in shell_screen(client)
    (drive_c / "KEEN" / "HELLO.COM").write_bytes(b"\xb4\x4c\xcd\x21")
    (drive_c / "RUN" / "HELLO.BAT").write_text("@echo here>OUT.TXT\r\n")
    client.type_string("HELLO\n")
    wait_for_text(client, "C:\\RUN>", timeout=20)
    time.sleep(1.0)
    assert (drive_c / "RUN" / "OUT.TXT").read_text().strip() == "here"


def test_append_without_x_does_not_run_programs(dosbox_e2e, tmp_path, keen4_dir):
    client, _ = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir, ["append C:\\KEEN", "KEEN4E"])
    assert "Illegal command" in shell_screen(client)


def test_type_reads_through_the_list_and_creation_stays_here(
        dosbox_e2e, tmp_path, keen4_dir):
    client, drive_c = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir,
        ["append C:\\KEEN", "type VENDOR.DOC>COPY.TXT"])
    shell_screen(client)
    copied = drive_c / "RUN" / "COPY.TXT"
    assert copied.read_bytes() == (drive_c / "KEEN" / "VENDOR.DOC").read_bytes()


def test_path_off_ignores_names_that_carry_a_path(dosbox_e2e, tmp_path, keen4_dir):
    client, drive_c = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir,
        ["append C:\\KEEN /path:off", "type C:\\RUN\\VENDOR.DOC>OFF.TXT",
         "append /path:on", "type C:\\RUN\\VENDOR.DOC>ON.TXT"])
    shell_screen(client)
    vendor = (drive_c / "KEEN" / "VENDOR.DOC").read_bytes()
    assert (drive_c / "RUN" / "OFF.TXT").read_bytes() != vendor
    assert (drive_c / "RUN" / "ON.TXT").read_bytes() == vendor


def test_semicolon_clears_the_list(dosbox_e2e, tmp_path, keen4_dir):
    client, drive_c = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir,
        ["append C:\\KEEN", "append ;", "type VENDOR.DOC>GONE.TXT"])
    shell_screen(client)
    vendor = (drive_c / "KEEN" / "VENDOR.DOC").read_bytes()
    assert (drive_c / "RUN" / "GONE.TXT").read_bytes() != vendor


def test_dir_does_not_list_appended_files(dosbox_e2e, tmp_path, keen4_dir):
    client, drive_c = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir,
        ["append /x", "append C:\\KEEN", "dir KEEN4E.EXE>DIR.TXT"])
    shell_screen(client)
    assert "File not found" in (drive_c / "RUN" / "DIR.TXT").read_text(errors="replace")


DEBUG_SCRIPT = "\r\n".join([
    "a", "mov ax,b700", "int 2f", "mov cx,ax", "mov ax,b706", "int 2f", "int 3", "",
    "g", "q", ""])


def test_int2f_reports_append_installed_and_its_state(dosbox_e2e, tmp_path, keen4_dir):
    drive_c = tmp_path / "c"
    (drive_c / "RUN").mkdir(parents=True)
    (drive_c / "RUN" / "B7.SCR").write_bytes(DEBUG_SCRIPT.encode("ascii"))
    shutil.copytree(keen4_dir, drive_c / "KEEN")
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {drive_c}", "c:", "cd \\RUN",
                        "debug <B7.SCR >BEFORE.TXT",
                        "append /x", "append C:\\KEEN",
                        "debug <B7.SCR >AFTER.TXT"],
        conf_dir=tmp_path, settings=dict(SETTINGS))
    shell_screen(inst.client)
    before = (drive_c / "RUN" / "BEFORE.TXT").read_text(errors="replace")
    after = (drive_c / "RUN" / "AFTER.TXT").read_text(errors="replace")
    assert "CX=B700" in before, before
    assert "CX=B7FF" in after, after
    # enabled, drive, path and /x: MS-DOS 4.0 APPEND.ASM mode_flags
    assert "BX=B001" in after, after


def test_subst_runs_keen_from_a_substituted_drive(dosbox_e2e, tmp_path, keen4_dir):
    client, _ = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir, ["subst K: C:\\KEEN", "K:", "KEEN4E"])
    wait_for_keen_graphics(client)


def test_subst_d_removes_the_drive(dosbox_e2e, tmp_path, keen4_dir):
    client, drive_c = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir,
        ["subst K: C:\\KEEN", "subst K: /d", "if exist K:\\KEEN4E.EXE echo still>K.TXT"])
    shell_screen(client)
    # The redirect creates K.TXT whether or not the condition holds.
    assert "still" not in (drive_c / "RUN" / "K.TXT").read_text(errors="replace")


@pytest.mark.xfail(strict=True, reason="JOIN cannot join drives yet (ada-622b)")
def test_join_shows_a_drive_inside_a_directory(dosbox_e2e, tmp_path, keen4_dir):
    client, drive_c = start_with_batch(
        dosbox_e2e, tmp_path, keen4_dir,
        ["subst K: C:\\KEEN", "join K: C:\\RUN\\JOINED",
         "if exist C:\\RUN\\JOINED\\KEEN4E.EXE echo joined>J.TXT"])
    shell_screen(client)
    assert (drive_c / "RUN" / "J.TXT").read_text().strip() == "joined"
