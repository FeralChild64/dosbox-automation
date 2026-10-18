// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
// Turning IPX on and off at runtime, the way CONFIG -set and `ipx on` do it:
// the section takes the new line, then runs its update handlers.

#include "hardware/network/ipx.h"

#include "config/setup.h"
#include "dos/dos.h"
#include "dos/dos_system.h"
#include "dosbox_test_fixture.h"
#include "hardware/memory.h"

#include <gtest/gtest.h>

#include <string>

namespace {

// IRQ 11, which the IPX emulation points at its ESR stub.
constexpr uint8_t IpxEsrVector = 0x73;

bool ipxnet_exists()
{
	return DOS_FindFirst("Z:\\IPXNET.COM", FatAttributeFlags{}, false);
}

void set_ipx_at_runtime(const std::string& value)
{
	auto* section  = get_section("ipx");
	auto* property = section->GetProperty("ipx");
	ASSERT_TRUE(section->HandleInputLine("ipx=" + value));
	section->ExecuteUpdate(*property);
}

class IpxSettingTest : public DOSBoxTestFixture {
protected:
	void SetUp() override
	{
		DOSBoxTestFixture::SetUp();
		vector_before_ipx = RealGetVec(IpxEsrVector);
	}

	void TearDown() override
	{
		IPX_Destroy();
		DOSBoxTestFixture::TearDown();
	}

	void start_engine_with_ipx(const std::string& value)
	{
		set_section_property_value("ipx", "ipx", value);
		IPX_Init();
	}

	RealPt vector_before_ipx = 0;
};

TEST_F(IpxSettingTest, TurningIpxOnAtRuntimeInstallsIt)
{
	start_engine_with_ipx("off");

	set_ipx_at_runtime("on");

	EXPECT_TRUE(ipxnet_exists());
	EXPECT_NE(RealGetVec(IpxEsrVector), vector_before_ipx);
	EXPECT_NE(RealGetVec(IpxEsrVector), 0u);
}

TEST_F(IpxSettingTest, TurningIpxOffAtRuntimeRemovesIt)
{
	start_engine_with_ipx("on");
	ASSERT_TRUE(ipxnet_exists());

	set_ipx_at_runtime("off");

	EXPECT_FALSE(ipxnet_exists());
	EXPECT_EQ(RealGetVec(IpxEsrVector), vector_before_ipx);
}

TEST_F(IpxSettingTest, OnOffOnLeavesOneWorkingInstall)
{
	start_engine_with_ipx("off");

	set_ipx_at_runtime("on");
	set_ipx_at_runtime("off");
	set_ipx_at_runtime("on");

	EXPECT_TRUE(ipxnet_exists());
	EXPECT_NE(RealGetVec(IpxEsrVector), vector_before_ipx);
	EXPECT_NE(RealGetVec(IpxEsrVector), 0u);
}

TEST_F(IpxSettingTest, SettingOnAgainKeepsTheInstall)
{
	start_engine_with_ipx("on");
	const auto esr_vector = RealGetVec(IpxEsrVector);

	set_ipx_at_runtime("on");

	EXPECT_TRUE(ipxnet_exists());
	EXPECT_EQ(RealGetVec(IpxEsrVector), esr_vector);
}

TEST_F(IpxSettingTest, DestroyAfterARuntimeOffRestoresTheVectorOnce)
{
	start_engine_with_ipx("on");
	set_ipx_at_runtime("off");

	IPX_Destroy();

	EXPECT_FALSE(ipxnet_exists());
	EXPECT_EQ(RealGetVec(IpxEsrVector), vector_before_ipx);
}

} // namespace
