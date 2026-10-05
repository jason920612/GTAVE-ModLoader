#pragma once
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "resource.hpp"

// RPF7 archives (research/phase0.md §11): reading unencrypted ("OPEN") and NG-encrypted ones, writing OPEN ones.
namespace loader::convert
{
	constexpr uint32_t kRpfMagic = 0x52504637;          // '7FPR'
	constexpr uint32_t kEncryptionOpen = 0x4E45504F;    // 'OPEN'
	constexpr uint32_t kEncryptionNg = 0x0FEFFFFF;

	// NG decryption in place; key index = (joaat(file name) + size) % 101. The game's own routine does this,
	// for legacy archives too (both versions use the same keys).
	using Decryptor = std::function<void(uint32_t keyIndex, uint8_t* data, uint32_t size)>;

	class Archive
	{
	public:
		struct Node
		{
			std::string name;
			bool directory = false;
			std::vector<uint32_t> children;
			uint64_t offset = 0;      // from the start of this archive
			uint32_t stored = 0;      // bytes on disk (0 = not compressed, use size)
			uint32_t size = 0;        // uncompressed size (binary files)
			bool resource = false;
			uint32_t flags[2] = {};   // resources: virtual and physical flags
			bool encrypted = false;   // binary files of NG archives
			uint64_t DiskSize() const { return stored ? stored : size; }
		};

		// `in` must stay open while the archive is used. `name` is the archive's file name (for NG keys).
		bool Open(std::shared_ptr<std::ifstream> in, uint64_t offset, uint64_t size, const std::string& name,
		    const Decryptor* decrypt, std::string& error);

		const std::vector<Node>& Nodes() const { return nodes_; }
		bool Encrypted() const { return encrypted_; }

		// Plain contents of a file: decrypted and decompressed; resources keep their RSC7 header.
		bool ReadFile(uint32_t index, Bytes& out, std::string& error) const;
		// An archive stored inside this one.
		bool OpenNested(uint32_t index, Archive& out, std::string& error) const;

	private:
		std::shared_ptr<std::ifstream> in_;
		uint64_t offset_ = 0;
		const Decryptor* decrypt_ = nullptr;
		bool encrypted_ = false;
		std::vector<Node> nodes_;
	};

	// An archive to write: a tree of directories, files and nested archives.
	struct WriteNode
	{
		std::string name;
		enum class Kind { Directory, File, Resource, Archive } kind = Kind::Directory;
		std::vector<WriteNode> children; // directories and nested archives
		Bytes data;                      // files; resources include their RSC7 header
	};

	// Builds an OPEN archive (files stored uncompressed, each on a 512-byte boundary).
	bool BuildArchive(const WriteNode& root, Bytes& out, std::string& error);
}
