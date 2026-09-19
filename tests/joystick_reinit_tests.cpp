// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include <unordered_map>

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "hardware/input/joystick.h"
#include "hardware/port.h"

extern std::unordered_map<io_port_t, io_read_f> io_read_handlers[io_widths];
extern std::unordered_map<io_port_t, io_write_f> io_write_handlers[io_widths];

namespace {

constexpr io_port_t joystick_port = 0x201;

class JoystickReinitTest : public DOSBoxTestFixture {
protected:
	void SetUp() override
	{
		DOSBoxTestFixture::SetUp();
		JOYSTICK_Init();
	}

	void TearDown() override
	{
		JOYSTICK_Destroy();
		DOSBoxTestFixture::TearDown();
	}

	bool has_read_handler() const
	{
		return io_read_handlers[0].count(joystick_port) > 0;
	}

	bool has_write_handler() const
	{
		return io_write_handlers[0].count(joystick_port) > 0;
	}
};

// Regression test: runtime joystick setting changes must not lose
// the I/O port handler. Before the fix, make_unique constructed the
// new JOYSTICK (installing the handler) before unique_ptr::operator=
// destroyed the old one (uninstalling it). The uninstall ran last
// and erased the freshly installed handler.
TEST_F(JoystickReinitTest, PortHandlerSurvivesRuntimeReinit)
{
	ASSERT_TRUE(has_read_handler());
	ASSERT_TRUE(has_write_handler());

	auto* section = get_section("joystick");
	auto* prop = section->GetProperty("autofire");
	section->ExecuteUpdate(*prop);

	EXPECT_TRUE(has_read_handler());
	EXPECT_TRUE(has_write_handler());
}

// The handler must survive repeated reinit cycles without
// accumulating stale state or crashing.
TEST_F(JoystickReinitTest, PortHandlerSurvivesRepeatedReinit)
{
	auto* section = get_section("joystick");
	auto* prop = section->GetProperty("autofire");

	for (int i = 0; i < 50; ++i) {
		section->ExecuteUpdate(*prop);
	}

	EXPECT_TRUE(has_read_handler());
	EXPECT_TRUE(has_write_handler());
}

} // namespace
