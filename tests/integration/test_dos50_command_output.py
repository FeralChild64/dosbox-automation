# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

"""PATH, APPEND and JOIN texts in the real binary; the unit tests stub messages.
Expected texts: MS-DOS 4.0 USA-MS.MSG and COMMAND.COM print_path (TCMD2A.ASM)."""

import time

import pytest


def screen_after(dosbox_e2e, tmp_path, lines):
    instance = dosbox_e2e(autoexec_lines=["cls", *lines], conf_dir=tmp_path)
    client = instance.client
    client.wait_shell(timeout=10)
    time.sleep(1.0)
    r = client.screen_text()
    assert r.status_code == 200, r.text
    return [line.rstrip() for line in r.json()["text"].splitlines()]


def output_of(screen, command, occurrence=0):
    prompts = [i for i, line in enumerate(screen) if line.endswith(">" + command)]
    prompt = prompts[occurrence]
    out = []
    for line in screen[prompt + 1 :]:
        if ">" in line and line.split(">", 1)[0].endswith(":\\"):
            break
        out.append(line)
    return [line for line in out if line]


def test_path_display_prefixes_the_variable_name(dosbox_e2e, tmp_path):
    screen = screen_after(dosbox_e2e, tmp_path, ["path C:\\ONE;C:\\TWO", "path"])
    assert output_of(screen, "path") == ["PATH=C:\\ONE;C:\\TWO"]


def test_path_cleared_prints_no_path(dosbox_e2e, tmp_path):
    screen = screen_after(dosbox_e2e, tmp_path, ["path;", "path"])
    assert output_of(screen, "path") == ["No Path"]


def test_append_texts(dosbox_e2e, tmp_path):
    screen = screen_after(
        dosbox_e2e,
        tmp_path,
        [
            "append",
            "append /q",
            "append /e c:\\x",
            "append c:\\x",
            "append",
            "append;",
            "append",
        ],
    )
    assert output_of(screen, "append") == ["No Append"]
    assert output_of(screen, "append /q") == ["Invalid parameter"]
    assert output_of(screen, "append /e c:\\x") == ["Invalid combination of parameters"]
    assert output_of(screen, "append c:\\x") == []
    assert output_of(screen, "append", occurrence=1) == ["APPEND=c:\\x"]
    assert output_of(screen, "append;") == []
    assert output_of(screen, "append", occurrence=2) == ["No Append"]


def test_append_env_mode_sets_and_clears_the_variable(dosbox_e2e, tmp_path):
    screen = screen_after(
        dosbox_e2e,
        tmp_path,
        ["append /e", "append b:\\letters;a:\\reports", "set", "append ;", "set"],
    )
    assert "APPEND=b:\\letters;a:\\reports" in output_of(screen, "set")
    cleared = screen[screen.index("Z:\\>append ;") :]
    assert not [line for line in cleared if line.startswith("APPEND=")]


@pytest.mark.parametrize(
    "command",
    ["join d: c:\\sales", "join d: /d", "join d: c:\\", "join /q"],
)
def test_join_failures_set_errorlevel(dosbox_e2e, tmp_path, command):
    screen = screen_after(dosbox_e2e, tmp_path, [command, "if errorlevel 1 echo LEVEL1"])
    assert output_of(screen, "if errorlevel 1 echo LEVEL1") == ["LEVEL1"]


def test_join_list_succeeds(dosbox_e2e, tmp_path):
    screen = screen_after(dosbox_e2e, tmp_path, ["join", "if errorlevel 1 echo LEVEL1"])
    assert output_of(screen, "if errorlevel 1 echo LEVEL1") == []


def test_set_skips_a_leading_comma_like_command_com(dosbox_e2e, tmp_path):
    # MS-DOS 4.0 TENV.ASM GETARG runs SCANOFF over every DELIM character.
    screen = screen_after(dosbox_e2e, tmp_path, ["set,a=1", "set a"])
    assert output_of(screen, "set a") == ["A=1"]


def test_path_skips_a_leading_comma_but_keeps_a_semicolon(dosbox_e2e, tmp_path):
    # MS-DOS 4.0 TMISC2.ASM PSCANOFF: ';' is not a delimiter for PATH.
    screen = screen_after(dosbox_e2e, tmp_path, ["path,C:\\DOS", "path"])
    assert output_of(screen, "path") == ["PATH=C:\\DOS"]


def test_path_with_only_delimiters_shows_the_path(dosbox_e2e, tmp_path):
    # PGETARG reaches the end of the line and PATH goes to disppath.
    screen = screen_after(dosbox_e2e, tmp_path, ["path C:\\DOS", "path,", "path"])
    assert output_of(screen, "path,") == ["PATH=C:\\DOS"]
    assert output_of(screen, "path") == ["PATH=C:\\DOS"]
