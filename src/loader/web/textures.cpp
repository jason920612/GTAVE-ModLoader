#include "textures.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "../game/natives.hpp"
#include "../log.hpp"
#include "../paths.hpp"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace loader::web::textures
{
	namespace
	{
		constexpr uint64_t kRequestTxd = 0xDFA2EF8E04127DD5ULL;
		constexpr uint64_t kTxdLoaded = 0x0145F696AAAAD2E4ULL;
		constexpr uint64_t kTxdNotNeeded = 0xBE2CACCF5A8AA805ULL;
		constexpr uint64_t kTextureResolution = 0x35736EE65BD00C11ULL;
		constexpr uint64_t kDrawRect = 0x3A618A217E5154F0ULL;
		constexpr uint64_t kDrawSprite = 0xE7FFAE5EBF23D890ULL;
		constexpr uint64_t kDrawSpriteUv = 0x95812F9B26074726ULL; // DRAW_SPRITE_ARX_WITH_UV
		constexpr uint64_t kGfxDrawOrder = 0x61BB1D9B3A95D802ULL;
		constexpr int kPresentsBeforeCopy = 4; // frames between drawing and the frame that shows it

		struct Job
		{
			std::string dictionary, texture;
			std::function<void(const std::wstring&)> done;
		};
		std::mutex g_jobMutex;
		std::deque<Job> g_jobs;

		std::wstring Lower(const std::string& s)
		{
			std::wstring w(s.begin(), s.end());
			std::transform(w.begin(), w.end(), w.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
			return w;
		}
		std::filesystem::path CacheFile(const std::string& dictionary, const std::string& texture)
		{
			return paths::Get().root / L"browser" / L"cache" / L"textures" / Lower(dictionary) / (Lower(texture) + L".png");
		}
		bool SafeName(const std::string& s)
		{
			return !s.empty() && s.size() < 64 && std::all_of(s.begin(), s.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
		}

		// ---- back buffer read-back, shared with the render thread ---------------------------

		enum Shot : int
		{
			Idle,
			Drawing,  // the game draws the sprite; the render thread counts presents
			Copying,  // copy recorded, waiting for the GPU
			Ready     // pixels (BGRA) available
		};
		std::atomic<int> g_shot = Idle;
		int g_shotX = 0, g_shotY = 0, g_shotW = 0, g_shotH = 0;
		int g_presents = 0;
		uint64_t g_fenceValue = 0;
		ID3D12Resource* g_readback = nullptr;
		uint64_t g_readbackSize = 0;
		UINT g_readbackPitch = 0;
		DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
		std::vector<uint8_t> g_pixels;
		std::atomic<int> g_screenW = 0, g_screenH = 0;

		float Half(uint16_t h)
		{
			const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
			float v = exp == 0 ? std::ldexp(static_cast<float>(mant), -24) : exp == 31 ? INFINITY : std::ldexp(static_cast<float>(mant | 0x400), static_cast<int>(exp) - 25);
			return sign ? -v : v;
		}
		uint8_t ToSrgb8(float linear)
		{
			linear = std::clamp(linear, 0.0f, 1.0f);
			const float s = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
			return static_cast<uint8_t>(std::lround(s * 255));
		}

		// Back buffer texels -> BGRA8.
		bool Convert(const uint8_t* src, UINT pitch, int w, int h, std::vector<uint8_t>& out)
		{
			out.resize(size_t(w) * h * 4);
			for (int y = 0; y < h; ++y)
			{
				const uint8_t* row = src + size_t(y) * pitch;
				uint8_t* o = out.data() + size_t(y) * w * 4;
				for (int x = 0; x < w; ++x, o += 4)
					switch (g_format)
					{
					case DXGI_FORMAT_B8G8R8A8_UNORM:
					case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
					case DXGI_FORMAT_B8G8R8X8_UNORM:
						std::memcpy(o, row + x * 4, 4);
						break;
					case DXGI_FORMAT_R8G8B8A8_UNORM:
					case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
						o[0] = row[x * 4 + 2], o[1] = row[x * 4 + 1], o[2] = row[x * 4 + 0], o[3] = 255;
						break;
					case DXGI_FORMAT_R10G10B10A2_UNORM:
					{
						uint32_t v;
						std::memcpy(&v, row + x * 4, 4);
						o[2] = static_cast<uint8_t>((v & 0x3FF) >> 2), o[1] = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2), o[0] = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2), o[3] = 255;
						break;
					}
					case DXGI_FORMAT_R16G16B16A16_FLOAT:
					{
						uint16_t c[4];
						std::memcpy(c, row + x * 8, 8);
						o[2] = ToSrgb8(Half(c[0])), o[1] = ToSrgb8(Half(c[1])), o[0] = ToSrgb8(Half(c[2])), o[3] = 255;
						break;
					}
					default:
						return false;
					}
			}
			return true;
		}

		bool SavePng(const std::filesystem::path& file, int w, int h, const std::vector<uint8_t>& bgra)
		{
			std::error_code ec;
			std::filesystem::create_directories(file.parent_path(), ec);
			const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
			bool ok = false;
			IWICImagingFactory* factory = nullptr;
			IWICStream* stream = nullptr;
			IWICBitmapEncoder* encoder = nullptr;
			IWICBitmapFrameEncode* frame = nullptr;
			if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
			    SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
			    SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
			    SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)))
			{
				WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
				ok = SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
				     SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()))) && SUCCEEDED(frame->Commit()) &&
				     SUCCEEDED(encoder->Commit());
			}
			for (IUnknown* p : {static_cast<IUnknown*>(frame), static_cast<IUnknown*>(encoder), static_cast<IUnknown*>(stream), static_cast<IUnknown*>(factory)})
				if (p)
					p->Release();
			if (com)
				CoUninitialize();
			if (!ok)
				std::filesystem::remove(file, ec);
			return ok;
		}

		// ---- the job being captured (game thread) -------------------------------------------

		enum class Step
		{
			None,
			Loading,
			OnBlack,
			OnWhite
		};
		Step g_step = Step::None;
		Job g_job;
		int g_waitFrames = 0;
		int g_texW = 0, g_texH = 0;
		std::vector<uint8_t> g_onBlack;
		// Textures larger than 90% of the screen are captured in tiles (drawn 1:1 with texture coordinates).
		struct Tile
		{
			int x, y, w, h;
		};
		std::vector<Tile> g_tiles;
		size_t g_tile = 0;
		std::vector<uint8_t> g_image; // the whole texture, BGRA

		uint64_t Bits(float f)
		{
			return std::bit_cast<uint32_t>(f);
		}
		uint64_t Call(uint64_t hash, std::initializer_list<uint64_t> args, uint64_t* extra = nullptr)
		{
			game::natives::Invocation inv;
			inv.Begin(hash);
			for (auto a : args)
				inv.Push(a);
			game::natives::Call(inv);
			if (extra)
				std::memcpy(extra, inv.result, sizeof(inv.result));
			return inv.result[0];
		}

		void Finish(const std::wstring& file)
		{
			if (!g_job.dictionary.empty())
				Call(kTxdNotNeeded, {reinterpret_cast<uint64_t>(g_job.dictionary.c_str())});
			auto done = std::move(g_job.done);
			g_job = {};
			g_step = Step::None;
			g_shot = Idle;
			if (done)
				done(file);
		}

		// Draws the current tile of the texture 1:1 over a square of `shade` in the middle of the screen.
		void Draw(int shade)
		{
			const int sw = g_screenW, sh = g_screenH;
			const Tile& t = g_tiles[g_tile];
			const int w = t.w, h = t.h;
			const int x = (sw - w) / 2, y = (sh - h) / 2;
			g_shotX = x, g_shotY = y, g_shotW = w, g_shotH = h;
			const float cx = (x + w / 2.0f) / sw, cy = (y + h / 2.0f) / sh, nw = static_cast<float>(w) / sw, nh = static_cast<float>(h) / sh;
			Call(kGfxDrawOrder, {7});
			Call(kDrawRect, {Bits(cx), Bits(cy), Bits(nw + 8.0f / sw), Bits(nh + 8.0f / sh), static_cast<uint64_t>(shade), static_cast<uint64_t>(shade),
			                    static_cast<uint64_t>(shade), 255, 0});
			const uint64_t dict = reinterpret_cast<uint64_t>(g_job.dictionary.c_str()), tex = reinterpret_cast<uint64_t>(g_job.texture.c_str());
			if (g_tiles.size() == 1)
				Call(kDrawSprite, {dict, tex, Bits(cx), Bits(cy), Bits(nw), Bits(nh), Bits(0.0f), 255, 255, 255, 255, 0, 0});
			else
				Call(kDrawSpriteUv, {dict, tex, Bits(cx), Bits(cy), Bits(nw), Bits(nh), Bits(static_cast<float>(t.x) / g_texW), Bits(static_cast<float>(t.y) / g_texH),
				                        Bits(static_cast<float>(t.x + t.w) / g_texW), Bits(static_cast<float>(t.y + t.h) / g_texH), Bits(0.0f), 255, 255, 255, 255, 0});
		}
	}

	void Request(const std::string& dictionary, const std::string& texture, std::function<void(const std::wstring&)> done)
	{
		if (!SafeName(dictionary) || !SafeName(texture))
		{
			done(L"");
			return;
		}
		std::error_code ec;
		if (const auto file = CacheFile(dictionary, texture); std::filesystem::is_regular_file(file, ec))
		{
			done(file.wstring());
			return;
		}
		std::lock_guard lock(g_jobMutex);
		g_jobs.push_back({dictionary, texture, std::move(done)});
	}

	void Tick()
	{
		switch (g_step)
		{
		case Step::None:
		{
			std::lock_guard lock(g_jobMutex);
			if (g_jobs.empty() || !g_screenW)
				return;
			g_job = std::move(g_jobs.front());
			g_jobs.pop_front();
			g_step = Step::Loading;
			g_waitFrames = 0;
			Call(kRequestTxd, {reinterpret_cast<uint64_t>(g_job.dictionary.c_str()), 0});
			return;
		}
		case Step::Loading:
			if (!Call(kTxdLoaded, {reinterpret_cast<uint64_t>(g_job.dictionary.c_str())}))
			{
				if (++g_waitFrames > 300)
				{
					log::Warn("textures: dictionary {} did not load", g_job.dictionary);
					Finish(L"");
				}
				return;
			}
			{
				uint64_t size[4];
				Call(kTextureResolution, {reinterpret_cast<uint64_t>(g_job.dictionary.c_str()), reinterpret_cast<uint64_t>(g_job.texture.c_str())}, size);
				float fw, fh;
				std::memcpy(&fw, &size[0], 4);
				std::memcpy(&fh, &size[1], 4);
				g_texW = static_cast<int>(fw), g_texH = static_cast<int>(fh);
			}
			if (g_texW <= 0 || g_texH <= 0)
			{
				log::Warn("textures: no texture {} in {}", g_job.texture, g_job.dictionary);
				Finish(L"");
				return;
			}
			{
				const int tw = static_cast<int>(g_screenW * 0.9f), th = static_cast<int>(g_screenH * 0.9f);
				g_tiles.clear();
				for (int y = 0; y < g_texH; y += th)
					for (int x = 0; x < g_texW; x += tw)
						g_tiles.push_back({x, y, std::min(tw, g_texW - x), std::min(th, g_texH - y)});
				g_tile = 0;
				g_image.assign(size_t(g_texW) * g_texH * 4, 0);
			}
			g_step = Step::OnBlack;
			g_presents = 0;
			Draw(0);
			g_shot = Drawing;
			return;
		case Step::OnBlack:
		case Step::OnWhite:
			if (g_shot != Ready)
			{
				Draw(g_step == Step::OnBlack ? 0 : 255);
				return;
			}
			if (g_step == Step::OnBlack)
			{
				g_onBlack = std::move(g_pixels);
				g_step = Step::OnWhite;
				g_presents = 0;
				Draw(255);
				g_shot = Drawing;
				return;
			}
			{
				// Over black: c = a*t; over white: c = a*t + (1 - a)  ->  a = 1 - (white - black), t = black / a.
				const Tile& t = g_tiles[g_tile];
				for (int y = 0; y < t.h; ++y)
					for (int x = 0; x < t.w; ++x)
					{
						const size_t i = (size_t(y) * g_shotW + x) * 4;
						uint8_t* o = g_image.data() + (size_t(t.y + y) * g_texW + t.x + x) * 4;
						int a = 255;
						for (int c = 0; c < 3; ++c)
							a = std::min(a, 255 - (g_pixels[i + c] - g_onBlack[i + c]));
						a = std::clamp(a, 0, 255);
						for (int c = 0; c < 3; ++c)
							o[c] = a ? static_cast<uint8_t>(std::min(255, g_onBlack[i + c] * 255 / a)) : 0;
						o[3] = static_cast<uint8_t>(a);
					}
				if (++g_tile < g_tiles.size())
				{
					g_step = Step::OnBlack;
					g_presents = 0;
					Draw(0);
					g_shot = Drawing;
					return;
				}
				const auto file = CacheFile(g_job.dictionary, g_job.texture);
				const bool ok = SavePng(file, g_texW, g_texH, g_image);
				if (ok)
					log::Info("textures: {}/{} saved ({} x {}, {} tile(s))", g_job.dictionary, g_job.texture, g_texW, g_texH, g_tiles.size());
				else
					log::Warn("textures: could not save {}/{}", g_job.dictionary, g_job.texture);
				Finish(ok ? file.wstring() : L"");
			}
			return;
		}
	}

	void OnRender(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer, uint64_t signal, uint64_t completed)
	{
		const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
		g_screenW = static_cast<int>(desc.Width);
		g_screenH = static_cast<int>(desc.Height);
		g_format = desc.Format;
		const int shot = g_shot;
		if (shot == Copying && completed >= g_fenceValue)
		{
			void* data = nullptr;
			D3D12_RANGE range{0, static_cast<SIZE_T>(g_readbackSize)};
			if (SUCCEEDED(g_readback->Map(0, &range, &data)))
			{
				if (!Convert(static_cast<uint8_t*>(data), g_readbackPitch, g_shotW, g_shotH, g_pixels))
					g_pixels.assign(size_t(g_shotW) * g_shotH * 4, 0);
				D3D12_RANGE none{0, 0};
				g_readback->Unmap(0, &none);
			}
			g_shot = Ready;
			return;
		}
		if (shot != Drawing || ++g_presents < kPresentsBeforeCopy)
			return;

		const UINT bpp = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
		g_readbackPitch = (g_shotW * bpp + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		const uint64_t size = uint64_t(g_readbackPitch) * g_shotH;
		if (size > g_readbackSize)
		{
			if (g_readback)
				g_readback->Release();
			g_readback = nullptr;
			D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = size;
			buffer.Height = 1;
			buffer.DepthOrArraySize = 1;
			buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback))))
			{
				g_readbackSize = 0;
				g_pixels.assign(size_t(g_shotW) * g_shotH * 4, 0);
				g_shot = Ready;
				return;
			}
			g_readbackSize = size;
		}
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = backBuffer;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		list->ResourceBarrier(1, &barrier);
		D3D12_TEXTURE_COPY_LOCATION dst{g_readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
		dst.PlacedFootprint.Footprint = {desc.Format, static_cast<UINT>(g_shotW), static_cast<UINT>(g_shotH), 1, g_readbackPitch};
		D3D12_TEXTURE_COPY_LOCATION src{backBuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
		src.SubresourceIndex = 0;
		const D3D12_BOX box{static_cast<UINT>(g_shotX), static_cast<UINT>(g_shotY), 0, static_cast<UINT>(g_shotX + g_shotW), static_cast<UINT>(g_shotY + g_shotH), 1};
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		list->ResourceBarrier(1, &barrier);
		g_fenceValue = signal;
		g_shot = Copying;
	}
}
