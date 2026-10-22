#include "generate.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "rewrite.hpp"

namespace loader::char4
{
	const std::vector<std::string> kAwareScripts = {
	    // shops, money and the player's state
	    "shop_controller", "gunclub_shop", "carmod_shop", "blip_controller", "atm_trigger", "respawn_controller",
	    // services (appinternet: its bank function is how mods pay, ml::game::Pay)
	    "appinternet", "taxilauncher", "taxiservice", "launcher_carwash", "carwash1", "carwash2", "vehicle_gen_controller",
	    // world purchases
	    "ob_vend1", "ob_vend2", "ob_telescope", "act_cinema", "cablecar", "fairgroundhub", "rollercoaster", "stripclub",
	    "stripclub_drinking", "ob_drinking_shots", "pb_prostitute",
	    // activities and their launchers
	    "launcher_golf", "golf", "launcher_tennis", "tennis", "tennis_ambient", "launcher_darts", "darts", "launcher_range",
	    "range_modern", "launcher_hunting_ambient", "hunting_ambient", "launcher_offroadracing", "offroad_races", "launcher_racing",
	    "mission_race", "country_race", "controller_races", "launcher_stunts", "stunt_plane_races"};

	namespace
	{
		// Bumped whenever the rules change, so the files are made again.
		constexpr int kVersion = 9;

		std::string FileStamp(const std::filesystem::path& file)
		{
			std::error_code ec;
			const auto size = std::filesystem::file_size(file, ec);
			const auto time = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
			return std::format("{}:{}", size, time);
		}

		std::string ReadText(const std::filesystem::path& file)
		{
			std::ifstream in(file, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(in), {});
		}

		// Writes the file only when its contents change (the generated pack is cached by the files' times).
		bool WriteIfChanged(const std::filesystem::path& file, const void* data, size_t size)
		{
			if (std::filesystem::exists(file))
			{
				const std::string current = ReadText(file);
				if (current.size() == size && std::equal(current.begin(), current.end(), static_cast<const char*>(data)))
					return true;
			}
			std::ofstream out(file, std::ios::binary | std::ios::trunc);
			out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
			return static_cast<bool>(out);
		}

		// Every file of an archive and its nested archives (path with folders and archive names).
		void Walk(const convert::Archive& archive, const std::string& prefix, const std::function<void(const convert::Archive&, uint32_t, const std::string&)>& visit)
		{
			const std::function<void(uint32_t, const std::string&)> walk = [&](uint32_t dir, const std::string& base) {
				for (const uint32_t i : archive.Nodes()[dir].children)
				{
					const auto& n = archive.Nodes()[i];
					const std::string path = base + n.name;
					if (n.directory)
						walk(i, path + "/");
					else if (!n.resource && !n.stored && path.ends_with(".rpf"))
					{
						convert::Archive nested;
						std::string error;
						if (archive.OpenNested(i, nested, error))
							Walk(nested, path + "/", visit);
					}
					else
						visit(archive, i, path);
				}
			};
			if (!archive.Nodes().empty())
				walk(0, prefix);
		}

		bool OpenArchive(const std::filesystem::path& file, const convert::Decryptor* decrypt, convert::Archive& archive, std::string& error)
		{
			auto in = std::make_shared<std::ifstream>(file, std::ios::binary);
			std::error_code ec;
			const auto size = std::filesystem::file_size(file, ec);
			if (!*in || ec)
			{
				error = "cannot open " + file.filename().string();
				return false;
			}
			return archive.Open(in, 0, size, file.filename().string(), decrypt, error);
		}

		std::string StripComments(std::string text)
		{
			for (size_t at = text.find("<!--"); at != std::string::npos; at = text.find("<!--", at))
			{
				const size_t end = text.find("-->", at);
				text.erase(at, end == std::string::npos ? std::string::npos : end + 3 - at);
			}
			return text;
		}

		std::map<std::string, std::string> Attributes(std::string_view tag)
		{
			std::map<std::string, std::string> out;
			for (size_t at = 0;;)
			{
				const size_t eq = tag.find("=\"", at);
				if (eq == std::string_view::npos)
					break;
				size_t start = eq;
				while (start > 0 && (std::isalnum(static_cast<unsigned char>(tag[start - 1])) || tag[start - 1] == '_'))
					--start;
				const size_t end = tag.find('"', eq + 2);
				if (end == std::string_view::npos)
					break;
				out[std::string(tag.substr(start, eq - start))] = std::string(tag.substr(eq + 2, end - eq - 2));
				at = end + 1;
			}
			return out;
		}

		// The officer's stats: every per-character stat as SP3_<name> with the same type, packing, save category and
		// owner; never a profile stat (the protagonists' profile copies stay theirs) nor a character stat (the engine
		// would expand it to SP0-SP2). Label stats are left out: declaring them makes the game crash when story mode
		// starts (M0, research/phase0.md §55.6), and the officer plays no missions.
		std::string Sp3Stats(const std::string& setup)
		{
			std::string out = "<StatsSetup version=\"1\">\n    <stats>\n";
			const std::string text = StripComments(setup);
			for (size_t at = text.find("<stat "); at != std::string::npos; at = text.find("<stat ", at + 1))
			{
				const size_t end = text.find('>', at);
				if (end == std::string::npos)
					break;
				auto attrs = Attributes(std::string_view(text).substr(at, end - at));
				if (attrs["characterStat"] != "true" || attrs["Type"] == "label" || attrs["Name"].empty())
					continue;
				out += std::format("\t\t<stat Name=\"SP3_{}\"", attrs["Name"]);
				for (const char* key : {"Type", "packed", "SaveCategory", "online", "Owner", "FlushPriority"})
					if (const auto it = attrs.find(key); it != attrs.end())
						out += std::format(" {}=\"{}\"", key, it->second);
				out += " profile=\"false\" characterStat=\"false\" />\n";
			}
			return out + "\t</stats>\n</StatsSetup>\n";
		}
	}

	Generated Generate(const std::filesystem::path& gameDir, const std::filesystem::path& outDir, const convert::Decryptor* decrypt)
	{
		Generated result;
		std::error_code ec;
		std::filesystem::create_directories(outDir, ec);
		const auto update = gameDir / L"update" / L"update.rpf", update2 = gameDir / L"update" / L"update2.rpf";
		std::string names;
		for (const auto& s : kAwareScripts)
			names += s + ",";
		const std::string stamp = std::format("v{}|{}|{}|{}", kVersion, FileStamp(update), FileStamp(update2), names);

		// Unchanged game files: the files made last time.
		if (ReadText(outDir / L"stamp.txt") == stamp && std::filesystem::exists(outDir / L"sp3stats.xml"))
		{
			std::istringstream list(ReadText(outDir / L"scripts.txt"));
			for (std::string line; std::getline(list, line);)
				if (line.starts_with("+ ") && std::filesystem::exists(outDir / (line.substr(2) + ".ysc")))
					result.scripts.push_back(line.substr(2));
				else if (line.starts_with("- "))
					result.warnings.push_back(line.substr(2));
			result.ok = !result.scripts.empty();
			return result;
		}
		if (!decrypt)
		{
			result.warnings.push_back("the game's archive decryption was not found");
			return result;
		}

		// The game's per-character stats.
		std::string setup, error;
		{
			convert::Archive archive;
			if (OpenArchive(update, decrypt, archive, error))
				Walk(archive, "", [&](const convert::Archive& a, uint32_t i, const std::string& path) {
					if (setup.empty() && path == "common/data/spstatssetup.xml")
					{
						convert::Bytes data;
						std::string readError;
						if (a.ReadFile(i, data, readError))
							setup.assign(data.begin(), data.end());
					}
				});
		}
		if (setup.empty())
		{
			result.warnings.push_back("common/data/spstatssetup.xml not found in update.rpf " + error);
			return result;
		}
		CharacterStats stats;
		stats.Parse(StripComments(setup));
		const std::string sp3 = Sp3Stats(setup);
		if (stats.stats.empty() || !WriteIfChanged(outDir / L"sp3stats.xml", sp3.data(), sp3.size()))
		{
			result.warnings.push_back("the SP3 stats could not be written");
			return result;
		}

		// The scripts, from script_rel.rpf in update2.rpf.
		std::map<std::string, convert::Bytes> vanilla;
		{
			convert::Archive archive;
			const std::set<std::string> wanted(kAwareScripts.begin(), kAwareScripts.end());
			if (OpenArchive(update2, decrypt, archive, error))
				Walk(archive, "", [&](const convert::Archive& a, uint32_t i, const std::string& path) {
					const size_t slash = path.rfind('/');
					if (path.size() < 4 || !path.ends_with(".ysc") || path.find("script_rel.rpf/") == std::string::npos)
						return;
					std::string name = path.substr(slash + 1, path.size() - slash - 5);
					std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (!wanted.contains(name) || vanilla.contains(name))
						return;
					convert::Bytes data;
					std::string readError;
					if (a.ReadFile(i, data, readError))
						vanilla[name] = std::move(data);
				});
		}
		// The unlock bits of the weapons the officer is never offered (cleared at runtime, game/char4).
		{
			std::string masks;
			Program gunclub;
			std::string why;
			if (const auto it = vanilla.find("gunclub_shop"); it != vanilla.end() && gunclub.Read(it->second, why))
			{
				const auto bits = ForbiddenUnlockBits(gunclub);
				for (size_t k = 0; k < bits.size(); ++k)
					masks += std::format("WEAP_UNLOCK_{} {}\n", k, bits[k]);
			}
			if (masks.empty())
				result.warnings.push_back("gunclub_shop: the weapon unlock table was not found; the officer's unlocks are not limited");
			WriteIfChanged(outDir / L"unlockmasks.txt", masks.data(), masks.size());
		}
		std::string list;
		for (const std::string& name : kAwareScripts)
		{
			const auto file = outDir / (name + ".ysc");
			std::string why;
			const auto it = vanilla.find(name);
			Program p;
			Report report;
			convert::Bytes out;
			if (it == vanilla.end())
				why = "not found in the game's archives";
			else if (!p.Read(it->second, why) || !Rewrite(p, stats, report, why) || !p.Write(out, why))
				why = why.empty() ? "cannot be rewritten" : why;
			if (why.empty() && WriteIfChanged(file, out.data(), out.size()))
			{
				result.scripts.push_back(name);
				list += std::format("+ {}\n", name);
				list += std::format("  get_cur {}, is_playable {}, chains {}; switches: stat {}, loose {}, character {}; moved {}; account checks {}\n",
				    report.curCalls, report.playableCalls, report.chains, report.statSwitches, report.looseSwitches, report.identitySwitches,
				    report.moved, report.accountChecks);
				for (const auto& n : report.notes)
					list += "  " + n + "\n";
				continue;
			}
			std::filesystem::remove(file, ec);
			const std::string warning = std::format("{}: {}", name, why.empty() ? "cannot be written" : why);
			result.warnings.push_back(warning);
			list += "- " + warning + "\n";
		}
		WriteIfChanged(outDir / L"scripts.txt", list.data(), list.size());
		result.ok = !result.scripts.empty();
		if (result.ok)
			WriteIfChanged(outDir / L"stamp.txt", stamp.data(), stamp.size());
		return result;
	}
}
