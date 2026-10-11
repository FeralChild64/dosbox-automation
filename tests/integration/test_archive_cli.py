# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#
"""The management switches of design 6.1, run without a display:
--archive-cache status|empty and --sidecar list, with the CLI exit codes."""

import io
import os
import re
import subprocess
import tarfile
from pathlib import Path

from conftest import DOSBOX_BIN
from test_archive_mount import build_zip, settings_for, wait_text


def run_cli(tmp_path: Path, *words: str, timeout: int = 60) -> subprocess.CompletedProcess:
    env = {**os.environ, "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy",
           "HOME": str(tmp_path), "XDG_CONFIG_HOME": str(tmp_path / ".config")}
    cmd = [DOSBOX_BIN, "--noprimaryconf", "--nolocalconf",
           "--set", f"archive_cache_path={tmp_path / 'cache'}",
           "--set", f"archive_sidecar_path={tmp_path / 'saves'}", *words]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env,
                          check=False)


def build_tgz(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(path, "w:gz") as t:
        for name, data in [("game/doom.exe", b"x" * 100), ("game/readme.txt", b"hi\r\n")]:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = 631152000
            t.addfile(info, io.BytesIO(data))
    return path


def test_cache_status_on_a_fresh_setup_says_no_cache_yet(tmp_path):
    r = run_cli(tmp_path, "--archive-cache", "status")
    assert r.returncode == 0, r.stderr
    assert "no cache yet" in r.stdout, r.stdout
    assert not (tmp_path / "cache").exists()


# Decision 1b (2026-10-08): nothing to do is not a failure.
def test_cache_empty_on_a_fresh_setup_says_no_cache(tmp_path):
    r = run_cli(tmp_path, "--archive-cache", "empty")
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    assert "no cache at" in r.stdout and "nothing to empty" in r.stdout, r.stdout
    assert not (tmp_path / "cache").exists()


def test_cache_status_and_empty_after_a_mount(dosbox_e2e, tmp_path):
    tgz = build_tgz(tmp_path / "game" / "doom.tar.gz")
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {tgz}", "c:", "type readme.txt"],
        conf_dir=tmp_path / "game",
        settings={**settings_for(tmp_path), "archive_sequential": "cache"},
    )
    wait_text(inst.client, "hi")
    inst.shutdown()

    r = run_cli(tmp_path, "--archive-cache", "status")
    assert r.returncode == 0, r.stderr
    assert "doom.tar-" in r.stdout and str(tgz) in r.stdout, r.stdout
    r = run_cli(tmp_path, "--archive-cache", "empty")
    assert r.returncode == 0, r.stderr
    assert "removed 1" in r.stdout, r.stdout
    r = run_cli(tmp_path, "--archive-cache", "status")
    assert r.returncode == 0, r.stderr
    assert "doom.tar-" not in r.stdout and "no entries" in r.stdout, r.stdout


def test_sidecar_list_names_the_archive_and_its_state(dosbox_e2e, tmp_path):
    zip_path = build_zip(tmp_path / "game" / "doom.zip")
    inst = dosbox_e2e(
        autoexec_lines=[f"mount c {zip_path}", "c:", "echo saved > game.sav", "dir /b"],
        conf_dir=tmp_path / "game",
        settings=settings_for(tmp_path),
    )
    wait_text(inst.client, "GAME.SAV")
    inst.shutdown()

    r = run_cli(tmp_path, "--sidecar", "list")
    assert r.returncode == 0, r.stderr
    assert "doom-" in r.stdout and "same" in r.stdout and str(zip_path) in r.stdout, r.stdout
    assert re.search(r"\b[0-9a-f]{64}\b", r.stdout), r.stdout

    zip_path.unlink()
    r = run_cli(tmp_path, "--sidecar", "list")
    assert r.returncode == 0, r.stderr
    assert "missing" in r.stdout, r.stdout


def test_sidecar_list_without_a_saves_directory_says_so(tmp_path):
    r = run_cli(tmp_path, "--sidecar", "list")
    assert r.returncode == 0, r.stderr
    assert "no sidecars" in r.stdout, r.stdout
    assert not (tmp_path / "saves").exists()


def test_an_unknown_cache_action_is_a_usage_error(tmp_path):
    r = run_cli(tmp_path, "--archive-cache", "wipe")
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    assert "status" in r.stderr and "empty" in r.stderr, r.stderr


def test_an_unknown_sidecar_action_is_a_usage_error(tmp_path):
    r = run_cli(tmp_path, "--sidecar", "clean")
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    assert "list" in r.stderr, r.stderr


# Without its word the switch must still end before any display: a bare
# --archive-cache once fell through and started the emulator.
def test_a_switch_without_its_word_is_a_usage_error(tmp_path):
    r = run_cli(tmp_path, "--archive-cache", timeout=15)
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    assert "status" in r.stderr and "empty" in r.stderr, r.stderr
    r = run_cli(tmp_path, "--sidecar", "--noconsole", timeout=15)
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    assert "list" in r.stderr, r.stderr
