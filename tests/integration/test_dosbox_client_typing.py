# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

import pytest
from dosbox_client import DosboxClient


def typed(text, **kwargs):
    client = DosboxClient("http://127.0.0.1:1", token="x")
    sent = []
    client.input_sequence = lambda events: sent.append(events)
    client.type_string(text, **kwargs)
    return [(e["key"], e["pressed"]) for e in sent[0]]


def test_a_double_quote_is_shift_and_quote():
    assert typed('"') == [
        ("KBD_leftshift", True),
        ("KBD_quote", True),
        ("KBD_quote", False),
        ("KBD_leftshift", False),
    ]


def test_an_apostrophe_is_the_quote_key():
    assert typed("'") == [("KBD_quote", True), ("KBD_quote", False)]


@pytest.mark.parametrize(
    "ch,key,shift",
    [
        (";", "KBD_semicolon", False),
        ("[", "KBD_leftbracket", False),
        ("]", "KBD_rightbracket", False),
        ("`", "KBD_grave", False),
        ("~", "KBD_grave", True),
        ("{", "KBD_leftbracket", True),
        ("}", "KBD_rightbracket", True),
        ("|", "KBD_backslash", True),
    ],
)
def test_the_remaining_punctuation(ch, key, shift):
    events = typed(ch)
    assert (key, True) in events
    assert (("KBD_leftshift", True) in events) == shift


def test_strict_refuses_a_character_without_a_key():
    with pytest.raises(ValueError, match="ü"):
        typed("Schatten über", strict=True)


def test_without_strict_an_unknown_character_is_still_skipped():
    assert typed("ä") == []
