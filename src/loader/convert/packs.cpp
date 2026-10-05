#include "packs.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <unordered_map>

#include "../log.hpp"
#include "../paths.hpp"
#include "effects.hpp"
#include "ytd.hpp"
#include "yft.hpp"

namespace loader::convert
{
	namespace
	{
		// Bump when the converters change, so cached packs are rebuilt.
		constexpr int kConverterVersion = 3;

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
		enum class Legacy { None, TextureDictionary, Fragment };
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
			return Legacy::None;
		}

		int CountLegacy(const Archive& archive, std::string& error)
		{
			int count = 0;
			const auto& nodes = archive.Nodes();
			for (uint32_t i = 0; i < nodes.size(); ++i)
			{
				if (LegacyKind(nodes[i]) != Legacy::None)
					++count;
				else if (IsArchive(nodes[i]))
				{
					Archive nested;
					std::string nestedError;
					if (archive.OpenNested(i, nested, nestedError))
						count += CountLegacy(nested, error);
				}
			}
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
				if (!archive.ReadFile(index, data, error))
					return false;
				node.kind = n.resource ? WriteNode::Kind::Resource : WriteNode::Kind::File;
				const Legacy kind = LegacyKind(n);
				if (kind != Legacy::None)
				{
					SetProgress([&](Progress& p) { p.file = inner; });
					Bytes converted;
					std::string why;
					const bool ok = kind == Legacy::TextureDictionary ? ConvertTextureDictionary(data, converted, why)
					                                                 : ConvertFragment(data, *job.effects, converted, job.result->warnings, why);
					if (ok)
					{
						data = std::move(converted);
						++job.result->convertedFiles;
					}
					else // keep the legacy file (the game will not load it) and convert the rest
						job.result->warnings.push_back(std::format("{} not converted: {}", inner, why));
					SetProgress([&](Progress& p) { ++p.done; });
				}
				node.data = std::move(data);
				out.children.push_back(std::move(node));
			}
			return true;
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
		const int total = CountLegacy(archive, result.error);
		if (!total)
			return result;

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
		ok = ok && Build(archive, 0, root, job, "", result.error) && BuildArchive(root, packed, result.error);
		if (ok)
		{
			std::filesystem::create_directories(cacheDir, ec);
			const auto temp = cacheDir / L"dlc.rpf.tmp";
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			ok = out && out.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
			out.close();
			ok = ok && (std::filesystem::rename(temp, cached, ec), !ec);
			if (ok)
				std::ofstream(stampFile, std::ios::trunc) << stamp << "\n";
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
