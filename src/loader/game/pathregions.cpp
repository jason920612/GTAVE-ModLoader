#include "pathregions.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstring>
#include <tuple>

#include "../config.hpp"
#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::game::pathregions
{
	namespace
	{
#include "pathregions_data.inc"

		uint8_t* g_block = nullptr;

		// Executable memory within 2 GB of `target` (the patches jump to it with rel32).
		uint8_t* AllocateNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO info{};
			GetSystemInfo(&info);
			const uintptr_t granularity = info.dwAllocationGranularity;
			const uintptr_t start = target & ~(granularity - 1);
			for (uintptr_t step = granularity; step < 0x7FF00000; step += granularity)
				for (const uintptr_t at : {start - step, start + step})
					if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
						return static_cast<uint8_t*>(p);
			return nullptr;
		}

		void Write(uintptr_t at, const uint8_t* bytes, size_t size)
		{
			DWORD old;
			VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &old);
			std::memcpy(reinterpret_cast<void*>(at), bytes, size);
			VirtualProtect(reinterpret_cast<void*>(at), size, old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
		}

		// The path store (its streaming module: +8 first slot, +0x10 slot count), and a path query that loads its address
		// (lea rcx,[rip+...]). The store is constructed after the loader is in (no vtable yet), so that code checks the build.
		constexpr uintptr_t kThePaths = 0x4426E10, kThePathsRef = 0xF02E77;
		// Streaming registration, the test whether the file's slot already exists: mov eax,[rsi+r10*8+4] (flags);
		// mov edx,eax; and edx,2000000h; or edx,ebp (handle); je <new slot>. r10 is the slot, ebp its file handle.
		constexpr uintptr_t kSlotExists = 0xCE356B;
		constexpr uint8_t kSlotExistsBytes[] = {0x42, 0x8B, 0x44, 0xD6, 0x04, 0x89, 0xC2, 0x81, 0xE2, 0x00, 0x00, 0x00, 0x02, 0x09, 0xEA, 0x74, 0x54};

		// The vehicle AI's choice of the next node (0x161B940) follows each link of the node it is at into the link's
		// region (link: u16 area, u16 node): a region not loaded, the link is passed over (0x161BE50); a node number past
		// the region's count (0x161BF6D) or a node that says another area (0x161BF7F) it takes on with no node and reads
		// later (+0x20 at 0x161C854: the game exits). Those links are passed over the same way; what they were is kept for
		// the log (research/phase0.md §61.27).
		constexpr uintptr_t kBadIndex = 0x161BF6D, kBadArea = 0x161BF7F, kSkipLink = 0x161BE50;
		constexpr uint8_t kBadIndexBytes[] = {0x31, 0xD2, 0x48, 0x89, 0x54, 0x24, 0x60, 0x31, 0xD2}; // xor edx,edx; mov [rsp+60h],rdx; xor edx,edx
		constexpr uint8_t kBadAreaBytes[] = {0x31, 0xD2, 0x48, 0x89, 0x54, 0x24, 0x60, 0x31, 0xD2};
		struct LinkGuard
		{
			volatile long count;
			uint32_t link;        // area | node << 16
			uint32_t regionCount; // the nodes the loaded region has
			uint32_t nodeArea;    // what the node there says (0xFFFFFFFF: past the count)
		};
		LinkGuard* g_guard = nullptr;

		// The route search (0xF4FFE0: the vehicles' routes, the GPS): its expansion of a node follows each link into the
		// link's region - the area checked (at most 1023, a region loaded, with nodes) but not the node: one past the
		// region's count, or one of another area, is taken on as a node there and put into the search's open lists through
		// its first 16 bytes; when it is not one, the game writes through what lies there (0xF51346: crashed after a
		// crossing that did not come, the band's regions holding the other land's nodes while they waited to be unloaded).
		// Its walk back along the route reads the links' nodes the same way (0xF517EC). Both pass over such links now, as
		// the other path through the search does (0xF5100E: the count and the area checked).
		constexpr uintptr_t kSearchLink = 0xF507A9, kSearchSkip = 0xF50790, kBackLink = 0xF517EC, kBackSkip = 0xF517C0;
		constexpr uint8_t kSearchLinkBytes[] = {0x48, 0xC1, 0xE9, 0x10, 0x48, 0x8D, 0x04, 0x89};             // shr rcx,10h; lea rax,[rcx+rcx*4]
		constexpr uint8_t kSearchSkipBytes[] = {0x44, 0x89, 0xF6, 0x8B, 0x84, 0x24, 0xB8, 0x00, 0x00, 0x00}; // mov esi,r14d; mov eax,[rsp+0B8h]
		constexpr uint8_t kBackLinkBytes[] = {0x48, 0xC1, 0xEE, 0x10, 0x44, 0x8B, 0x69, 0x04};               // shr rsi,10h; mov r13d,[rcx+4]
		constexpr uint8_t kBackSkipBytes[] = {0x48, 0x83, 0xC1, 0x08, 0x48, 0xFF, 0xC8};                     // add rcx,8; dec rax
		struct RouteGuard
		{
			volatile long count;
			uint32_t link; // area | node << 16 (the last passed over)
		};
		RouteGuard* g_routeGuard = nullptr;
	}

	bool Install()
	{
		if (!config::Get().worldPathRegions || g_block)
			return g_block != nullptr;
		const auto module = pattern::Module::Main();
		// the build must be the one the stubs were made from: every byte they replace as expected
		for (const PathSite& s : kPathSites)
			if (std::memcmp(reinterpret_cast<const void*>(module.base + s.rva), s.original, s.length) != 0)
			{
				log::Warn("world path regions: code at {:#x} differs from the build the patches were made for; not patched", s.rva);
				return false;
			}
		for (const PathRewrite& w : kPathRewrites)
			if (std::memcmp(reinterpret_cast<const void*>(module.base + w.rva), w.original, w.length) != 0)
			{
				log::Warn("world path regions: code at {:#x} differs from the build the patches were made for; not patched", w.rva);
				return false;
			}

		const bool grid = config::Get().worldPathGrid;
		if (grid)
		{
			for (const PathRedirect& r : kPathGrid)
				if (std::memcmp(reinterpret_cast<const void*>(module.base + r.rva), r.original, r.length) != 0)
				{
					log::Warn("world path regions: code at {:#x} differs from the build the patches were made for; not patched", r.rva);
					return false;
				}
			for (const PathGridImmediate& m : kPathGridImmediates)
				if (m.rva && std::memcmp(reinterpret_cast<const void*>(module.base + m.rva), m.original, m.length) != 0)
				{
					log::Warn("world path regions: code at {:#x} differs from the build the patches were made for; not patched", m.rva);
					return false;
				}
		}

		// the grid's constants after the stubs, 16-byte aligned
		const size_t constants = (kPathStubs + sizeof(kPathStubBytes) + 15) & ~size_t(15);
		const size_t size = constants + sizeof(kPathGridConstants);
		g_block = AllocateNear(module.base, size);
		if (!g_block)
		{
			log::Error("world path regions: no memory within 2 GB of the game");
			return false;
		}
		// tables zeroed (every region at its own place), the stubs after them
		std::memcpy(g_block + kPathStubs, kPathStubBytes, sizeof(kPathStubBytes));
		for (const PathReloc& r : kPathRelocs)
		{
			const int64_t rel = static_cast<int64_t>(module.base + r.target) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(g_block) + r.end);
			const int32_t rel32 = static_cast<int32_t>(rel);
			std::memcpy(g_block + r.at, &rel32, 4);
		}
		FlushInstructionCache(GetCurrentProcess(), g_block, size);

		for (const PathSite& s : kPathSites)
		{
			uint8_t jump[32];
			std::memset(jump, 0x90, sizeof(jump));
			jump[0] = 0xE9;
			const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(g_block + s.stub) - static_cast<intptr_t>(module.base + s.rva + 5));
			std::memcpy(jump + 1, &rel, 4);
			Write(module.base + s.rva, jump, s.length);
		}
		for (const PathRewrite& w : kPathRewrites)
			Write(module.base + w.rva, w.replacement, w.length);
		log::Info("world path regions: {} reads of node/junction positions patched ({} stub bytes at {})", std::size(kPathSites),
		    sizeof(kPathStubBytes), static_cast<void*>(g_block));
		if (grid)
		{
			// 1024 m regions from -16384 (32 x 32 as before): the path code's grid constants read from scaled copies
			std::memcpy(g_block + constants, kPathGridConstants, sizeof(kPathGridConstants));
			for (const PathRedirect& r : kPathGrid)
			{
				uint8_t bytes[16];
				std::memcpy(bytes, r.original, r.length);
				const int32_t disp = static_cast<int32_t>(reinterpret_cast<intptr_t>(g_block + constants + r.constant * 16) -
				                                          static_cast<intptr_t>(module.base + r.rva + r.length));
				std::memcpy(bytes + r.dispOffset, &disp, 4);
				Write(module.base + r.rva, bytes, r.length);
			}
			size_t immediates = 0;
			for (const PathGridImmediate& m : kPathGridImmediates)
				if (m.rva)
				{
					uint8_t bytes[16];
					std::memcpy(bytes, m.original, m.length);
					std::memcpy(bytes + m.immOffset, &m.value, 4);
					Write(module.base + m.rva, bytes, m.length);
					++immediates;
				}
			log::Info("world path regions: region grid of 1024 m ({} constant reads moved, {} immediates)", std::size(kPathGrid), immediates);
			// A region within +-8192 m keeps absolute positions (the game's own network, as it was); one outside holds
			// them relative to its south-west corner (0..4095 units of 0.25 m). worldgen writes regions the same way.
			for (int area = 0; area < 1024; ++area)
			{
				const int cx = area % 32, cy = area / 32;
				if (cx >= 8 && cx < 24 && cy >= 8 && cy < 24)
					continue;
				SetOrigin(static_cast<uint16_t>(area), (cx * 1024 - 16384) * 4, (cy * 1024 - 16384) * 4);
			}
		}
		return true;
	}

	bool Installed()
	{
		return g_block != nullptr;
	}

	bool AllowNewRegionFiles()
	{
		const auto module = pattern::Module::Main();
		const uintptr_t site = module.base + kSlotExists, paths = module.base + kThePaths;
		const auto* ref = reinterpret_cast<const uint8_t*>(module.base + kThePathsRef);
		if (std::memcmp(reinterpret_cast<const void*>(site), kSlotExistsBytes, sizeof(kSlotExistsBytes)) != 0 || ref[0] != 0x48 ||
		    ref[1] != 0x8D || ref[2] != 0x0D || pattern::Rip(module.base + kThePathsRef + 3) != paths)
		{
			log::Warn("world path regions: streaming registration differs from the build this was made for; road regions "
			          "the game has no file for cannot be added");
			return false;
		}
		const uintptr_t afterTest = site + sizeof(kSlotExistsBytes), newSlot = afterTest + kSlotExistsBytes[16];
		uint8_t* stub = AllocateNear(module.base, 64);
		if (!stub)
		{
			log::Warn("world path regions: no memory within 2 GB of the game; road regions the game has no file for cannot be added");
			return false;
		}
		uint8_t* at = stub;
		const auto bytes = [&](std::initializer_list<uint8_t> list) {
			for (const uint8_t b : list)
				*at++ = b;
		};
		const auto rel32 = [&](uintptr_t target) { // the instruction's last operand
			const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(at + 4));
			std::memcpy(at, &rel, 4);
			at += 4;
		};
		bytes({0x42, 0x8B, 0x44, 0xD6, 0x04}); // mov eax,[rsi+r10*8+4]
		bytes({0x85, 0xED, 0x75, 0x1D});       // test ebp,ebp; jnz original
		bytes({0x3D, 0x00, 0x00, 0x00, 0x02}); // cmp eax,2000000h (a placeholder, not loaded or requested)
		bytes({0x75, 0x16});                   // jne original
		bytes({0x44, 0x89, 0xD2});             // mov edx,r10d
		bytes({0x2B, 0x15});                   // sub edx,[the path store's first slot]
		rel32(paths + 8);
		bytes({0x3B, 0x15}); // cmp edx,[its slot count]
		rel32(paths + 0x10);
		bytes({0x73, 0x05}); // jae original
		bytes({0xE9});       // jmp <new slot>
		rel32(newSlot);
		// original:
		bytes({0x89, 0xC2, 0x81, 0xE2, 0x00, 0x00, 0x00, 0x02, 0x09, 0xEA}); // mov edx,eax; and edx,2000000h; or edx,ebp
		bytes({0x0F, 0x84});                                                 // je <new slot>
		rel32(newSlot);
		bytes({0xE9}); // jmp <after the test>
		rel32(afterTest);
		FlushInstructionCache(GetCurrentProcess(), stub, 64);

		uint8_t jump[sizeof(kSlotExistsBytes)];
		std::memset(jump, 0x90, sizeof(jump));
		jump[0] = 0xE9;
		const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - static_cast<intptr_t>(site + 5));
		std::memcpy(jump + 1, &rel, 4);
		Write(site, jump, sizeof(jump));
		log::Info("world path regions: road regions the game has no file for can be added (+{:#x})", kSlotExists);
		return true;
	}

	bool GuardNextNodeLinks()
	{
		const auto module = pattern::Module::Main();
		const uintptr_t paths = module.base + kThePaths;
		const auto* ref = reinterpret_cast<const uint8_t*>(module.base + kThePathsRef);
		if (std::memcmp(reinterpret_cast<const void*>(module.base + kBadIndex), kBadIndexBytes, sizeof(kBadIndexBytes)) != 0 ||
		    std::memcmp(reinterpret_cast<const void*>(module.base + kBadArea), kBadAreaBytes, sizeof(kBadAreaBytes)) != 0 || ref[0] != 0x48 ||
		    ref[1] != 0x8D || ref[2] != 0x0D || pattern::Rip(module.base + kThePathsRef + 3) != paths)
		{
			log::Warn("world path regions: the vehicle AI's next-node choice differs from the build this was made for; links to "
			          "nodes that are not there are left as they are");
			return false;
		}
		uint8_t* block = AllocateNear(module.base, 256);
		if (!block)
			return false;
		g_guard = reinterpret_cast<LinkGuard*>(block + 192);
		uint8_t* at = block;
		const auto bytes = [&](std::initializer_list<uint8_t> list) {
			for (const uint8_t b : list)
				*at++ = b;
		};
		const auto rel32 = [&](uintptr_t target, int after = 0) { // (rip-relative: from the instruction's end, `after` bytes on)
			const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(at + 4 + after));
			std::memcpy(at, &rel, 4);
			at += 4;
		};
		const auto guard = reinterpret_cast<uintptr_t>(g_guard);
		const auto stub = [&](bool badArea) {
			uint8_t* start = at;
			bytes({0xF0, 0xFF, 0x05}); // lock inc dword [count]
			rel32(guard + offsetof(LinkGuard, count));
			bytes({0x89, 0x05}); // mov [link],eax
			rel32(guard + offsetof(LinkGuard, link));
			bytes({0x8B, 0x51, 0x18, 0x89, 0x15}); // mov edx,[rcx+18h] (the region's node count); mov [regionCount],edx
			rel32(guard + offsetof(LinkGuard, regionCount));
			if (badArea)
			{
				bytes({0x89, 0x1D}); // mov [nodeArea],ebx
				rel32(guard + offsetof(LinkGuard, nodeArea));
				bytes({0x44, 0x89, 0xCB, 0x4D, 0x89, 0xD1, 0x44, 0x8B, 0x54, 0x24, 0x68}); // mov ebx,r9d; mov r9,r10; mov r10d,[rsp+68h]
			}
			else
			{
				bytes({0xC7, 0x05}); // mov dword [nodeArea],-1
				rel32(guard + offsetof(LinkGuard, nodeArea), 4);
				bytes({0xFF, 0xFF, 0xFF, 0xFF});
			}
			bytes({0x48, 0x8D, 0x35}); // lea rsi,[ThePaths]
			rel32(paths);
			bytes({0x48, 0x8B, 0xBC, 0x24, 0xA0, 0x00, 0x00, 0x00}); // mov rdi,[rsp+0A0h]
			bytes({0xE9});                                           // jmp <the link passed over>
			rel32(module.base + kSkipLink);
			return start;
		};
		uint8_t* badIndex = stub(false);
		uint8_t* badArea = stub(true);
		FlushInstructionCache(GetCurrentProcess(), block, 256);
		for (const auto& [site, target] : {std::pair{module.base + kBadIndex, badIndex}, std::pair{module.base + kBadArea, badArea}})
		{
			uint8_t jump[sizeof(kBadIndexBytes)];
			std::memset(jump, 0x90, sizeof(jump));
			jump[0] = 0xE9;
			const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(target) - static_cast<intptr_t>(site + 5));
			std::memcpy(jump + 1, &rel, 4);
			Write(site, jump, sizeof(jump));
		}
		log::Info("world path regions: the vehicle AI passes over links to nodes that are not there (+{:#x}, +{:#x})", kBadIndex, kBadArea);
		return true;
	}

	bool GuardRouteSearchLinks()
	{
		const auto module = pattern::Module::Main();
		const uintptr_t paths = module.base + kThePaths;
		for (const auto& [rva, bytes, length] : {std::tuple{kSearchLink, kSearchLinkBytes, sizeof(kSearchLinkBytes)},
		         std::tuple{kSearchSkip, kSearchSkipBytes, sizeof(kSearchSkipBytes)}, std::tuple{kBackLink, kBackLinkBytes, sizeof(kBackLinkBytes)},
		         std::tuple{kBackSkip, kBackSkipBytes, sizeof(kBackSkipBytes)}})
			if (std::memcmp(reinterpret_cast<const void*>(module.base + rva), bytes, length) != 0)
			{
				log::Warn("world path regions: the route search's code at {:#x} differs from the build this was made for; its links to nodes that "
				          "are not there are left as they are",
				    rva);
				return false;
			}
		uint8_t* block = AllocateNear(module.base, 256);
		if (!block)
			return false;
		g_routeGuard = reinterpret_cast<RouteGuard*>(block + 224);
		const auto guard = reinterpret_cast<uintptr_t>(g_routeGuard);
		uint8_t* at = block;
		const auto bytes = [&](std::initializer_list<uint8_t> list) {
			for (const uint8_t b : list)
				*at++ = b;
		};
		const auto rel32 = [&](uintptr_t target, int after = 0) { // (rip-relative: from the instruction's end, `after` bytes on)
			const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(at + 4 + after));
			std::memcpy(at, &rel, 4);
			at += 4;
		};
		// (a jump within the stub, its target patched once known)
		const auto forward = [&](std::initializer_list<uint8_t> opcode) {
			bytes(opcode);
			uint8_t* hole = at;
			at += 4;
			return hole;
		};
		const auto land = [&](uint8_t* hole) {
			const int32_t rel = static_cast<int32_t>(at - (hole + 4));
			std::memcpy(hole, &rel, 4);
		};
		// the search's expansion of a node: the link in ecx, its area in r15, the area's nodes in rsi
		uint8_t* search = at;
		bytes({0x50, 0x52});                   // push rax; push rdx
		bytes({0x89, 0xC8, 0xC1, 0xE8, 0x10}); // mov eax,ecx; shr eax,16 (the node)
		bytes({0x48, 0x8D, 0x15});             // lea rdx,[ThePaths]
		rel32(paths);
		bytes({0x4A, 0x8B, 0x94, 0xFA, 0x40, 0x16, 0x00, 0x00}); // mov rdx,[rdx+r15*8+1640h] (the area's region)
		bytes({0x3B, 0x42, 0x18});                               // cmp eax,[rdx+18h] (its node count)
		uint8_t* past = forward({0x0F, 0x83});                   // jae bad
		bytes({0x48, 0x8D, 0x04, 0x80});                         // lea rax,[rax+rax*4]
		bytes({0x66, 0x44, 0x3B, 0x7C, 0xC6, 0x10});             // cmp r15w,[rsi+rax*8+10h] (the area the node says)
		uint8_t* other = forward({0x0F, 0x85});                  // jne bad
		bytes({0x5A, 0x58});                                     // pop rdx; pop rax
		bytes({0x48, 0xC1, 0xE9, 0x10, 0x48, 0x8D, 0x04, 0x89}); // shr rcx,10h; lea rax,[rcx+rcx*4] (as the game has them)
		bytes({0xE9});
		rel32(module.base + kSearchLink + sizeof(kSearchLinkBytes));
		land(past), land(other);
		bytes({0x89, 0x0D}); // mov [link],ecx
		rel32(guard + offsetof(RouteGuard, link));
		bytes({0xF0, 0xFF, 0x05}); // lock inc dword [count]
		rel32(guard + offsetof(RouteGuard, count));
		bytes({0x5A, 0x58, 0xE9}); // pop rdx; pop rax; jmp <the link passed over, as one into a region not loaded>
		rel32(module.base + kSearchSkip);
		// the route's walk back: the link in esi, its region in rdx, the region's nodes in r12
		uint8_t* back = at;
		bytes({0x50});                                           // push rax
		bytes({0x89, 0xF0, 0xC1, 0xE8, 0x10});                   // mov eax,esi; shr eax,16 (the node)
		bytes({0x3B, 0x42, 0x18});                               // cmp eax,[rdx+18h]
		past = forward({0x0F, 0x83});                            // jae bad
		bytes({0x48, 0x8D, 0x04, 0x80});                         // lea rax,[rax+rax*4]
		bytes({0x66, 0x41, 0x3B, 0x74, 0xC4, 0x10});             // cmp si,[r12+rax*8+10h]
		other = forward({0x0F, 0x85});                           // jne bad
		bytes({0x58});                                           // pop rax
		bytes({0x48, 0xC1, 0xEE, 0x10, 0x44, 0x8B, 0x69, 0x04}); // shr rsi,10h; mov r13d,[rcx+4] (as the game has them)
		bytes({0xE9});
		rel32(module.base + kBackLink + sizeof(kBackLinkBytes));
		land(past), land(other);
		bytes({0x89, 0x35}); // mov [link],esi
		rel32(guard + offsetof(RouteGuard, link));
		bytes({0xF0, 0xFF, 0x05}); // lock inc dword [count]
		rel32(guard + offsetof(RouteGuard, count));
		bytes({0x58, 0xE9}); // pop rax; jmp <the next link>
		rel32(module.base + kBackSkip);
		FlushInstructionCache(GetCurrentProcess(), block, 256);
		for (const auto& [site, target] : {std::pair{module.base + kSearchLink, search}, std::pair{module.base + kBackLink, back}})
		{
			uint8_t jump[8];
			std::memset(jump, 0x90, sizeof(jump));
			jump[0] = 0xE9;
			const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(target) - static_cast<intptr_t>(site + 5));
			std::memcpy(jump + 1, &rel, 4);
			Write(site, jump, sizeof(jump));
		}
		log::Info("world path regions: the route search passes over links to nodes that are not there (+{:#x}, +{:#x})", kSearchLink, kBackLink);
		return true;
	}

	void ReportGuards()
	{
		static long routeReported = 0;
		if (g_routeGuard && g_routeGuard->count != routeReported)
		{
			const long now = g_routeGuard->count;
			const uint32_t link = g_routeGuard->link;
			log::Warn("world path regions: the route search passed over {} link(s) to nodes that are not there (the last: area {} node {})",
			    now - routeReported, link & 0xFFFF, link >> 16);
			routeReported = now;
		}
		static long reported = 0;
		if (!g_guard || g_guard->count == reported)
			return;
		const long now = g_guard->count;
		const uint32_t link = g_guard->link, nodeArea = g_guard->nodeArea;
		if (nodeArea == 0xFFFFFFFF)
			log::Warn("world path regions: {} link(s) to nodes that are not there passed over (the last: area {} node {}, the loaded region has {})",
			    now - reported, link & 0xFFFF, link >> 16, g_guard->regionCount);
		else
			log::Warn("world path regions: {} link(s) to nodes that are not there passed over (the last: area {} node {}, a node of area {} there; "
			          "the loaded region has {})",
			    now - reported, link & 0xFFFF, link >> 16, nodeArea, g_guard->regionCount);
		reported = now;
	}

	void SetOrigin(uint16_t area, int32_t x, int32_t y)
	{
		if (!g_block)
			return;
		auto* t16 = reinterpret_cast<int32_t*>(g_block + kPathTable16) + area * 4;
		auto* t8 = reinterpret_cast<int32_t*>(g_block + kPathTable8) + area * 2;
		t16[0] = x, t16[1] = y;
		t8[0] = x, t8[1] = y;
	}
}
