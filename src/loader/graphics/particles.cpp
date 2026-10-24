#include "particles.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <MinHook.h>

#include "../log.hpp"

namespace loader::graphics::particles
{
	namespace
	{
		struct Digest
		{
			uint64_t low, high;
		};
		// The game's particle pixel shaders: the digest a compiled shader carries (its container's bytes 4..20).
		constexpr Digest kParticleShaders[] = {
#include "particle_shaders.inc"
		};

		// vtable slots (d3d12.h's C interface): ID3D12Device2, ID3D12PipelineLibrary1, ID3D12GraphicsCommandList4,
		// ID3D12CommandQueue.
		constexpr size_t kCreateGraphicsPipelineState = 10, kCreateRenderTargetView = 20, kCreatePipelineLibrary = 44, kCreatePipelineState = 47;
		constexpr size_t kLoadGraphicsPipeline = 9, kLoadPipeline = 13;
		constexpr size_t kClose = 9, kReset = 10, kDrawInstanced = 12, kDrawIndexedInstanced = 13, kSetPipelineState = 25, kResourceBarrier = 26,
		                 kOMSetRenderTargets = 46, kSetPredication = 55, kExecuteIndirect = 59, kBeginRenderPass = 68;
		constexpr size_t kExecuteCommandLists = 10;

		using CreateGraphicsPipelineStateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
		using CreatePipelineStateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
		using CreateRenderTargetViewFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
		using LoadGraphicsPipelineFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
		using LoadPipelineFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary1*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
		using CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
		using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
		using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
		using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
		using SetPipelineStateFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
		using SetPredicationFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, D3D12_PREDICATION_OP);
		using ExecuteIndirectFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
		using BeginRenderPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
		    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
		using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

		CreateGraphicsPipelineStateFn g_createGraphicsPipelineState = nullptr;
		CreatePipelineStateFn g_createPipelineState = nullptr;
		CreateRenderTargetViewFn g_createRenderTargetView = nullptr;
		LoadGraphicsPipelineFn g_loadGraphicsPipeline = nullptr;
		LoadPipelineFn g_loadPipeline = nullptr;
		CloseFn g_close = nullptr;
		ResetFn g_reset = nullptr;
		DrawInstancedFn g_drawInstanced = nullptr;
		DrawIndexedInstancedFn g_drawIndexedInstanced = nullptr;
		SetPipelineStateFn g_setPipelineState = nullptr;
		ResourceBarrierFn g_resourceBarrier = nullptr;
		OMSetRenderTargetsFn g_omSetRenderTargets = nullptr;
		SetPredicationFn g_setPredication = nullptr;
		ExecuteIndirectFn g_executeIndirect = nullptr;
		BeginRenderPassFn g_beginRenderPass = nullptr;
		ExecuteCommandListsFn g_executeCommandLists = nullptr;
		std::vector<void*> g_traceHooks; // (render passes and submissions: hooked while tracing only)

		// The pipelines made with a particle pixel shader: an open-addressing table of pipeline pointers (a pointer
		// reused for another pipeline is marked again when that one is made), read without a lock on every
		// SetPipelineState.
		constexpr size_t kTableBits = 14, kTableSize = size_t{1} << kTableBits;
		std::atomic<uintptr_t> g_keys[kTableSize];
		std::atomic<uint8_t> g_marks[kTableSize];
		std::mutex g_tableMutex;
		std::unordered_map<uintptr_t, uint32_t> g_shaderOf; // (every pipeline: its pixel shader's digest, for the trace)

		size_t Slot(uintptr_t key)
		{
			return static_cast<size_t>(((key >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kTableBits));
		}

		bool IsParticle(const void* pipeline)
		{
			const auto key = reinterpret_cast<uintptr_t>(pipeline);
			for (size_t i = Slot(key), n = 0; key && n < kTableSize; i = (i + 1) & (kTableSize - 1), ++n)
			{
				const uintptr_t k = g_keys[i].load(std::memory_order_acquire);
				if (k == key)
					return g_marks[i].load(std::memory_order_relaxed) != 0;
				if (k == 0)
					return false;
			}
			return false;
		}

		void Note(const void* pipeline, const D3D12_SHADER_BYTECODE* ps)
		{
			if (!pipeline)
				return;
			Digest digest{};
			if (ps && ps->pShaderBytecode && ps->BytecodeLength >= 20)
				std::memcpy(&digest, static_cast<const uint8_t*>(ps->pShaderBytecode) + 4, sizeof(digest));
			const bool particle = std::any_of(std::begin(kParticleShaders), std::end(kParticleShaders),
			    [&](const Digest& d) { return d.low == digest.low && d.high == digest.high; });
			const auto key = reinterpret_cast<uintptr_t>(pipeline);
			std::lock_guard lock(g_tableMutex);
			g_shaderOf[key] = static_cast<uint32_t>(digest.low);
			for (size_t i = Slot(key), n = 0; n < kTableSize; i = (i + 1) & (kTableSize - 1), ++n)
			{
				const uintptr_t k = g_keys[i].load(std::memory_order_relaxed);
				if (k == key)
				{
					g_marks[i].store(particle ? 1 : 0, std::memory_order_relaxed);
					return;
				}
				if (k == 0)
				{
					if (particle) // (only particles are kept)
					{
						g_marks[i].store(1, std::memory_order_relaxed);
						g_keys[i].store(key, std::memory_order_release);
					}
					return;
				}
			}
		}

		// The pixel shader in a pipeline state stream: subobjects aligned to a pointer, each a type and then its data
		// (at the data's own alignment). Null when a subobject is of a type not known here.
		const D3D12_SHADER_BYTECODE* StreamPixelShader(const D3D12_PIPELINE_STATE_STREAM_DESC* desc)
		{
			if (!desc || !desc->pPipelineStateSubobjectStream)
				return nullptr;
			const auto* at = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
			const uint8_t* const end = at + desc->SizeInBytes;
			while (at + sizeof(uint32_t) <= end)
			{
				D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
				std::memcpy(&type, at, sizeof(type));
				size_t size = 0, align = 0;
				const auto as = [&]<typename T>(T*) { size = sizeof(T), align = alignof(T); };
				switch (type)
				{
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: as(static_cast<ID3D12RootSignature**>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: as(static_cast<D3D12_SHADER_BYTECODE*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: as(static_cast<D3D12_STREAM_OUTPUT_DESC*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: as(static_cast<D3D12_BLEND_DESC*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: as(static_cast<UINT*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: as(static_cast<D3D12_RASTERIZER_DESC*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: as(static_cast<D3D12_DEPTH_STENCIL_DESC*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: as(static_cast<D3D12_INPUT_LAYOUT_DESC*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: as(static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: as(static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: as(static_cast<D3D12_RT_FORMAT_ARRAY*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: as(static_cast<DXGI_FORMAT*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: as(static_cast<DXGI_SAMPLE_DESC*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: as(static_cast<D3D12_CACHED_PIPELINE_STATE*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: as(static_cast<D3D12_PIPELINE_STATE_FLAGS*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: as(static_cast<D3D12_DEPTH_STENCIL_DESC1*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: as(static_cast<D3D12_VIEW_INSTANCING_DESC*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: as(static_cast<D3D12_DEPTH_STENCIL_DESC2*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: as(static_cast<D3D12_RASTERIZER_DESC1*>(nullptr)); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: as(static_cast<D3D12_RASTERIZER_DESC2*>(nullptr)); break;
				default: return nullptr;
				}
				const size_t offset = (sizeof(uint32_t) + align - 1) / align * align;
				if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS)
					return at + offset + size <= end ? reinterpret_cast<const D3D12_SHADER_BYTECODE*>(at + offset) : nullptr;
				at += (offset + size + sizeof(void*) - 1) / sizeof(void*) * sizeof(void*);
			}
			return nullptr;
		}

		// The render target views made (a descriptor's address to its resource), and those of the scene colour (read
		// without a lock on every OMSetRenderTargets).
		std::mutex g_viewMutex;
		std::unordered_map<SIZE_T, ID3D12Resource*> g_views;
		constexpr size_t kSceneViews = 8;
		std::atomic<SIZE_T> g_sceneViews[kSceneViews];
		std::atomic<ID3D12Resource*> g_scene = nullptr;

		bool IsSceneView(SIZE_T view)
		{
			for (const auto& v : g_sceneViews)
				if (view && v.load(std::memory_order_relaxed) == view)
					return true;
			return false;
		}

		// (g_viewMutex held)
		void SetSceneView(SIZE_T view, bool scene)
		{
			for (auto& v : g_sceneViews)
				if (v.load(std::memory_order_relaxed) == view)
				{
					if (!scene)
						v.store(0, std::memory_order_relaxed);
					return;
				}
			if (scene)
				for (auto& v : g_sceneViews)
					if (SIZE_T empty = 0; v.compare_exchange_strong(empty, view))
						return;
		}

		// The copies before and after the particles, and the predicate: a 64-bit word, 0 until the first copy before them
		// in the frame ran (each list's copy before them is skipped where it is not 0), set back to 0 after DLSS. All three
		// at rest in their states between uses: the copies NON_PIXEL_SHADER_RESOURCE, the predicate COMMON.
		std::mutex g_copyMutex; // (making them)
		ID3D12Resource* g_before = nullptr;
		ID3D12Resource* g_after = nullptr;
		ID3D12Resource* g_predicate = nullptr;
		std::atomic<uint32_t> g_runs = 0;  // runs of particles recorded since the last frame was taken
		std::atomic<uint32_t> g_frame = 0; // frames taken

		// A list's particles, as its own thread records it (in this frame: one left from an earlier frame is of a list
		// closed or reset on another thread since, and is let go).
		struct ListState
		{
			ID3D12GraphicsCommandList* list = nullptr;
			uint32_t frame = 0;
			bool particle = false; // the pipeline set last draws particles
			bool scene = false;    // the first render target bound last is the scene colour
			bool copied = false;   // its (predicated) copy before the particles is recorded
			bool run = false;      // particles drawn since the last copy after them
			ID3D12Resource* predicate = nullptr; // the game's own predication on the list, set again after ours
			UINT64 predicateOffset = 0;
			D3D12_PREDICATION_OP predicateOp = D3D12_PREDICATION_OP_EQUAL_ZERO;
			uint32_t age = 0;
		};
		thread_local ListState t_lists[4];
		thread_local uint32_t t_age = 0;
		thread_local bool t_busy = false; // (our own commands: the hooks pass them on)

		bool MakeTexture(ID3D12Resource* scene, ID3D12Resource*& texture)
		{
			D3D12_RESOURCE_DESC desc = scene->GetDesc();
			if (texture)
			{
				const D3D12_RESOURCE_DESC had = texture->GetDesc();
				if (had.Width == desc.Width && had.Height == desc.Height && had.Format == desc.Format)
					return true;
				// (frames still in flight may read it: released a moment later)
				std::thread([old = texture] {
					Sleep(3000);
					old->Release();
				}).detach();
				texture = nullptr;
			}
			ID3D12Device* device = nullptr;
			if (FAILED(scene->GetDevice(IID_PPV_ARGS(&device))))
				return false;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			desc.Alignment = 0;
			desc.MipLevels = 1;
			desc.DepthOrArraySize = 1;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;
			bool made = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			    nullptr, IID_PPV_ARGS(&texture)));
			if (made && !g_predicate)
			{
				// (zeroed as it is made: no copy before the particles has run)
				D3D12_RESOURCE_DESC buffer{};
				buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				buffer.Width = 256;
				buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
				buffer.SampleDesc.Count = 1;
				buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				made = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_predicate)));
			}
			device->Release();
			if (!made && texture)
			{
				texture->Release();
				texture = nullptr;
			}
			return made;
		}

		D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
		{
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
			return b;
		}

		// The predicate set to a value (the list's own, after what it recorded so far).
		void SetPredicate(ID3D12GraphicsCommandList* list, uint32_t value)
		{
			ID3D12GraphicsCommandList2* list2 = nullptr;
			if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2))))
				return;
			D3D12_RESOURCE_BARRIER b = Transition(g_predicate, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			list->ResourceBarrier(1, &b);
			const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER write{g_predicate->GetGPUVirtualAddress(), value};
			list2->WriteBufferImmediate(1, &write, nullptr);
			std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
			list->ResourceBarrier(1, &b);
			list2->Release();
		}

		// The scene colour copied (the scene bound to draw into: in RENDER_TARGET state); predicated: skipped where the
		// predicate is set already, and then set.
		void Copy(ID3D12GraphicsCommandList* list, ListState& s, ID3D12Resource* scene, ID3D12Resource* to, bool predicated)
		{
			t_busy = true;
			D3D12_RESOURCE_BARRIER barriers[2] = {Transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
			    Transition(to, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
			list->ResourceBarrier(2, barriers);
			if (predicated)
			{
				// (the predicate is promoted from COMMON to PREDICATION as it is read)
				D3D12_RESOURCE_BARRIER b = Transition(g_predicate, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PREDICATION);
				list->ResourceBarrier(1, &b);
				list->SetPredication(g_predicate, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
			}
			else if (s.predicate)
				list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO); // (not the game's own predicate)
			D3D12_TEXTURE_COPY_LOCATION dst{to, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, src{scene, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
			dst.SubresourceIndex = src.SubresourceIndex = 0;
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			if (!predicated && s.predicate)
				list->SetPredication(s.predicate, s.predicateOffset, s.predicateOp);
			if (predicated)
			{
				list->SetPredication(s.predicate, s.predicateOffset, s.predicateOp);
				D3D12_RESOURCE_BARRIER b = Transition(g_predicate, D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COMMON);
				list->ResourceBarrier(1, &b);
				SetPredicate(list, 1);
			}
			std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
			std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
			list->ResourceBarrier(2, barriers);
			t_busy = false;
		}

		void Record(const void* list, char kind, uint64_t value);

		// The first particle of a run about to be drawn: at the list's first, the copy before the particles (if it runs
		// first of the frame's).
		void StartRun(ID3D12GraphicsCommandList* list, ListState& s)
		{
			ID3D12Resource* scene = g_scene.load(std::memory_order_acquire);
			if (!scene || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
				return;
			{
				std::lock_guard lock(g_copyMutex);
				const bool first = !g_before;
				if (!MakeTexture(scene, g_before) || !MakeTexture(scene, g_after))
					return;
				if (first)
				{
					const D3D12_RESOURCE_DESC desc = scene->GetDesc();
					log::Info("ue5 particles: copies of the scene colour around its particles ({}x{}, format {})", desc.Width, desc.Height, static_cast<int>(desc.Format));
				}
			}
			if (!s.copied)
			{
				s.copied = true;
				Copy(list, s, scene, g_before, true);
				Record(list, 'C', 0);
			}
			s.run = true;
			++g_runs;
		}

		// After a run of particles (before what comes next touches the scene colour): the copy after them (the last to
		// run is the one left).
		void EndRun(ID3D12GraphicsCommandList* list, ListState& s)
		{
			s.run = false;
			ID3D12Resource* scene = g_scene.load(std::memory_order_acquire);
			if (!scene || !g_after)
				return;
			Copy(list, s, scene, g_after, false);
			Record(list, 'A', 0);
		}

		ListState& StateOf(ID3D12GraphicsCommandList* list)
		{
			const uint32_t frame = g_frame.load(std::memory_order_relaxed);
			ListState* oldest = &t_lists[0];
			for (ListState& s : t_lists)
			{
				if (s.list == list && s.frame == frame)
				{
					s.age = ++t_age;
					return s;
				}
				if (s.age < oldest->age)
					oldest = &s;
			}
			// (a list not seen on this thread in this frame: the least recent one's place - that one is not recorded
			// into again from here: it may be closed by now)
			for (ListState& s : t_lists)
				if (s.list == list)
					s = ListState{};
			*oldest = ListState{};
			oldest->list = list;
			oldest->frame = frame;
			oldest->age = ++t_age;
			return *oldest;
		}

		void Forget(ID3D12GraphicsCommandList* list)
		{
			for (ListState& s : t_lists)
				if (s.list == list)
					s = ListState{};
		}

		// Before a draw: a run of particles starts or ends.
		void BeforeDraw(ID3D12GraphicsCommandList* list)
		{
			if (t_busy)
				return;
			ListState& s = StateOf(list);
			const bool particles = s.particle && s.scene;
			if (particles && !s.run)
				StartRun(list, s);
			else if (!particles && s.run)
				EndRun(list, s);
		}

		// ---- research: one frame's calls (Trace) ----

		struct Event
		{
			LONGLONG time;
			DWORD thread;
			const void* list;
			char kind; // S pipeline, R render target, D draws, B render pass, X submitted, T frame, C / A copy before / after, Z reset
			uint64_t value;
		};
		std::atomic<int> g_tracing = 0; // 1 armed (from the next frame), 2 recording
		std::mutex g_traceMutex;
		std::vector<Event> g_events;
		std::wstring g_tracePath;
		thread_local const void* t_drawList = nullptr;
		thread_local uint64_t t_draws = 0;

		void Record(const void* list, char kind, uint64_t value)
		{
			if (g_tracing.load(std::memory_order_relaxed) != 2)
				return;
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			std::lock_guard lock(g_traceMutex);
			if (g_events.size() < 2'000'000)
				g_events.push_back({now.QuadPart, GetCurrentThreadId(), list, kind, value});
		}

		void CountDraw(const void* list)
		{
			if (g_tracing.load(std::memory_order_relaxed) != 2)
				return;
			if (t_drawList != list)
			{
				t_drawList = list;
				t_draws = 0;
			}
			if (++t_draws % 64 == 1)
				Record(list, 'D', t_draws);
		}

		void WriteTrace(std::vector<Event> events, std::wstring path)
		{
			for (void* target : g_traceHooks)
				MH_DisableHook(target);
			std::unordered_map<uintptr_t, uint32_t> shaders;
			{
				std::lock_guard lock(g_tableMutex);
				shaders = g_shaderOf;
			}
			std::unordered_map<SIZE_T, ID3D12Resource*> views;
			{
				std::lock_guard lock(g_viewMutex);
				views = g_views;
			}
			std::ofstream out(std::filesystem::path(path), std::ios::binary);
			const ID3D12Resource* scene = g_scene.load();
			out << std::format("scene {}\n", static_cast<const void*>(scene));
			for (const Event& e : events)
			{
				std::string what;
				switch (e.kind)
				{
				case 'S':
				{
					const auto found = shaders.find(static_cast<uintptr_t>(e.value));
					what = std::format("pipeline {:#x} ps {:08x}{}", e.value, found == shaders.end() ? 0u : found->second,
					    IsParticle(reinterpret_cast<const void*>(e.value)) ? " particle" : "");
					break;
				}
				case 'R':
				{
					const auto found = views.find(static_cast<SIZE_T>(e.value));
					ID3D12Resource* r = found == views.end() ? nullptr : found->second;
					what = std::format("target {}{}", static_cast<const void*>(r), r && r == scene ? " scene" : "");
					break;
				}
				case 'D': what = std::format("draws {}", e.value); break;
				case 'B': what = std::format("render pass, target {:#x}", e.value); break;
				case 'X': what = std::format("submitted, queue {:#x}", e.value); break;
				case 'T': what = "frame (after DLSS)"; break;
				case 'C': what = "copy before particles"; break;
				case 'A': what = "copy after particles"; break;
				case 'Z': what = "reset"; break;
				}
				out << std::format("{} {} {} {}\n", e.time, e.thread, e.list, what);
			}
			log::Info("ue5 particles: trace of {} calls written", events.size());
		}

		// ---- hooks ----

		HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
		{
			const HRESULT hr = g_createGraphicsPipelineState(device, desc, riid, out);
			if (SUCCEEDED(hr) && out && desc)
				Note(*out, &desc->PS);
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreatePipelineState(ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out)
		{
			const HRESULT hr = g_createPipelineState(device, desc, riid, out);
			if (SUCCEEDED(hr) && out)
				Note(*out, StreamPixelShader(desc));
			return hr;
		}

		HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(ID3D12PipelineLibrary* library, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
		{
			const HRESULT hr = g_loadGraphicsPipeline(library, name, desc, riid, out);
			if (SUCCEEDED(hr) && out && desc)
				Note(*out, &desc->PS);
			return hr;
		}

		HRESULT STDMETHODCALLTYPE LoadPipeline(ID3D12PipelineLibrary1* library, LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out)
		{
			const HRESULT hr = g_loadPipeline(library, name, desc, riid, out);
			if (SUCCEEDED(hr) && out)
				Note(*out, StreamPixelShader(desc));
			return hr;
		}

		void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Device* device, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE view)
		{
			{
				std::lock_guard lock(g_viewMutex);
				g_views[view.ptr] = resource;
				SetSceneView(view.ptr, resource && resource == g_scene.load(std::memory_order_relaxed));
			}
			g_createRenderTargetView(device, resource, desc, view);
		}

		HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* list)
		{
			if (!t_busy)
			{
				for (ListState& s : t_lists)
					if (s.list == list)
					{
						if (s.run)
							EndRun(list, s);
						s = ListState{};
					}
			}
			return g_close(list);
		}

		HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline)
		{
			Forget(list);
			Record(list, 'Z', 0);
			const HRESULT hr = g_reset(list, allocator, pipeline);
			if (!t_busy && pipeline && IsParticle(pipeline))
				StateOf(list).particle = true;
			return hr;
		}

		void STDMETHODCALLTYPE SetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
		{
			Record(list, 'S', reinterpret_cast<uintptr_t>(pipeline));
			if (!t_busy)
				StateOf(list).particle = IsParticle(pipeline);
			g_setPipelineState(list, pipeline);
		}

		void STDMETHODCALLTYPE OMSetRenderTargets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL range,
		    const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
		{
			const SIZE_T first = count && targets ? targets[0].ptr : 0;
			Record(list, 'R', first);
			if (!t_busy)
				StateOf(list).scene = IsSceneView(first);
			g_omSetRenderTargets(list, count, targets, range, depth);
		}

		void STDMETHODCALLTYPE ResourceBarrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
		{
			if (!t_busy)
			{
				// (the scene colour changing state: the copy after the particles first, while it is a render target still)
				ID3D12Resource* scene = g_scene.load(std::memory_order_relaxed);
				for (ListState& s : t_lists)
					if (s.list == list && s.run)
						for (UINT i = 0; i < count; ++i)
							if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? barriers[i].Transition.pResource == scene
							                                                                : barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
							{
								EndRun(list, s);
								break;
							}
			}
			g_resourceBarrier(list, count, barriers);
		}

		void STDMETHODCALLTYPE DrawInstanced(ID3D12GraphicsCommandList* list, UINT a, UINT b, UINT c, UINT d)
		{
			BeforeDraw(list);
			CountDraw(list);
			g_drawInstanced(list, a, b, c, d);
		}

		void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D12GraphicsCommandList* list, UINT a, UINT b, UINT c, INT d, UINT e)
		{
			BeforeDraw(list);
			CountDraw(list);
			g_drawIndexedInstanced(list, a, b, c, d, e);
		}

		void STDMETHODCALLTYPE SetPredication(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer, UINT64 offset, D3D12_PREDICATION_OP op)
		{
			if (!t_busy)
			{
				ListState& s = StateOf(list);
				s.predicate = buffer, s.predicateOffset = offset, s.predicateOp = op;
				static std::atomic_bool told = false;
				if (buffer && !told.exchange(true))
					log::Info("ue5 particles: the game predicates its own commands too (set again after the copies')");
			}
			g_setPredication(list, buffer, offset, op);
		}

		void STDMETHODCALLTYPE ExecuteIndirect(ID3D12GraphicsCommandList* list, ID3D12CommandSignature* signature, UINT most, ID3D12Resource* arguments,
		    UINT64 argumentsOffset, ID3D12Resource* counts, UINT64 countsOffset)
		{
			BeforeDraw(list);
			g_executeIndirect(list, signature, most, arguments, argumentsOffset, counts, countsOffset);
		}

		void STDMETHODCALLTYPE BeginRenderPass(ID3D12GraphicsCommandList4* list, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
		    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags)
		{
			Record(list, 'B', count && targets ? targets[0].cpuDescriptor.ptr : 0);
			g_beginRenderPass(list, count, targets, depth, flags);
		}

		void STDMETHODCALLTYPE ExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
		{
			for (UINT i = 0; i < count; ++i)
				Record(lists[i], 'X', reinterpret_cast<uintptr_t>(queue));
			g_executeCommandLists(queue, count, lists);
		}

		template <typename Fn>
		bool Hook(void** vtable, size_t slot, Fn detour, Fn& original, bool enable = true)
		{
			void* target = vtable[slot];
			if (MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) != MH_OK)
				return false;
			if (!enable)
			{
				g_traceHooks.push_back(target);
				return true;
			}
			return MH_EnableHook(target) == MH_OK;
		}

		void** Vtable(void* object)
		{
			return *static_cast<void***>(object);
		}

		// The hooks, from a probe device's objects (D3D12's own classes: the game's objects are of the same).
		void Probe()
		{
			static std::atomic_bool done = false;
			if (done.exchange(true))
				return;
			ID3D12Device* device = nullptr;
			ID3D12Device2* device2 = nullptr;
			ID3D12Device1* device1 = nullptr;
			ID3D12PipelineLibrary1* library = nullptr;
			ID3D12CommandAllocator* allocator = nullptr;
			ID3D12GraphicsCommandList* list = nullptr;
			ID3D12GraphicsCommandList4* list4 = nullptr;
			ID3D12CommandQueue* queue = nullptr;
			bool ok = false, libraryHooked = false;
			do
			{
				if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
					break;
				D3D12_COMMAND_QUEUE_DESC qd{};
				qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
				    FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))) ||
				    FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
					break;
				list->Close();
				void** d = Vtable(device);
				ok = Hook(d, kCreateGraphicsPipelineState, &CreateGraphicsPipelineState, g_createGraphicsPipelineState) &&
				     Hook(d, kCreateRenderTargetView, &CreateRenderTargetView, g_createRenderTargetView);
				if (ok && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2))))
					ok = Hook(Vtable(device2), kCreatePipelineState, &CreatePipelineState, g_createPipelineState);
				if (ok && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device1))) && SUCCEEDED(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library))))
					libraryHooked = Hook(Vtable(library), kLoadGraphicsPipeline, &LoadGraphicsPipeline, g_loadGraphicsPipeline) &&
					                Hook(Vtable(library), kLoadPipeline, &LoadPipeline, g_loadPipeline);
				void** l = Vtable(list);
				ok = ok && Hook(l, kClose, &Close, g_close) && Hook(l, kReset, &Reset, g_reset) && Hook(l, kSetPipelineState, &SetPipelineState, g_setPipelineState) &&
				     Hook(l, kResourceBarrier, &ResourceBarrier, g_resourceBarrier) && Hook(l, kOMSetRenderTargets, &OMSetRenderTargets, g_omSetRenderTargets) &&
				     Hook(l, kDrawInstanced, &DrawInstanced, g_drawInstanced) && Hook(l, kDrawIndexedInstanced, &DrawIndexedInstanced, g_drawIndexedInstanced) &&
				     Hook(l, kExecuteIndirect, &ExecuteIndirect, g_executeIndirect) && Hook(l, kSetPredication, &SetPredication, g_setPredication);
				// (for the trace: enabled while tracing)
				if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))))
					Hook(Vtable(list4), kBeginRenderPass, &BeginRenderPass, g_beginRenderPass, false);
				Hook(Vtable(queue), kExecuteCommandLists, &ExecuteCommandLists, g_executeCommandLists, false);
			} while (false);
			for (IUnknown* object : std::initializer_list<IUnknown*>{list4, list, allocator, queue, library, device1, device2, device})
				if (object)
					object->Release();
			if (ok)
				log::Info("ue5 particles: D3D12 hooked ({} particle shaders known{})", std::size(kParticleShaders), libraryHooked ? "" : "; no pipeline library");
			else
				log::Warn("ue5 particles: D3D12 could not be hooked; the game's particles are not shown over the renderer's picture");
		}

		// ntdll's DLL load notification (as dlss.cpp's): d3d12.dll loaded -> the hooks, from a thread (not under the
		// loader lock).
		struct UnicodeString
		{
			USHORT length, maximumLength;
			PWSTR buffer;
		};
		struct DllNotificationData
		{
			ULONG flags;
			const UnicodeString* fullName;
			const UnicodeString* baseName;
			PVOID base;
			ULONG size;
		};
		using DllNotificationFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID context);
		using LdrRegisterDllNotificationFn = LONG(NTAPI*)(ULONG flags, DllNotificationFn callback, PVOID context, PVOID* cookie);
		PVOID g_cookie = nullptr;

		VOID CALLBACK OnDll(ULONG reason, const DllNotificationData* data, PVOID)
		{
			if (reason != 1 || !data || !data->baseName || !data->baseName->buffer)
				return;
			const std::wstring_view base(data->baseName->buffer, data->baseName->length / sizeof(wchar_t));
			if (base.size() == 9 && _wcsnicmp(base.data(), L"d3d12.dll", 9) == 0)
				std::thread(&Probe).detach();
		}
	}

	void Start()
	{
		const auto reg = reinterpret_cast<LdrRegisterDllNotificationFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
		if (!reg || reg(0, &OnDll, nullptr, &g_cookie) != 0)
			log::Warn("ue5 particles: could not watch DLL loads");
		if (GetModuleHandleW(L"d3d12.dll"))
			std::thread(&Probe).detach();
	}

	void SetScene(ID3D12Resource* scene)
	{
		if (g_scene.load(std::memory_order_relaxed) == scene)
			return;
		std::lock_guard lock(g_viewMutex);
		g_scene.store(scene, std::memory_order_release);
		for (auto& v : g_sceneViews)
			v.store(0, std::memory_order_relaxed);
		for (const auto& [view, resource] : g_views)
			if (resource && resource == scene)
				SetSceneView(view, true);
	}

	Layer TakeLayer(ID3D12GraphicsCommandList* list)
	{
		Layer layer;
		g_frame.fetch_add(1, std::memory_order_relaxed);
		if (g_runs.exchange(0) > 0 && list)
		{
			std::lock_guard lock(g_copyMutex);
			if (g_before && g_after && g_predicate)
			{
				layer = {g_before, g_after};
				// (the next frame's first copy before the particles is to run again)
				t_busy = true;
				SetPredicate(list, 0);
				t_busy = false;
			}
		}
		const int tracing = g_tracing.load(std::memory_order_relaxed);
		if (tracing == 2)
		{
			Record(nullptr, 'T', 0);
			// (a frame and the next: the copies against both frame marks)
			static uint32_t frames = 0;
			if (++frames >= 2)
			{
				frames = 0;
				g_tracing = 0;
				std::vector<Event> events;
				{
					std::lock_guard lock(g_traceMutex);
					events.swap(g_events);
				}
				std::thread(&WriteTrace, std::move(events), g_tracePath).detach();
			}
		}
		else if (tracing == 1)
		{
			g_tracing = 2;
			Record(nullptr, 'T', 0);
		}
		return layer;
	}

	void Trace(const std::wstring& path)
	{
		if (g_tracing != 0)
			return;
		g_tracePath = path;
		for (void* target : g_traceHooks)
			MH_EnableHook(target);
		g_tracing = 1;
	}
}
