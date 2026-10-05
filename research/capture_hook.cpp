// Research capture used with tools/rebuild_resource.py (not built). Paste into
// src/loader/game/dlcpacks.cpp's anonymous namespace and hook the game's inflate
// (signature "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC A8 00 00 00 B8 FE FF FF FF 48 85 C9 0F 84")
// with HookInflate; arm g_capStart/g_capEnd (GetTickCount64 window) once story mode runs.
// Records every inflate call's output (address, bytes); one zlib stream fills several pages.
		// RESEARCH (temporary): keep every page the streamer finishes inflating during a time window,
		// so a resource can be rebuilt from its page map before placement code rewrites it.
		using InflateFn = int (*)(void* stream, int flush);
		InflateFn g_origInflate = nullptr;
		std::mutex g_capMutex;
		struct CapturedPage
		{
			uintptr_t address;
			std::vector<uint8_t> data;
		};
		std::vector<CapturedPage> g_pages;
		size_t g_capBytes = 0;
		std::atomic<uint64_t> g_capStart = 0, g_capEnd = 0; // GetTickCount64 window
		std::atomic<bool> g_capWritten = false;

		void WriteCapture()
		{
			std::lock_guard lock(g_capMutex);
			FILE* data = _wfopen((paths::Get().root / L"capture.bin").c_str(), L"wb");
			FILE* index = _wfopen((paths::Get().root / L"capture.idx").c_str(), L"w");
			if (!data || !index)
				return;
			uint64_t off = 0;
			for (const auto& p : g_pages)
			{
				fwrite(p.data.data(), 1, p.data.size(), data);
				fprintf(index, "%llx %zx %llx\n", static_cast<unsigned long long>(p.address), p.data.size(), static_cast<unsigned long long>(off));
				off += p.data.size();
			}
			fclose(data);
			fclose(index);
			log::Info("research: wrote {} captured pages ({:#x} bytes)", g_pages.size(), off);
		}

		using OodleFn = int64_t (*)(const uint8_t*, int64_t, uint8_t*, int64_t, int, int, int, void*, int64_t, void*, void*, void*, int64_t, int);
		OodleFn g_origOodle = nullptr;
		int64_t HookOodle(const uint8_t* comp, int64_t compLen, uint8_t* raw, int64_t rawLen, int a5, int a6, int a7, void* a8, int64_t a9, void* a10,
		    void* a11, void* a12, int64_t a13, int a14)
		{
			const int64_t got = g_origOodle(comp, compLen, raw, rawLen, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
			static std::atomic<int> logged = 0;
			if (logged++ < 20)
				log::Info("research: oodle {:#x} -> {:#x} (raw {:#x})", compLen, got, rawLen);
			const uint64_t now = GetTickCount64();
			if (got > 0 && g_capStart && now >= g_capStart && now <= g_capEnd)
			{
				std::lock_guard lock(g_capMutex);
				if (g_capBytes + got <= (1500ull << 20))
				{
					g_pages.push_back({reinterpret_cast<uintptr_t>(raw), std::vector<uint8_t>(raw, raw + got)});
					g_capBytes += got;
				}
			}
			return got;
		}

		int HookInflate(void* stream, int flush)
		{
			if (!stream)
				return g_origInflate(stream, flush);
			auto* z = static_cast<uint8_t*>(stream);
			uint8_t* before = *reinterpret_cast<uint8_t**>(z + 0x10);
			const int ret = g_origInflate(stream, flush);
			uint8_t* after = *reinterpret_cast<uint8_t**>(z + 0x10);
			const uint64_t now = GetTickCount64();
			if (!g_capStart || now < g_capStart)
				return ret;
			if (now > g_capEnd)
			{
				if (!g_capWritten.exchange(true))
					WriteCapture();
				return ret;
			}
			// Each call may write to a different page: keep exactly what this call produced.
			if (after > before)
			{
				std::lock_guard lock(g_capMutex);
				const size_t n = after - before;
				if (g_capBytes + n <= (1500ull << 20))
				{
					g_pages.push_back({reinterpret_cast<uintptr_t>(before), std::vector<uint8_t>(before, after)});
					g_capBytes += n;
				}
			}
			return ret;
		}

