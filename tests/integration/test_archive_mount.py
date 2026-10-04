# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#
"""Archive mounts through MOUNT: a zip served read-only to DOS.

Builds a small zip in the test, mounts it from the autoexec, and reads
DIR and TYPE back through the screen text route. The archive's bytes
are unchanged afterwards.
"""

import hashlib
import shutil
import time
import zipfile
from pathlib import Path

import pytest

STAMP = (1990, 1, 1, 0, 0, 0)


def build_zip(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for name, data in [
            ("game/doom.exe", b"x" * 3000),
            ("game/data/level.wad", b"w" * 5000),
            ("game/readme.txt", b"hello from the archive\r\n"),
        ]:
            info = zipfile.ZipInfo(name, date_time=STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    return path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def screen(client) -> str:
    r = client.screen_text()
    assert r.status_code == 200, r.text
    return "\n".join(line.rstrip() for line in r.json()["text"].split("\n"))


def wait_text(client, needle, timeout=15.0):
    deadline = time.monotonic() + timeout
    text = ""
    while time.monotonic() < deadline:
        text = screen(client)
        if needle in text:
            return text
        time.sleep(0.2)
    raise AssertionError(f"{needle!r} not found:\n{text}")


def settings_for(tmp_path):
    return {
        "archive_cache_path": str(tmp_path / "cache"),
        "archive_sidecar_path": str(tmp_path / "saves"),
    }


def test_zip_mounts_and_dir_lists_its_members(dosbox_e2e, tmp_path):
    zip_path = build_zip(tmp_path / "game" / "doom.zip")
    before = sha256(zip_path)
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "c:", "dir /b", "type readme.txt"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    text = wait_text(inst.client, "hello from the archive")
    # D10: the single top directory became the root, so DIR lists its
    # children and no GAME entry (the console line names GAME as the root).
    lines = text.split("\n")
    start = next(i for i, line in enumerate(lines) if line.endswith(">dir /b"))
    listing = []
    for line in lines[start + 1:]:
        if ">" in line and line.split(">", 1)[0].endswith(":\\"):
            break
        if line.strip():
            listing.append(line.strip())
    assert listing == ["DOOM.EXE", "DATA", "README.TXT"], text
    assert "Using GAME as the root" in text, text
    assert sha256(zip_path) == before


def test_writes_are_refused_on_an_archive_drive(dosbox_e2e, tmp_path):
    # Without a sidecar (D1) the archive drive is read-only.
    zip_path = build_zip(tmp_path / "game" / "doom.zip")
    inst = dosbox_e2e(
        autoexec_lines=[
            f"mount c {zip_path} -nosidecar",
            "c:",
            "echo test > new.txt",
            "del readme.txt",
            "dir /b",
        ],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    text = wait_text(inst.client, "README.TXT")
    assert "NEW.TXT" not in text, text
    assert "Access denied" in text or "denied" in text.lower(), text


def test_sequential_archive_is_refused_unless_cached(dosbox_e2e, tmp_path):
    import tarfile

    tgz = tmp_path / "game" / "doom.tar.gz"
    tgz.parent.mkdir(parents=True)
    with tarfile.open(tgz, "w:gz") as t:
        for name, data in [("game/doom.exe", b"x" * 100), ("game/readme.txt", b"hi\r\n")]:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = 631152000
            import io

            t.addfile(info, io.BytesIO(data))
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {tgz}"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    text = wait_text(inst.client, "sequential archive")
    assert "archive_sequential" in text, text

    inst2 = dosbox_e2e(
        autoexec_lines=[f"mount c {tgz}", "c:", "type readme.txt"],
        conf_dir=tmp_path / "game",
        settings={**settings_for(tmp_path), "archive_sequential": "cache"},
    )
    wait_text(inst2.client, "hi")


def test_mount_listing_names_the_archive(dosbox_e2e, tmp_path):
    zip_path = build_zip(tmp_path / "game" / "doom.zip")
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "mount"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    text = wait_text(inst.client, "Archive")
    assert "doom.zip" in text, text


def test_cache_cleanup_runs_at_engine_start_without_a_mount(dosbox_e2e, tmp_path):
    zip_path = build_zip(tmp_path / "game" / "doom.zip")
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "c:", "type readme.txt"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    wait_text(inst.client, "hello from the archive")
    inst.shutdown()
    # A directory under a spread letter with files but no toml is a damaged
    # entry (design D14); the cleanup removes it.
    damaged = tmp_path / "cache" / "z" / "zombie-00000000"
    (damaged / "files").mkdir(parents=True)
    (damaged / "files" / "0").write_bytes(b"zzz")
    inst2 = dosbox_e2e(
        autoexec_lines=["echo READY-MARK"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    wait_text(inst2.client, "READY-MARK")
    assert not damaged.exists()


# DOOM 1.9 shareware as id published it (doom19s.zip, 1995-02-03): INSTALL.BAT,
# DEICE.EXE, DOOMS_19.DAT and the two archive halves. Fetched once into the
# ignored disks directory, like the e2e installs' media.
DOOM19S_URL = "https://ftp.fu-berlin.de/pc/games/idgames/idstuff/doom/doom19s.zip"
DOOM19S_SHA256 = "cacf0142b31ca1af00796b4a0339e07992ac5f21bc3f81e7532fe1b5e1b486e6"
DOOM19S_DIR = Path(__file__).resolve().parents[1] / "files" / "disks" / "doom-shareware-zip"


def fetch_doom19s() -> Path:
    """The shareware zip from the idgames mirror, or a skip naming the issue."""
    import email.utils
    import os
    import urllib.error
    import urllib.request

    target = DOOM19S_DIR / "doom19s.zip"
    if target.exists() and sha256(target) == DOOM19S_SHA256:
        return target
    DOOM19S_DIR.mkdir(parents=True, exist_ok=True)
    part = target.with_suffix(".zip.part")
    try:
        with urllib.request.urlopen(DOOM19S_URL, timeout=60) as r:  # nosec B310 - fixed https URL
            part.write_bytes(r.read())
            stamp = r.headers.get("Last-Modified")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        pytest.skip(f"download issue: {DOOM19S_URL}: {e}")
    if sha256(part) != DOOM19S_SHA256:
        part.unlink()
        pytest.skip(f"download issue: {DOOM19S_URL}: checksum differs from the pinned file")
    if stamp:
        when = email.utils.parsedate_to_datetime(stamp).timestamp()
        os.utime(part, (when, when))
    part.replace(target)
    return target


def test_doom_shareware_zip_mounts_and_lists_the_installer(dosbox_e2e, tmp_path):
    # The policy wants the archive under the conf's directory (D5).
    (tmp_path / "game").mkdir()
    zip_path = tmp_path / "game" / "doom19s.zip"
    shutil.copy2(fetch_doom19s(), zip_path)
    before = sha256(zip_path)
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "c:", "dir /b", "type install.bat"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    text = wait_text(inst.client, "Error installing DOOMS_19.EXE!")
    for name in ("INSTALL.BAT", "DEICE.EXE", "DOOMS_19.DAT", "DOOMS_19.1", "DOOMS_19.2"):
        assert name in text, text
    assert sha256(zip_path) == before


def press(client, key):
    for pressed in (True, False):
        r = client.press_key(key, pressed)
        assert r.status_code == 200, r.text


def test_doom_shareware_installs_into_the_sidecar(dosbox_e2e, tmp_path):
    """INSTALL.BAT from the zip: DEICE joins the halves into DOOMS_19.EXE, PKSFX
    unpacks into \\DOOMS, the batch deletes the EXE and starts SETUP. Every
    write lands in the sidecar; the zip keeps its bytes."""
    (tmp_path / "game").mkdir()
    zip_path = tmp_path / "game" / "doom19s.zip"
    shutil.copy2(fetch_doom19s(), zip_path)
    before = sha256(zip_path)
    settings = settings_for(tmp_path)
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "c:", "install.bat"],
        conf_dir=tmp_path / "game",
        settings=settings,
    )
    wait_text(inst.client, "Which drive to install to", timeout=60)
    inst.client.type_string("C")
    wait_text(inst.client, "Enter directory name to copy into", timeout=30)
    time.sleep(1)
    press(inst.client, "KBD_enter")
    wait_text(inst.client, "Press 'Y' to create", timeout=30)
    time.sleep(1)
    inst.client.type_string("Y")
    # SETUP's main menu is the batch's last line.
    wait_text(inst.client, "F1=Warp", timeout=240)
    assert sha256(zip_path) == before
    saves = [d for d in (tmp_path / "saves").iterdir() if d.is_dir()]
    assert len(saves) == 1, saves
    dooms = saves[0] / "DOOMS"
    assert (dooms / "DOOM1.WAD").stat().st_size == 4196020
    assert (dooms / "DOOM.EXE").stat().st_size == 709905
    assert (dooms / "SETUP.EXE").exists()
    assert not list((tmp_path / "saves").rglob("DOOMS_19.EXE"))
