// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "hardware/input/private/intel8042.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "hardware/port.h"

// Wide reads on 0x60/0x64 must compose adjacent ports (ISA bus behaviour).
// The dword VMware registration shadows the IO layer's own composition.

namespace {

uint8_t adjacent_value[8] = {};

uint8_t read_adjacent([[maybe_unused]] io_port_t port,
                      [[maybe_unused]] io_width_t width)
{
	return adjacent_value[port & 0x7];
}

class I8042WideRead : public ::testing::Test {
protected:
	void SetUp() override
	{
		I8042_Init();

		for (const io_port_t port :
		     {0x61, 0x62, 0x63, 0x65, 0x66, 0x67}) {
			IO_RegisterReadHandler(port,
			                       read_adjacent,
			                       io_width_t::byte);
		}
		for (auto& value : adjacent_value) {
			value = 0;
		}
	}
};

TEST_F(I8042WideRead, word_read_of_data_port_composes_0x61)
{
	adjacent_value[0x61 & 0x7] = 0xa5;

	const auto value = IO_ReadW(0x60);
	EXPECT_EQ(value & 0xff, IO_ReadB(0x60));
	EXPECT_EQ((value >> 8) & 0xff, 0xa5);
}

TEST_F(I8042WideRead, dword_read_of_data_port_composes_adjacent)
{
	adjacent_value[0x61 & 0x7] = 0xa5;
	adjacent_value[0x62 & 0x7] = 0x5a;
	adjacent_value[0x63 & 0x7] = 0x3c;

	const auto value = IO_ReadD(0x60);
	EXPECT_EQ(value & 0xff, IO_ReadB(0x60));
	EXPECT_EQ((value >> 8) & 0xff, 0xa5);
	EXPECT_EQ((value >> 16) & 0xff, 0x5a);
	EXPECT_EQ((value >> 24) & 0xff, 0x3c);
}

TEST_F(I8042WideRead, word_read_of_status_port_composes_0x65)
{
	adjacent_value[0x65 & 0x7] = 0xc3;

	const auto value = IO_ReadW(0x64);
	EXPECT_EQ(value & 0xff, IO_ReadB(0x64));
	EXPECT_EQ((value >> 8) & 0xff, 0xc3);
}

TEST_F(I8042WideRead, dword_read_of_status_port_composes_adjacent)
{
	adjacent_value[0x65 & 0x7] = 0xc3;
	adjacent_value[0x66 & 0x7] = 0x81;
	adjacent_value[0x67 & 0x7] = 0x42;

	const auto value = IO_ReadD(0x64);
	EXPECT_EQ(value & 0xff, IO_ReadB(0x64));
	EXPECT_EQ((value >> 8) & 0xff, 0xc3);
	EXPECT_EQ((value >> 16) & 0xff, 0x81);
	EXPECT_EQ((value >> 24) & 0xff, 0x42);
}

// Wide write composition: high bytes forwarded to adjacent ports.

uint8_t written_value[8] = {};

void write_adjacent(io_port_t port, uint8_t value,
                    [[maybe_unused]] io_width_t width)
{
	written_value[port & 0x7] = value;
}

class I8042WideWrite : public ::testing::Test {
protected:
	void SetUp() override
	{
		I8042_Init();

		for (const io_port_t port :
		     {0x65, 0x66, 0x67}) {
			IO_RegisterWriteHandler(port,
			                        write_adjacent,
			                        io_width_t::byte);
		}
		for (auto& v : written_value) {
			v = 0;
		}
	}
};

TEST_F(I8042WideWrite, word_write_to_command_port_forwards_high_byte)
{
	IO_WriteW(0x64, 0xab00);

	EXPECT_EQ(written_value[0x65 & 0x7], 0xab);
}

TEST_F(I8042WideWrite, dword_write_to_command_port_forwards_high_bytes)
{
	IO_WriteD(0x64, 0x42'81'ab'00);

	EXPECT_EQ(written_value[0x65 & 0x7], 0xab);
	EXPECT_EQ(written_value[0x66 & 0x7], 0x81);
	EXPECT_EQ(written_value[0x67 & 0x7], 0x42);
}

TEST_F(I8042WideWrite, byte_write_to_command_port_does_not_forward)
{
	IO_WriteB(0x64, 0xfe);

	EXPECT_EQ(written_value[0x65 & 0x7], 0);
	EXPECT_EQ(written_value[0x66 & 0x7], 0);
	EXPECT_EQ(written_value[0x67 & 0x7], 0);
}

} // namespace
