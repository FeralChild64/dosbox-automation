<!-- This file is part of the dosbox-automation Project.
     License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net -->

# RAR fixtures for the archive reader tests

libarchive cannot write RAR, and RAR 7.x cannot write RAR4 archives any
more (the `-ma4` switch is gone), so the RAR4 fixtures come from
libarchive's own test suite and only the RAR5 one was made here.

Files from libarchive (https://github.com/libarchive/libarchive,
`libarchive/test/`, BSD-2-Clause, decoded from their `.uu` form with
the commit time of the source file kept as the mtime):

| Here | Upstream file | Content |
|---|---|---|
| plain-v4.rar | test_read_format_rar_compress_normal.rar | non-solid RAR4, two files, two directories, one symlink |
| solid-v4.rar | test_read_format_rar4_solid_encrypted.rar | solid and encrypted RAR4; the solid flag is what the test is for |
| encrypted-v4.rar | test_read_format_rar4_encrypted.rar | RAR4 with encrypted data, plain headers |
| encrypted-headers-v4.rar | test_read_format_rar_encryption_header.rar | RAR4 with encrypted headers |
| multi-v4.part1.rar | test_read_format_rar_multivolume.part0001.rar | first volume of four; the other three are not needed for the refusal |
| encrypted-v5.rar | test_read_format_rar5_encrypted.rar | RAR5 with encrypted data, plain headers |
| encrypted-headers-v5.rar | test_read_format_rar5_encrypted_filenames.rar | RAR5 with encrypted headers |

Made here, synthetic content (3000 bytes of `x` as GAME.EXE, 500 bytes
of `l` as data/level1.dat, `hello` as readme.txt, all dated 1990-01-01):

| Here | Tool | Command |
|---|---|---|
| plain-v5.rar | RAR 7.23 x64 on the Windows builder, 2026-09-27 | `rar a -ma5 -m3 plain-v5.rar GAME.EXE data readme.txt` |

No solid RAR4 without encryption exists in the house; the solid flag
is read from the main header before libarchive opens the file, so the
combined fixture exercises that check.

# PKZIP 1.x fixtures (legacy/)

libarchive indexes shrink (method 1) and implode (method 6) members but
cannot decode them, and no current tool writes them, so these two zips
were made by PKZIP 1.10 itself, running under dosbox-automation.

The inputs are ours: `legacy/make_inputs.py` writes them into
`legacy/inputs/` from a fixed seed, every file dated 1990-03-15 12:00 UTC.
Run it again and the bytes are the same.

| Here | Tool | Command |
|---|---|---|
| legacy/SHRUNK.ZIP | PKZIP 1.10 (03-15-90), `PKZ110.EXE` from the archive.org item `pctoday_disk2`, md5 4d1a65b76ef4914b4932ba15c252fd4e | `pkzip -es SHRUNK.ZIP TINY.TXT SMALL.TXT LARGE.TXT SMALL.BIN LARGE.BIN EMPTY.DAT` |
| legacy/IMPLODED.ZIP | the same | `pkzip -ex IMPLODED.ZIP TINY.TXT SMALL.TXT LARGE.TXT SMALL.BIN LARGE.BIN EMPTY.DAT` |

Both ran 2026-10-04 under dosbox-automation 0.85.1 (6d100), headless,
with `TZ=UTC` and `keyboard_layout = us`, from a `mount c .` of a work
directory holding `PKZIP.EXE` and the inputs. Info-ZIP's `unzip -t`
passes both, so PKWARE's encoder and Info-ZIP's decoder agree on every
member before our tests use them.

What PKZIP chose, from `zipinfo`:

| Member | SHRUNK.ZIP | IMPLODED.ZIP |
|---|---|---|
| TINY.TXT (200) | shrunk | shrunk (PKZIP shrinks anything under 320 bytes) |
| SMALL.TXT (3000) | shrunk | imploded, 4K window, 2 trees |
| LARGE.TXT (60000) | shrunk | imploded, 8K window, 3 trees |
| SMALL.BIN (3000) | shrunk | imploded, 4K window, 2 trees |
| LARGE.BIN (60000) | shrunk | imploded, 4K window, 2 trees |
| EMPTY.DAT (0) | stored | stored |

PKZIP 1.10 does not write the other two implode variants (4K with three
trees, 8K with two). A probe with text from 8 to 120 KiB and binary up to
300 KiB gave only 8K/3 for larger text and 4K/2 for everything else, so
those two combinations are covered by the decoder's hostile tests and by
reading the code, not by a vector.

sha256:
- SHRUNK.ZIP 5c11a0f06c05e24c133eb5998b805689cb20c6d4a9bd76b6e6d7bbd93b2833bf
- IMPLODED.ZIP 2b20ad0598115abca3de64b7195f5f2549ae6c4a0c898423d565c201990a9171
