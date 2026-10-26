#include "gamefiles.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <tuple>

namespace ue5export
{
	namespace fs = std::filesystem;
	using loader::convert::Archive;
	using loader::convert::Bytes;

	namespace
	{
		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		// The inner text of every <tag>...</tag> between from and to (not nested in itself).
		std::vector<std::string> Tags(const std::string& xml, const std::string& tag, size_t from = 0, size_t to = std::string::npos)
		{
			std::vector<std::string> out;
			const std::string open = "<" + tag + ">", close = "</" + tag + ">";
			to = std::min(to, xml.size());
			for (size_t at = xml.find(open, from); at != std::string::npos && at < to; at = xml.find(open, at + 1))
			{
				const size_t end = xml.find(close, at);
				if (end == std::string::npos || end > to)
					break;
				out.push_back(xml.substr(at + open.size(), end - at - open.size()));
			}
			return out;
		}

		std::string Trim(std::string s)
		{
			const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
			s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), space));
			s.erase(std::find_if_not(s.rbegin(), s.rend(), space).base(), s.end());
			return s;
		}

		bool OpenFile(const fs::path& file, const loader::convert::Decryptor& decrypt, std::shared_ptr<Archive>& out, std::string& error)
		{
			const auto in = std::make_shared<std::ifstream>(file, std::ios::binary);
			if (!*in)
				return error = std::format("cannot open it (errno {})", errno), false;
			std::error_code ec;
			out = std::make_shared<Archive>();
			return out->Open(in, 0, fs::file_size(file, ec), file.filename().string(), &decrypt, error);
		}

		// A node by path ("x64/levels/a.rpf") from the root of an archive.
		std::optional<uint32_t> NodeAt(const Archive& archive, const std::string& path)
		{
			uint32_t node = 0;
			size_t at = 0;
			while (at < path.size())
			{
				const size_t slash = path.find('/', at);
				const std::string name = Lower(path.substr(at, slash == std::string::npos ? std::string::npos : slash - at));
				const auto& children = archive.Nodes()[node].children;
				const auto it = std::find_if(children.begin(), children.end(), [&](uint32_t c) { return Lower(archive.Nodes()[c].name) == name; });
				if (it == children.end())
					return std::nullopt;
				node = *it;
				if (slash == std::string::npos)
					break;
				at = slash + 1;
			}
			return node;
		}

		struct Pack
		{
			std::string device;                       // "dlc_patchday3ng"
			std::set<std::string> sets;               // change sets story mode executes
			std::map<std::string, bool> overlay;      // data file -> overlay
			std::map<std::string, std::vector<std::string>> enable; // change set -> files
			std::vector<std::string> startup;         // RPF data files not disabled: mounted with the pack
			std::set<std::string> parents;            // GTXD_PARENTING_DATA files (texture dictionaries' parents)
		};

		Pack ReadPack(const std::string& setup, const std::string& content)
		{
			Pack p;
			if (const auto device = Tags(setup, "deviceName"); !device.empty())
				p.device = Lower(Trim(device[0]));
			// <NameHash>GROUP_X</NameHash> <ContentChangeSets> <Item>set</Item>... </ContentChangeSets>
			for (size_t at = setup.find("<NameHash>"); at != std::string::npos; at = setup.find("<NameHash>", at + 1))
			{
				const size_t end = setup.find("</NameHash>", at);
				const std::string group = Trim(setup.substr(at + 10, end - at - 10));
				const size_t next = setup.find("<NameHash>", end);
				if (group == "GROUP_MAP")
					continue; // the online map (story mode's is GROUP_MAP_SP)
				for (const std::string& set : Tags(setup, "Item", end, next))
					p.sets.insert(Lower(Trim(set)));
			}
			const size_t files = content.find("<dataFiles>"), filesEnd = content.find("</dataFiles>");
			for (const std::string& item : Tags(content, "Item", files, filesEnd))
			{
				const auto name = Tags(item, "filename"), type = Tags(item, "fileType");
				if (name.empty() || type.empty())
					continue;
				const std::string file = Lower(Trim(name[0]));
				if (Trim(type[0]) == "GTXD_PARENTING_DATA")
				{
					p.parents.insert(file);
					if (item.find("<disabled value=\"true\"") == std::string::npos)
						p.startup.push_back(file);
					continue;
				}
				if (Trim(type[0]) != "RPF_FILE")
					continue;
				p.overlay[file] = item.find("<overlay value=\"true\"") != std::string::npos;
				if (item.find("<disabled value=\"true\"") == std::string::npos)
					p.startup.push_back(file);
			}
			for (size_t at = content.find("<changeSetName>"); at != std::string::npos; at = content.find("<changeSetName>", at + 1))
			{
				const size_t end = content.find("</changeSetName>", at);
				const std::string set = Lower(Trim(content.substr(at + 15, end - at - 15)));
				const size_t next = content.find("<changeSetName>", end);
				const size_t list = content.find("<filesToEnable>", end), listEnd = content.find("</filesToEnable>", end);
				if (list != std::string::npos && list < next && listEnd < next)
					for (const std::string& file : Tags(content, "Item", list, listEnd))
						p.enable[set].push_back(Lower(Trim(file)));
			}
			return p;
		}
	}

	void GameFiles::Register(const std::string& path, const std::shared_ptr<const Archive>& archive, const std::string& source)
	{
		const std::string key = Lower(path);
		if (archives_.contains(key))
			order_.erase(std::find(order_.begin(), order_.end(), key)); // replaced: mounted again, later
		archives_[key] = {archive, source};
		order_.push_back(key);
	}

	void GameFiles::Mount(const std::shared_ptr<const Archive>& archive, uint32_t node, const std::string& prefix, const std::string& source, bool topLevel)
	{
		for (const uint32_t c : archive->Nodes()[node].children)
		{
			const Archive::Node& n = archive->Nodes()[c];
			const std::string path = prefix + Lower(n.name);
			if (n.directory)
			{
				Mount(archive, c, path + "/", source + "/" + n.name, topLevel);
				continue;
			}
			if (path.ends_with(".rpf") && topLevel)
			{
				auto nested = std::make_shared<Archive>();
				std::string error;
				if (archive->OpenNested(c, *nested, error))
					Register(path, nested, source + "/" + n.name);
				else
					warnings.push_back(source + "/" + n.name + ": " + error);
				continue;
			}
			loose_[path] = {archive, c, source + "/" + n.name};
		}
	}

	bool GameFiles::Open(const fs::path& gameDir, const loader::convert::Decryptor& decrypt, std::string& error)
	{
		decrypt_ = &decrypt;
		// Base: x64*.rpf as platform:/, common.rpf as common:/.
		std::vector<fs::path> base;
		for (const auto& e : fs::directory_iterator(gameDir))
			if (e.path().extension() == ".rpf")
				base.push_back(e.path());
		std::sort(base.begin(), base.end());
		for (const fs::path& file : base)
		{
			std::shared_ptr<Archive> archive;
			if (!OpenFile(file, decrypt, archive, error))
				return error = file.filename().string() + ": " + error, false;
			const std::string name = Lower(file.filename().string());
			Mount(archive, 0, name == "common.rpf" ? "common/" : "", name, true);
		}
		// The title update: x64\<path> replaces platform:/<path>, common\<path> common:/<path>.
		for (const char* update : {"update.rpf", "update2.rpf"})
		{
			std::shared_ptr<Archive> archive;
			if (!OpenFile(gameDir / "update" / update, decrypt, archive, error))
				return error = std::string(update) + ": " + error, false;
			for (const uint32_t c : archive->Nodes()[0].children)
			{
				const Archive::Node& n = archive->Nodes()[c];
				const std::string top = Lower(n.name);
				if (n.directory && (top == "x64" || top == "common"))
					Mount(archive, c, top == "x64" ? "" : "common/", std::string(update) + "/" + n.name, true);
			}
		}
		// DLC packs, in dlclist.xml order.
		const auto list = loose_.find("common/data/dlclist.xml");
		Bytes listData;
		if (list == loose_.end() || !list->second.archive->ReadFile(list->second.node, listData, error))
			return error = "dlclist.xml: " + error, false;
		std::vector<std::string> items = Tags(std::string(listData.begin(), listData.end()), "Item");
		for (const fs::path& extra : extraPacks)
			items.push_back("extra:" + extra.string());
		std::vector<std::pair<std::string, File>> parentFiles;
		for (std::string item : items)
		{
			item = item.starts_with("extra:") ? item : Lower(Trim(item));
			// The pack's archives: one device over dlc.rpf, dlc1.rpf, dlc2.rpf... (the enhanced game splits packs).
			std::vector<std::pair<std::shared_ptr<Archive>, std::string>> parts;
			if (item.starts_with("extra:"))
			{
				// a mod's pack: mounted like the game's (its folder's name as the pack's)
				const fs::path file = item.substr(6);
				std::shared_ptr<Archive> part;
				std::string e;
				if (OpenFile(file, decrypt, part, e))
					parts.emplace_back(part, file.generic_string());
				else
					warnings.push_back(file.string() + ": " + e);
				item = "mods:/" + Lower(file.parent_path().filename().string()) + "/";
			}
			if (item.starts_with("platform:/dlcpacks/"))
			{
				const std::string where = "dlcpacks/" + item.substr(19) + "dlc.rpf";
				if (const auto it = archives_.find(where); it != archives_.end())
					parts.emplace_back(std::const_pointer_cast<Archive>(it->second.archive), where);
			}
			else if (item.starts_with("dlcpacks:/"))
			{
				const fs::path dir = gameDir / "update" / "x64" / "dlcpacks" / item.substr(10);
				for (int i = 0; i < 10; ++i)
				{
					const fs::path file = dir / (i ? std::format("dlc{}.rpf", i) : std::string("dlc.rpf"));
					std::shared_ptr<Archive> part;
					std::string e;
					if (fs::exists(file) && OpenFile(file, decrypt, part, e))
						parts.emplace_back(part, fs::relative(file, gameDir).generic_string());
					else if (fs::exists(file))
						warnings.push_back(file.string() + ": " + e);
				}
			}
			if (parts.empty())
			{
				if (item.find("dlcpacks") != std::string::npos)
					warnings.push_back("no pack " + item);
				continue;
			}
			// (archive, node, where) of a path in the pack.
			const auto find = [&](const std::string& path) -> std::optional<std::tuple<std::shared_ptr<Archive>, uint32_t, std::string>> {
				for (const auto& [part, where] : parts)
					if (const auto node = NodeAt(*part, path))
						return std::make_tuple(part, *node, where);
				return std::nullopt;
			};
			const auto readText = [&](const char* name) {
				Bytes data;
				std::string e;
				const auto at = find(name);
				return at && std::get<0>(*at)->ReadFile(std::get<1>(*at), data, e) ? std::string(data.begin(), data.end()) : std::string();
			};
			const Pack p = ReadPack(readText("setup2.xml"), readText("content.xml"));
			std::vector<std::string> mount = p.startup;
			for (const std::string& set : p.sets)
				if (const auto it = p.enable.find(set); it != p.enable.end())
					mount.insert(mount.end(), it->second.begin(), it->second.end());
			std::set<std::string> done;
			for (const std::string& file : mount)
			{
				// "dlc_patchday3ng:/%platform%/levels/gta5/x.rpf" -> "x64/levels/gta5/x.rpf" in the pack.
				const size_t colon = file.find(":/");
				if (colon == std::string::npos || file.substr(0, colon) != p.device || !done.insert(file).second)
					continue;
				std::string inPack = file.substr(colon + 2);
				if (inPack.starts_with("%platform%/"))
					inPack = "x64/" + inPack.substr(11);
				if (p.parents.contains(file))
				{
					// (the pack's texture parents: a data file of the pack itself, beside its archives)
					if (const auto at = find(inPack))
						parentFiles.emplace_back(Lower(inPack.substr(inPack.rfind('/') + 1)),
						    File{std::get<0>(*at), std::get<1>(*at), std::get<2>(*at) + "/" + inPack});
					continue;
				}
				if (!file.ends_with(".rpf"))
					continue;
				const auto at = find(inPack);
				if (!at)
				{
					warnings.push_back(std::format("{}: no {}", item, inPack));
					continue;
				}
				const auto& [pack, node, where] = *at;
				auto nested = std::make_shared<Archive>();
				std::string e;
				if (!pack->OpenNested(node, *nested, e))
				{
					warnings.push_back(std::format("{}/{}: {}", where, inPack, e));
					continue;
				}
				const std::string path = inPack.starts_with("x64/") ? inPack.substr(4) : inPack;
				// (An overlay archive often holds only the files it changes: the archive it overlays stays, and its
				// files win by name.)
				Register("dlc/" + item.substr(item.rfind(":/") + 2) + path, nested, where + "/" + inPack);
			}
		}
		// Every file of the active archives by name (later ones win), loose files last.
		for (const std::string& key : order_)
		{
			if (key.starts_with("dlcpacks/"))
				continue; // (packs of the base archives: their active parts are mounted above)
			const Mounted& m = archives_[key];
			const auto walk = [&](auto&& self, uint32_t node) -> void {
				for (const uint32_t c : m.archive->Nodes()[node].children)
				{
					const Archive::Node& n = m.archive->Nodes()[c];
					if (n.directory)
						self(self, c);
					else
					{
						File file{m.archive, c, m.source + "/" + n.name};
						const std::string name = Lower(n.name);
						if (name.ends_with(".meta") || name.ends_with(".ymf") || name.ends_with(".xml") || name.ends_with("gtxd.ymt")) // (data files, every archive's manifest, the texture parents)
							all_[name].push_back(file);
						byName_[name] = std::move(file);
					}
				}
			};
			walk(walk, 0);
		}
		for (const auto& [path, file] : loose_)
		{
			const std::string name = path.substr(path.rfind('/') + 1);
			if (name.ends_with(".meta") || name.ends_with(".ymf") || name.ends_with(".xml") || name.ends_with("gtxd.ymt")) // (data files, every archive's manifest, the texture parents)
				all_[name].push_back(file);
			byName_[name] = file;
		}
		// (the packs' texture parents, each read with its pack)
		for (auto& [name, file] : parentFiles)
		{
			all_[name].push_back(file);
			byName_[name] = std::move(file);
		}
		activeArchives = order_.size();
		return true;
	}

	std::vector<GameFiles::File> GameFiles::AllOf(const std::string& name) const
	{
		const auto it = all_.find(name);
		return it == all_.end() ? std::vector<File>{} : it->second;
	}

	const GameFiles::File* GameFiles::Find(const std::string& name) const
	{
		const auto it = byName_.find(name);
		return it == byName_.end() ? nullptr : &it->second;
	}

	bool GameFiles::Read(const File& file, Bytes& out, std::string& error) const
	{
		return file.archive->ReadFile(file.node, out, error);
	}
}
