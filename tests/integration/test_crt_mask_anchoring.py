# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

"""The CRT shaders' phosphor mask is anchored to the image, not the window.

Issue #19: the final shader pass draws into a viewport centred in the
window, and the mask shaders used to take their coordinates from the
window-relative fragment position. The parity of the centring margin then
chose which mask phase sat under the image, so the same frame had two
looks depending on the window size. With the mask anchored through
OUTPUT_ORIGIN, two windows whose widths differ by two logical pixels (so
the centred margin differs by one) render the same pixels.

The offscreen driver reports display scale 1, so a one-pixel change moves
the centred margin by half a pixel and truncation hides it; two pixels
flip the parity. Each instance is sampled several times because the
text-mode cursor blinks: some frame of one instance must match some frame
of the other. Matching allows one count of 255 per channel, the float
noise of a viewport moved by a pixel (measured: at most 1 on 95 pixels
with the anchoring, 9342 pixels off by more than 3 percent without it).
"""

import io
import time

from conftest import start_dosbox_instance
from PIL import Image, ImageChops

FRAME_SAMPLES = 8
SAMPLE_GAP_S = 0.15
CHANNEL_TOLERANCE = 1
MAX_DIFFERING_FRACTION = 0.0005


def _grab_frames(instance) -> list[Image.Image]:
    frames = []
    for _ in range(FRAME_SAMPLES):
        r = instance.client.frame(fmt="png", mode="rendered")
        assert r.status_code == 200, f"frame grab failed: {r.status_code}"
        frames.append(Image.open(io.BytesIO(r.content)).convert("RGB"))
        time.sleep(SAMPLE_GAP_S)
    return frames


def _frames_at_window_width(tmp_path, width_px: int) -> list[Image.Image]:
    instance = start_dosbox_instance(
        tmp_path / f"w{width_px}",
        settings={"output": "opengl", "window_size": f"{width_px}x1066"},
    )
    try:
        instance.client.wait_shell(timeout=15)
        time.sleep(2.0)
        info = instance.client.frame_info().json()
        assert info["video_mode"]["bios_mode_number"] == 3, info
        return _grab_frames(instance)
    finally:
        instance.shutdown()


def differing_fraction(a: Image.Image, b: Image.Image) -> float:
    """Fraction of pixels where any channel differs by more than the tolerance."""
    assert a.size == b.size, f"frame sizes differ: {a.size} vs {b.size}"
    diff = ImageChops.difference(a, b)
    over = None
    for band in diff.split():
        mask = band.point(lambda v: 255 if v > CHANNEL_TOLERANCE else 0)
        over = mask if over is None else ImageChops.lighter(over, mask)
    assert over is not None
    width, height = a.size
    return over.histogram()[255] / (width * height)


def test_mask_phase_does_not_follow_the_window_margin(tmp_path):
    narrow = _frames_at_window_width(tmp_path, 1422)
    wide = _frames_at_window_width(tmp_path, 1424)

    best = min(differing_fraction(a, b) for a in narrow for b in wide)

    assert best <= MAX_DIFFERING_FRACTION, (
        f"closest frame pair differs in {best:.2%} of pixels: "
        "the mask phase follows the window margin"
    )
