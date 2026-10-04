# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#
"""The sidecar of an archive mount: writes land there, deletions and
renames are recorded, a remount shows the union, a moved archive finds
its sidecar by hash, a deleted sidecar brings the base back."""

import shutil
import time

from test_archive_mount import build_zip, sha256, wait_text

SIDECAR_TOML = ".source-archive.toml"


def settings_for(tmp_path):
    return {
        "archive_cache_path": str(tmp_path / "cache"),
        "archive_sidecar_path": str(tmp_path / "saves"),
    }


def only_sidecar(tmp_path):
    dirs = [d for d in (tmp_path / "saves").iterdir() if d.is_dir()]
    assert len(dirs) == 1, dirs
    return dirs[0]


def test_writes_deletes_and_renames_land_in_the_sidecar(dosbox_e2e, tmp_path):
    zip_path = build_zip(tmp_path / "game" / "doom.zip")
    before = sha256(zip_path)
    inst = dosbox_e2e(
        autoexec_lines=[
            f"mount c {zip_path}",
            "c:",
            "echo saved > game.sav",
            "md saves",
            "del readme.txt",
            "ren doom.exe doom.old",
            "dir /b",
        ],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    text = wait_text(inst.client, "DOOM.OLD")
    assert "README.TXT" not in text.split("dir /b", 1)[1], text
    sidecar = only_sidecar(tmp_path)
    assert (sidecar / "GAME.SAV").read_text().strip() == "saved"
    assert (sidecar / "SAVES").is_dir()
    assert (sidecar / "DOOM.OLD").is_file()
    # The two-second window of D3, then the record holds both changes.
    time.sleep(3.0)
    record = (sidecar / SIDECAR_TOML).read_text()
    assert 'op = "delete"' in record and 'path = "README.TXT"' in record, record
    assert 'op = "rename"' in record and 'to = "DOOM.OLD"' in record, record
    assert sha256(zip_path) == before
    inst.shutdown()

    inst2 = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "c:", "dir /b", "type game.sav"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    text = wait_text(inst2.client, "saved")
    listing = text.split("dir /b", 1)[1]
    assert "DOOM.OLD" in listing and "README.TXT" not in listing and "DOOM.EXE" not in listing, text


def test_a_moved_archive_finds_its_sidecar_by_hash(dosbox_e2e, tmp_path):
    zip_path = build_zip(tmp_path / "game" / "doom.zip")
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "c:", "echo one > one.txt"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    wait_text(inst.client, "one.txt")
    inst.shutdown()
    moved = tmp_path / "game" / "elsewhere" / "doom.zip"
    moved.parent.mkdir()
    zip_path.rename(moved)
    inst2 = dosbox_e2e(
        autoexec_lines=[f"mount c {moved}", "c:", "type one.txt"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    wait_text(inst2.client, "one")


def test_deleting_the_sidecar_brings_the_base_back(dosbox_e2e, tmp_path):
    zip_path = build_zip(tmp_path / "game" / "doom.zip")
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "c:", "del readme.txt", "dir /b"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    wait_text(inst.client, "DATA")
    inst.shutdown()
    shutil.rmtree(only_sidecar(tmp_path))
    inst2 = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "c:", "type readme.txt"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    wait_text(inst2.client, "hello from the archive")


def test_a_second_engine_on_the_same_archive_is_refused(dosbox_e2e, tmp_path):
    zip_path = build_zip(tmp_path / "game" / "doom.zip")
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    wait_text(inst.client, "mounted as C")
    inst2 = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    wait_text(inst2.client, "in use by another mount")
