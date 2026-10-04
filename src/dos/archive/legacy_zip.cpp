// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
// Portions adapted from Info-ZIP UnZip 6.0 (https://infozip.sourceforge.net):
// unshrink.c (Greg Roelofs), explode.c and huft_build/huft_free from
// inflate.c (Mark Adler and Info-ZIP contributors). Licensed under the
// Info-ZIP license, reproduced below as it requires.
// Local changes: ported from UnZip's globals to a pull source and a push
// sink, output bounded by the declared size, the two explode loops merged,
// unshrink ends when fewer bits than a code remain, text mode and platform
// branches removed. This is not an Info-ZIP release.
//
// Copyright (c) 1990-2009 Info-ZIP.  All rights reserved.
//
// For the purposes of this copyright and license, "Info-ZIP" is defined as
// the following set of individuals:
//
//    Mark Adler, John Bush, Karl Davis, Harald Denker, Jean-Michel Dubois,
//    Jean-loup Gailly, Hunter Goatley, Ed Gordon, Ian Gorman, Chris Herborth,
//    Dirk Haase, Greg Hartwig, Robert Heath, Jonathan Hudson, Paul Kienitz,
//    David Kirschbaum, Johnny Lee, Onno van der Linden, Igor Mandrichenko,
//    Steve P. Miller, Sergio Monesi, Keith Owens, George Petrov, Greg Roelofs,
//    Kai Uwe Rommel, Steve Salisbury, Dave Smith, Steven M. Schweda,
//    Christian Spieler, Cosmin Truta, Antoine Verheijen, Paul von Behren,
//    Rich Wales, Mike White.
//
// This software is provided "as is," without warranty of any kind, express
// or implied.  In no event shall Info-ZIP or its contributors be held liable
// for any direct, indirect, incidental, special or consequential damages
// arising out of the use of or inability to use this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the above disclaimer and the following restrictions:
//
//     1. Redistributions of source code (in whole or in part) must retain
//        the above copyright notice, definition, disclaimer, and this list
//        of conditions.
//
//     2. Redistributions in binary form (compiled executables and libraries)
//        must reproduce the above copyright notice, definition, disclaimer,
//        and this list of conditions in documentation and/or other materials
//        provided with the distribution.  Additional documentation is not needed
//        for executables where a command line license option provides these and
//        a note regarding this option is in the executable's startup banner.  The
//        sole exception to this condition is redistribution of a standard
//        UnZipSFX binary (including SFXWiz) as part of a self-extracting archive;
//        that is permitted without inclusion of this license, as long as the
//        normal SFX banner has not been removed from the binary or disabled.
//
//     3. Altered versions--including, but not limited to, ports to new operating
//        systems, existing ports with new graphical interfaces, versions with
//        modified or added functionality, and dynamic, shared, or static library
//        versions not from Info-ZIP--must be plainly marked as such and must not
//        be misrepresented as being the original source or, if binaries,
//        compiled from the original source.  Such altered versions also must not
//        be misrepresented as being Info-ZIP releases--including, but not
//        limited to, labeling of the altered versions with the names "Info-ZIP"
//        (or any variation thereof, including, but not limited to, different
//        capitalizations), "Pocket UnZip," "WiZ" or "MacZip" without the
//        explicit permission of Info-ZIP.  Such altered versions are further
//        prohibited from misrepresentative use of the Zip-Bugs or Info-ZIP
//        e-mail addresses or the Info-ZIP URL(s), such as to imply Info-ZIP
//        will provide support for the altered versions.
//
//     4. Info-ZIP retains the right to use the names "Info-ZIP," "Zip," "UnZip,"
//        "UnZipSFX," "WiZ," "Pocket UnZip," "Pocket Zip," and "MacZip" for its
//        own source and binary releases.
//
#include "dos/archive/legacy_zip.h"

#include "utils/checks.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

CHECK_NARROWING();

namespace LegacyZip {

namespace {

constexpr std::array<unsigned, 17> mask_bits = {0x0000,
                                                0x0001, 0x0003, 0x0007, 0x000f,
                                                0x001f, 0x003f, 0x007f, 0x00ff,
                                                0x01ff, 0x03ff, 0x07ff, 0x0fff,
                                                0x1fff, 0x3fff, 0x7fff, 0xffff};

bool Fail(std::string* detail, const char* why)
{
	if (detail) {
		*detail = why;
	}
	return false;
}

// UnZip's NEXTBYTE over the member's data, at most `limit` bytes of it;
// past the end it hands out zeros and counts them.
class ByteReader {
public:
	ByteReader(const Source& source, const uint64_t limit)
	        : source(source),
	          remaining(limit)
	{}

	std::optional<uint8_t> TryNext()
	{
		if (pos == len && !Refill()) {
			return {};
		}
		++taken;
		return buffer[pos++];
	}

	uint8_t Next()
	{
		if (const auto byte = TryNext()) {
			return *byte;
		}
		++overread;
		return 0;
	}

	uint64_t taken    = 0;
	uint64_t overread = 0;

private:
	bool Refill()
	{
		if (remaining == 0) {
			return false;
		}
		const auto want = static_cast<size_t>(
		        std::min<uint64_t>(buffer.size(), remaining));
		const auto got = std::min(source(buffer.data(), want), want);
		remaining -= got;
		pos = 0;
		len = got;
		return got > 0;
	}

	const Source& source;
	uint64_t remaining              = 0;
	std::array<uint8_t, 4096> buffer = {};
	size_t pos                      = 0;
	size_t len                      = 0;
};

// Collects output and hands it to the sink; anything past out_size is a
// stream that lies about its size.
class Output {
public:
	Output(const Sink& sink, const uint64_t out_size)
	        : sink(sink),
	          limit(out_size)
	{}

	bool Put(const uint8_t* data, const size_t size, std::string* detail)
	{
		if (size > limit - produced) {
			return Fail(detail, "more output than the declared size");
		}
		produced += size;
		if (size > 0 && !sink(data, size)) {
			sink_failed = true;
			return Fail(detail, "the output was not taken");
		}
		return true;
	}

	uint64_t produced = 0;
	bool sink_failed  = false;

private:
	const Sink& sink;
	uint64_t limit = 0;
};

Result Finish(const Output& out, const bool ok)
{
	if (out.sink_failed) {
		return Result::SinkFailed;
	}
	return ok ? Result::Ok : Result::Corrupt;
}

// ---- unshrink.c ----

constexpr int MAX_BITS        = 13;
constexpr unsigned HSIZE      = 1u << MAX_BITS;
constexpr uint16_t BOGUSCODE  = 256;
constexpr uint16_t CODE_MASK  = HSIZE - 1;
constexpr uint16_t FREE_CODE  = HSIZE;
constexpr uint16_t HAS_CHILD  = HSIZE << 1;
constexpr size_t OutBufBytes  = 32768;

struct ShrinkState {
	std::array<uint16_t, HSIZE> parent = {};
	std::array<uint8_t, HSIZE> value   = {};
	std::array<uint8_t, HSIZE> stack   = {};
	std::array<uint8_t, OutBufBytes> outbuf = {};
};

void partial_clear(ShrinkState& st, const uint16_t lastcodeused)
{
	// first loop:  mark each parent as such
	for (unsigned code = BOGUSCODE + 1; code <= lastcodeused; ++code) {
		const auto cparent = static_cast<uint16_t>(st.parent[code] & CODE_MASK);
		if (cparent > BOGUSCODE) {
			st.parent[cparent] |= HAS_CHILD;
		}
	}
	// second loop:  clear all nodes *not* marked as parents; reset flag bits
	for (unsigned code = BOGUSCODE + 1; code <= lastcodeused; ++code) {
		if (st.parent[code] & HAS_CHILD) {
			st.parent[code] &= static_cast<uint16_t>(~HAS_CHILD);
		} else {
			st.parent[code] = FREE_CODE;
		}
	}
}

} // namespace

uint32_t Crc32(const uint32_t crc, const uint8_t* data, const size_t size)
{
	static const auto table = [] {
		std::array<uint32_t, 256> t = {};
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int bit = 0; bit < 8; ++bit) {
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			}
			t[i] = c;
		}
		return t;
	}();
	uint32_t c = ~crc;
	for (size_t i = 0; i < size; ++i) {
		c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
	}
	return ~c;
}

Result Unshrink(const Source& in, const uint64_t out_size, const Sink& sink,
                std::string* detail)
{
	auto state = std::make_unique<ShrinkState>();
	auto& st   = *state;
	ByteReader reader(in, std::numeric_limits<uint64_t>::max());
	Output out(sink, out_size);

	uint64_t bitbuf = 0;
	int bits_left   = 0;
	// READBITS (unzpriv.h:2785); false where UnZip sets zipeof.
	auto readbits = [&](const int nbits, uint16_t& dest) {
		if (nbits > bits_left) {
			while (bits_left <= 56) {
				const auto byte = reader.TryNext();
				if (!byte) {
					break;
				}
				bitbuf |= static_cast<uint64_t>(*byte) << bits_left;
				bits_left += 8;
			}
			if (nbits > bits_left) {
				return false;
			}
		}
		dest = static_cast<uint16_t>(bitbuf & mask_bits[static_cast<size_t>(nbits)]);
		bitbuf >>= nbits;
		bits_left -= nbits;
		return true;
	};

	size_t outcnt = 0;
	auto put      = [&](const uint8_t byte) {
		st.outbuf[outcnt++] = byte;
		if (outcnt == st.outbuf.size()) {
			const auto ok = out.Put(st.outbuf.data(), outcnt, detail);
			outcnt        = 0;
			return ok;
		}
		return true;
	};
	auto finish = [&](const bool ok) {
		if (ok && outcnt > 0) {
			if (!out.Put(st.outbuf.data(), outcnt, detail)) {
				return Finish(out, false);
			}
		}
		if (ok && out.produced != out_size) {
			Fail(detail, "less output than the declared size");
			return Result::Corrupt;
		}
		return Finish(out, ok);
	};

	uint16_t lastfreecode = BOGUSCODE;
	for (unsigned code = 0; code < BOGUSCODE; ++code) {
		st.value[code]  = static_cast<uint8_t>(code);
		st.parent[code] = BOGUSCODE;
	}
	for (unsigned code = BOGUSCODE + 1; code < HSIZE; ++code) {
		st.parent[code] = FREE_CODE;
	}

	int codesize     = 9;
	uint16_t oldcode = 0;
	if (!readbits(codesize, oldcode)) {
		return finish(true);
	}
	auto finalval = static_cast<uint8_t>(oldcode);
	if (!put(finalval)) {
		return finish(false);
	}

	const int stacktop = static_cast<int>(HSIZE) - 1;
	while (true) {
		uint16_t code = 0;
		if (!readbits(codesize, code)) {
			break;
		}
		if (code == BOGUSCODE) {
			if (!readbits(codesize, code)) {
				break;
			}
			if (code == 1) {
				++codesize;
				if (codesize > MAX_BITS) {
					Fail(detail, "shrink code size over 13 bits");
					return finish(false);
				}
			} else if (code == 2) {
				partial_clear(st, lastfreecode);
				lastfreecode = BOGUSCODE;
			}
			continue;
		}

		// Translate code:  traverse tree from leaf back to root.
		int newstr             = stacktop;
		const uint16_t curcode = code;
		if (st.parent[code] == FREE_CODE) {
			st.stack[static_cast<size_t>(newstr--)] = finalval;
			code                                    = oldcode;
		}
		while (code != BOGUSCODE) {
			if (newstr < 0) {
				Fail(detail, "shrink stream overflows the decoding stack");
				return finish(false);
			}
			if (st.parent[code] == FREE_CODE) {
				st.stack[static_cast<size_t>(newstr--)] = finalval;
				code                                    = oldcode;
			} else {
				st.stack[static_cast<size_t>(newstr--)] = st.value[code];
				code = static_cast<uint16_t>(st.parent[code] & CODE_MASK);
			}
		}
		const int len = stacktop - newstr++;
		finalval      = st.stack[static_cast<size_t>(newstr)];

		// Write expanded string in reverse order to output buffer.
		for (int p = newstr; p < newstr + len; ++p) {
			if (!put(st.stack[static_cast<size_t>(p)])) {
				return finish(false);
			}
		}

		// Add new leaf (first character of newstr) to tree as child of oldcode.
		code = static_cast<uint16_t>(lastfreecode + 1);
		while (code < HSIZE && st.parent[code] != FREE_CODE) {
			++code;
		}
		lastfreecode = code;
		if (code >= HSIZE) {
			Fail(detail, "shrink stream overflows the code table");
			return finish(false);
		}
		st.value[code]  = finalval;
		st.parent[code] = oldcode;
		oldcode         = curcode;
	}
	return finish(true);
}

namespace {

// ---- inflate.c: huft_build, huft_free ----

struct huft {
	uint8_t e = 0; // number of extra bits or operation
	uint8_t b = 0; // number of bits in this code or subcode
	union {
		uint16_t n; // literal, length base, or distance base
		huft* t;    // pointer to next level of table
	} v = {};
};

constexpr unsigned BMAX         = 16;
constexpr unsigned N_MAX        = 288;
constexpr uint8_t INVALID_CODE  = 99;

void huft_free(huft* t)
{
	// Go through linked list, freeing from the malloced (t[-1]) address.
	huft* p = t;
	while (p != nullptr) {
		huft* q = (--p)->v.t;
		std::free(p);
		p = q;
	}
}

// Owns the chain huft_build links through each table's dummy first entry.
struct HuftTable {
	huft* t = nullptr;
	HuftTable()                            = default;
	HuftTable(const HuftTable&)            = delete;
	HuftTable& operator=(const HuftTable&) = delete;
	~HuftTable()
	{
		huft_free(t);
	}
};

// Zero on success, one for an incomplete code set, two for invalid input,
// three when out of memory (inflate.c:1551).
int huft_build(const unsigned* b, unsigned n, const unsigned s, const uint16_t* d,
               const uint8_t* e, huft** t, unsigned* m)
{
	unsigned a                = 0;  // counter for codes of length k
	unsigned c[BMAX + 1]      = {}; // bit length count table
	unsigned el               = 0;  // length of EOB code (value 256)
	unsigned f                = 0;  // i repeats in table every f entries
	int g                     = 0;  // maximum code length
	int h                     = 0;  // table level
	unsigned i                = 0;  // counter, current code
	unsigned j                = 0;  // counter
	int k                     = 0;  // number of bits in current code
	int lx[BMAX + 1]          = {}; // memory for l[-1..BMAX-1]
	int* l                    = lx + 1;
	const unsigned* p         = nullptr;
	huft* q                   = nullptr;
	huft r                    = {};
	huft* u[BMAX]             = {};
	unsigned v[N_MAX]         = {};
	int w                     = 0;  // bits before this table == (l * h)
	unsigned x[BMAX + 1]      = {}; // bit offsets, then code stack
	unsigned* xp              = nullptr;
	int y                     = 0;  // number of dummy codes added
	unsigned z                = 0;  // number of entries in current table

	// Generate counts for each bit length
	el = n > 256 ? b[256] : BMAX;
	p  = b;
	i  = n;
	do {
		if (*p > BMAX) {
			return 2;
		}
		c[*p]++;
		p++;
	} while (--i);
	if (c[0] == n) {
		*t = nullptr;
		*m = 0;
		return 0;
	}

	// Find minimum and maximum length, bound *m by those
	for (j = 1; j <= BMAX; j++) {
		if (c[j]) {
			break;
		}
	}
	k = static_cast<int>(j);
	if (*m < j) {
		*m = j;
	}
	for (i = BMAX; i; i--) {
		if (c[i]) {
			break;
		}
	}
	g = static_cast<int>(i);
	if (*m > i) {
		*m = i;
	}

	// Adjust last length count to fill out codes, if needed
	for (y = 1 << j; j < i; j++, y <<= 1) {
		if ((y -= static_cast<int>(c[j])) < 0) {
			return 2; // bad input: more codes than bits
		}
	}
	if ((y -= static_cast<int>(c[i])) < 0) {
		return 2;
	}
	c[i] += static_cast<unsigned>(y);

	// Generate starting offsets into the value table for each length
	x[1] = j = 0;
	p        = c + 1;
	xp       = x + 2;
	while (--i) {
		*xp++ = (j += *p++);
	}

	// Make a table of values in order of bit lengths
	p = b;
	i = 0;
	do {
		if ((j = *p++) != 0) {
			v[x[j]++] = i;
		}
	} while (++i < n);
	n = x[g];

	// Generate the Huffman codes and for each, make the table entries
	x[0] = i      = 0;
	const unsigned* pv = v;
	h            = -1;
	w = l[-1] = 0;
	u[0]         = nullptr;
	q            = nullptr;
	z            = 0;

	for (; k <= g; k++) {
		a = c[k];
		while (a--) {
			// make tables up to required level
			while (k > w + l[h]) {
				w += l[h++];

				// compute minimum size table less than or equal to *m bits
				z = (z = static_cast<unsigned>(g - w)) > *m ? *m : z;
				if ((f = 1u << (j = static_cast<unsigned>(k - w))) > a + 1) {
					f -= a + 1;
					xp = c + k;
					while (++j < z) {
						if ((f <<= 1) <= *++xp) {
							break;
						}
						f -= *xp;
					}
				}
				if (static_cast<unsigned>(w) + j > el && static_cast<unsigned>(w) < el) {
					j = el - static_cast<unsigned>(w);
				}
				z    = 1u << j;
				l[h] = static_cast<int>(j);

				// allocate and link in new table
				q = static_cast<huft*>(std::malloc((z + 1) * sizeof(huft)));
				if (q == nullptr) {
					if (h) {
						huft_free(u[0]);
					}
					return 3;
				}
				std::memset(static_cast<void*>(q), 0, (z + 1) * sizeof(huft));
				*t = q + 1;
				*(t = &(q->v.t)) = nullptr;
				u[h] = ++q;

				// connect to last table, if there is one
				if (h) {
					x[h]  = i;
					r.b   = static_cast<uint8_t>(l[h - 1]);
					r.e   = static_cast<uint8_t>(32 + j);
					r.v.t = q;
					j = (i & ((1u << w) - 1)) >> (w - l[h - 1]);
					u[h - 1][j] = r;
				}
			}

			// set up table entry in r
			r.b = static_cast<uint8_t>(k - w);
			if (pv >= v + n) {
				r.e = INVALID_CODE;
			} else if (*pv < s) {
				r.e   = static_cast<uint8_t>(*pv < 256 ? 32 : 31);
				r.v.n = static_cast<uint16_t>(*pv++);
			} else {
				r.e   = e[*pv - s];
				r.v.n = d[*pv++ - s];
			}

			// fill code-like entries with r
			f = 1u << (k - w);
			for (j = i >> w; j < z; j += f) {
				q[j] = r;
			}

			// backwards increment the k-bit code i
			for (j = 1u << (k - 1); i & j; j >>= 1) {
				i ^= j;
			}
			i ^= j;

			// backup over finished tables
			while ((i & ((1u << w) - 1)) != x[h]) {
				w -= l[--h];
			}
		}
	}

	*m = static_cast<unsigned>(l[0]);
	return y != 0 && g != 1;
}

// ---- explode.c ----

constexpr uint16_t cplen2[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
                               15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                               28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
                               41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
                               54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65};
constexpr uint16_t cplen3[] = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                               16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                               29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                               42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
                               55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66};
constexpr uint8_t extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8};
constexpr uint16_t cpdist4[] = {1,    65,   129,  193,  257,  321,  385,  449,
                                513,  577,  641,  705,  769,  833,  897,  961,
                                1025, 1089, 1153, 1217, 1281, 1345, 1409, 1473,
                                1537, 1601, 1665, 1729, 1793, 1857, 1921, 1985,
                                2049, 2113, 2177, 2241, 2305, 2369, 2433, 2497,
                                2561, 2625, 2689, 2753, 2817, 2881, 2945, 3009,
                                3073, 3137, 3201, 3265, 3329, 3393, 3457, 3521,
                                3585, 3649, 3713, 3777, 3841, 3905, 3969, 4033};
constexpr uint16_t cpdist8[] = {1,    129,  257,  385,  513,  641,  769,  897,
                                1025, 1153, 1281, 1409, 1537, 1665, 1793, 1921,
                                2049, 2177, 2305, 2433, 2561, 2689, 2817, 2945,
                                3073, 3201, 3329, 3457, 3585, 3713, 3841, 3969,
                                4097, 4225, 4353, 4481, 4609, 4737, 4865, 4993,
                                5121, 5249, 5377, 5505, 5633, 5761, 5889, 6017,
                                6145, 6273, 6401, 6529, 6657, 6785, 6913, 7041,
                                7169, 7297, 7425, 7553, 7681, 7809, 7937, 8065};

constexpr unsigned wszimpl = 0x8000;
// NEEDBITS may fetch one byte past the data before the code length is
// known; more zero bytes than that is a truncated stream.
constexpr uint64_t MaxOverread = 4;

// Bit lengths for a code representation (explode.c:222).
bool get_tree(ByteReader& in, unsigned* l, const unsigned n)
{
	unsigned i = in.Next() + 1u; // length/count pairs to read
	unsigned k = 0;              // next code
	do {
		unsigned j       = in.Next();
		const unsigned b = (j & 0xf) + 1; // bits in code (1..16)
		j                = ((j & 0xf0) >> 4) + 1;
		if (k + j > n) {
			return false;
		}
		do {
			l[k++] = b;
		} while (--j);
	} while (--i);
	return k == n && in.overread == 0;
}

struct ExplodeState {
	std::array<uint8_t, wszimpl> slide = {};
};

} // namespace

Result Explode(const Source& in, const uint64_t compressed_size,
               const uint64_t out_size, const bool window_8k,
               const bool literal_tree, const Sink& sink, std::string* detail)
{
	ByteReader reader(in, compressed_size);
	Output out(sink, out_size);
	auto state = std::make_unique<ExplodeState>();
	auto& slide = state->slide;

	HuftTable tb = {};
	HuftTable tl = {};
	HuftTable td = {};
	unsigned bb  = 0;
	unsigned bl  = 7;
	unsigned bd  = 0;
	unsigned bdl = 0;
	unsigned l[256] = {};

	auto build = [&](HuftTable& table, const unsigned n, const unsigned s,
	                 const uint16_t* d, const uint8_t* e, unsigned* m) {
		const auto r = huft_build(l, n, s, d, e, &table.t, m);
		if (r == 3) {
			table.t = nullptr; // huft_build freed what it had made
		}
		return r == 0 && table.t != nullptr;
	};
	auto refuse = [&](const char* why) {
		Fail(detail, why);
		return Result::Corrupt;
	};
	// After decoding began: a sink failure outranks the stream's fault.
	auto fail = [&](const char* why) {
		Fail(detail, why);
		return Finish(out, false);
	};

	if (literal_tree) {
		bb = 9;
		if (!get_tree(reader, l, 256) || !build(tb, 256, 256, nullptr, nullptr, &bb)) {
			return refuse("invalid literal code in the implode stream");
		}
		if (!get_tree(reader, l, 64) || !build(tl, 64, 0, cplen3, extra, &bl)) {
			return refuse("invalid length code in the implode stream");
		}
	} else {
		if (!get_tree(reader, l, 64) || !build(tl, 64, 0, cplen2, extra, &bl)) {
			return refuse("invalid length code in the implode stream");
		}
	}
	bd = (compressed_size - std::min(compressed_size, reader.taken)) > 200000 ? 8 : 7;
	if (!get_tree(reader, l, 64)) {
		return refuse("invalid distance code in the implode stream");
	}
	bdl = window_8k ? 7 : 6;
	if (!build(td, 64, 0, window_8k ? cpdist8 : cpdist4, extra, &bd)) {
		return refuse("invalid distance code in the implode stream");
	}

	uint32_t b = 0; // bit buffer
	unsigned k = 0; // number of bits in bit buffer
	auto need  = [&](const unsigned n) {
		while (k < n) {
			b |= static_cast<uint32_t>(reader.Next()) << k;
			k += 8;
		}
		return reader.overread <= MaxOverread;
	};
	auto dump = [&](const unsigned n) {
		b >>= n;
		k -= n;
	};
	// DECODEHUFT: implode stores its codes bit-inverted, hence ~b.
	auto decode = [&](const huft* htab, const unsigned bits, const unsigned mask,
	                  const huft*& t, unsigned& e) {
		if (!need(bits)) {
			return false;
		}
		t = htab + ((~b) & mask);
		while (true) {
			dump(t->b);
			if ((e = t->e) <= 32) {
				return true;
			}
			if (e == INVALID_CODE) {
				return false;
			}
			e &= 31;
			if (!need(e)) {
				return false;
			}
			t = t->v.t + ((~b) & mask_bits[e]);
		}
	};

	unsigned w     = 0; // current window position
	unsigned u     = 1; // true if unflushed
	const auto mb  = mask_bits[bb];
	const auto ml  = mask_bits[bl];
	const auto md  = mask_bits[bd];
	const auto mdl = mask_bits[bdl];
	auto flush     = [&](const unsigned count) {
		return out.Put(slide.data(), count, detail);
	};

	uint64_t s = out_size;
	while (s > 0) {
		const huft* t = nullptr;
		unsigned e    = 0;
		if (!need(1)) {
			return fail("the implode stream ends early");
		}
		if (b & 1) {
			dump(1);
			s--;
			if (tb.t) {
				if (!decode(tb.t, bb, mb, t, e)) {
					return fail("invalid literal in the implode stream");
				}
				slide[w++] = static_cast<uint8_t>(t->v.n);
			} else {
				if (!need(8)) {
					return fail("the implode stream ends early");
				}
				slide[w++] = static_cast<uint8_t>(b);
				dump(8);
			}
			if (w == wszimpl) {
				if (!flush(w)) {
					return Finish(out, false);
				}
				w = u = 0;
			}
		} else {
			dump(1);
			if (!need(bdl)) {
				return fail("the implode stream ends early");
			}
			unsigned d = b & mdl;
			dump(bdl);
			if (!decode(td.t, bd, md, t, e)) {
				return fail("invalid distance in the implode stream");
			}
			d = w - d - t->v.n;
			if (!decode(tl.t, bl, ml, t, e)) {
				return fail("invalid length in the implode stream");
			}
			unsigned n = t->v.n;
			if (e) {
				if (!need(8)) {
					return fail("the implode stream ends early");
				}
				n += b & 0xff;
				dump(8);
			}

			// do the copy
			s = (s > n ? s - n : 0);
			do {
				e = wszimpl - ((d &= wszimpl - 1) > w ? d : w);
				if (e > n) {
					e = n;
				}
				n -= e;
				if (u && w <= d) {
					std::memset(slide.data() + w, 0, e);
					w += e;
					d += e;
				} else if (w - d >= e) {
					std::memcpy(slide.data() + w, slide.data() + d, e);
					w += e;
					d += e;
				} else {
					do {
						slide[w++] = slide[d++];
					} while (--e);
				}
				if (w == wszimpl) {
					if (!flush(w)) {
						return Finish(out, false);
					}
					w = u = 0;
				}
			} while (n);
		}
	}

	if (!flush(w)) {
		return Finish(out, false);
	}
	// explode.c:361; bytes left over only warn in UnZip, bytes missing fail.
	if (reader.overread > (k >> 3)) {
		return refuse("the implode stream needed bytes past its end");
	}
	return Result::Ok;
}

} // namespace LegacyZip
