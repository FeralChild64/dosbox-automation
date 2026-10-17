// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "dos/floppy_image.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <system_error>

#include "ints/bios_disk.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace FloppyImage {

namespace {

FILE* OpenForReading(const std::filesystem::path& path)
{
#if defined(WIN32)
	return _wfopen(path.wstring().c_str(), L"rbN");
#else
	return std::fopen(path.c_str(), "rbe");
#endif
}

// MS-DOS 2.0 boot record BPB: 0Bh sector size, 13h total sectors,
// 18h sectors/track, 1Ah heads.
bool BpbNamesGeometry(const std::filesystem::path& host_path, const DiskGeometry& geo)
{
	FILE* f = OpenForReading(host_path);
	if (!f) {
		return false;
	}
	std::array<uint8_t, 512> bs = {};
	const bool read_ok = std::fread(bs.data(), 1, bs.size(), f) == bs.size();
	std::fclose(f);
	if (!read_ok || bs[510] != 0x55 || bs[511] != 0xaa) {
		return false;
	}
	auto u16 = [&](const size_t at) {
		return static_cast<uint32_t>(bs[at] | (bs[at + 1] << 8));
	};
	return u16(0x0b) == 512 && u16(0x13) == geo.ksize * 2 &&
	       u16(0x18) == geo.secttrack && u16(0x1a) == geo.headscyl;
}

} // namespace

bool IsFloppy(const std::filesystem::path& host_path)
{
	std::error_code ec = {};
	const auto size    = std::filesystem::file_size(host_path, ec);
	if (ec) {
		return false;
	}
	const auto size_kb     = static_cast<uint32_t>(size / 1024);
	const auto& geometries = BIOS_GetDiskGeometryList();
	const auto table_size  = [size_kb](const DiskGeometry& geo) {
                return geo.ksize == size_kb;
	};
	if (std::ranges::any_of(geometries, table_size)) {
		return true;
	}
	return std::ranges::any_of(geometries, [&](const DiskGeometry& geo) {
		return geo.ksize + 1 == size_kb && BpbNamesGeometry(host_path, geo);
	});
}

} // namespace FloppyImage
