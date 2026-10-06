#include "packs.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <unordered_map>

#include "../log.hpp"
#include "../paths.hpp"
#include "effects.hpp"
#include "peds.hpp"
#include "ytd.hpp"
#include "yft.hpp"

namespace loader::convert
{
	namespace
	{
		// Bump when the converters change, so cached packs are rebuilt.
		constexpr int kConverterVersion = 12;

		std::mutex g_progressMutex;
		Progress g_progress;

		void SetProgress(const std::function<void(Progress&)>& change)
		{
			std::lock_guard lock(g_progressMutex);
			change(g_progress);
		}

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
			return s;
		}

		bool IsArchive(const Archive::Node& n)
		{
			return !n.directory && !n.resource && !n.stored && Lower(n.name).ends_with(".rpf");
		}

		// Legacy resources this converter handles, recognised by extension and the version in the entry flags
		// (version = (virtual flags >> 28) << 4 | physical flags >> 28).
		// Unsupported: legacy versions the game cannot load and there is no converter for yet.
		enum class Legacy { None, TextureDictionary, Fragment, Drawable, Unsupported };
		constexpr std::pair<std::string_view, uint32_t> kUnsupported[] = {{".ypt", 68}}; // particle effects
		Legacy LegacyKind(const Archive::Node& n)
		{
			if (!n.resource)
				return Legacy::None;
			const uint32_t version = ((n.flags[0] >> 28) << 4) | (n.flags[1] >> 28);
			const std::string name = Lower(n.name);
			if (name.ends_with(".ytd") && version == kLegacyTxdVersion)
				return Legacy::TextureDictionary;
			if (name.ends_with(".yft") && version == kLegacyFragmentVersion)
				return Legacy::Fragment;
			if ((name.ends_with(".ydr") || name.ends_with(".ydd")) && version == kLegacyDrawableVersion)
				return Legacy::Drawable;
			for (const auto& [extension, legacy] : kUnsupported)
				if (name.ends_with(extension) && version == legacy)
					return Legacy::Unsupported;
			return Legacy::None;
		}

		std::string UnsupportedWarning(const std::string& path)
		{
			return std::format("{} 是舊版格式，目前還無法轉換，遊戲不會載入它", path);
		}

		// Convertible legacy files; unsupported ones are listed in `unsupported`.
		int CountLegacy(const Archive& archive, const std::string& path, std::vector<std::string>& unsupported)
		{
			int count = 0;
			const auto& nodes = archive.Nodes();
			const auto walk = [&](auto&& self, uint32_t dir, const std::string& prefix) -> void {
				for (const uint32_t i : nodes[dir].children)
				{
					const auto& n = nodes[i];
					const std::string inner = prefix + n.name;
					if (n.directory)
						self(self, i, inner + "/");
					else if (const Legacy kind = LegacyKind(n); kind == Legacy::Unsupported)
						unsupported.push_back(inner);
					else if (kind != Legacy::None)
						++count;
					else if (IsArchive(n))
					{
						Archive nested;
						std::string ignored;
						if (archive.OpenNested(i, nested, ignored))
							count += CountLegacy(nested, inner + "/", unsupported);
					}
				}
			};
			walk(walk, 0, path);
			return count;
		}

		struct Job
		{
			std::string pack;
			const std::unordered_map<uint32_t, Effect>* effects;
			PackResult* result;
		};

		bool Build(const Archive& archive, uint32_t dir, WriteNode& out, Job& job, const std::string& path, std::string& error)
		{
			for (const uint32_t index : archive.Nodes()[dir].children)
			{
				const Archive::Node& n = archive.Nodes()[index];
				const std::string inner = path + n.name;
				WriteNode node;
				node.name = n.name;
				if (n.directory)
				{
					if (!Build(archive, index, node, job, inner + "/", error))
						return false;
					out.children.push_back(std::move(node));
					continue;
				}
				if (IsArchive(n))
				{
					Archive nested;
					std::string nestedError;
					if (archive.OpenNested(index, nested, nestedError))
					{
						node.kind = WriteNode::Kind::Archive;
						if (!Build(nested, 0, node, job, inner + "/", error))
							return false;
						out.children.push_back(std::move(node));
						continue;
					}
					log::Warn("convert {}: {} kept as it is ({})", job.pack, inner, nestedError);
				}
				Bytes data;
				std::string readError;
				if (!archive.ReadFile(index, data, readError))
				{
					// An unreadable file (e.g. damaged, or encrypted with keys we do not have) is left out.
					job.result->warnings.push_back(std::format("{} 無法讀取，已略過：{}", inner, readError));
					continue;
				}
				node.kind = n.resource ? WriteNode::Kind::Resource : WriteNode::Kind::File;
				const Legacy kind = LegacyKind(n);
				if (kind == Legacy::Unsupported)
					job.result->warnings.push_back(UnsupportedWarning(inner));
				else if (kind != Legacy::None)
				{
					SetProgress([&](Progress& p) { p.file = inner; });
					Bytes converted;
					std::string why;
					const bool ok = kind == Legacy::TextureDictionary ? ConvertTextureDictionary(data, converted, why)
					                : kind == Legacy::Fragment ? ConvertDrawableResource(data, kLegacyFragmentVersion, kEnhancedFragmentVersion, *job.effects, converted, job.result->warnings, why)
					                                           : ConvertDrawableResource(data, kLegacyDrawableVersion, kEnhancedDrawableVersion, *job.effects, converted, job.result->warnings, why);
					if (ok)
					{
						data = std::move(converted);
						++job.result->convertedFiles;
					}
					else // keep the legacy file (the game will not load it) and convert the rest
						job.result->warnings.push_back(std::format("{} 未轉換（遊戲不會載入它）：{}", inner, why));
					SetProgress([&](Progress& p) { ++p.done; });
				}
				if (n.resource && Lower(n.name).ends_with(".ymt"))
				{
					// Ped variation data: the ped's drawables are in the .ydd of the same name next to it.
					const std::string stem = Lower(n.name.substr(0, n.name.size() - 4));
					for (const uint32_t sibling : archive.Nodes()[dir].children)
					{
						const Archive::Node& s = archive.Nodes()[sibling];
						if (!s.resource || Lower(s.name) != stem + ".ydd")
							continue;
						Bytes dictionary;
						std::string ignored;
						if (archive.ReadFile(sibling, dictionary, ignored))
							DropMissingComponents(data, dictionary, inner, job.result->warnings);
						break;
					}
				}
				node.data = std::move(data);
				out.children.push_back(std::move(node));
			}
			return true;
		}

		void CollectPaths(const WriteNode& node, const std::string& prefix, std::vector<std::string>& out)
		{
			for (const WriteNode& c : node.children)
			{
				const std::string path = prefix + Lower(c.name);
				out.push_back(path);
				if (c.kind == WriteNode::Kind::Directory || c.kind == WriteNode::Kind::Archive)
					CollectPaths(c, path + "/", out);
			}
		}

		// Legacy packs often list files in content.xml that they do not contain (left over from templates).
		// The legacy game skipped them; Enhanced never finishes loading story mode. Drop those entries.
		void DropMissingContent(WriteNode& root, PackResult& result)
		{
			auto it = std::find_if(root.children.begin(), root.children.end(), [](const WriteNode& n) { return Lower(n.name) == "content.xml"; });
			if (it == root.children.end())
				return;
			std::vector<std::string> paths;
			CollectPaths(root, "", paths);
			const auto exists = [&](std::string ref) {
				ref = Lower(ref);
				const size_t device = ref.find(":/");
				if (device != std::string::npos)
					ref = ref.substr(device + 2);
				if (const size_t platform = ref.find("%platform%"); platform != std::string::npos)
					ref.replace(platform, 10, "x64");
				while (!ref.empty() && ref.back() == '/')
					ref.pop_back();
				return ref.empty() || std::any_of(paths.begin(), paths.end(), [&](const std::string& p) { return p == ref || p.starts_with(ref + "/"); });
			};
			std::string xml(it->data.begin(), it->data.end());
			std::vector<std::string> dropped;
			// <Item> ... <filename>X</filename> ... </Item> (data files) and <Item>X</Item> (change sets).
			for (size_t at = 0; (at = xml.find("<Item>", at)) != std::string::npos;)
			{
				const size_t end = xml.find("</Item>", at);
				if (end == std::string::npos)
					break;
				const std::string item = xml.substr(at + 6, end - at - 6);
				if (item.find("<Item>") != std::string::npos) // a container (e.g. a change set): look inside it
				{
					at += 6;
					continue;
				}
				std::string ref = item;
				if (const size_t f = item.find("<filename>"); f != std::string::npos)
				{
					const size_t g = item.find("</filename>", f);
					ref = g == std::string::npos ? std::string() : item.substr(f + 10, g - f - 10);
				}
				const bool isPath = ref.find(":/") != std::string::npos;
				if (isPath && !exists(ref))
				{
					size_t from = at;
					while (from > 0 && (xml[from - 1] == ' ' || xml[from - 1] == '\t'))
						--from;
					size_t to = end + 7;
					if (to < xml.size() && xml[to] == '\r')
						++to;
					if (to < xml.size() && xml[to] == '\n')
						++to;
					if (std::find(dropped.begin(), dropped.end(), ref) == dropped.end())
						dropped.push_back(ref);
					xml.erase(from, to - from);
					at = from;
				}
				else
					at = end + 7;
			}
			if (dropped.empty())
				return;
			it->data.assign(xml.begin(), xml.end());
			for (const auto& ref : dropped)
				result.warnings.push_back(std::format("content.xml 引用了包內不存在的 {}，已移除該項目", ref));
		}

		std::string SourceStamp(const std::filesystem::path& file)
		{
			std::error_code ec;
			const auto size = std::filesystem::file_size(file, ec);
			const auto time = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
			return std::format("{} {} {}", size, time, kConverterVersion);
		}

		std::string ReadText(const std::filesystem::path& file)
		{
			std::ifstream in(file);
			std::string line;
			std::getline(in, line);
			return line;
		}

		const std::unordered_map<uint32_t, Effect>* Effects()
		{
			static std::unordered_map<uint32_t, Effect> effects;
			static const bool loaded = LoadEffects(effects);
			return loaded ? &effects : nullptr;
		}
	}

	PackResult PreparePack(const std::string& name, const std::filesystem::path& dir, const Decryptor* decrypt,
	    const std::function<void()>& onConvert)
	{
		PackResult result;
		result.dir = dir;
		const auto source = dir / L"dlc.rpf";
		auto in = std::make_shared<std::ifstream>(source, std::ios::binary);
		std::error_code ec;
		const auto size = std::filesystem::file_size(source, ec);
		Archive archive;
		if (!*in || !archive.Open(in, 0, size, "dlc.rpf", decrypt, result.error))
		{
			// The game opens it itself (and reports problems); nothing to convert that we can see.
			log::Info("dlc pack {}: {}", name, result.error.empty() ? "cannot be read" : result.error);
			result.error.clear();
			return result;
		}
		std::vector<std::string> unsupported;
		const int total = CountLegacy(archive, "", unsupported);
		if (!total)
		{
			// Nothing to convert; still tell about legacy files the game will not load.
			for (const auto& path : unsupported)
				result.warnings.push_back(UnsupportedWarning(path));
			for (const auto& w : result.warnings)
				log::Warn("dlc pack {}: {}", name, w);
			return result;
		}

		const auto cacheDir = paths::Get().root / L"cache" / std::filesystem::path(std::u8string(name.begin(), name.end()));
		const auto cached = cacheDir / L"dlc.rpf";
		const auto stampFile = cacheDir / L"source.txt";
		const std::string stamp = SourceStamp(source);
		result.dir = cacheDir;
		if (std::filesystem::is_regular_file(cached, ec) && ReadText(stampFile) == stamp)
		{
			result.state = PackState::Converted;
			result.fromCache = true;
			result.convertedFiles = total;
			std::ifstream saved(cacheDir / L"warnings.txt");
			for (std::string line; std::getline(saved, line);)
				if (!line.empty())
					result.warnings.push_back(line);
			log::Info("dlc pack {}: using the converted copy ({} legacy file(s))", name, total);
			return result;
		}

		log::Info("dlc pack {}: converting {} legacy file(s)", name, total);
		if (onConvert)
			onConvert();
		SetProgress([&](Progress& p) { p = {true, name, {}, 0, total}; });
		Job job{name, Effects(), &result};
		WriteNode root;
		bool ok = job.effects != nullptr;
		if (!ok)
			result.error = "the game's shader effects could not be read";
		Bytes packed;
		ok = ok && Build(archive, 0, root, job, "", result.error);
		if (ok)
			DropMissingContent(root, result);
		ok = ok && BuildArchive(root, packed, result.error);
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
				std::ofstream(stampFile, std::ios::trunc) << stamp << "\n";
				std::ofstream saved(cacheDir / L"warnings.txt", std::ios::trunc); // shown again when the cache is used
				for (const auto& w : result.warnings)
					saved << w << "\n";
			}
			else
				result.error = "cannot write " + std::string(reinterpret_cast<const char*>(cached.u8string().c_str()));
		}
		SetProgress([&](Progress& p) { p.active = false; });
		if (!ok)
		{
			result.state = PackState::Failed;
			result.dir = dir;
			log::Error("dlc pack {}: conversion failed: {}", name, result.error);
			return result;
		}
		result.state = PackState::Converted;
		for (const auto& w : result.warnings)
			log::Warn("dlc pack {}: {}", name, w);
		log::Info("dlc pack {}: converted {} file(s) into {}", name, result.convertedFiles, std::string(reinterpret_cast<const char*>(cached.u8string().c_str())));
		return result;
	}

	Progress CurrentProgress()
	{
		std::lock_guard lock(g_progressMutex);
		return g_progress;
	}
}
