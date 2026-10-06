#include "replace.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <string_view>

#include "../log.hpp"
#include "../paths.hpp"
#include "archive.hpp"
#include "oiv.hpp"

namespace loader::convert
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s)
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			return s;
		}

		std::string Utf8(const std::u8string& s)
		{
			return std::string(s.begin(), s.end());
		}

		std::string Extension(const std::string& name)
		{
			const size_t dot = name.rfind('.');
			return dot == std::string::npos ? std::string() : Lower(name.substr(dot));
		}

		// Files the streaming system loads by name.
		constexpr std::array<std::string_view, 14> kStreaming{".yft", ".ytd", ".ydr", ".ydd", ".ycd", ".ybn", ".ypt", ".ymap", ".ytyp",
		    ".ynv", ".ynd", ".yld", ".yed", ".ymt"};
		// Not game data (readme files, pictures, ...): skipped without a warning.
		constexpr std::array<std::string_view, 12> kIgnored{".txt", ".md", ".pdf", ".png", ".jpg", ".jpeg", ".gif", ".url", ".ini", ".json",
		    ".html", ".log"};

		// Data file types by root element (content.xml fileType). Other XML files only override entries.
		constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kDataFileTypes{{
		    {"CHandlingDataMgr", "HANDLING_FILE"},
		    {"CVehicleModelInfo__InitDataList", "VEHICLE_METADATA_FILE"},
		    {"CVehicleModelInfoVarGlobal", "CARCOLS_FILE"},
		    {"CVehicleModelInfoVariation", "VEHICLE_VARIATION_FILE"},
		    {"CVehicleMetadataMgr", "VEHICLE_LAYOUTS_FILE"},
		    {"CWeaponInfoBlob", "WEAPONINFO_FILE"},
		    {"CPedModelInfo__InitDataList", "PED_METADATA_FILE"},
		    {"CWeaponAnimationsSets", "WEAPON_ANIMATIONS_FILE"},
		}};

		struct Input
		{
			std::string path; // inside the mod folder, for messages
			std::string name; // file name, lower case
			Bytes data;
		};

		std::string RootTag(std::string_view xml)
		{
			for (size_t i = xml.find('<'); i != std::string_view::npos; i = xml.find('<', i + 1))
			{
				if (i + 1 < xml.size() && (xml[i + 1] == '?' || xml[i + 1] == '!'))
					continue;
				size_t e = i + 1;
				while (e < xml.size() && xml[e] != ' ' && xml[e] != '>' && xml[e] != '/' && xml[e] != '\t' && xml[e] != '\r' && xml[e] != '\n')
					++e;
				return std::string(xml.substr(i + 1, e - i - 1));
			}
			return {};
		}

		void CollectArchive(const Archive& archive, uint32_t dir, const std::string& prefix, std::vector<Input>& out, std::vector<std::string>& warnings)
		{
			for (const uint32_t i : archive.Nodes()[dir].children)
			{
				const auto& n = archive.Nodes()[i];
				const std::string path = prefix + n.name;
				if (n.directory)
				{
					CollectArchive(archive, i, path + "/", out, warnings);
					continue;
				}
				if (!n.resource && !n.stored && Lower(n.name).ends_with(".rpf"))
				{
					Archive nested;
					std::string error;
					if (archive.OpenNested(i, nested, error))
						CollectArchive(nested, 0, path + "/", out, warnings);
					else
						warnings.push_back(std::format("{} 無法開啟，已略過：{}", path, error));
					continue;
				}
				Input in{path, Lower(n.name), {}};
				std::string error;
				if (archive.ReadFile(i, in.data, error))
					out.push_back(std::move(in));
				else
					warnings.push_back(std::format("{} 無法讀取，已略過：{}", path, error));
			}
		}

		std::filesystem::path ModCache(const std::string& mod)
		{
			return paths::Get().root / L"cache" / std::filesystem::path(std::u8string(mod.begin(), mod.end()));
		}

		// The files an .oiv package adds, except DLC packs (ExtractOivPacks). Archives are read like loose ones.
		void CollectOiv(const std::filesystem::path& file, const std::string& path, const Decryptor* decrypt, std::vector<Input>& out,
		    std::vector<std::string>& warnings)
		{
			Oiv oiv;
			std::string error;
			if (!ReadOiv(file, oiv, error))
			{
				warnings.push_back(std::format("{} 不是可讀的安裝包，已略過：{}", path, error));
				return;
			}
			for (const auto& w : oiv.warnings)
				warnings.push_back(std::format("{}：{}", path, w));
			for (const OivFile& f : oiv.files)
			{
				if (f.dlcPack)
					continue;
				Input in{path + ":" + f.target, f.name, {}};
				if (!ReadOivFile(file, f.source, in.data, error))
				{
					warnings.push_back(std::format("{}：{} 無法讀取，已略過：{}", path, f.source, error));
					continue;
				}
				if (Extension(f.name) != ".rpf")
				{
					out.push_back(std::move(in));
					continue;
				}
				// The archive reader works on files.
				auto temp = std::filesystem::temp_directory_path() / std::filesystem::path(std::u8string(f.name.begin(), f.name.end()));
				temp += std::format(".{}.tmp", GetCurrentProcessId());
				std::ofstream(temp, std::ios::binary).write(reinterpret_cast<const char*>(in.data.data()), static_cast<std::streamsize>(in.data.size()));
				{
					auto stream = std::make_shared<std::ifstream>(temp, std::ios::binary);
					Archive archive;
					if (archive.Open(stream, 0, in.data.size(), f.name, decrypt, error))
						CollectArchive(archive, 0, in.path + "/", out, warnings);
					else
						warnings.push_back(std::format("{} 無法開啟，已略過：{}", in.path, error));
				}
				std::error_code ec;
				std::filesystem::remove(temp, ec);
			}
		}

		// Every file of the mod: loose files, and the files inside .rpf archives and .oiv packages.
		std::vector<Input> Collect(const std::filesystem::path& dir, const Decryptor* decrypt, std::vector<std::string>& warnings)
		{
			std::vector<Input> out;
			std::error_code ec;
			for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
			{
				if (!it->is_regular_file(ec))
					continue;
				const std::string path = Utf8(std::filesystem::relative(it->path(), dir, ec).generic_u8string());
				const std::string name = Lower(Utf8(it->path().filename().u8string()));
				if (Extension(name) == ".oiv")
				{
					CollectOiv(it->path(), path, decrypt, out, warnings);
					continue;
				}
				if (Extension(name) == ".rpf")
				{
					auto in = std::make_shared<std::ifstream>(it->path(), std::ios::binary);
					Archive archive;
					std::string error;
					if (*in && archive.Open(in, 0, std::filesystem::file_size(it->path(), ec), name, decrypt, error))
						CollectArchive(archive, 0, path + "/", out, warnings);
					else
						warnings.push_back(std::format("{} 無法開啟，已略過：{}", path, error));
					continue;
				}
				std::ifstream f(it->path(), std::ios::binary);
				out.push_back({path, name, Bytes((std::istreambuf_iterator<char>(f)), {})});
			}
			return out;
		}

		// Size and time of every file in the folder, and the converter version.
		std::string FolderStamp(const std::filesystem::path& dir)
		{
			std::vector<std::string> lines;
			std::error_code ec;
			for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
				if (it->is_regular_file(ec))
					lines.push_back(std::format("{} {} {}", Utf8(it->path().generic_u8string()), it->file_size(ec),
					    it->last_write_time(ec).time_since_epoch().count()));
			std::sort(lines.begin(), lines.end());
			std::string stamp = ConverterStamp() + " replacement 1";
			for (const auto& l : lines)
				stamp += "\n" + l;
			return stamp;
		}

		std::string ReadAll(const std::filesystem::path& file)
		{
			std::ifstream in(file, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), {});
		}

		WriteNode Folder(std::string name)
		{
			WriteNode n;
			n.name = std::move(name);
			return n;
		}

		WriteNode TextFile(std::string name, const std::string& text)
		{
			WriteNode n;
			n.name = std::move(name);
			n.kind = WriteNode::Kind::File;
			n.data.assign(text.begin(), text.end());
			return n;
		}
	}

	bool LooksLikeReplacementMod(const std::filesystem::path& dir)
	{
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (!it->is_regular_file(ec))
				continue;
			const std::string ext = Extension(Utf8(it->path().filename().u8string()));
			if (ext == ".rpf" || ext == ".oiv" || ext == ".meta" || std::find(kStreaming.begin(), kStreaming.end(), ext) != kStreaming.end())
				return true;
		}
		return false;
	}

	PackResult PrepareReplacement(const std::string& name, const std::filesystem::path& dir, const Decryptor* decrypt,
	    const std::function<void()>& onConvert, xmlmerge::Overrides& overrides, ReplacementFiles& files)
	{
		PackResult result;
		std::vector<Input> inputs = Collect(dir, decrypt, result.warnings);

		const std::string device = std::format("dlc_mlr{:08x}", Joaat(name));
		WriteNode stream = Folder("ml_stream.rpf");
		stream.kind = WriteNode::Kind::Archive;
		std::vector<std::pair<std::string, std::string>> dataFiles; // pack path, fileType
		WriteNode dataDir = Folder("ml");
		int legacy = 0;
		for (Input& in : inputs)
		{
			const std::string ext = Extension(in.name);
			if (std::find(kStreaming.begin(), kStreaming.end(), ext) != kStreaming.end())
			{
				if (std::any_of(stream.children.begin(), stream.children.end(), [&](const WriteNode& n) { return n.name == in.name; }))
				{
					result.warnings.push_back(std::format("{} 和模組內另一個同名檔案重複，只使用第一個", in.path));
					continue;
				}
				WriteNode node;
				node.name = in.name;
				const bool resource = in.data.size() >= 16 && *reinterpret_cast<const uint32_t*>(in.data.data()) == 0x37435352;
				node.kind = resource ? WriteNode::Kind::Resource : WriteNode::Kind::File;
				legacy += resource && IsLegacyResource(in.name, in.data);
				node.data = std::move(in.data);
				files.streaming.push_back(in.name);
				stream.children.push_back(std::move(node));
				continue;
			}
			if (ext == ".meta" || ext == ".xml")
			{
				if (in.name == "content.xml" || in.name == "setup2.xml")
					continue; // pack files of the mod's archive; this pack has its own
				const std::string_view xml(reinterpret_cast<const char*>(in.data.data()), in.data.size());
				std::string error;
				const int entries = overrides.AddFile(xml, name, error);
				if (!error.empty())
				{
					result.warnings.push_back(std::format("{} 不是可讀的 XML，已略過", in.path));
					continue;
				}
				const std::string root = RootTag(xml);
				const auto type = std::find_if(kDataFileTypes.begin(), kDataFileTypes.end(), [&](const auto& t) { return t.first == root; });
				if (type != kDataFileTypes.end())
				{
					const std::string file = std::format("{}_{}", dataFiles.size(), in.name);
					dataFiles.emplace_back("common/data/ml/" + file, std::string(type->second));
					WriteNode node;
					node.name = file;
					node.kind = WriteNode::Kind::File;
					node.data = std::move(in.data);
					dataDir.children.push_back(std::move(node));
				}
				else if (entries == 0)
					result.warnings.push_back(std::format("{}（{}）沒有可覆蓋的項目，已略過", in.path, root));
				continue;
			}
			if (std::find(kIgnored.begin(), kIgnored.end(), ext) == kIgnored.end())
				result.warnings.push_back(std::format("{} 的檔案類型目前不支援，已略過", in.path));
		}

		if (stream.children.empty() && dataFiles.empty())
		{
			result.empty = true;
			result.state = PackState::Native;
			for (const auto& w : result.warnings)
				log::Warn("replacement mod {}: {}", name, w);
			return result;
		}

		const auto cacheDir = ModCache(name);
		const auto cached = cacheDir / L"dlc.rpf";
		const std::string stamp = FolderStamp(dir);
		result.dir = cacheDir;
		result.state = PackState::Converted;
		result.convertedFiles = legacy;
		std::error_code ec;
		if (std::filesystem::is_regular_file(cached, ec) && ReadAll(cacheDir / L"source.txt") == stamp)
		{
			result.fromCache = true;
			result.warnings.clear(); // the ones saved when the pack was built
			std::ifstream saved(cacheDir / L"warnings.txt");
			for (std::string line; std::getline(saved, line);)
				if (!line.empty())
					result.warnings.push_back(line);
			log::Info("replacement mod {}: using the generated pack ({} streaming file(s), {} data file(s))", name, stream.children.size(), dataFiles.size());
			for (const auto& w : result.warnings)
				log::Warn("replacement mod {}: {}", name, w);
			return result;
		}

		if (legacy && onConvert)
			onConvert();
		for (WriteNode& node : stream.children)
		{
			if (node.kind != WriteNode::Kind::Resource || !IsLegacyResource(node.name, node.data))
				continue;
			Bytes converted;
			std::string error;
			if (ConvertResourceFile(node.name, node.data, converted, result.warnings, error))
				node.data = std::move(converted);
			else // keep the legacy file (the game will not load it) and convert the rest
				result.warnings.push_back(std::format("{} 未轉換（遊戲不會載入它）：{}", node.name, error));
		}

		std::string items, enable;
		const auto addItem = [&](const std::string& file, std::string_view type, bool overlay) {
			items += std::format("    <Item>\n      <filename>{}:/{}</filename>\n      <fileType>{}</fileType>\n      <overlay value=\"{}\" />\n"
			                     "      <disabled value=\"true\" />\n      <persistent value=\"true\" />\n    </Item>\n",
			    device, file, type, overlay ? "true" : "false");
			enable += std::format("        <Item>{}:/{}</Item>\n", device, file);
		};
		if (!stream.children.empty())
			addItem("%PLATFORM%/ml_stream.rpf", "RPF_FILE", true); // overlay: replaces the game's files of the same name
		for (const auto& [file, type] : dataFiles)
			addItem(file, type, false);
		const std::string changeSet = device + "_AUTOGEN";
		const std::string content = std::format(
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CDataFileMgr__ContentsOfDataFileXml>\n  <disabledFiles />\n  <includedXmlFiles />\n"
		    "  <includedDataFiles />\n  <dataFiles>\n{}  </dataFiles>\n  <contentChangeSets>\n    <Item>\n      <changeSetName>{}</changeSetName>\n"
		    "      <filesToDisable />\n      <filesToEnable>\n{}      </filesToEnable>\n      <txdToLoad />\n      <txdToUnload />\n"
		    "      <residentResources />\n      <unregisterResources />\n    </Item>\n  </contentChangeSets>\n  <patchFiles />\n"
		    "</CDataFileMgr__ContentsOfDataFileXml>\n",
		    items, changeSet, enable);
		const std::string setup = std::format(
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SSetupData>\n  <deviceName>{}</deviceName>\n  <datFile>content.xml</datFile>\n"
		    "  <timeStamp>00/00/0000 00:00:00</timeStamp>\n  <nameHash>{}</nameHash>\n  <contentChangeSetGroups>\n    <Item>\n"
		    "      <NameHash>GROUP_STARTUP</NameHash>\n      <ContentChangeSets>\n        <Item>{}</Item>\n      </ContentChangeSets>\n"
		    "    </Item>\n  </contentChangeSetGroups>\n  <type>EXTRACONTENT_COMPAT_PACK</type>\n  <order value=\"999\" />\n</SSetupData>\n",
		    device, device.substr(4), changeSet);

		WriteNode root;
		root.children.push_back(TextFile("content.xml", content));
		root.children.push_back(TextFile("setup2.xml", setup));
		if (!stream.children.empty())
		{
			WriteNode platform = Folder("x64");
			platform.children.push_back(std::move(stream));
			root.children.push_back(std::move(platform));
		}
		if (!dataFiles.empty())
		{
			WriteNode common = Folder("common"), data = Folder("data");
			data.children.push_back(std::move(dataDir));
			common.children.push_back(std::move(data));
			root.children.push_back(std::move(common));
		}

		Bytes packed;
		bool ok = BuildArchive(root, packed, result.error);
		if (ok)
		{
			std::filesystem::create_directories(cacheDir, ec);
			const auto temp = cacheDir / L"dlc.rpf.tmp";
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			ok = out && out.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
			out.close();
			ok = ok && (std::filesystem::rename(temp, cached, ec), !ec);
			if (ok)
			{
				std::ofstream(cacheDir / L"source.txt", std::ios::binary | std::ios::trunc) << stamp;
				std::ofstream saved(cacheDir / L"warnings.txt", std::ios::trunc); // shown again when the cache is used
				for (const auto& w : result.warnings)
					saved << w << "\n";
			}
			else
				result.error = "cannot write " + Utf8(cached.u8string());
		}
		if (!ok)
		{
			result.state = PackState::Failed;
			result.dir = dir;
			log::Error("replacement mod {}: {}", name, result.error);
			return result;
		}
		for (const auto& w : result.warnings)
			log::Warn("replacement mod {}: {}", name, w);
		log::Info("replacement mod {}: generated a pack ({} streaming file(s), {} legacy converted, {} data file(s))", name,
		    files.streaming.size(), legacy, dataFiles.size());
		return result;
	}

	std::vector<OivPack> ExtractOivPacks(const std::string& mod, const std::filesystem::path& dir, std::vector<std::string>& warnings)
	{
		std::vector<OivPack> packs;
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (!it->is_regular_file(ec) || Extension(Utf8(it->path().filename().u8string())) != ".oiv")
				continue;
			Oiv oiv;
			std::string error;
			if (!ReadOiv(it->path(), oiv, error))
				continue; // reported by PrepareReplacement
			const std::string stamp = std::format("{} {} {}", Utf8(it->path().generic_u8string()), it->file_size(ec),
			    it->last_write_time(ec).time_since_epoch().count());
			for (const OivFile& f : oiv.files)
			{
				if (!f.dlcPack)
					continue;
				OivPack pack{std::format("{}-{}", mod, f.packName), ModCache(mod) / L"oiv" / std::filesystem::path(std::u8string(f.packName.begin(), f.packName.end()))};
				const auto file = pack.dir / L"dlc.rpf";
				if (!std::filesystem::is_regular_file(file, ec) || ReadAll(pack.dir / L"source.txt") != stamp)
				{
					Bytes data;
					if (!ReadOivFile(it->path(), f.source, data, error))
					{
						warnings.push_back(std::format("{}：{} 無法讀取，已略過：{}", Utf8(it->path().filename().u8string()), f.source, error));
						continue;
					}
					std::filesystem::create_directories(pack.dir, ec);
					std::ofstream(file, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
					std::ofstream(pack.dir / L"source.txt", std::ios::binary | std::ios::trunc) << stamp;
					log::Info("replacement mod {}: extracted DLC pack {} from {}", mod, f.packName, Utf8(it->path().filename().u8string()));
				}
				packs.push_back(std::move(pack));
			}
		}
		return packs;
	}
}
