// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/member_path.h"

#include "archive_fixtures.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(WIN32)
#include <sys/stat.h>
#endif

using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Filter;
using ArchiveFixtures::Kind;
using ArchiveFixtures::WriteArchive;
using ArchiveFixtures::WriteBytes;
using ArchiveFixtures::WriteCompressed;
using namespace ArchiveMount;

namespace {

namespace fs = std::filesystem;

class MemberPathTest : public testing::Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("member_path_");
		ASSERT_FALSE(tmp.empty());
		fs::create_directories(tmp / "games");
		auto error = std::string();
		ASSERT_TRUE(WriteArchive(tmp / "games" / "game.zip",
		                         Kind::ZipDeflate,
		                         {
		                                 {"disk1.img", std::string(4096, 'a')}
                },
		                         nullptr,
		                         &error))
		        << error;
		std::ofstream(tmp / "games" / "fake.zip") << "not an archive";
		// Plan 4b: the single-image containers and two that are not.
		ASSERT_TRUE(WriteArchive(tmp / "games" / "game.imz",
		                         Kind::ZipDeflate,
		                         {
		                                 {"DISK1.IMA",
		                                  std::string(4096, 'b')}
                }));
		ASSERT_TRUE(WriteCompressed(tmp / "games" / "disk.img.xz",
		                            Filter::Xz,
		                            std::string(4096, 'c')));
		ASSERT_TRUE(WriteCompressed(tmp / "games" / "notes.txt.gz",
		                            Filter::Gzip,
		                            "hello"));
		ASSERT_TRUE(WriteArchive(tmp / "games" / "game.tar.gz",
		                         Kind::TarGz,
		                         {
		                                 {"A.TXT", "a"}
                }));
		// The sniff reads the signature only (EZB ISZ spec v1.00, 3.1).
		auto isz = std::vector<uint8_t>{'I', 's', 'Z', '!'};
		isz.resize(64, 0);
		WriteBytes(tmp / "games" / "cd.isz", isz);
		// N2: an ISO under its own name, and the same bytes under a zip
		// name.
		const auto iso = ArchiveFixtures::IsoWithFiles({
		        {"GAMES/X.ZIP", std::string(100, 'z')}
                });
		WriteBytes(tmp / "games" / "disc.iso",
		           std::vector<uint8_t>(iso.begin(), iso.end()));
		WriteBytes(tmp / "games" / "isobytes.zip",
		           std::vector<uint8_t>(iso.begin(), iso.end()));
	}
	void TearDown() override
	{
		std::error_code ec = {};
		fs::remove_all(tmp, ec);
	}
	std::string P(const std::string& rest) const
	{
		return (tmp / "games").string() + "/" + rest;
	}
	fs::path tmp = {};
};

// The sidecar record holds UTF-8; on Windows a narrow path would be read in
// the code page, so the round trip only holds through these two.
TEST(PathUtf8, ARecordedNameComesBackAsTheSamePath)
{
	const auto name = fs::path(u8"Spiel \u00e4\u30b2\u30fc\u30e0.zip");
	const auto back = PathFromUtf8(PathToUtf8(name));
	ASSERT_TRUE(back);
	EXPECT_EQ(*back, name);
	EXPECT_EQ(PathToUtf8(*back), PathToUtf8(name));
	ASSERT_TRUE(PathFromUtf8(""));
	EXPECT_TRUE(PathFromUtf8("")->empty());
#if defined(WIN32)
	EXPECT_FALSE(PathFromUtf8("bad \xff\xfe name"));
#endif
}

TEST_F(MemberPathTest, SplitsAtTheArchive)
{
	const auto split = SplitMemberPath(P("game.zip/disk1.img"));
	ASSERT_TRUE(split);
	EXPECT_EQ(split->archive, fs::path(P("game.zip")));
	EXPECT_EQ(split->member, "disk1.img");
}

TEST_F(MemberPathTest, AnIsoHostSplitsOnlyWhenAMemberFollows)
{
	const auto split = SplitMemberPath(P("disc.iso/GAMES/X.ZIP/disk1.img"));
	ASSERT_TRUE(split);
	EXPECT_EQ(split->archive, fs::path(P("disc.iso")));
	EXPECT_EQ(split->member, "GAMES/X.ZIP/disk1.img");
	// A bare ISO is a CD image for MOUNT, never an archive file.
	EXPECT_FALSE(IsArchiveFile(P("disc.iso")));
	EXPECT_TRUE(IsMemberHostFile(P("disc.iso")));
	EXPECT_TRUE(IsMemberHostFile(P("game.zip")));
	// D12: an ISO's bytes under a zip name are no host at all.
	EXPECT_FALSE(IsMemberHostFile(P("isobytes.zip")));
	EXPECT_FALSE(SplitMemberPath(P("isobytes.zip/GAMES/X.ZIP")));
}

TEST_F(MemberPathTest, KeepsTheMemberSeparatorsAsTyped)
{
	const auto split = SplitMemberPath(P("game.zip/cd\\disk1.cue"));
	ASSERT_TRUE(split);
	EXPECT_EQ(split->member, "cd\\disk1.cue");
}

TEST_F(MemberPathTest, AnExistingPathIsNoMemberPath)
{
	EXPECT_FALSE(SplitMemberPath(P("game.zip")));
	EXPECT_FALSE(SplitMemberPath((tmp / "games").string()));
}

TEST_F(MemberPathTest, AFileThatIsNoArchiveEndsTheWalk)
{
	EXPECT_FALSE(SplitMemberPath(P("fake.zip/disk1.img")));
}

TEST_F(MemberPathTest, AMissingComponentEndsTheWalk)
{
	EXPECT_FALSE(SplitMemberPath(P("missing/game.zip/disk1.img")));
}

TEST_F(MemberPathTest, AnEmptyMemberIsNoMemberPath)
{
	EXPECT_FALSE(SplitMemberPath(P("game.zip/")));
}

TEST_F(MemberPathTest, IsArchiveFileWantsExtensionAndSignature)
{
	EXPECT_TRUE(IsArchiveFile(P("game.zip")));
	EXPECT_FALSE(IsArchiveFile(P("fake.zip")));
	fs::copy_file(P("game.zip"), P("game.dat"));
	EXPECT_FALSE(IsArchiveFile(P("game.dat")));
}

// The legacy LZMA_Alone tar (plan 4b, Task 2b); .tlz is lzip's, not ours.
TEST_F(MemberPathTest, ATarLzmaIsAnArchiveFile)
{
	ASSERT_TRUE(WriteArchive(P("old.tar.lzma"),
	                         Kind::TarLzma,
	                         {
	                                 {"A.TXT", "a"}
        }));
	EXPECT_TRUE(IsArchiveFile(P("old.tar.lzma")));
	fs::copy_file(P("old.tar.lzma"), P("UPPER.TAR.LZMA"));
	EXPECT_TRUE(IsArchiveFile(P("UPPER.TAR.LZMA")));
	fs::copy_file(P("old.tar.lzma"), P("old.tlz"));
	EXPECT_FALSE(IsArchiveFile(P("old.tlz")));
}

TEST(CompressedImageName, RecognisesAnImageExtensionUnderACompressionOne)
{
	EXPECT_TRUE(IsCompressedImageName("disk1.img.xz"));
	EXPECT_TRUE(IsCompressedImageName("GAME.ISO.ZST"));
	EXPECT_TRUE(IsCompressedImageName("a/b/boot.ima.gz"));
	EXPECT_TRUE(IsCompressedImageName("x.dsk.bz2"));
	EXPECT_TRUE(IsCompressedImageName("disk.1440.gz"));
	EXPECT_FALSE(IsCompressedImageName("game.tar.gz"));
	EXPECT_FALSE(IsCompressedImageName("notes.txt.gz"));
	EXPECT_FALSE(IsCompressedImageName("disk1.img"));
	EXPECT_FALSE(IsCompressedImageName(".img.xz"));
	EXPECT_FALSE(IsCompressedImageName("disk1.img.lzma"));
	EXPECT_FALSE(IsCompressedImageName("img.xz"));
}

// Only the ASCII suffix matters; Windows must not run the name through the
// narrow code page, where these bytes do not convert.
TEST(ContainerKindByName, NamesThePromisedHead)
{
	EXPECT_EQ(ContainerKindByName("a/game.imz"), HeadKind::Zip);
	EXPECT_EQ(ContainerKindByName("CD.ISZ"), HeadKind::Isz);
	EXPECT_EQ(ContainerKindByName("disk1.img.xz"), HeadKind::Xz);
	EXPECT_EQ(ContainerKindByName("boot.ima.GZ"), HeadKind::Gzip);
	EXPECT_EQ(ContainerKindByName("x.dsk.bz2"), HeadKind::Bzip2);
	EXPECT_EQ(ContainerKindByName("game.iso.zst"), HeadKind::Zstd);
	EXPECT_FALSE(ContainerKindByName("game.zip"));
	EXPECT_FALSE(ContainerKindByName("game.tar.gz"));
	EXPECT_FALSE(ContainerKindByName(".imz"));
}

TEST(CompressedImageName, AnyBytesMayComeBeforeTheSuffix)
{
	EXPECT_TRUE(IsCompressedImageName("\xe3\x83\x87\xe3\x82\xa3.iso.xz"));
	EXPECT_TRUE(IsCompressedImageName("\xff\xfe.img.gz"));
	EXPECT_TRUE(IsCompressedImageName("d\xc3\xafsk\\boot.ima.zst"));
	EXPECT_FALSE(IsCompressedImageName("\xe3\x83\x87.txt.gz"));
}

TEST_F(MemberPathTest, AContainerWithANameOutsideAsciiIsALoneMember)
{
	const auto path = tmp / "games" /
	                  fs::path(u8"\u30c7\u30a3\u30b9\u30af.img.xz");
	ASSERT_TRUE(WriteCompressed(path, Filter::Xz, std::string(4096, 'd')));
	EXPECT_TRUE(IsSingleImageContainer(path));
	const auto lone = LoneImageMember(path);
	ASSERT_TRUE(lone);
	EXPECT_TRUE(lone->lone);
}

TEST_F(MemberPathTest, AnArchiveWithANameOutsideAsciiIsAnArchiveFile)
{
	const auto path = tmp / "games" / fs::path(u8"\u30b2\u30fc\u30e0.zip");
	fs::copy_file(tmp / "games" / "game.zip", path);
	EXPECT_TRUE(IsArchiveFile(path));
}

TEST_F(MemberPathTest, AContainerFileIsALoneMember)
{
	for (const auto* name : {"game.imz", "disk.img.xz", "cd.isz"}) {
		const auto lone = LoneImageMember(P(name));
		ASSERT_TRUE(lone) << name;
		EXPECT_EQ(lone->archive, fs::path(P(name)));
		EXPECT_TRUE(lone->member.empty());
		EXPECT_TRUE(lone->lone);
	}
	EXPECT_FALSE(LoneImageMember(P("game.zip")));
	EXPECT_FALSE(LoneImageMember(P("notes.txt.gz")));
	EXPECT_FALSE(LoneImageMember(P("nosuch.imz")));
	EXPECT_FALSE(LoneImageMember(P("game.tar.gz")));
}

// The name and the head must agree: a renamed file is not a container.
TEST_F(MemberPathTest, AContainerNameOverTheWrongHeadIsNotAContainer)
{
	fs::copy_file(P("game.zip"), P("zip.isz"));
	fs::copy_file(P("cd.isz"), P("isz.imz"));
	fs::copy_file(P("game.tar.gz"), P("tar.img.xz"));
	fs::copy_file(P("fake.zip"), P("text.img.gz"));
	for (const auto* name : {"zip.isz", "isz.imz", "tar.img.xz", "text.img.gz"}) {
		EXPECT_FALSE(IsSingleImageContainer(P(name))) << name;
	}
}

TEST_F(MemberPathTest, AnArchiveFileIsNotAContainerAndAContainerIsNotAnArchive)
{
	EXPECT_FALSE(IsArchiveFile(P("game.imz")));
	EXPECT_FALSE(IsArchiveFile(P("cd.isz")));
	EXPECT_FALSE(IsArchiveFile(P("disk.img.xz")));
	EXPECT_FALSE(IsSingleImageContainer(P("game.zip")));
	EXPECT_FALSE(IsSingleImageContainer(P("game.tar.gz")));
}

std::vector<Entry> Index()
{
	auto file = [](int ordinal, const char* utf8, const char* dos) {
		Entry e     = {};
		e.ordinal   = ordinal;
		e.utf8_path = utf8;
		e.dos_path  = dos;
		e.size      = 10;
		return e;
	};
	auto dir   = file(ImpliedOrdinal, "Disks", "DISKS");
	dir.is_dir = true;
	return {dir,
	        file(0, "Disks/Disk One.img", "DISKS\\DISKON~1.IMG"),
	        file(1, "Disks/disk2.img", "DISKS\\DISK2.IMG"),
	        file(2, "a.img", "A.IMG"),
	        file(3, "A.IMG", "A~1.IMG"),
	        file(4, "Disk Two.img", "DISKTW~1.IMG"),
	        file(5, "DISK TWO.IMG", "DISKTW~2.IMG")};
}

#if !defined(WIN32)
TEST_F(MemberPathTest, IsArchiveFileDoesNotWaitOnAFifo)
{
	const auto fifo = tmp / "games" / "pipe.zip";
	ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
	EXPECT_FALSE(IsArchiveFile(fifo));
	const auto lone_fifo = tmp / "games" / "pipe.img.xz";
	ASSERT_EQ(mkfifo(lone_fifo.c_str(), 0600), 0);
	EXPECT_FALSE(IsSingleImageContainer(lone_fifo));
}
#endif

TEST_F(MemberPathTest, AChainIsJoinedToItsHostWithASlashOnEveryHost)
{
	const auto host = tmp / "games" / "coll.zip";
	EXPECT_EQ(PathToUtf8(PathWithChain(host, "a.zip/b.zip")),
	          PathToUtf8(host) + "/a.zip/b.zip");
	EXPECT_EQ(PathWithChain(host, ""), host);
}

TEST_F(MemberPathTest, AJoinedChainSplitsBackIntoItsHostAndChain)
{
	const auto host  = tmp / "games" / "game.zip";
	const auto split = SplitMemberPath(
	        PathToUtf8(PathWithChain(host, "inner.zip/disk1.img")));
	ASSERT_TRUE(split);
	EXPECT_EQ(split->archive, host);
	EXPECT_EQ(split->member, "inner.zip/disk1.img");
}

TEST(FindMember, MatchesTheUtf8PathWithEitherSeparator)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	const auto* found = FindMember(entries, "Disks\\Disk One.img", &rejection);
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->ordinal, 0);
}

TEST(FindMember, MatchesTheDosPath)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	const auto* found = FindMember(entries, "DISKS/DISKON~1.IMG", &rejection);
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->ordinal, 0);
}

TEST(FindMember, MatchesWithoutCaseWhenUnique)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	const auto* found  = FindMember(entries, "disks/DISK2.img", &rejection);
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->ordinal, 1);
}

TEST(FindMember, AnExactSpellingWinsOverACaseTwin)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "a.img", &rejection)->ordinal, 2);
	EXPECT_EQ(FindMember(entries, "A.IMG", &rejection)->ordinal, 3);
}

TEST(FindMember, ACaseTwinWithoutAnExactMatchIsAmbiguous)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "disk two.Img", &rejection), nullptr);
	EXPECT_EQ(rejection, ArchiveNames::Rejection::None);
	// An 8.3 spelling is the DOS name, which is unique: no ambiguity.
	EXPECT_EQ(FindMember(entries, "a.Img", &rejection)->ordinal, 2);
}

TEST(FindMember, ADirectoryNeverMatches)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "Disks", &rejection), nullptr);
}

TEST(FindMember, DotComponentsAreRejected)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "Disks/../a.img", &rejection), nullptr);
	EXPECT_EQ(rejection, ArchiveNames::Rejection::DotComponent);
}

TEST(FindMember, AnAbsoluteMemberIsRejected)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "/a.img", &rejection), nullptr);
	EXPECT_EQ(rejection, ArchiveNames::Rejection::AbsolutePath);
}

} // namespace
