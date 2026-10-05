#include "effects.hpp"

#include <algorithm>

#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::convert
{
	namespace
	{
		// Effect lookup by name hash (GTA5_Enhanced.exe +0x1F12140): a hash map whose nodes form one list.
		// At +0x62: mov r8, [rip + list head]. Nodes: next @0, u32 hash @0x10, effect @0x18.
		constexpr const char* kLookup = "56 41 89 C9 41 C1 E9 10 89 C8 C1 E8 18 44 0F B6 C1 48 BA 25 23 22 84 E4 9C F2 CB";
		constexpr size_t kHeadOperand = 0x62 + 3;

		struct Node
		{
			Node* next;
			Node* prev;
			uint32_t hash;
			uint32_t pad;
			uint8_t** effect; // -> effect data
		};

		// Effect data: +0x38 parameter descriptors (12 bytes: u16 index, u8 type, u8 register, u32 old name,
		// u32 name), +0x50 parameter block size, +0x5C descriptor count.
		bool ReadEffect(const uint8_t* data, Effect& out)
		{
			const auto* descriptors = *reinterpret_cast<const uint8_t* const*>(data + 0x38);
			const uint8_t count = data[0x5C];
			out.block = *reinterpret_cast<const uint32_t*>(data + 0x50);
			std::vector<std::pair<uint16_t, uint32_t>> textures;
			for (uint8_t i = 0; i < count; ++i)
			{
				const uint8_t* d = descriptors + 12 * static_cast<size_t>(i);
				const uint16_t index = *reinterpret_cast<const uint16_t*>(d);
				const uint32_t oldName = *reinterpret_cast<const uint32_t*>(d + 4), name = *reinterpret_cast<const uint32_t*>(d + 8);
				if (d[2] == 0)
					textures.emplace_back(index, name);
				else if (d[2] == 3)
					out.constants[oldName] = name;
			}
			std::sort(textures.begin(), textures.end());
			for (const auto& t : textures)
				out.textures.push_back(t.second);
			return true;
		}

		bool Walk(Node* head, std::unordered_map<uint32_t, Effect>& out)
		{
			int guard = 0;
			for (Node* n = head->next; n && n != head && guard < 100000; n = n->next, ++guard)
			{
				if (!n->effect || !*n->effect)
					continue;
				Effect effect;
				ReadEffect(*n->effect, effect);
				out.emplace(n->hash, std::move(effect));
			}
			return guard < 100000;
		}
	}

	bool LoadEffects(std::unordered_map<uint32_t, Effect>& out)
	{
		const auto module = pattern::Module::Main();
		const auto lookup = pattern::Find(module.text, pattern::Pattern::Parse(kLookup));
		if (!lookup)
		{
			log::Error("convert: effect table not found");
			return false;
		}
		auto* head = *reinterpret_cast<Node**>(pattern::Rip(*lookup + kHeadOperand));
		if (!head || !Walk(head, out))
		{
			log::Error("convert: could not read the effect table");
			return false;
		}
		return !out.empty();
	}
}
