// Runs the fourth-character rewrite (src/loader/char4) on extracted game scripts, outside the game.
// usage: char4gen roundtrip <in dir>                          (read and write every script; report differences)
//        char4gen rewrite <in dir> <out dir> <spstatssetup.xml> <script> [<script> ...]
// zlib1.dll is the game's: run it from the game folder (or with the game folder on PATH).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../../src/loader/char4/rewrite.hpp"

namespace
{
	using loader::char4::Bytes;
	using loader::char4::Program;

	Bytes ReadAll(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return Bytes(std::istreambuf_iterator<char>(in), {});
	}

	std::string Differences(const Program& a, const Program& b)
	{
		std::string out;
		const auto check = [&](bool same, const char* what) {
			if (!same)
				out += std::string(out.empty() ? "" : ", ") + what;
		};
		check(a.code == b.code, "code");
		check(a.natives == b.natives, "natives");
		check(a.statics == b.statics, "statics");
		check(a.strings == b.strings, "strings");
		check(a.globals == b.globals, "globals");
		check(a.globalsWord == b.globalsWord, "globals word");
		check(a.name == b.name, "name");
		for (const size_t o : {0x18, 0x20, 0x58})
			check(std::equal(a.header.begin() + o, a.header.begin() + o + 4, b.header.begin() + o), "header");
		return out;
	}
}

int main(int argc, char** argv)
{
	if (argc >= 3 && std::string(argv[1]) == "roundtrip")
	{
		int bad = 0, total = 0;
		for (const auto& entry : std::filesystem::directory_iterator(argv[2]))
		{
			if (entry.path().extension() != ".ysc")
				continue;
			++total;
			Program p, q;
			std::string error;
			Bytes out;
			if (!p.Read(ReadAll(entry.path()), error) || !p.Write(out, error) || !q.Read(out, error))
			{
				std::printf("%s: %s\n", entry.path().filename().string().c_str(), error.c_str());
				++bad;
				continue;
			}
			if (const std::string d = Differences(p, q); !d.empty())
			{
				std::printf("%s: differs (%s)\n", p.name.c_str(), d.c_str());
				++bad;
			}
		}
		std::printf("%d scripts, %d bad\n", total, bad);
		return bad ? 1 : 0;
	}
	if (argc >= 6 && std::string(argv[1]) == "rewrite")
	{
		loader::char4::CharacterStats stats;
		{
			const Bytes xml = ReadAll(argv[4]);
			stats.Parse(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()));
		}
		std::printf("%zu character stats\n", stats.stats.size());
		std::filesystem::create_directories(argv[3]);
		for (int i = 5; i < argc; ++i)
		{
			Program p;
			std::string error;
			const auto in = std::filesystem::path(argv[2]) / (std::string(argv[i]) + ".ysc");
			if (!p.Read(ReadAll(in), error))
			{
				std::printf("%s: %s\n", argv[i], error.c_str());
				continue;
			}
			const size_t before = p.code.size();
			loader::char4::Report r;
			Bytes out;
			if (!loader::char4::Rewrite(p, stats, r, error) || !p.Write(out, error))
			{
				std::printf("%s: NOT REWRITTEN: %s\n", argv[i], error.c_str());
				continue;
			}
			std::ofstream(std::filesystem::path(argv[3]) / (std::string(argv[i]) + ".ysc"), std::ios::binary)
			    .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
			std::printf("%s: code %zu -> %zu; get_cur %d, is_playable %d, chains %d; switches: stat %d, loose %d, identity %d; moved %d; "
			            "account checks %d; %zu stats\n",
			    argv[i], before, p.code.size(), r.curCalls, r.playableCalls, r.chains, r.statSwitches, r.looseSwitches, r.identitySwitches, r.moved,
			    r.accountChecks, r.stats.size());
			for (const auto& n : r.notes)
				std::printf("   %s\n", n.c_str());
		}
		return 0;
	}
	std::printf("usage: char4gen roundtrip <dir> | rewrite <in dir> <out dir> <spstatssetup.xml> <script>...\n");
	return 2;
}
