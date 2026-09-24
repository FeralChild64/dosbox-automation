// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "dos/dos.h"

#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "hardware/memory.h"

#include "dosbox_test_fixture.h"

namespace {

class DOS_PSPEnvironmentTest : public DOSBoxTestFixture {};

constexpr uint16_t PspParagraphs = 0x10;
constexpr uint16_t EnvParagraphs = 0x10;

uint16_t allocate(uint16_t paragraphs)
{
	uint16_t segment = 0;
	uint16_t blocks  = paragraphs;
	EXPECT_TRUE(DOS_AllocateMemory(&segment, &blocks));
	return segment;
}

// With no current PSP a plain allocation looks free and is handed out
// again; a PSP block owns itself, as DOS_Execute sets it up.
uint16_t allocate_psp()
{
	const uint16_t current = dos.psp();
	dos.psp(MCB_DOS);
	const uint16_t segment = allocate(PspParagraphs);
	dos.psp(current);
	DOS_MCB(segment - 1).SetPSPSeg(segment);
	return segment;
}

// An environment block owned by the program at psp_segment, as
// DOS_Execute allocates it while the child is the current PSP.
uint16_t allocate_owned_by(uint16_t psp_segment, uint16_t paragraphs)
{
	const uint16_t current = dos.psp();
	dos.psp(psp_segment);
	const uint16_t segment = allocate(paragraphs);
	dos.psp(current);
	for (uint16_t i = 0; i < paragraphs * 16; ++i) {
		mem_writeb(PhysicalMake(segment, i), 0);
	}
	return segment;
}

TEST_F(DOS_PSPEnvironmentTest, an_environment_without_a_memory_block_is_refused)
{
	const uint16_t psp_segment = allocate_psp();
	const uint16_t block       = allocate(EnvParagraphs);
	// One paragraph into our own block: the paragraph before it holds a
	// size word as an MCB would, but no 'M' or 'Z' signature.
	constexpr uint16_t ForgedSizeParagraphs = 0x08;
	mem_writeb(PhysicalMake(block, 0), 0);
	mem_writew(PhysicalMake(block, 3), ForgedSizeParagraphs);
	for (uint16_t i = 0; i < ForgedSizeParagraphs * 16; ++i) {
		mem_writeb(PhysicalMake(block + 1, i), 0);
	}
	DOS_PSP psp(psp_segment);
	psp.SetEnvironment(block + 1);

	EXPECT_FALSE(psp.SetEnvironmentValue("APPEND", "c:\\x"));
	EXPECT_EQ(mem_readb(PhysicalMake(block + 1, 0)), 0);

	DOS_FreeMemory(block);
	DOS_FreeMemory(psp_segment);
}

TEST_F(DOS_PSPEnvironmentTest, a_freed_environment_block_is_refused)
{
	const uint16_t psp_segment = allocate_psp();
	const uint16_t env_segment = allocate_owned_by(psp_segment,
	                                               EnvParagraphs);
	DOS_PSP psp(psp_segment);
	psp.SetEnvironment(env_segment);
	DOS_FreeMemory(env_segment);

	EXPECT_FALSE(psp.SetEnvironmentValue("APPEND", "c:\\x"));
	EXPECT_EQ(mem_readb(PhysicalMake(env_segment, 0)), 0);

	DOS_FreeMemory(psp_segment);
}

TEST_F(DOS_PSPEnvironmentTest, a_block_without_a_terminator_is_left_as_it_was)
{
	const uint16_t psp_segment = allocate_psp();
	const uint16_t env_segment = allocate_owned_by(psp_segment,
	                                               EnvParagraphs);
	constexpr uint8_t Filler = 'X';
	for (uint16_t i = 0; i < EnvParagraphs * 16; ++i) {
		mem_writeb(PhysicalMake(env_segment, i), Filler);
	}
	DOS_PSP psp(psp_segment);
	psp.SetEnvironment(env_segment);

	EXPECT_FALSE(psp.SetEnvironmentValue("APPEND", "c:\\x"));
	for (uint16_t i = 0; i < EnvParagraphs * 16; ++i) {
		ASSERT_EQ(mem_readb(PhysicalMake(env_segment, i)), Filler) << i;
	}

	DOS_FreeMemory(env_segment);
	DOS_FreeMemory(psp_segment);
}

// ada-hb5a: the old code compacted the entries before it knew the new
// one would not fit.
TEST_F(DOS_PSPEnvironmentTest, a_change_that_does_not_fit_leaves_the_environment_as_it_was)
{
	constexpr uint16_t SmallParagraphs = 2;
	const uint16_t psp_segment = allocate_psp();
	const uint16_t env_segment = allocate_owned_by(psp_segment,
	                                               SmallParagraphs);
	constexpr std::string_view Entries = {"A=1\0APPEND=x\0B=2\0", 17};
	for (size_t i = 0; i < Entries.size(); ++i) {
		mem_writeb(PhysicalMake(env_segment, static_cast<uint16_t>(i)),
		           static_cast<uint8_t>(Entries[i]));
	}
	DOS_PSP psp(psp_segment);
	psp.SetEnvironment(env_segment);

	EXPECT_FALSE(psp.SetEnvironmentValue("APPEND", "c:\\a\\much\\longer\\list\\of\\dirs"));
	for (size_t i = 0; i < Entries.size(); ++i) {
		ASSERT_EQ(mem_readb(PhysicalMake(env_segment, static_cast<uint16_t>(i))),
		          static_cast<uint8_t>(Entries[i]))
		        << i;
	}
	EXPECT_EQ(psp.GetEnvironmentValue("APPEND"), "x");

	DOS_FreeMemory(env_segment);
	DOS_FreeMemory(psp_segment);
}

TEST_F(DOS_PSPEnvironmentTest, an_entry_longer_than_1024_bytes_reads_back_whole)
{
	constexpr uint16_t LargeParagraphs = 0x80;
	const uint16_t psp_segment         = allocate_psp();
	const uint16_t env_segment = allocate_owned_by(psp_segment,
	                                               LargeParagraphs);
	DOS_PSP psp(psp_segment);
	psp.SetEnvironment(env_segment);
	const std::string value(1030, 'x');

	EXPECT_TRUE(psp.SetEnvironmentValue("A", value));
	EXPECT_EQ(psp.GetEnvironmentValue("A"), value);
	const auto all = psp.GetAllRawEnvironmentStrings();
	ASSERT_EQ(all.size(), 1u);
	EXPECT_EQ(all[0], "A=" + value);

	DOS_FreeMemory(env_segment);
	DOS_FreeMemory(psp_segment);
}

TEST_F(DOS_PSPEnvironmentTest, an_allocated_environment_takes_the_value)
{
	const uint16_t psp_segment = allocate_psp();
	const uint16_t env_segment = allocate_owned_by(psp_segment,
	                                               EnvParagraphs);
	DOS_PSP psp(psp_segment);
	psp.SetEnvironment(env_segment);

	EXPECT_TRUE(psp.SetEnvironmentValue("APPEND", "c:\\x"));
	EXPECT_EQ(psp.GetEnvironmentValue("APPEND"), "c:\\x");

	DOS_FreeMemory(env_segment);
	DOS_FreeMemory(psp_segment);
}

} // namespace
