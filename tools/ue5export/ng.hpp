#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The game's archive encryption (NG) outside the game (research/phase0.md §58): a 16-byte block cipher of 272 table
// lookups (tools/ng_lift.py), keyed by one of 101 keys. The keys and tables are the game's data: captured once from
// the running game (or a dump of it) into the export folder, never part of this project.
namespace ue5export::ng
{
	struct Keys
	{
		std::array<std::array<uint32_t, 68>, 101> words{}; // per key: 17 rounds x 4 words
		std::array<uint8_t, 272> tableOf{};                 // per word of the program: its table
		std::vector<std::array<uint32_t, 256>> tables;
	};

	// The game's image in memory, by RVA: the running process (GTA5_Enhanced.exe) or a dump (raw offset == RVA).
	bool ReadProcessImage(std::vector<uint8_t>& image, std::string& error);
	bool ReadDumpImage(const std::filesystem::path& file, std::vector<uint8_t>& image, std::string& error);

	// Finds the archive decryption in the image and reads its keys and tables.
	bool Capture(const std::vector<uint8_t>& image, Keys& out, std::string& error);

	bool Save(const Keys& keys, const std::filesystem::path& file, std::string& error);
	bool Load(const std::filesystem::path& file, Keys& keys, std::string& error);

	// In place, whole 16-byte blocks (the rest is left as it is, like the game does).
	void Decrypt(const Keys& keys, uint32_t keyIndex, uint8_t* data, uint32_t size);
}
