#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// A PE image read from disk: a normal executable, or a dump of the running game (tools/dump.py: sections at their
// virtual addresses).
namespace relocate
{
	struct Section
	{
		std::string name;
		uint32_t rva = 0, size = 0;
		uint32_t fileOffset = 0, fileSize = 0;
		bool code = false;
	};

	struct RuntimeFunction
	{
		uint32_t begin, end;
	};

	class Image
	{
	public:
		bool Load(const std::filesystem::path& file, std::string& error);

		uint64_t Base() const { return base_; }
		const std::vector<Section>& Sections() const { return sections_; }
		const std::vector<RuntimeFunction>& Functions() const { return functions_; }

		// Bytes at an RVA (nullptr when not backed by the file).
		const uint8_t* At(uint32_t rva, size_t size = 1) const;
		const Section* SectionOf(uint32_t rva) const;
		bool IsCode(uint32_t rva) const;
		// Index into Functions() of the function containing `rva`, or -1.
		int FunctionOf(uint32_t rva) const;

	private:
		std::vector<uint8_t> file_;
		uint64_t base_ = 0;
		std::vector<Section> sections_;
		std::vector<RuntimeFunction> functions_;
	};
}
