// Test host for mlbrowser.dll outside the game: loads it from a worker thread like ModLoader.dll does, opens a page,
// and writes the rendered frame to a BMP.  usage: browser_test <ModLoader dir> <url> <out.bmp>
#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../../src/browser/mlbrowser.h"

static void OnQuery(int64_t id, const char* request);
static MLB_RespondFn g_respond;

static void OnLog(int level, const char* text)
{
	std::printf("[%d] %s\n", level, text);
}
static void OnQuery(int64_t id, const char* request)
{
	std::printf("query %lld: %s\n", static_cast<long long>(id), request);
	g_respond(id, 1, "123456");
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 4)
		return 1;
	const std::filesystem::path root = argv[1];
	const std::wstring wurl = argv[2];
	const std::string url(wurl.begin(), wurl.end());
	const std::filesystem::path out = argv[3];
	int result = 1;
	std::thread([&] {
		const auto dir = root / L"browser";
		HMODULE m = LoadLibraryExW((dir / L"mlbrowser.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!m)
		{
			std::printf("LoadLibrary failed %lu\n", GetLastError());
			return;
		}
		auto init = reinterpret_cast<MLB_InitFn>(GetProcAddress(m, "MLB_Init"));
		auto open = reinterpret_cast<MLB_OpenFn>(GetProcAddress(m, "MLB_Open"));
		auto frame = reinterpret_cast<MLB_FrameFn>(GetProcAddress(m, "MLB_Frame"));
		g_respond = reinterpret_cast<MLB_RespondFn>(GetProcAddress(m, "MLB_Respond"));
		const std::wstring browserDir = dir.wstring(), cacheDir = (dir / L"cache").wstring(), web = (root / L"web").wstring();
		const wchar_t* roots[] = {web.c_str()};
		MLBConfig cfg{browserDir.c_str(), cacheDir.c_str(), roots, 1, "zh-TW", {&OnQuery, &OnLog}};
		std::printf("init...\n");
		if (!init(&cfg))
		{
			std::printf("init failed\n");
			return;
		}
		const int w = 1280, h = 720;
		open(url.c_str(), w, h);
		std::vector<uint8_t> pixels(size_t(w) * h * 4);
		uint32_t serial = 0;
		for (int i = 0; i < 50; ++i)
		{
			Sleep(100);
			frame(&serial, pixels.data(), w * 4, w, h);
		}
		std::printf("frames: serial %u\n", serial);
		BITMAPFILEHEADER fh{0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + pixels.size()), 0, 0,
		    sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
		BITMAPINFOHEADER ih{sizeof(ih), w, -h, 1, 32, BI_RGB};
		std::ofstream f(out, std::ios::binary);
		f.write(reinterpret_cast<char*>(&fh), sizeof(fh));
		f.write(reinterpret_cast<char*>(&ih), sizeof(ih));
		f.write(reinterpret_cast<char*>(pixels.data()), pixels.size());
		result = serial ? 0 : 2;
	}).join();
	std::printf("done %d\n", result);
	std::fflush(stdout);
	TerminateProcess(GetCurrentProcess(), result);
}
