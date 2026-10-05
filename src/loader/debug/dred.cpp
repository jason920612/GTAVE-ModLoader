#include "dred.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <thread>

#include "../log.hpp"
#include "../paths.hpp"
#include "../ui/overlay.hpp"

namespace loader::debug
{
	namespace
	{
		std::string Narrow(const wchar_t* s)
		{
			if (!s)
				return {};
			std::string out;
			for (; *s; ++s)
				out += *s < 0x80 ? static_cast<char>(*s) : '?';
			return out;
		}

		std::string Name(const char* a, const wchar_t* w)
		{
			if (w && *w)
				return Narrow(w);
			return a ? a : "";
		}

		void LogBreadcrumbs(ID3D12DeviceRemovedExtendedData1* dred)
		{
			D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 out{};
			if (FAILED(dred->GetAutoBreadcrumbsOutput1(&out)))
			{
				log::Warn("dred: no breadcrumbs");
				return;
			}
			for (auto* node = out.pHeadAutoBreadcrumbNode; node; node = node->pNext)
			{
				const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
				if (done >= node->BreadcrumbCount)
					continue; // finished command list
				log::Warn("dred: list '{}' queue '{}': {} of {} ops done",
					Name(node->pCommandListDebugNameA, node->pCommandListDebugNameW),
					Name(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW), done, node->BreadcrumbCount);
				const UINT from = done > 8 ? done - 8 : 0;
				const UINT to = node->BreadcrumbCount < done + 8 ? node->BreadcrumbCount : done + 8;
				for (UINT i = from; i < to; ++i)
				{
					std::string context;
					for (UINT c = 0; c < node->BreadcrumbContextsCount; ++c)
						if (node->pBreadcrumbContexts[c].BreadcrumbIndex == i)
							context = Narrow(node->pBreadcrumbContexts[c].pContextString);
					log::Warn("dred:   {}{} op {} {}", i == done ? "-> " : "   ", i, static_cast<int>(node->pCommandHistory[i]), context);
				}
			}
		}

		void LogPageFault(ID3D12DeviceRemovedExtendedData1* dred)
		{
			D3D12_DRED_PAGE_FAULT_OUTPUT1 out{};
			if (FAILED(dred->GetPageFaultAllocationOutput1(&out)))
			{
				log::Warn("dred: no page fault data");
				return;
			}
			log::Warn("dred: page fault at GPU VA {:#x}", out.PageFaultVA);
			for (auto* n = out.pHeadExistingAllocationNode; n; n = n->pNext)
				log::Warn("dred:   existing allocation '{}' type {}", Name(n->ObjectNameA, n->ObjectNameW), static_cast<int>(n->AllocationType));
			for (auto* n = out.pHeadRecentFreedAllocationNode; n; n = n->pNext)
				log::Warn("dred:   recently freed '{}' type {}", Name(n->ObjectNameA, n->ObjectNameW), static_cast<int>(n->AllocationType));
		}

		// Without the debug layer, an invalid call only shows up as a failing Close() and then device removal:
		// hook Close and CopyBufferRegion (shared by every command list) and log the game code that called them.
		using CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
		using CopyBufferRegionFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
		CloseFn g_close = nullptr;
		CopyBufferRegionFn g_copyBufferRegion = nullptr;
		std::atomic<int> g_reports = 0;

		uintptr_t GameRva(void* address)
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			return reinterpret_cast<uintptr_t>(address) - base;
		}

		HRESULT STDMETHODCALLTYPE HookClose(ID3D12GraphicsCommandList* list)
		{
			const HRESULT hr = g_close(list);
			if (FAILED(hr) && g_reports++ < 20)
				log::Error("d3d12: Close() failed {:#x}, called from exe+{:#x}", static_cast<uint32_t>(hr), GameRva(_ReturnAddress()));
			return hr;
		}

		void STDMETHODCALLTYPE HookCopyBufferRegion(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset, UINT64 bytes)
		{
			if (dst && src)
			{
				const UINT64 dstSize = dst->GetDesc().Width, srcSize = src->GetDesc().Width;
				if ((dstOffset + bytes > dstSize || srcOffset + bytes > srcSize) && g_reports++ < 20)
					log::Error("d3d12: CopyBufferRegion out of range: dst {:#x}+{:#x} of {:#x}, src {:#x}+{:#x} of {:#x}, from exe+{:#x}",
						dstOffset, bytes, dstSize, srcOffset, bytes, srcSize, GameRva(_ReturnAddress()));
			}
			g_copyBufferRegion(list, dst, dstOffset, src, srcOffset, bytes);
		}

		void HookCommandLists(ID3D12Device* device)
		{
			ID3D12CommandAllocator* allocator = nullptr;
			ID3D12GraphicsCommandList* list = nullptr;
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
			    FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))))
			{
				log::Warn("dred: could not create a command list");
				return;
			}
			void** vtable = *reinterpret_cast<void***>(list);
			const bool ok = MH_CreateHook(vtable[9], reinterpret_cast<void*>(&HookClose), reinterpret_cast<void**>(&g_close)) == MH_OK &&
			                MH_CreateHook(vtable[15], reinterpret_cast<void*>(&HookCopyBufferRegion), reinterpret_cast<void**>(&g_copyBufferRegion)) == MH_OK &&
			                MH_EnableHook(vtable[9]) == MH_OK && MH_EnableHook(vtable[15]) == MH_OK;
			log::Info("dred: command list hooks {}", ok ? "installed" : "failed");
			list->Release();
			allocator->Release();
		}

		// Logs new debug-layer errors (only when the layer was enabled).
		void LogInfoQueue(ID3D12Device* device, UINT64& next)
		{
			ID3D12InfoQueue* queue = nullptr;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
				return;
			const UINT64 count = queue->GetNumStoredMessages();
			for (; next < count; ++next)
			{
				SIZE_T size = 0;
				if (FAILED(queue->GetMessage(next, nullptr, &size)) || !size)
					continue;
				std::vector<char> buffer(size);
				auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
				if (SUCCEEDED(queue->GetMessage(next, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
					log::Warn("d3d12 debug: [{}] {}", static_cast<int>(message->ID), message->pDescription);
			}
			queue->Release();
		}

		void Watch()
		{
			UINT64 nextMessage = 0;
			bool hooked = false;
			for (;;)
			{
				Sleep(500);
				ID3D12Device* device = ui::Device();
				if (!device)
					continue;
				if (!hooked)
				{
					HookCommandLists(device);
					hooked = true;
				}
				LogInfoQueue(device, nextMessage);
				const HRESULT reason = device->GetDeviceRemovedReason();
				if (reason == S_OK)
					continue;
				log::Error("dred: device removed, reason {:#x}", static_cast<uint32_t>(reason));
				ID3D12DeviceRemovedExtendedData1* dred = nullptr;
				if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred))))
				{
					LogBreadcrumbs(dred);
					LogPageFault(dred);
					dred->Release();
				}
				else
					log::Warn("dred: interface not available");
				return;
			}
		}
	}

	void StartDred()
	{
		const auto flag = paths::Get().root / L"debug_dred.txt";
		if (!std::filesystem::exists(flag))
			return;
		std::string options;
		std::getline(std::ifstream(flag), options);
		const bool layer = options.find("layer") != std::string::npos;
		std::thread([layer] {
			if (layer)
			{
				// Needs the Windows "Graphics Tools" feature: the game runs on the system D3D12 runtime, which is newer
				// than the Agility SDK it ships, so that SDK's layers do not match.
				ID3D12Debug* debugInterface = nullptr;
				const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface));
				if (SUCCEEDED(hr))
				{
					debugInterface->EnableDebugLayer();
					debugInterface->Release();
					log::Info("dred: debug layer enabled");
				}
				else
					log::Warn("dred: debug layer not available ({:#x})", static_cast<uint32_t>(hr));
			}
			ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
			if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
			{
				log::Warn("dred: settings interface not available");
				return;
			}
			settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings->Release();
			log::Info("dred: enabled");
			Watch();
		}).detach();
	}
}
