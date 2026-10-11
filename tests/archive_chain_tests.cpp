// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/member_path.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "archive_fixtures.h"
#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_helper_client.h"
#include "dos/archive/chain_resolver.h"
#include "test_temp_dir.h"

using namespace ArchiveMount;
using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Kind;
using ArchiveFixtures::WriteArchive;
namespace fs = std::filesystem;

// --- IsArchiveName ---

TEST(IsArchiveName, RecognisesArchiveExtensions)
{
	EXPECT_TRUE(IsArchiveName("game.zip"));
	EXPECT_TRUE(IsArchiveName("GAME.ZIP"));
	EXPECT_TRUE(IsArchiveName("data.7z"));
	EXPECT_TRUE(IsArchiveName("pack.rar"));
	EXPECT_TRUE(IsArchiveName("files.tar.gz"));
	EXPECT_TRUE(IsArchiveName("files.tgz"));
	EXPECT_TRUE(IsArchiveName("files.tar.xz"));
	EXPECT_TRUE(IsArchiveName("files.tar.zst"));
	EXPECT_TRUE(IsArchiveName("files.tar.bz2"));
	EXPECT_TRUE(IsArchiveName("files.tar.lzma"));
}

TEST(IsArchiveName, RejectsNonArchiveNames)
{
	EXPECT_FALSE(IsArchiveName("disk1.img"));
	EXPECT_FALSE(IsArchiveName("game.iso"));
	EXPECT_FALSE(IsArchiveName("readme.txt"));
	EXPECT_FALSE(IsArchiveName("disk.img.xz"));
	EXPECT_FALSE(IsArchiveName(""));
}

// --- SplitChain: no nesting ---

TEST(SplitChain, APlainMemberHasNoLevels)
{
	MemberPath host  = {};
	host.archive     = "/tmp/game.zip";
	host.member      = "disk1.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	EXPECT_TRUE(chain->levels.empty());
	EXPECT_EQ(chain->host.member, "disk1.img");
}

TEST(SplitChain, ADirectoryMemberHasNoLevels)
{
	MemberPath host  = {};
	host.archive     = "/tmp/game.zip";
	host.member      = "subdir/disk1.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	EXPECT_TRUE(chain->levels.empty());
	EXPECT_EQ(chain->host.member, "subdir/disk1.img");
}

TEST(SplitChain, ALoneMemberHasNoLevels)
{
	MemberPath host  = {};
	host.archive     = "/tmp/game.imz";
	host.lone        = true;
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	EXPECT_TRUE(chain->levels.empty());
	EXPECT_TRUE(chain->host.lone);
}

TEST(SplitChain, AnEmptyMemberHasNoLevels)
{
	MemberPath host  = {};
	host.archive     = "/tmp/game.zip";
	host.member      = "";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	EXPECT_TRUE(chain->levels.empty());
}

// --- SplitChain: one nested level ---

TEST(SplitChain, OneInnerArchive)
{
	MemberPath host  = {};
	host.archive     = "/tmp/outer.zip";
	host.member      = "inner.zip/disk1.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 1u);
	EXPECT_EQ(chain->host.member, "inner.zip");
	EXPECT_EQ(chain->levels[0].archive_member, "inner.zip");
	EXPECT_EQ(chain->levels[0].leaf_member, "disk1.img");
}

TEST(SplitChain, InnerArchiveInASubdirectory)
{
	MemberPath host  = {};
	host.archive     = "/tmp/bundle.zip";
	host.member      = "games/thexder.zip/DISK1.IMG";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 1u);
	EXPECT_EQ(chain->host.member, "games/thexder.zip");
	EXPECT_EQ(chain->levels[0].archive_member, "games/thexder.zip");
	EXPECT_EQ(chain->levels[0].leaf_member, "DISK1.IMG");
}

TEST(SplitChain, LeafInASubdirectory)
{
	MemberPath host  = {};
	host.archive     = "/tmp/bundle.zip";
	host.member      = "inner.7z/GAME/disk1.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 1u);
	EXPECT_EQ(chain->levels[0].archive_member, "inner.7z");
	EXPECT_EQ(chain->levels[0].leaf_member, "GAME/disk1.img");
}

// --- SplitChain: two and three levels ---

TEST(SplitChain, TwoLevelsDeep)
{
	MemberPath host  = {};
	host.archive     = "/tmp/bundle.zip";
	host.member      = "middle.rar/inner.zip/disk1.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 2u);
	EXPECT_EQ(chain->host.member, "middle.rar");
	EXPECT_EQ(chain->levels[0].archive_member, "middle.rar");
	EXPECT_EQ(chain->levels[0].leaf_member, "");
	EXPECT_EQ(chain->levels[1].archive_member, "inner.zip");
	EXPECT_EQ(chain->levels[1].leaf_member, "disk1.img");
}

TEST(SplitChain, ThreeLevelsIsTheMaximum)
{
	MemberPath host  = {};
	host.archive     = "/tmp/a.zip";
	host.member      = "b.zip/c.rar/d.7z/disk.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 3u);
	EXPECT_EQ(chain->levels[0].archive_member, "b.zip");
	EXPECT_EQ(chain->levels[1].archive_member, "c.rar");
	EXPECT_EQ(chain->levels[2].archive_member, "d.7z");
	EXPECT_EQ(chain->levels[2].leaf_member, "disk.img");
}

TEST(SplitChain, FourLevelsIsRefused)
{
	MemberPath host = {};
	host.archive    = "/tmp/a.zip";
	host.member     = "b.zip/c.rar/d.7z/e.tar/disk.img";
	EXPECT_FALSE(SplitChain(host));
}

// --- SplitChain: edge cases ---

TEST(SplitChain, AnArchiveNameAsTheLastComponentIsALeaf)
{
	MemberPath host  = {};
	host.archive     = "/tmp/outer.zip";
	host.member      = "inner.zip";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	EXPECT_TRUE(chain->levels.empty());
	EXPECT_EQ(chain->host.member, "inner.zip");
}

TEST(SplitChain, BackslashSeparatorsWork)
{
	MemberPath host  = {};
	host.archive     = "/tmp/outer.zip";
	host.member      = "inner.zip\\disk1.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 1u);
	EXPECT_EQ(chain->levels[0].archive_member, "inner.zip");
	EXPECT_EQ(chain->levels[0].leaf_member, "disk1.img");
}

TEST(SplitChain, ConsecutiveSeparatorsAreSkipped)
{
	MemberPath host  = {};
	host.archive     = "/tmp/outer.zip";
	host.member      = "inner.zip//disk1.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 1u);
	EXPECT_EQ(chain->levels[0].leaf_member, "disk1.img");
}

TEST(SplitChain, ADotComponentIsNotAnArchiveName)
{
	MemberPath host  = {};
	host.archive     = "/tmp/outer.zip";
	host.member      = "../inner.zip/disk1.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 1u);
	EXPECT_EQ(chain->levels[0].archive_member, "../inner.zip");
	EXPECT_EQ(chain->levels[0].leaf_member, "disk1.img");
}

TEST(SplitChain, TarGzInTheMiddle)
{
	MemberPath host  = {};
	host.archive     = "/tmp/outer.zip";
	host.member      = "files.tar.gz/README.TXT";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 1u);
	EXPECT_EQ(chain->levels[0].archive_member, "files.tar.gz");
	EXPECT_EQ(chain->levels[0].leaf_member, "README.TXT");
}

TEST(SplitChain, IsoIsNotAnArchiveLevel)
{
	MemberPath host  = {};
	host.archive     = "/tmp/outer.zip";
	host.member      = "disc.iso/GAME/FILE.DAT";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	EXPECT_TRUE(chain->levels.empty());
	EXPECT_EQ(chain->host.member, "disc.iso/GAME/FILE.DAT");
}

TEST(SplitChain, TwoLevelsWithSubdirectories)
{
	MemberPath host  = {};
	host.archive     = "/tmp/collection.zip";
	host.member      = "games/thexder.zip/disks/disk1.img";
	const auto chain = SplitChain(host);
	ASSERT_TRUE(chain);
	ASSERT_EQ(chain->levels.size(), 1u);
	EXPECT_EQ(chain->host.member, "games/thexder.zip");
	EXPECT_EQ(chain->levels[0].archive_member, "games/thexder.zip");
	EXPECT_EQ(chain->levels[0].leaf_member, "disks/disk1.img");
}

// --- ResolveChain through the helper and the cache ---

class ChainResolveTest : public testing::Test {
protected:
	fs::path tmp_dir                     = {};
	Limits limits                        = {};
	ArchiveHelperClient::Options options = {};
	std::unique_ptr<ArchiveCache> cache  = {};

	void SetUp() override
	{
		tmp_dir = TestTempDir::MakeUnique("archive_chain_");
		ASSERT_FALSE(tmp_dir.empty());
		options.helper_path = DOSBOX_ARCHIVE_HELPER_PATH;
		options.scratch_dir = tmp_dir / "scratch";
		options.test_hooks  = true;
		fs::create_directories(options.scratch_dir);
		auto opened = ArchiveCache::Open(tmp_dir / "cache", CacheOptions{});
		ASSERT_EQ(opened.status, CacheStatus::Ok) << opened.detail;
		cache = std::move(opened.cache);
	}
	void TearDown() override
	{
		cache.reset();
		std::error_code ec = {};
		fs::remove_all(tmp_dir, ec);
	}
	fs::path Built(const std::string& name, const Kind kind,
	               const std::vector<FileEntry>& entries)
	{
		const auto path = tmp_dir / name;
		auto error      = std::string();
		EXPECT_TRUE(WriteArchive(path, kind, entries, nullptr, &error))
		        << error;
		return path;
	}
	static std::string Slurp(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::stringstream out;
		out << in.rdbuf();
		return out.str();
	}
	std::unique_ptr<ArchiveHelperClient> OpenOk(const fs::path& path)
	{
		auto opened = ArchiveHelperClient::Open(path, limits, options);
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		return std::move(opened.client);
	}
	static ChainedPath Chain(const fs::path& archive, const std::string& member)
	{
		MemberPath host  = {};
		host.archive     = archive;
		host.member      = member;
		const auto chain = SplitChain(host);
		EXPECT_TRUE(chain) << member;
		return chain ? *chain : ChainedPath{};
	}
	ChainOptions Options() const
	{
		ChainOptions out   = {};
		out.helper_options = options;
		out.limits         = limits;
		return out;
	}
	static const Entry* Leaf(const ChainResult& result, const std::string& name)
	{
		if (!result.leaf_client) {
			return nullptr;
		}
		auto rejection = ArchiveNames::Rejection::None;
		return FindMember(result.leaf_client->Entries(), name, &rejection);
	}
	static std::string Read(ArchiveSource& source, const int ordinal)
	{
		std::vector<uint8_t> out = {};
		auto detail              = std::string();
		EXPECT_EQ(source.Materialize(ordinal, out, &detail), Error::None)
		        << detail;
		return std::string(out.begin(), out.end());
	}
	// The house RAR (RAR5 by default); absent on the builders.
	static std::optional<fs::path> RarBinary()
	{
		const char* home = std::getenv("HOME");
		if (!home) {
			return std::nullopt;
		}
		const auto rar = fs::path(home) / "bin" / "rar" / "rar";
		if (!fs::is_regular_file(rar)) {
			return std::nullopt;
		}
		return rar;
	}
};

TEST_F(ChainResolveTest, ZipInZipResolvesToTheLeafMember)
{
	const auto image = std::string(4096, 'd');
	const auto inner = Built("inner.zip",
	                         Kind::ZipDeflate,
	                         {
	                                 {"disk1.img", image}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", Slurp(inner)}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);

	const auto result = ResolveChain(Chain(outer, "inner.zip/disk1.img"),
	                                 *host,
	                                 outer,
	                                 *cache,
	                                 Options());
	ASSERT_EQ(result.error, Error::None) << result.detail;
	ASSERT_EQ(result.links.size(), 1u);
	EXPECT_EQ(result.links[0].resolved_member, "inner.zip");
	EXPECT_TRUE(fs::is_regular_file(result.links[0].cached_path));
	EXPECT_EQ(result.leaf_archive, result.links[0].cached_path);
	EXPECT_NE(result.leaf_client, host.get());
	EXPECT_EQ(result.total_entries, 2);

	const auto* leaf = Leaf(result, "disk1.img");
	ASSERT_NE(leaf, nullptr);
	EXPECT_EQ(leaf->size, 4096);
	EXPECT_EQ(Read(*result.leaf_client, leaf->ordinal), image);
}

TEST_F(ChainResolveTest, APlainMemberKeepsTheHostClient)
{
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"disk1.img", "x"}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);

	const auto result = ResolveChain(
	        Chain(outer, "disk1.img"), *host, outer, *cache, Options());
	EXPECT_EQ(result.error, Error::None) << result.detail;
	EXPECT_TRUE(result.links.empty());
	EXPECT_EQ(result.leaf_client, host.get());
	EXPECT_EQ(result.leaf_archive, outer);
	EXPECT_EQ(result.total_entries, 1);
}

TEST_F(ChainResolveTest, SevenZipInZipResolvesToTheTextFile)
{
	const auto inner = Built("inner.7z",
	                         Kind::SevenZip,
	                         {
	                                 {"README.TXT", "hello"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.7z", Slurp(inner)}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);

	// The 7z is the leaf's container, unpacked whole by Resolve: N8 asks
	// the sequential setting for it as for a middle level.
	const auto chain = Chain(outer, "inner.7z/README.TXT");
	const auto refused = ResolveChain(chain, *host, outer, *cache, Options());
	EXPECT_EQ(refused.error, Error::SolidUnsupported);
	EXPECT_EQ(refused.failed_level, 1);

	auto allowed               = Options();
	allowed.sequential_allowed = true;
	const auto result = ResolveChain(chain, *host, outer, *cache, allowed);
	ASSERT_EQ(result.error, Error::None) << result.detail;
	ASSERT_EQ(result.links.size(), 1u);
	EXPECT_EQ(result.leaf_client->GetFormat(), Format::SevenZip);

	const auto* leaf = Leaf(result, "README.TXT");
	ASSERT_NE(leaf, nullptr);
	EXPECT_EQ(leaf->size, 5);
	EXPECT_EQ(Read(*result.leaf_client, leaf->ordinal), "hello");
}

TEST_F(ChainResolveTest, TwoLevelsDeepResolvesThroughBothCaches)
{
	const auto image  = std::string(2048, 'i');
	const auto inner  = Built("inner.zip",
                                 Kind::ZipDeflate,
	                          {
                                         {"disk1.img", image}
        });
	const auto middle = Built("middle.zip",
	                          Kind::ZipDeflate,
	                          {
	                                  {"sub/", "", true},
	                                  {"sub/inner.zip", Slurp(inner)}
        });
	const auto outer  = Built("outer.zip",
                                 Kind::ZipStored,
	                          {
                                         {"middle.zip", Slurp(middle)}
        });
	auto host         = OpenOk(outer);
	ASSERT_TRUE(host);

	const auto result = ResolveChain(Chain(outer, "middle.zip/sub/inner.zip/disk1.img"),
	                                 *host,
	                                 outer,
	                                 *cache,
	                                 Options());
	ASSERT_EQ(result.error, Error::None) << result.detail;
	ASSERT_EQ(result.links.size(), 2u);
	EXPECT_EQ(result.links[0].resolved_member, "middle.zip");
	EXPECT_EQ(result.links[1].resolved_member, "sub/inner.zip");
	EXPECT_NE(result.links[0].cached_path, result.links[1].cached_path);
	EXPECT_EQ(result.leaf_client, result.links[1].client.get());
	// outer (1) + middle (2) + inner (1).
	EXPECT_EQ(result.total_entries, 4);

	const auto* leaf = Leaf(result, "disk1.img");
	ASSERT_NE(leaf, nullptr);
	EXPECT_EQ(Read(*result.leaf_client, leaf->ordinal), image);
}

// N8: a sequential archive in the middle of the chain is extracted from
// whole; the setting decides whether that is allowed.
TEST_F(ChainResolveTest, ASequentialMiddleLevelNeedsTheSetting)
{
	const auto inner  = Built("inner.zip",
                                 Kind::ZipStored,
	                          {
                                         {"disk1.img", "x"}
        });
	const auto middle = Built("middle.7z",
	                          Kind::SevenZip,
	                          {
	                                  {"inner.zip", Slurp(inner)}
        });
	const auto outer  = Built("outer.zip",
                                 Kind::ZipStored,
	                          {
                                         {"middle.7z", Slurp(middle)}
        });
	auto host         = OpenOk(outer);
	ASSERT_TRUE(host);
	const auto chain = Chain(outer, "middle.7z/inner.zip/disk1.img");

	const auto refused = ResolveChain(chain, *host, outer, *cache, Options());
	EXPECT_EQ(refused.error, Error::SolidUnsupported);
	EXPECT_EQ(refused.failed_level, 1);
	EXPECT_EQ(refused.leaf_client, nullptr);
	EXPECT_NE(refused.detail.find("inner.zip"), std::string::npos)
	        << refused.detail;

	auto allowed               = Options();
	allowed.sequential_allowed = true;
	const auto result = ResolveChain(chain, *host, outer, *cache, allowed);
	ASSERT_EQ(result.error, Error::None) << result.detail;
	ASSERT_EQ(result.links.size(), 2u);
	EXPECT_NE(Leaf(result, "disk1.img"), nullptr);
}

// N8 covers the leaf's container too: Resolve unpacks it whole.
TEST_F(ChainResolveTest, ASequentialLeafContainerNeedsTheSetting)
{
	const auto inner = Built("inner.tar.gz",
	                         Kind::TarGz,
	                         {
	                                 {"disk1.img", "x"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.tar.gz", Slurp(inner)}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);
	const auto chain = Chain(outer, "inner.tar.gz/disk1.img");

	const auto refused = ResolveChain(chain, *host, outer, *cache, Options());
	EXPECT_EQ(refused.error, Error::SolidUnsupported);
	EXPECT_EQ(refused.failed_level, 1);
	EXPECT_EQ(refused.leaf_client, nullptr);

	auto allowed               = Options();
	allowed.sequential_allowed = true;
	const auto result = ResolveChain(chain, *host, outer, *cache, allowed);
	ASSERT_EQ(result.error, Error::None) << result.detail;
	EXPECT_EQ(result.leaf_client->GetFormat(), Format::Tar);
	EXPECT_NE(Leaf(result, "disk1.img"), nullptr);
}

TEST_F(ChainResolveTest, ZipInRarFollowsTheSequentialSetting)
{
	const auto rar = RarBinary();
	if (!rar) {
		GTEST_SKIP() << "no ~/bin/rar/rar on this host";
	}
	const auto image = std::string(1024, 'r');
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"disk1.img", image}
        });
	const auto outer = tmp_dir / "outer.rar";
	const auto cmd   = "\"" + rar->string() + "\" a -ep1 -idq \"" +
	                 outer.string() + "\" \"" + inner.string() + "\"";
	ASSERT_EQ(std::system(cmd.c_str()), 0) << cmd;
	auto host = OpenOk(outer);
	ASSERT_TRUE(host);
	ASSERT_EQ(host->GetAccess(), Access::Sequential);
	const auto chain = Chain(outer, "inner.zip/disk1.img");

	const auto refused = ResolveChain(chain, *host, outer, *cache, Options());
	EXPECT_EQ(refused.error, Error::SolidUnsupported);
	EXPECT_EQ(refused.failed_level, 0);

	auto allowed               = Options();
	allowed.sequential_allowed = true;
	const auto result = ResolveChain(chain, *host, outer, *cache, allowed);
	ASSERT_EQ(result.error, Error::None) << result.detail;
	ASSERT_EQ(result.links.size(), 1u);
	const auto* leaf = Leaf(result, "disk1.img");
	ASSERT_NE(leaf, nullptr);
	EXPECT_EQ(Read(*result.leaf_client, leaf->ordinal), image);
}

TEST_F(ChainResolveTest, AMissingInnerArchiveIsNotFoundAtItsLevel)
{
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"disk1.img", "x"}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);

	const auto result = ResolveChain(Chain(outer, "nosuch.zip/disk1.img"),
	                                 *host,
	                                 outer,
	                                 *cache,
	                                 Options());
	EXPECT_EQ(result.error, Error::NotFound);
	EXPECT_EQ(result.failed_level, 0);
	EXPECT_TRUE(result.links.empty());
	EXPECT_EQ(result.leaf_client, nullptr);
	EXPECT_NE(result.detail.find("nosuch.zip"), std::string::npos)
	        << result.detail;
}

TEST_F(ChainResolveTest, AMissingLeafMemberIsAbsentFromTheLeafClient)
{
	const auto inner = Built("inner.zip",
	                         Kind::ZipDeflate,
	                         {
	                                 {"disk1.img", "x"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", Slurp(inner)}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);

	// The chain stops at the archive levels; the leaf is Resolve's lookup.
	const auto result = ResolveChain(Chain(outer, "inner.zip/nosuch.img"),
	                                 *host,
	                                 outer,
	                                 *cache,
	                                 Options());
	ASSERT_EQ(result.error, Error::None) << result.detail;
	ASSERT_EQ(result.links.size(), 1u);
	EXPECT_EQ(Leaf(result, "nosuch.img"), nullptr);
	EXPECT_NE(Leaf(result, "disk1.img"), nullptr);
}

TEST_F(ChainResolveTest, ADotComponentInALevelIsRejectedByName)
{
	const auto inner = Built("inner.zip",
	                         Kind::ZipDeflate,
	                         {
	                                 {"disk1.img", "x"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", Slurp(inner)}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);

	const auto result = ResolveChain(Chain(outer, "../inner.zip/disk1.img"),
	                                 *host,
	                                 outer,
	                                 *cache,
	                                 Options());
	EXPECT_EQ(result.error, Error::NotFound);
	EXPECT_EQ(result.failed_level, 0);
	EXPECT_TRUE(result.links.empty());
	EXPECT_NE(result.detail.find(".."), std::string::npos) << result.detail;
	EXPECT_NE(result.detail.find(ArchiveNames::RejectionText(
	                  ArchiveNames::Rejection::DotComponent)),
	          std::string::npos)
	        << result.detail;
}

TEST_F(ChainResolveTest, ADirectoryNamedLikeAnArchiveIsNotALevel)
{
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip/", "", true},
	                                 {"inner.zip/disk1.img", "x"}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);

	const auto result = ResolveChain(Chain(outer, "inner.zip/disk1.img"),
	                                 *host,
	                                 outer,
	                                 *cache,
	                                 Options());
	EXPECT_EQ(result.error, Error::NotFound);
	EXPECT_EQ(result.failed_level, 0);
	EXPECT_TRUE(result.links.empty());
}

// N7: the entry cap counts every level, so two archives under the
// per-archive limit still add up over the chain's.
TEST_F(ChainResolveTest, TheTotalEntryCapCountsEveryLevel)
{
	auto inner_entries = std::vector<FileEntry>{};
	for (int i = 0; i < 30; ++i) {
		inner_entries.push_back({"f" + std::to_string(i) + ".txt", "x"});
	}
	const auto inner = Built("inner.zip", Kind::ZipDeflate, inner_entries);
	auto outer_entries = std::vector<FileEntry>{
	        {"inner.zip", Slurp(inner)}
        };
	for (int i = 0; i < 29; ++i) {
		outer_entries.push_back({"o" + std::to_string(i) + ".txt", "x"});
	}
	const auto outer = Built("outer.zip", Kind::ZipStored, outer_entries);
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);
	const auto chain = Chain(outer, "inner.zip/f0.txt");

	auto capped              = Options();
	capped.max_total_entries = 59; // 30 + 30 is one over
	const auto refused = ResolveChain(chain, *host, outer, *cache, capped);
	EXPECT_EQ(refused.error, Error::TooManyEntries);
	EXPECT_EQ(refused.failed_level, 0);
	EXPECT_EQ(refused.total_entries, 60);
	EXPECT_EQ(refused.leaf_client, nullptr);

	capped.max_total_entries = 60;
	const auto result = ResolveChain(chain, *host, outer, *cache, capped);
	EXPECT_EQ(result.error, Error::None) << result.detail;
	EXPECT_EQ(result.total_entries, 60);
}

TEST_F(ChainResolveTest, TheSameInnerArchiveTwiceSharesOneCacheEntry)
{
	const auto inner = Built("inner.zip",
	                         Kind::ZipDeflate,
	                         {
	                                 {"disk1.img", "x"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", Slurp(inner)}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);
	const auto chain = Chain(outer, "inner.zip/disk1.img");

	const auto first = ResolveChain(chain, *host, outer, *cache, Options());
	const auto second = ResolveChain(chain, *host, outer, *cache, Options());
	ASSERT_EQ(first.error, Error::None) << first.detail;
	ASSERT_EQ(second.error, Error::None) << second.detail;
	ASSERT_EQ(first.links.size(), 1u);
	ASSERT_EQ(second.links.size(), 1u);
	EXPECT_EQ(first.links[0].cached_path, second.links[0].cached_path);
	EXPECT_EQ(cache->Measure().entries, 1);
}
