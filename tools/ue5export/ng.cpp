#include "ng.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <optional>

namespace ue5export::ng
{
	namespace
	{
		struct Op
		{
			uint8_t fromKey; // word = key word (1) or an earlier word (0)
			uint16_t base;
			uint8_t fromInput; // looked up by an input byte (1) or a byte of an earlier word (0)
			uint16_t source;
			uint8_t shift;
		};
		constexpr Op kProgram[272] = {
#include "ng_program.inc"
		};
		constexpr uint16_t kOut[4] = {259, 263, 267, 271};

		constexpr uint32_t kMagic = 0x474E3545; // 'E5NG'
		constexpr uint32_t kVersion = 1;
		constexpr uint32_t kKeySize = 0x140;    // the game's key: 16 bytes, then the 68 words
		constexpr uint32_t kKeyCount = 101;

		std::optional<size_t> Find(const std::vector<uint8_t>& image, const std::vector<int>& pattern, size_t from = 0, size_t to = SIZE_MAX)
		{
			to = std::min(to, image.size() - pattern.size());
			for (size_t i = from; i < to; ++i)
			{
				size_t k = 0;
				while (k < pattern.size() && (pattern[k] < 0 || image[i + k] == pattern[k]))
					++k;
				if (k == pattern.size())
					return i;
			}
			return std::nullopt;
		}

		int32_t Rel(const std::vector<uint8_t>& image, size_t at)
		{
			int32_t v;
			std::memcpy(&v, image.data() + at, 4);
			return v;
		}

		// rel32 of the instruction whose 4-byte displacement is at `at` and which ends right after it.
		size_t Target(const std::vector<uint8_t>& image, size_t at) { return at + 4 + Rel(image, at); }
	}

	bool ReadProcessImage(std::vector<uint8_t>& image, std::string& error)
	{
		DWORD pid = 0;
		if (const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE)
		{
			PROCESSENTRY32W entry{sizeof(entry)};
			for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry))
				if (_wcsicmp(entry.szExeFile, L"GTA5_Enhanced.exe") == 0)
					pid = entry.th32ProcessID;
			CloseHandle(snap);
		}
		if (!pid)
			return error = "the game is not running", false;
		uintptr_t base = 0;
		DWORD size = 0;
		if (const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid); snap != INVALID_HANDLE_VALUE)
		{
			MODULEENTRY32W module{sizeof(module)};
			if (Module32FirstW(snap, &module))
				base = reinterpret_cast<uintptr_t>(module.modBaseAddr), size = module.modBaseSize;
			CloseHandle(snap);
		}
		const HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
		if (!process || !base)
			return error = std::format("cannot read the game's memory (error {})", GetLastError()), false;
		image.assign(size, 0);
		for (DWORD off = 0; off < size; off += 0x1000)
		{
			SIZE_T read = 0;
			ReadProcessMemory(process, reinterpret_cast<void*>(base + off), image.data() + off, std::min<DWORD>(0x1000, size - off), &read);
		}
		CloseHandle(process);
		return true;
	}

	bool ReadDumpImage(const std::filesystem::path& file, std::vector<uint8_t>& image, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return error = "cannot open the dump", false;
		image.assign(std::istreambuf_iterator<char>(in), {});
		return true;
	}

	bool Capture(const std::vector<uint8_t>& image, Keys& out, std::string& error)
	{
		// The NG routine: push r14 / rsi / rdi / rbp / rbx; mov eax, 0x7FB0; call (a 0x7FB0-byte frame: the 101 keys
		// copied to the stack).
		const auto routine = Find(image, {0x41, 0x56, 0x56, 0x57, 0x55, 0x53, 0xB8, 0xB0, 0x7F, 0x00, 0x00, 0xE8});
		if (!routine)
			return error = "the archive decryption was not found", false;
		// Its first lea rdx, [rip+x] is the first key; the keys follow each other.
		const auto keysLea = Find(image, {0x48, 0x8D, 0x15}, *routine, *routine + 0x80);
		const auto nextLea = keysLea ? Find(image, {0x48, 0x8D, 0x15}, *keysLea + 7, *routine + 0x100) : std::nullopt;
		if (!keysLea || !nextLea || Target(image, *nextLea + 3) - Target(image, *keysLea + 3) != kKeySize)
			return error = "the keys were not found", false;
		const size_t keys = Target(image, *keysLea + 3);
		// mov r9, r14; call: the per-buffer loop, whose first call is the block function.
		const auto loopCall = Find(image, {0x4D, 0x89, 0xF1, 0xE8}, *routine, *routine + 0x1000);
		if (!loopCall)
			return error = "the decryption loop was not found", false;
		const size_t loop = Target(image, *loopCall + 4);
		const auto blockCall = Find(image, {0xE8}, loop, loop + 0x200);
		if (!blockCall)
			return error = "the block function was not found", false;
		const size_t block = Target(image, *blockCall + 1);
		// One table per word, in order: lea rdx, [rip+table]; mov ecx, [rdx+rcx*4].
		std::map<size_t, uint8_t> tableIndex;
		size_t at = block;
		for (size_t word = 0; word < 272; ++word)
		{
			const auto lea = Find(image, {0x48, 0x8D, 0x15, -1, -1, -1, -1, 0x8B, 0x0C, 0x8A}, at, block + 0x8000);
			if (!lea)
				return error = std::format("the block function's table {} was not found", word), false;
			const size_t table = Target(image, *lea + 3);
			if (table + 1024 > image.size())
				return error = "a table is outside the image", false;
			const auto [it, added] = tableIndex.emplace(table, static_cast<uint8_t>(tableIndex.size()));
			out.tableOf[word] = it->second;
			at = *lea + 10;
		}
		out.tables.assign(tableIndex.size(), {});
		for (const auto& [table, index] : tableIndex)
			std::memcpy(out.tables[index].data(), image.data() + table, 1024);
		for (uint32_t k = 0; k < kKeyCount; ++k)
			std::memcpy(out.words[k].data(), image.data() + keys + k * kKeySize + 0x10, sizeof(out.words[k]));
		return true;
	}

	bool Save(const Keys& keys, const std::filesystem::path& file, std::string& error)
	{
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		std::ofstream out(file, std::ios::binary);
		const uint32_t header[3] = {kMagic, kVersion, static_cast<uint32_t>(keys.tables.size())};
		out.write(reinterpret_cast<const char*>(header), sizeof(header));
		out.write(reinterpret_cast<const char*>(keys.words.data()), sizeof(keys.words));
		out.write(reinterpret_cast<const char*>(keys.tableOf.data()), sizeof(keys.tableOf));
		out.write(reinterpret_cast<const char*>(keys.tables.data()), keys.tables.size() * sizeof(keys.tables[0]));
		if (!out)
			return error = "cannot write the keys", false;
		return true;
	}

	bool Load(const std::filesystem::path& file, Keys& keys, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		uint32_t header[3] = {};
		if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kMagic || header[1] != kVersion)
			return error = std::format("no keys at {} (run: ue5export keys, with the game running)", file.string()), false;
		keys.tables.assign(header[2], {});
		in.read(reinterpret_cast<char*>(keys.words.data()), sizeof(keys.words));
		in.read(reinterpret_cast<char*>(keys.tableOf.data()), sizeof(keys.tableOf));
		in.read(reinterpret_cast<char*>(keys.tables.data()), keys.tables.size() * sizeof(keys.tables[0]));
		if (!in)
			return error = "the keys file is cut short", false;
		for (const uint8_t t : keys.tableOf)
			if (t >= keys.tables.size())
				return error = "the keys file is damaged", false;
		return true;
	}

	void Decrypt(const Keys& keys, uint32_t keyIndex, uint8_t* data, uint32_t size)
	{
		const auto& key = keys.words[keyIndex % kKeyCount];
		uint32_t w[272];
		for (uint32_t off = 0; off + 16 <= size; off += 16)
		{
			uint8_t* in = data + off;
			for (uint32_t i = 0; i < 272; ++i)
			{
				const Op& op = kProgram[i];
				const uint32_t byte = op.fromInput ? in[op.source] : (w[op.source] >> op.shift) & 0xFF;
				w[i] = (op.fromKey ? key[op.base] : w[op.base]) ^ keys.tables[keys.tableOf[i]][byte];
			}
			for (int j = 0; j < 4; ++j)
				std::memcpy(in + 4 * j, &w[kOut[j]], 4);
		}
	}
}
