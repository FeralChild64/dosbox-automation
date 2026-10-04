// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_toml.h"
#include "dos/archive/descriptor.h"

#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#if !defined(WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace ArchiveToml;
using ArchiveMount::CloseDescriptor;
using ArchiveMount::OpenRegularFileNoFollow;

namespace {

std::string Slurp(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), {});
}

TEST(ArchiveToml, ParsesTheThreeValueKinds)
{
	auto error = std::string();
	const auto doc =
	        Parse("# comment\n\n"
	              "archive_path = \"/games/doom.zip\"\n"
	              "entry_count = 42\n"
	              "negative = -7\n"
	              "complete = true   # trailing comment\n",
	              &error);
	ASSERT_TRUE(doc) << error;
	EXPECT_EQ(doc->root.String("archive_path"), "/games/doom.zip");
	EXPECT_EQ(doc->root.Integer("entry_count"), 42);
	EXPECT_EQ(doc->root.Integer("negative"), -7);
	EXPECT_EQ(doc->root.Boolean("complete"), true);
	EXPECT_FALSE(doc->root.String("entry_count"));
	EXPECT_FALSE(doc->root.Integer("missing"));
	EXPECT_TRUE(doc->arrays.empty());
}

TEST(ArchiveToml, DecodesStringEscapes)
{
	auto error = std::string();
	const auto doc = Parse("s = \"a\\\"b\\\\c\\nd\\te\\u00e9\\U0001F600\"\n",
	                       &error);
	ASSERT_TRUE(doc) << error;
	EXPECT_EQ(doc->root.String("s"),
	          std::string("a\"b\\c\nd\te\xc3\xa9\xf0\x9f\x98\x80"));
}

TEST(ArchiveToml, ParsesTableArraysInOrder)
{
	auto error = std::string();
	const auto doc =
	        Parse("version = 1\n"
	              "[[changes]]\n"
	              "op = \"delete\"\n"
	              "path = \"A.TXT\"\n"
	              "[[changes]]\n"
	              "op = \"rename\"\n"
	              "from = \"B.TXT\"\n"
	              "to = \"C.TXT\"\n"
	              "[[other]]\n"
	              "x = 1\n",
	              &error);
	ASSERT_TRUE(doc) << error;
	EXPECT_EQ(doc->root.Integer("version"), 1);
	const auto changes = doc->Array("changes");
	ASSERT_EQ(changes.size(), 2u);
	EXPECT_EQ(changes[0]->String("op"), "delete");
	EXPECT_EQ(changes[1]->String("to"), "C.TXT");
	EXPECT_EQ(doc->Array("other").size(), 1u);
	EXPECT_TRUE(doc->Array("none").empty());
}

TEST(ArchiveToml, RefusesWhatTheEngineNeverWrites)
{
	const char* bad[] = {
	        "[table]\n",
	        "a.b = 1\n",
	        "x = {a = 1}\n",
	        "x = 1.5\n",
	        "x = 1979-05-27\n",
	        "x = 'literal'\n",
	        "x = \"unterminated\n",
	        "x = \"ctrl\x01\"\n",
	        "x = \"bad\\q\"\n",
	        "x = \"\\u12\"\n",
	        "x = 1_000\n",
	        "x = 99999999999999999999\n",
	        "x = -\n",
	        "x\n",
	        "= 1\n",
	        "bad key = 1\n",
	        "x = 1 y = 2\n",
	        "x = 1\nx = 2\n",
	        "[[a]]\nx = 1\n[[a]]\nx = 1\n[[a]]\nx = 1\nx = 2\n",
	        "[[\n",
	        "[[a]] trailing\n",
	        "x = \"\\uD800\"\n",
	};
	for (const auto* text : bad) {
		auto error = std::string();
		EXPECT_FALSE(Parse(text, &error)) << text;
		EXPECT_FALSE(error.empty()) << text;
	}
}

TEST(ArchiveToml, RefusesOversizedInput)
{
	auto error     = std::string();
	const auto big = std::string(MaxDocumentBytes + 1, '#');
	EXPECT_FALSE(Parse(big, &error));
	auto lines = std::string();
	for (int i = 0; i <= MaxDocumentLines; ++i) {
		lines += "\n";
	}
	EXPECT_FALSE(Parse(lines, &error));
	const auto key = "k = \"" + std::string(MaxValueBytes + 1, 'v') + "\"\n";
	EXPECT_FALSE(Parse(key, &error));
	EXPECT_FALSE(Parse(std::string(MaxKeyBytes + 1, 'k') + " = 1\n", &error));
}

TEST(ArchiveToml, SerializeRoundTripsAndIsDeterministic)
{
	Document doc = {};
	doc.root.Set("archive_path", "C:\\Games\\d\"o\"om.zip");
	doc.root.Set("entry_count", int64_t{42});
	doc.root.Set("complete", false);
	doc.root.Set("unicode", "caf\xc3\xa9 \x01");
	Table change = {};
	change.Set("op", "delete");
	change.Set("path", "A.TXT");
	doc.arrays.emplace_back("changes", change);
	doc.arrays.emplace_back("changes", change);

	const auto text = Serialize(doc);
	EXPECT_EQ(text, Serialize(doc));
	auto error      = std::string();
	const auto back = Parse(text, &error);
	ASSERT_TRUE(back) << error << "\n" << text;
	EXPECT_EQ(back->root.String("archive_path"), "C:\\Games\\d\"o\"om.zip");
	EXPECT_EQ(back->root.Integer("entry_count"), 42);
	EXPECT_EQ(back->root.Boolean("complete"), false);
	EXPECT_EQ(back->root.String("unicode"), "caf\xc3\xa9 \x01");
	EXPECT_EQ(back->Array("changes").size(), 2u);
	EXPECT_NE(text.find("\\u0001"), std::string::npos);
}

TEST(ArchiveToml, SetReplacesAnExistingKeyInPlace)
{
	Table t = {};
	t.Set("a", int64_t{1});
	t.Set("b", int64_t{2});
	t.Set("a", "one");
	ASSERT_EQ(t.pairs.size(), 2u);
	EXPECT_EQ(t.pairs[0].first, "a");
	EXPECT_EQ(t.String("a"), "one");
	EXPECT_FALSE(t.Integer("a"));
}

TEST(ArchiveToml, WritesAtomicallyAndReadsBack)
{
	const auto dir = TestTempDir::MakeUnique("archive_toml_");
	ASSERT_FALSE(dir.empty());
	const auto path = dir / "x.toml";
	Document doc    = {};
	doc.root.Set("v", int64_t{1});
	auto error = std::string();
	ASSERT_TRUE(WriteAtomically(path, doc, &error)) << error;
	EXPECT_TRUE(fs::is_regular_file(path));
	// No temporary left beside it.
	auto count = 0;
	for (const auto& e : fs::directory_iterator(dir)) {
		(void)e;
		++count;
	}
	EXPECT_EQ(count, 1);
	doc.root.Set("v", int64_t{2});
	ASSERT_TRUE(WriteAtomically(path, doc, &error)) << error;
	const auto back = ReadFile(path, &error);
	ASSERT_TRUE(back) << error;
	EXPECT_EQ(back->root.Integer("v"), 2);
	std::error_code ec = {};
	fs::remove_all(dir, ec);
}

TEST(ArchiveToml, ReadFileRefusesMissingOversizedAndNonRegular)
{
	const auto dir = TestTempDir::MakeUnique("archive_toml_");
	ASSERT_FALSE(dir.empty());
	auto error = std::string();
	EXPECT_FALSE(ReadFile(dir / "missing.toml", &error));
	EXPECT_FALSE(ReadFile(dir, &error));
	{
		std::ofstream out(dir / "big.toml", std::ios::binary);
		out << std::string(MaxDocumentBytes + 1, '#');
	}
	EXPECT_FALSE(ReadFile(dir / "big.toml", &error));
	EXPECT_NE(error.find("large"), std::string::npos);
	std::error_code ec = {};
	fs::remove_all(dir, ec);
}

// Sidecars and caches live wherever the user keeps games; on Windows a
// narrow open fails for names outside the ANSI code page.
TEST(ArchiveToml, ReadsBackUnderANameOutsideTheAnsiCodePage)
{
	const auto base = TestTempDir::MakeUnique("archive_toml_");
	ASSERT_FALSE(base.empty());
	const auto dir = base / fs::path(u8"\u0106wiczenia-\u017b\u00f3\u0142w");
	fs::create_directories(dir);
	const auto path = dir / "x.toml";
	Document doc    = {};
	doc.root.Set("v", int64_t{7});
	auto error = std::string();
	ASSERT_TRUE(WriteAtomically(path, doc, &error)) << error;
	const auto back = ReadFile(path, &error);
	ASSERT_TRUE(back) << error;
	EXPECT_EQ(back->root.Integer("v"), 7);
	std::error_code ec = {};
	fs::remove_all(base, ec);
}

#if !defined(WIN32)
TEST(ArchiveToml, WriteAtomicallyNeverFollowsALinkAtTheTarget)
{
	const auto dir = TestTempDir::MakeUnique("archive_toml_");
	ASSERT_FALSE(dir.empty());
	const auto victim = dir / "victim";
	{
		std::ofstream out(victim, std::ios::binary);
		out << "keep";
	}
	const auto link = dir / "x.toml";
	fs::create_symlink(victim, link);
	Document doc = {};
	doc.root.Set("v", int64_t{1});
	auto error = std::string();
	// The rename replaces the link itself; the victim is untouched.
	ASSERT_TRUE(WriteAtomically(link, doc, &error)) << error;
	EXPECT_EQ(Slurp(victim), "keep");
	EXPECT_TRUE(fs::is_regular_file(fs::symlink_status(link)));
	std::error_code ec = {};
	fs::remove_all(dir, ec);
}

// A record in a game folder can be swapped after any check by path; the
// open itself must refuse what is not a regular file, without blocking.
TEST(ArchiveToml, TheRecordOpenRefusesFifosAndLinksWithoutBlocking)
{
	const auto dir = TestTempDir::MakeUnique("archive_toml_");
	ASSERT_FALSE(dir.empty());
	const auto fifo = dir / "fifo.toml";
	ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
	EXPECT_EQ(OpenRegularFileNoFollow(fifo), -1);
	const auto real = dir / "real.toml";
	{
		std::ofstream out(real, std::ios::binary);
		out << "v = 1\n";
	}
	const auto link = dir / "link.toml";
	fs::create_symlink(real, link);
	EXPECT_EQ(OpenRegularFileNoFollow(link), -1);
	const auto fd = OpenRegularFileNoFollow(real);
	EXPECT_GE(fd, 0);
	CloseDescriptor(fd);
	EXPECT_EQ(OpenRegularFileNoFollow(dir), -1);
	std::error_code ec = {};
	fs::remove_all(dir, ec);
}
#endif

} // namespace
