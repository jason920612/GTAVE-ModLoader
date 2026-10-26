#include "resolve.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <format>

namespace ue5export
{
	using loader::convert::Bytes;

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	uint32_t Joaat(std::string_view s)
	{
		uint32_t h = 0;
		for (const char c : s)
		{
			h += static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)));
			h += h << 10;
			h ^= h >> 6;
		}
		h += h << 3;
		h ^= h >> 11;
		return h + (h << 15);
	}

	namespace
	{
		// Shader parameter names (matched by hash, research/phase0.md §58.4).
		const std::map<uint32_t, std::string>& ParameterNames()
		{
			static const std::map<uint32_t, std::string> names = [] {
				std::map<uint32_t, std::string> m;
				for (const char* n : {"DiffuseTex", "BumpTex", "SpecularTex", "DetailTex", "DirtTex", "DirtBumpTex", "TintPaletteTex",
				         "DiffuseTexture_layer0", "DiffuseTexture_layer1", "DiffuseTexture_layer2", "DiffuseTexture_layer3",
				         "BumpTexture_layer0", "BumpTexture_layer1", "BumpTexture_layer2", "BumpTexture_layer3", "HeightTexture",
				         "LookupTexture", "NoiseTex", "SnowTex", "DamageTexture", "PlateBgTex", "PlateBgBumpTex", "VolumeTex", "bumpiness",
				         "wetnessMultiplier", "useTessellation", "HardAlphaBlend", "globalAnimUV0", "globalAnimUV1", "emissiveMultiplier",
				         "specularIntensityMult", "specularFalloffMult", "specularFresnel", "detailSettings", "tintPaletteSelector",
				         "AlphaTest", "alphaTestValue", "materialWetnessMultiplier", "parallaxSelfShadowAmount", "heightScale", "heightBias",
				         "umGlobalParams", "WindGlobalParams"})
					m[Joaat(n)] = n;
				return m;
			}();
			return names;
		}
	}

	std::string ParameterName(uint32_t hash)
	{
		const auto& names = ParameterNames();
		const auto it = names.find(hash);
		return it != names.end() ? it->second : std::format("{:08x}", hash);
	}

	std::shared_ptr<const Resolver::Index> Resolver::s_index;

	void Resolver::SetIndex(std::shared_ptr<const Index> index)
	{
		std::atomic_store(&s_index, std::move(index));
	}

	std::shared_ptr<const Resolver::Index> Resolver::BuildIndex(const GameFiles& files, const std::filesystem::path& gameDir,
	    const loader::convert::Decryptor& decrypt, unsigned workers, const std::filesystem::path& cache)
	{
		auto index = std::make_shared<Index>();
		{
			std::ifstream in(cache);
			for (std::string line; std::getline(in, line);)
				if (const size_t tab = line.find('\t'); tab != std::string::npos)
					(*index)[line.substr(0, tab)] = static_cast<uint32_t>(std::stoul(line.substr(tab + 1), nullptr, 16));
		}
		// (the game's dictionaries once, into the cache; mods' packs (--extra) every time, over the game's: they change
		// from one export to the next)
		const auto fromMod = [](const GameFiles::File& f) {
			for (const auto& pack : GameFiles::extraPacks)
				if (f.path.starts_with(pack.generic_string()))
					return true;
			return false;
		};
		const bool cached = !index->empty();
		std::vector<std::string> game, mods;
		for (const auto& [name, file] : files.Files())
			if (name.ends_with(".ytd"))
				(fromMod(file) ? mods : game).push_back(name);
		const auto add = [&](const std::vector<std::string>& names, bool replace) {
			std::atomic<size_t> next = 0;
			std::mutex mutex;
			std::vector<std::thread> threads;
			for (unsigned w = 0; w < std::max(1u, workers); ++w)
				threads.emplace_back([&] {
					GameFiles own;
					std::string error;
					if (!own.Open(gameDir, decrypt, error))
						return;
					for (size_t i; (i = next++) < names.size();)
					{
						const GameFiles::File* f = own.Find(names[i]);
						Bytes data;
						loader::convert::Resource resource;
						if (!f || !own.Read(*f, data, error) || !loader::convert::Read(data, resource, error))
							continue;
						const drawable::View view(resource);
						const uint32_t dictionary = Joaat(names[i].substr(0, names[i].size() - 4));
						const bool hi = names[i].find('+') != std::string::npos;
						std::lock_guard lock(mutex);
						for (const auto& t : drawable::Textures(view, drawable::View::kRoot))
						{
							// (the plain dictionary rather than its +hi / +hidr twin: the lookup tries +hi itself)
							auto [it, added] = index->try_emplace(Lower(t.name), dictionary);
							if (!added && (hi || !replace))
								continue;
							it->second = dictionary;
						}
					}
				});
			for (auto& t : threads)
				t.join();
		};
		if (!cached)
		{
			add(game, false);
			std::ofstream out(cache);
			for (const auto& [name, dictionary] : *index)
				out << name << '\t' << std::hex << dictionary << std::dec << '\n';
		}
		add(mods, true);
		return index;
	}

	Resolver::Resolver(const GameFiles& files, size_t cacheBytes) : files_(files), cacheBytes_(cacheBytes)
	{
		for (const auto& [name, file] : files.Files())
			if (const size_t dot = name.rfind('.'); dot != std::string::npos)
				byHash_[name.substr(dot + 1)][Joaat(name.substr(0, dot))] = name;
		ReadParents();
	}

	std::string Resolver::Stem(uint32_t hash) const
	{
		for (const char* ext : {"ydr", "yft", "ydd", "ytd"})
			if (const auto e = byHash_.find(ext); e != byHash_.end())
				if (const auto it = e->second.find(hash); it != e->second.end())
					return it->second.substr(0, it->second.rfind('.'));
		return std::format("{:08x}", hash);
	}

	std::string Resolver::DictionaryName(uint32_t hash) const
	{
		const auto& names = byHash_.at("ytd");
		const auto it = names.find(hash);
		return it == names.end() ? std::string() : it->second.substr(0, it->second.size() - 4);
	}

	std::shared_ptr<const Resolver::Dictionary> Resolver::Get(uint32_t hash)
	{
		if (const auto it = cache_.find(hash); it != cache_.end())
		{
			recent_.splice(recent_.end(), recent_, it->second.second);
			return it->second.first;
		}
		auto textures = std::make_shared<Dictionary>();
		size_t bytes = 0;
		const auto& names = byHash_["ytd"];
		const auto name = names.find(hash);
		const GameFiles::File* file = name == names.end() ? nullptr : files_.Find(name->second);
		Bytes data;
		std::string error;
		loader::convert::Resource resource;
		if (file && files_.Read(*file, data, error) && loader::convert::Read(data, resource, error))
		{
			const drawable::View view(resource);
			for (auto& t : drawable::Textures(view, drawable::View::kRoot))
			{
				bytes += t.data.size();
				(*textures)[Lower(t.name)] = std::move(t);
			}
		}
		// (least recently used dictionaries go when the cache is full; models keep what they hold)
		while (cached_ + bytes > cacheBytes_ && !recent_.empty())
		{
			const auto old = cache_.find(recent_.front());
			for (const auto& [k, t] : *old->second.first)
				cached_ -= t.data.size();
			cache_.erase(old);
			recent_.pop_front();
		}
		recent_.push_back(hash);
		cached_ += bytes;
		cache_[hash] = {textures, std::prev(recent_.end())};
		return textures;
	}

	bool Resolver::In(uint32_t hash, const std::string& key, Texture& out)
	{
		const std::string name = DictionaryName(hash);
		for (const std::string& suffix : {std::string("+hi"), std::string()})
		{
			if (name.empty() && !suffix.empty())
				continue;
			const uint32_t h = suffix.empty() ? hash : Joaat(name + suffix);
			if (auto dict = Get(h); dict && dict->contains(key))
			{
				out = {dict, &dict->at(key), suffix.empty() ? name : name + suffix, key};
				return true;
			}
		}
		return false;
	}

	bool Resolver::Chain(uint32_t hash, const std::string& key, Texture& out)
	{
		for (int depth = 0; hash && depth < 16; ++depth)
		{
			if (In(hash, key, out))
				return true;
			const auto it = parents_.find(hash);
			hash = it == parents_.end() ? 0 : it->second;
		}
		return false;
	}

	// gtxd: the base game's binary table (RBF: string values "FD FF <u16 length> <u16> text\0", parent then child) and
	// the DLC packs' gtxd.meta (<parent>, <child>; a pack's own, beside its archives: the world builder's).
	void Resolver::ReadParents()
	{
		auto& names = byHash_["ytd"];
		const auto add = [&](const std::string& parent, const std::string& child) {
			const uint32_t p = Joaat(parent), c = Joaat(child);
			parents_[c] = p;
			names.try_emplace(p, Lower(parent) + ".ytd");
		};
		Bytes data;
		std::string error;
		// (every version of every *gtxd.ymt / *gtxd.meta: the base game's, the DLCs' (mph4_gtxd.ymt), not just the one Find gives)
		std::vector<GameFiles::File> tables;
		for (const auto& [name, file] : files_.Files())
			if (name.ends_with("gtxd.ymt") || name.ends_with("gtxd.meta"))
				for (const GameFiles::File& v : files_.AllOf(name))
					tables.push_back(v);
		for (const GameFiles::File& f : tables)
		{
			if (!files_.Read(f, data, error))
				continue;
			const std::string xml(data.begin(), data.end());
			for (size_t at = xml.find("<parent>"); at != std::string::npos; at = xml.find("<parent>", at + 1))
			{
				const size_t end = xml.find("</parent>", at), child = xml.find("<child>", at), childEnd = xml.find("</child>", at);
				if (end != std::string::npos && child != std::string::npos && childEnd != std::string::npos)
					add(xml.substr(at + 8, end - at - 8), xml.substr(child + 7, childEnd - child - 7));
			}
			std::vector<std::string> values;
			for (size_t i = 0; i + 6 < data.size(); ++i)
				if (data[i] == 0xFD && data[i + 1] == 0xFF)
				{
					const size_t n = data[i + 2] | data[i + 3] << 8;
					if (n && i + 6 + n <= data.size() && data[i + 5 + n] == 0)
					{
						values.emplace_back(reinterpret_cast<const char*>(data.data() + i + 6), n - 1);
						i += 5 + n;
					}
				}
			for (size_t k = 0; k + 1 < values.size(); k += 2)
				add(values[k], values[k + 1]);
		}
	}

	bool Resolver::Load(const mapdata::Archetype& a, Model& out, std::string& error)
	{
		// The model file: a drawable, a fragment, or a drawable in a dictionary.
		std::string ext;
		uint32_t inDictionary = 0;
		switch (a.assetType)
		{
		case mapdata::AssetType::Drawable:
			ext = "ydr";
			break;
		case mapdata::AssetType::Fragment:
			ext = "yft";
			break;
		case mapdata::AssetType::DrawableDictionary:
			ext = "ydd", inDictionary = a.assetName;
			break;
		default:
			return error = "no model (asset type)", false;
		}
		const uint32_t fileHash = a.assetType == mapdata::AssetType::DrawableDictionary ? a.drawableDictionary : a.assetName;
		const auto& files = byHash_[ext];
		const auto it = files.find(fileHash);
		const GameFiles::File* f = it == files.end() ? nullptr : files_.Find(it->second);
		if (!f)
			return error = "file not found", false;
		Bytes data;
		loader::convert::Resource resource;
		if (!files_.Read(*f, data, error) || !loader::convert::Read(data, resource, error))
			return error = it->second + ": " + error, false;
		const drawable::View view(resource);
		const auto drawables = drawable::Drawables(view, ext);
		const auto d = drawables.find(inDictionary);
		if (d == drawables.end() || !drawable::Read(view, d->second, out.model, error))
			return error = std::format("{} has no drawable {:08x} {}", it->second, inDictionary, error), false;
		out.model.lights = drawable::Lights(view, ext == "yft" ? drawable::View::kRoot + 0x110 : d->second + 0xB0, d->second);
		out.file = f->path;
		out.archetypeMin = a.bbMin, out.archetypeMax = a.bbMax;

		const std::string name = Stem(a.name);
		std::map<std::string, const drawable::Texture*> embedded;
		for (const auto& t : out.model.embedded)
			embedded[Lower(t.name)] = &t;
		auto hidr = Get(Joaat(name + "+hidr")); // (the drawable's HD textures, swapped in up close)
		for (const auto& m : out.model.materials)
		{
			auto& textures = out.textures.emplace_back();
			for (const auto& [param, textureName] : m.textures)
			{
				const std::string key = Lower(textureName);
				Texture t;
				bool found = true;
				if (hidr->contains(key))
					t = {hidr, &hidr->at(key), name + "+hidr", key};
				else if (const auto e = embedded.find(key); e != embedded.end())
					t = {nullptr, e->second, name, key}; // (the model holds it)
				else if (!Chain(a.textureDictionary, key, t) && !In(Joaat("mapdetail"), key, t)) // (env_*: the map's shared detail)
				{
					// (in a dictionary off the chain: the index of all of them)
					const auto index = std::atomic_load(&s_index);
					const auto at = index ? index->find(key) : Index::const_iterator{};
					found = index && at != index->end() && In(at->second, key, t);
				}
				if (found)
					textures[param] = std::move(t);
				else
					out.missing.push_back(key);
			}
		}
		return true;
	}
}
