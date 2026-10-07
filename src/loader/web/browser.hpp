#pragma once
#include <Windows.h>

#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct D3D12_GPU_DESCRIPTOR_HANDLE;

// The in-game web browser (src/browser, CEF): the game's own web pages, shown full screen by the overlay.
// Pages live in ModLoader\web\<host>\ and ModLoader\mods\<mod>\web\<host>\ and are opened as https://<host>/... .
namespace loader::web
{
	// Shows the browser at `url` (starts CEF on first use, in the background). Any thread.
	void Open(const std::string& url);
	void Close();
	bool IsOpen();

	// Answers a page's game.call() (JSON text; nullptr = null). Any thread.
	void Respond(int64_t id, bool ok, const char* result);
	// Sends an event to the open page. Any thread.
	void Emit(const char* event, const char* json);

	// Game thread, inside a script context: replaces the game's own browser, runs page calls.
	void Tick();

	// ---- overlay (render thread) ----
	using DescriptorAlloc = bool (*)(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
	// Records the upload of a new page frame; before the ImGui draw data. `frame` = back buffer index.
	void Upload(ID3D12Device* device, ID3D12GraphicsCommandList* list, unsigned frame, unsigned frameCount, DescriptorAlloc alloc);
	// Draws the browser (toolbar and page) inside the ImGui frame.
	void Draw();
	// Window messages while the browser is open; true = the game must not see it.
	bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
}
