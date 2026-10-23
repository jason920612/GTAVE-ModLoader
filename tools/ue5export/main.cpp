// ue5export: the game's world out to Unreal Engine 5, for the graphics mod (research/phase0.md §58). Everything it
// writes comes from the player's own game files and stays on their computer.
//
// usage:
//   ue5export keys  --out <dir> [--dump <image>]  the archive keys, once, from the running game (or a dump of it)
//   ue5export index --out <dir> --game <dir>      every file in the game's archives: <out>\index.tsv
//   ue5export extract --out <dir> --game <dir> --file <index path>   one file, plain: <out>iles\<name>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "convert/archive.hpp"
#include "ng.hpp"

namespace fs = std::filesystem;
using loader::convert::Archive;

namespace
{
	std::map<std::string, std::string> g_options;

	fs::path KeysFile() { return fs::path(g_options["out"]) / "keys" / "ng.bin"; }

	int Fail(const std::string& message)
	{
		std::fprintf(stderr, "%s\n", message.c_str());
		return 1;
	}

	int Keys()
	{
		std::vector<uint8_t> image;
		std::string error;
		const bool read = g_options.contains("dump") ? ue5export::ng::ReadDumpImage(g_options["dump"], image, error)
		                                             : ue5export::ng::ReadProcessImage(image, error);
		ue5export::ng::Keys keys;
		if (!read || !ue5export::ng::Capture(image, keys, error) || !ue5export::ng::Save(keys, KeysFile(), error))
			return Fail(error);
		std::printf("keys saved: %s (%zu tables)\n", KeysFile().string().c_str(), keys.tables.size());
		return 0;
	}

	struct Indexer
	{
		std::ofstream out;
		std::map<std::string, std::pair<uint64_t, uint64_t>> byType; // extension -> files, bytes
		uint64_t archives = 0;
		std::vector<std::string> errors;

		void Walk(const Archive& archive, uint32_t node, const std::string& path)
		{
			const Archive::Node& n = archive.Nodes()[node];
			for (const uint32_t c : n.children)
			{
				const Archive::Node& child = archive.Nodes()[c];
				const std::string childPath = path + "/" + child.name;
				if (child.directory)
				{
					Walk(archive, c, childPath);
					continue;
				}
				const size_t dot = child.name.rfind('.');
				const std::string ext = dot == std::string::npos ? "" : child.name.substr(dot + 1);
				out << childPath << '\t' << child.size << '\t' << (child.resource ? "r" : "b") << '\n';
				auto& [files, bytes] = byType[ext];
				++files, bytes += child.resource ? child.DiskSize() : child.size;
				if (ext == "rpf")
				{
					Archive nested;
					std::string error;
					if (archive.OpenNested(c, nested, error))
						Open(nested, childPath);
					else
						errors.push_back(childPath + ": " + error);
				}
			}
		}

		void Open(const Archive& archive, const std::string& path)
		{
			++archives;
			if (!archive.Nodes().empty())
				Walk(archive, 0, path);
		}
	};

	// One file out of the archives, by its index path ("x64n.rpf/levels/.../ch1_09.rpf/ch1_09_terrain+hi.ytd").
	int Extract(const loader::convert::Decryptor& decrypt)
	{
		const fs::path game = g_options["game"];
		std::string path = g_options["file"], error;
		const size_t first = path.find(".rpf/");
		if (first == std::string::npos)
			return Fail("not a path inside an archive");
		const fs::path file = game / path.substr(0, first + 4);
		auto in = std::make_shared<std::ifstream>(file, std::ios::binary);
		std::error_code ec;
		Archive archive;
		if (!archive.Open(in, 0, fs::file_size(file, ec), file.filename().string(), &decrypt, error))
			return Fail(error);
		std::string rest = path.substr(first + 5);
		for (uint32_t node = 0;;)
		{
			const size_t slash = rest.find('/');
			const std::string name = rest.substr(0, slash);
			const auto& children = archive.Nodes()[node].children;
			const auto it = std::find_if(children.begin(), children.end(), [&](uint32_t c) { return archive.Nodes()[c].name == name; });
			if (it == children.end())
				return Fail("not found: " + name);
			const Archive::Node& n = archive.Nodes()[*it];
			if (slash == std::string::npos)
			{
				loader::convert::Bytes data;
				if (!archive.ReadFile(*it, data, error))
					return Fail(error);
				const fs::path out = fs::path(g_options["out"]) / "files" / name;
				fs::create_directories(out.parent_path(), ec);
				std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
				std::printf("%s: %zu bytes\n", out.string().c_str(), data.size());
				return 0;
			}
			rest = rest.substr(slash + 1);
			if (n.directory)
				node = *it;
			else
			{
				Archive nested;
				if (!archive.OpenNested(*it, nested, error))
					return Fail(error);
				archive = std::move(nested);
				node = 0;
			}
		}
	}

	int Index(const loader::convert::Decryptor& decrypt)
	{
		const fs::path game = g_options["game"];
		std::string error;
		Indexer indexer;
		indexer.out.open(fs::path(g_options["out"]) / "index.tsv", std::ios::binary);
		const auto start = std::chrono::steady_clock::now();
		std::error_code ec;
		for (auto it = fs::recursive_directory_iterator(game, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			if (it->is_directory() && it->path().filename() == "ModLoader")
			{
				it.disable_recursion_pending();
				continue;
			}
			if (it->path().extension() != ".rpf")
				continue;
			const auto in = std::make_shared<std::ifstream>(it->path(), std::ios::binary);
			Archive archive;
			const std::string rel = fs::relative(it->path(), game).generic_string();
			if (archive.Open(in, 0, it->file_size(), it->path().filename().string(), &decrypt, error))
				indexer.Open(archive, rel);
			else
				indexer.errors.push_back(rel + ": " + error);
		}
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("%llu archives in %.1f s\n", static_cast<unsigned long long>(indexer.archives), seconds);
		for (const auto& [ext, count] : indexer.byType)
			std::printf("  %-8s %8llu files %10.1f MB\n", ext.c_str(), static_cast<unsigned long long>(count.first), count.second / 1048576.0);
		for (const auto& e : indexer.errors)
			std::printf("error: %s\n", e.c_str());
		return 0;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
		return Fail("usage: ue5export keys|index --out <dir> [--game <dir>] [--dump <image>]");
	for (int i = 2; i + 1 < argc; i += 2)
		if (std::string(argv[i]).starts_with("--"))
			g_options[argv[i] + 2] = argv[i + 1];
	if (!g_options.contains("out"))
		return Fail("--out <dir> is needed");
	const std::string command = argv[1];
	if (command == "keys")
		return Keys();
	if (!g_options.contains("game"))
		return Fail("--game <dir> is needed");
	ue5export::ng::Keys keys;
	std::string error;
	if (!ue5export::ng::Load(KeysFile(), keys, error))
		return Fail(error);
	const loader::convert::Decryptor decrypt = [&keys](uint32_t key, uint8_t* data, uint32_t size) { ue5export::ng::Decrypt(keys, key, data, size); };
	SetDllDirectoryW(fs::path(g_options["game"]).c_str()); // (the game's zlib1.dll)
	if (command == "index")
		return Index(decrypt);
	if (command == "extract")
		return g_options.contains("file") ? Extract(decrypt) : Fail("--file <path> is needed");
	return Fail("unknown command " + command);
}
