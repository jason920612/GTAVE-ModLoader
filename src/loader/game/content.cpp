#include "content.hpp"

#include <Windows.h>

#include <cstring>
#include <format>

#include <MinHook.h>

#include "../log.hpp"
#include "../pattern.hpp"
#include "sectors.hpp"

namespace loader::game::content
{
	namespace
	{
		// The manager (a pointer): +0x28 its packs (0x120 each), +0x30 how many (u16). A pack: +0x80 its name's hash,
		// +0x98 its change set groups (0x18 each: +0 the group's hash, +8 its change sets' hashes, +0x10 how many, u16),
		// +0xA0 how many groups (u16), +0x108 the change sets executed (group, change set: 8 bytes each), +0x110 how many.
		constexpr uintptr_t kManager = 0x29CAFC8;
		// 0xC26F00 (manager, pack, &group): each of the pack's change sets in the group executed (0xC272A0, flags 3);
		// 0xC2AFE0 (manager, pack's hash, group): each reverted (0xC274B0)
		constexpr uintptr_t kExecuteGroup = 0xC26F00, kRevertGroup = 0xC2AFE0;
		constexpr uint8_t kExecuteBytes[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x85, 0xD2};
		constexpr uint8_t kRevertBytes[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x58, 0x49, 0x89, 0xCC};
		constexpr size_t kPackSize = 0x120;

		using ExecuteGroupFn = void(__fastcall*)(void* manager, void* pack, const uint32_t* group);
		using RevertGroupFn = void(__fastcall*)(void* manager, uint32_t pack, uint32_t group);
		bool g_ok = false;
		ExecuteGroupFn g_executeGroup = nullptr; // (the game's, past the hook)
		constexpr uint32_t kGroupStartup = 0x5BF6578F, kGroupMapSp = 0x578F99E2; // GROUP_STARTUP, GROUP_MAP_SP

		// The game executing a pack's group (its start, story mode's, a session's restart): a dormant sector's road network
		// and maps left out (game/sectors.hpp).
		void __fastcall ExecuteGroupHook(void* manager, uint8_t* pack, const uint32_t* group)
		{
			const uint32_t hash = *reinterpret_cast<const uint32_t*>(pack + 0x80);
			if ((*group == kGroupStartup || *group == kGroupMapSp) && sectors::DormantHash(hash))
			{
				log::Info("content: pack {:08x}'s group {:08x} left out (a sector that waits)", hash, *group);
				return;
			}
			g_executeGroup(manager, pack, group);
		}

		uint8_t* Manager()
		{
			return *reinterpret_cast<uint8_t**>(pattern::Module::Main().base + kManager);
		}

		uint8_t* FindPack(uint32_t hash)
		{
			uint8_t* manager = Manager();
			if (!manager)
				return nullptr;
			auto* packs = *reinterpret_cast<uint8_t**>(manager + 0x28);
			const uint16_t count = *reinterpret_cast<uint16_t*>(manager + 0x30);
			for (uint16_t i = 0; i < count; ++i)
				if (*reinterpret_cast<uint32_t*>(packs + i * kPackSize + 0x80) == hash)
					return packs + i * kPackSize;
			return nullptr;
		}

		bool HasGroup(const uint8_t* pack, uint32_t group)
		{
			const auto* groups = *reinterpret_cast<uint8_t* const*>(pack + 0x98);
			const uint16_t count = *reinterpret_cast<const uint16_t*>(pack + 0xA0);
			for (uint16_t g = 0; g < count; ++g)
				if (*reinterpret_cast<const uint32_t*>(groups + g * 0x18) == group)
					return true;
			return false;
		}
	}

	bool Install()
	{
		const auto base = pattern::Module::Main().base;
		g_ok = std::memcmp(reinterpret_cast<const void*>(base + kExecuteGroup), kExecuteBytes, sizeof(kExecuteBytes)) == 0 &&
		       std::memcmp(reinterpret_cast<const void*>(base + kRevertGroup), kRevertBytes, sizeof(kRevertBytes)) == 0;
		if (!g_ok)
		{
			log::Warn("content: the game's DLC content manager differs from the build this was made for; change sets cannot be switched");
			return false;
		}
		const auto at = reinterpret_cast<void*>(base + kExecuteGroup);
		if (MH_CreateHook(at, reinterpret_cast<void*>(&ExecuteGroupHook), reinterpret_cast<void**>(&g_executeGroup)) != MH_OK || MH_EnableHook(at) != MH_OK)
		{
			log::Warn("content: the content manager's group execution could not be hooked; sectors that wait are loaded too");
			g_executeGroup = reinterpret_cast<ExecuteGroupFn>(at);
		}
		return true;
	}

	std::string Describe()
	{
		uint8_t* manager = g_ok ? Manager() : nullptr;
		if (!manager)
			return "no content manager";
		std::string out;
		auto* packs = *reinterpret_cast<uint8_t**>(manager + 0x28);
		const uint16_t count = *reinterpret_cast<uint16_t*>(manager + 0x30);
		for (uint16_t i = 0; i < count; ++i)
		{
			const uint8_t* pack = packs + i * kPackSize;
			out += std::format("pack {:08x} flags {:x}:", *reinterpret_cast<const uint32_t*>(pack + 0x80), *reinterpret_cast<const uint32_t*>(pack + 0x118));
			const auto* groups = *reinterpret_cast<uint8_t* const*>(pack + 0x98);
			for (uint16_t g = 0; g < *reinterpret_cast<const uint16_t*>(pack + 0xA0); ++g)
			{
				const uint8_t* group = groups + g * 0x18;
				out += std::format(" group {:08x} [", *reinterpret_cast<const uint32_t*>(group));
				const auto* sets = *reinterpret_cast<uint32_t* const*>(group + 8);
				for (uint16_t s = 0; s < *reinterpret_cast<const uint16_t*>(group + 0x10); ++s)
					out += std::format("{}{:08x}", s ? " " : "", sets[s]);
				out += "]";
			}
			out += " executed";
			const auto* executed = *reinterpret_cast<uint32_t* const*>(pack + 0x108);
			for (uint16_t e = 0; e < *reinterpret_cast<const uint16_t*>(pack + 0x110); ++e)
				out += std::format(" {:08x}/{:08x}", executed[2 * e], executed[2 * e + 1]);
			out += "\n";
		}
		return out;
	}

	bool Execute(uint32_t pack, uint32_t group)
	{
		uint8_t* p = g_ok ? FindPack(pack) : nullptr;
		if (!p || !HasGroup(p, group))
			return false;
		log::Info("content: pack {:08x}'s group {:08x} executed", pack, group);
		g_executeGroup(Manager(), p, &group);
		return true;
	}

	namespace
	{
		// a group's change sets (their hashes), or null
		const uint32_t* ChangeSets(const uint8_t* pack, uint32_t group, uint16_t& count)
		{
			const auto* groups = *reinterpret_cast<uint8_t* const*>(pack + 0x98);
			for (uint16_t g = 0; g < *reinterpret_cast<const uint16_t*>(pack + 0xA0); ++g)
				if (*reinterpret_cast<const uint32_t*>(groups + g * 0x18) == group)
				{
					count = *reinterpret_cast<const uint16_t*>(groups + g * 0x18 + 0x10);
					return *reinterpret_cast<uint32_t* const*>(groups + g * 0x18 + 8);
				}
			return nullptr;
		}
		// 0xC272A0 (manager, pack, &group, &change set, flags 3): one change set executed; 0xC274B0 (..., -1, 3): reverted
		constexpr uintptr_t kExecuteSet = 0xC272A0, kRevertSet = 0xC274B0;
		using ExecuteSetFn = void(__fastcall*)(void* manager, void* pack, const uint32_t* group, const uint32_t* set, uint32_t flags);
		using RevertSetFn = void(__fastcall*)(void* manager, void* pack, const uint32_t* group, const uint32_t* set, int32_t a, uint32_t flags);
	}

	int ExecuteAs(uint32_t pack, uint32_t group, uint32_t as)
	{
		uint8_t* p = g_ok ? FindPack(pack) : nullptr;
		uint16_t count = 0;
		const uint32_t* sets = p ? ChangeSets(p, group, count) : nullptr;
		if (!sets)
			return -1;
		const auto execute = reinterpret_cast<ExecuteSetFn>(pattern::Module::Main().base + kExecuteSet);
		for (uint16_t s = 0; s < count; ++s)
		{
			const uint32_t set = sets[s];
			execute(Manager(), p, &as, &set, 3);
		}
		log::Info("content: pack {:08x}'s group {:08x}: {} change set(s) executed as {:08x}", pack, group, count, as);
		return count;
	}

	int RevertAs(uint32_t pack, uint32_t group, uint32_t as)
	{
		uint8_t* p = g_ok ? FindPack(pack) : nullptr;
		uint16_t count = 0;
		const uint32_t* sets = p ? ChangeSets(p, group, count) : nullptr;
		if (!sets)
			return -1;
		const auto revert = reinterpret_cast<RevertSetFn>(pattern::Module::Main().base + kRevertSet);
		for (uint16_t s = 0; s < count; ++s)
		{
			const uint32_t set = sets[s];
			revert(Manager(), p, &as, &set, -1, 3);
		}
		log::Info("content: pack {:08x}'s group {:08x}: {} change set(s) reverted as {:08x}", pack, group, count, as);
		return count;
	}

	uint8_t* ManagerRecord()
	{
		return g_ok ? Manager() : nullptr;
	}

	uint8_t* PackRecord(uint32_t pack)
	{
		return g_ok ? FindPack(pack) : nullptr;
	}

	const uint32_t* GroupChangeSets(const uint8_t* pack, uint32_t group, uint16_t& count)
	{
		return pack ? ChangeSets(pack, group, count) : nullptr;
	}

	uint8_t* ChangeSetRecord(uint32_t changeSet)
	{
		// (the change sets' table: [+0x29CAFC0], beside the manager's pointer - 0xC27740 and 0xC272A0 read it)
		uint8_t* table = g_ok ? *reinterpret_cast<uint8_t**>(pattern::Module::Main().base + 0x29CAFC0) : nullptr;
		if (!table)
			return nullptr;
		auto* const* buckets = *reinterpret_cast<uint8_t** const*>(table + 0x30);
		const uint16_t count = *reinterpret_cast<const uint16_t*>(table + 0x38);
		if (!buckets || !count)
			return nullptr;
		for (uint8_t* r = buckets[changeSet % count]; r; r = *reinterpret_cast<uint8_t**>(r + 0xD0))
			if (*reinterpret_cast<const uint32_t*>(r) == changeSet)
				return r;
		return nullptr;
	}

	bool Revert(uint32_t pack, uint32_t group)
	{
		uint8_t* p = g_ok ? FindPack(pack) : nullptr;
		if (!p || !HasGroup(p, group))
			return false;
		log::Info("content: pack {:08x}'s group {:08x} reverted", pack, group);
		reinterpret_cast<RevertGroupFn>(pattern::Module::Main().base + kRevertGroup)(Manager(), pack, group);
		return true;
	}
}
