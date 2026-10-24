#include "particles.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
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
		// Its decals' (bullet holes, blood, scorch marks: the deferred ones, drawn into the G-buffer).
		constexpr Digest kDecalShaders[] = {
#include "decal_shaders.inc"
		};
		// What a pipeline draws (the marks in the table below).
		enum Kind : uint8_t
		{
			kOther = 0,
			kParticle = 1,
			kDecal = 2,
			kParticleOnce = 3, // particles whose drawing writes the stencil: not drawn a second time (the layer leaves them out)
		};

		// vtable slots (d3d12.h's C interface): ID3D12Device2, ID3D12PipelineLibrary1, ID3D12GraphicsCommandList4,
		// ID3D12CommandQueue.
		constexpr size_t kCreateGraphicsPipelineState = 10, kCreateRenderTargetView = 20, kCreatePipelineLibrary = 44, kCreatePipelineState = 47;
		constexpr size_t kLoadGraphicsPipeline = 9, kLoadPipeline = 13;
		constexpr size_t kClose = 9, kReset = 10, kDrawInstanced = 12, kDrawIndexedInstanced = 13, kSetPipelineState = 25, kResourceBarrier = 26,
		                 kOMSetRenderTargets = 46, kExecuteIndirect = 59, kBeginRenderPass = 68;
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
		ExecuteIndirectFn g_executeIndirect = nullptr;
		BeginRenderPassFn g_beginRenderPass = nullptr;
		ExecuteCommandListsFn g_executeCommandLists = nullptr;
		std::vector<void*> g_traceHooks; // (render passes: hooked while tracing only)

		// The pipelines made with a particle or a decal pixel shader: an open-addressing table of pipeline pointers (a
		// pointer reused for another pipeline is marked again when that one is made), read without a lock on every
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

		uint8_t KindOf(const void* pipeline)
		{
			const auto key = reinterpret_cast<uintptr_t>(pipeline);
			for (size_t i = Slot(key), n = 0; key && n < kTableSize; i = (i + 1) & (kTableSize - 1), ++n)
			{
				const uintptr_t k = g_keys[i].load(std::memory_order_acquire);
				if (k == key)
					return g_marks[i].load(std::memory_order_relaxed);
				if (k == 0)
					return kOther;
			}
			return kOther;
		}

		// What a pipeline does besides its pixel shader (a particle's is drawn a second time into the layer: research §60.21).
		struct Facts
		{
			D3D12_RENDER_TARGET_BLEND_DESC blend{}; // its first target's
			UINT targets = 1;
			bool depthWrites = false, stencilWrites = false;
		};

		template <typename Desc>
		void NoteDepthStencil(const Desc& d, Facts& facts)
		{
			const auto writes = [](const auto& face) {
				return face.StencilFailOp != D3D12_STENCIL_OP_KEEP || face.StencilDepthFailOp != D3D12_STENCIL_OP_KEEP || face.StencilPassOp != D3D12_STENCIL_OP_KEEP;
			};
			facts.depthWrites = d.DepthEnable && d.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
			if constexpr (std::is_same_v<Desc, D3D12_DEPTH_STENCIL_DESC2>)
				facts.stencilWrites = d.StencilEnable && ((d.FrontFace.StencilWriteMask && writes(d.FrontFace)) || (d.BackFace.StencilWriteMask && writes(d.BackFace)));
			else
				facts.stencilWrites = d.StencilEnable && d.StencilWriteMask && (writes(d.FrontFace) || writes(d.BackFace));
		}

		void Note(const void* pipeline, const D3D12_SHADER_BYTECODE* ps, const Facts& facts)
		{
			if (!pipeline)
				return;
			Digest digest{};
			if (ps && ps->pShaderBytecode && ps->BytecodeLength >= 20)
				std::memcpy(&digest, static_cast<const uint8_t*>(ps->pShaderBytecode) + 4, sizeof(digest));
			const auto known = [&](const auto& digests) {
				return std::any_of(std::begin(digests), std::end(digests), [&](const Digest& d) { return d.low == digest.low && d.high == digest.high; });
			};
			const uint8_t kind = known(kParticleShaders) ? (facts.stencilWrites ? kParticleOnce : kParticle) : known(kDecalShaders) ? kDecal : kOther;
			if (kind == kParticle || kind == kParticleOnce)
			{
				// (what the game's particles' pipelines do, each sort once: research)
				static std::mutex mutex;
				static std::vector<std::string> told;
				const D3D12_RENDER_TARGET_BLEND_DESC& b = facts.blend;
				std::string sort = std::format("blend {} {}/{}/{} alpha {}/{}/{} mask {:x}, {} targets{}{}", b.BlendEnable ? "on" : "off", static_cast<int>(b.SrcBlend),
				    static_cast<int>(b.DestBlend), static_cast<int>(b.BlendOp), static_cast<int>(b.SrcBlendAlpha), static_cast<int>(b.DestBlendAlpha),
				    static_cast<int>(b.BlendOpAlpha), b.RenderTargetWriteMask, facts.targets, facts.depthWrites ? ", writes depth" : "",
				    facts.stencilWrites ? ", writes the stencil (not drawn again)" : "");
				std::lock_guard lock(mutex);
				if (std::find(told.begin(), told.end(), sort) == told.end() && told.size() < 24)
				{
					told.push_back(sort);
					log::Info("ue5 particles: a particle pipeline: {}", sort);
				}
			}
			const auto key = reinterpret_cast<uintptr_t>(pipeline);
			std::lock_guard lock(g_tableMutex);
			g_shaderOf[key] = static_cast<uint32_t>(digest.low);
			for (size_t i = Slot(key), n = 0; n < kTableSize; i = (i + 1) & (kTableSize - 1), ++n)
			{
				const uintptr_t k = g_keys[i].load(std::memory_order_relaxed);
				if (k == key)
				{
					g_marks[i].store(kind, std::memory_order_relaxed);
					return;
				}
				if (k == 0)
				{
					if (kind != kOther) // (only particles and decals are kept)
					{
						g_marks[i].store(kind, std::memory_order_relaxed);
						g_keys[i].store(key, std::memory_order_release);
					}
					return;
				}
			}
		}

		// The pixel shader in a pipeline state stream, and the rest of the facts: subobjects aligned to a pointer, each a
		// type and then its data (at the data's own alignment). Null when a subobject is of a type not known here.
		const D3D12_SHADER_BYTECODE* StreamPixelShader(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, Facts& facts)
		{
			if (!desc || !desc->pPipelineStateSubobjectStream)
				return nullptr;
			const auto* at = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
			const uint8_t* const end = at + desc->SizeInBytes;
			const D3D12_SHADER_BYTECODE* ps = nullptr;
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
				default: return ps;
				}
				const size_t offset = (sizeof(uint32_t) + align - 1) / align * align;
				const uint8_t* data = at + offset;
				if (data + size > end)
					return ps;
				switch (type)
				{
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: ps = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(data); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: facts.blend = reinterpret_cast<const D3D12_BLEND_DESC*>(data)->RenderTarget[0]; break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: facts.targets = reinterpret_cast<const D3D12_RT_FORMAT_ARRAY*>(data)->NumRenderTargets; break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: NoteDepthStencil(*reinterpret_cast<const D3D12_DEPTH_STENCIL_DESC*>(data), facts); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: NoteDepthStencil(*reinterpret_cast<const D3D12_DEPTH_STENCIL_DESC1*>(data), facts); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: NoteDepthStencil(*reinterpret_cast<const D3D12_DEPTH_STENCIL_DESC2*>(data), facts); break;
				default: break;
				}
				at += (offset + size + sizeof(void*) - 1) / sizeof(void*) * sizeof(void*);
			}
			return ps;
		}

		Facts DescFacts(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
		{
			Facts facts;
			facts.blend = desc.BlendState.RenderTarget[0];
			facts.targets = desc.NumRenderTargets;
			NoteDepthStencil(desc.DepthStencilState, facts);
			return facts;
		}

		// The render target views made (a descriptor's address to its resource), and those of the scene colour (read
		// without a lock on every OMSetRenderTargets).
		std::mutex g_viewMutex;
		std::unordered_map<SIZE_T, ID3D12Resource*> g_views;
		std::unordered_map<SIZE_T, DXGI_FORMAT> g_viewFormats; // (the views' own formats)
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

		// The particles' layer (research §60.21): each particle the game draws into its scene colour is drawn again, with
		// the same pipeline and bindings, into two targets of our own of the scene's size and format - one cleared to black,
		// one to white. A blend is the source's part plus the target's times a factor (S + D F), so over black the layer
		// holds what the particles add (C) and over white that plus how much of what is behind them shows through (C + T).
		// Copying the scene colour before and after the lists that drew particles (as it was) took in everything else
		// those lists drew between them - windows, light shafts, lamps' coronas - and, as the lists that drew particles
		// were told apart by the frame they were recorded in (recording runs on into the next frame), in some frames and
		// not others: a lit room flickered. Render targets at rest; read and cleared after DLSS (TakeLayer, FinishLayer).
		struct ParticleLayer
		{
			ID3D12Resource* black = nullptr;
			ID3D12Resource* white = nullptr;
			ID3D12DescriptorHeap* heap = nullptr; // (their two render target views)
			D3D12_CPU_DESCRIPTOR_HANDLE views[2]{};
			UINT64 width = 0;
			UINT height = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // (the views': the scene colour's view's)
			std::atomic<bool> ready = false;          // cleared once after a frame (made with anything in it: not read before)

			~ParticleLayer()
			{
				for (IUnknown* object : std::initializer_list<IUnknown*>{black, white, heap})
					if (object)
						object->Release();
			}
		};
		std::atomic<ParticleLayer*> g_layer = nullptr;
		std::mutex g_layerMutex; // (making it)
		std::atomic<bool> g_particlesOn = true;
		// (the layer is read and cleared in the list that ends a frame: a list drawing the next frame's particles into it
		// must be submitted after that one - the frame it was recorded in, against the frames ended at its submission)
		std::mutex g_againMutex;
		std::unordered_map<const void*, uint32_t> g_againLists;
		uint32_t g_framesEnded = 0; // (g_copyMutex)

		// The copies of the G-buffer's first target (the surfaces' colour) before and after the lists that drew decals
		// (the game draws them in a list of their own, after its opaque surfaces: research §60.17), NON_PIXEL_SHADER_RESOURCE
		// at rest, made by small lists of our own put in among the game's as it submits them (research §60.15: copies
		// recorded inside the game's own lists - a barrier on its scene colour in the midst of its drawing, a predicate on
		// the copy - hung the GPU twice in night soaks): one before the first list that drew decals in a frame, one after
		// each such list (the last to run is left), and after the frame's end (the list with DLSS and the composite) the
		// first over the second - a frame that draws none then has none (not the last one's, where they were on the screen).
		std::mutex g_copyMutex; // (making them; our lists)
		ID3D12Resource* g_decalBefore = nullptr;
		ID3D12Resource* g_decalAfter = nullptr;
		std::atomic<uint32_t> g_frame = 0; // frames taken
		// the G-buffer target the decals go into (learnt from them), and whether the colours in it are sRGB-encoded
		std::atomic<ID3D12Resource*> g_decalTarget = nullptr;
		std::atomic<bool> g_decalSrgb = false;
		std::atomic<bool> g_decalsOn = true;

		// A resource's state at a list's start and end, from the barriers on it there (a render target throughout, where
		// none says else).
		struct Span
		{
			bool barrier = false; // a barrier on it was recorded
			bool odd = false;     // one not followed here (split, or a part of it)
			D3D12_RESOURCE_STATES first = D3D12_RESOURCE_STATE_RENDER_TARGET, last = D3D12_RESOURCE_STATE_RENDER_TARGET;

			void Note(const D3D12_RESOURCE_BARRIER& b)
			{
				if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE || (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != 0))
					odd = true;
				if (!barrier)
					first = b.Transition.StateBefore, barrier = true;
				last = b.Transition.StateAfter;
			}
		};

		// A list's state as its own thread records it (from its reset: a list is reset and recorded on one thread), for its
		// particles and decals.
		struct ListState
		{
			ID3D12GraphicsCommandList* list = nullptr;
			uint32_t frame = 0;                    // (when it was reset: one never submitted is let go)
			ID3D12PipelineState* pipeline = nullptr; // set last
			uint8_t kind = kOther;                 // what it draws
			bool scene = false;                    // the first render target bound last is the scene colour
			SIZE_T target = 0;                     // the first render target bound last (a view)
			// the render targets and depth bound last, as they were given (put back after a particle's second drawing)
			UINT targetCount = 0;
			D3D12_CPU_DESCRIPTOR_HANDLE targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			BOOL range = FALSE;
			bool hasDepth = false;
			D3D12_CPU_DESCRIPTOR_HANDLE depth{};
			bool again = false;  // particles were drawn into the layer
			bool decals = false; // decals were drawn
			Span decalSpan;
			uint32_t age = 0;
		};
		thread_local ListState t_lists[4];
		thread_local uint32_t t_age = 0;
		thread_local bool t_busy = false; // (our own commands: the hooks pass them on)

		// The lists that drew decals, closed and not yet submitted.
		struct Drawn
		{
			D3D12_RESOURCE_STATES first, last;
			uint32_t frame;
		};
		std::mutex g_drawnMutex;
		std::unordered_map<const void*, Drawn> g_decalLists;
		// the lists DLSS was recorded on (each ends a frame: its decals are past, the particles' layer is read and cleared
		// there), recorded and not yet submitted, with their frames' numbers
		std::deque<std::pair<const void*, uint32_t>> g_frameEnds;
		bool g_decalBeforeTaken = false; // (g_copyMutex) the copy before them, in the frame being submitted

		// Our lists: a ring, each reused once the queue is past it (else that copy is left out).
		struct CopyList
		{
			ID3D12CommandAllocator* allocator = nullptr;
			ID3D12GraphicsCommandList* list = nullptr;
			uint64_t done = 0; // the value of g_copyFence once the queue is past it
		};
		constexpr size_t kCopyLists = 32;
		CopyList g_copyLists[kCopyLists];
		size_t g_nextCopyList = 0;
		ID3D12Fence* g_copyFence = nullptr;
		uint64_t g_copyValue = 0;

		bool MakeTexture(ID3D12Device* device, ID3D12Resource* scene, ID3D12Resource*& texture, const char* what)
		{
			D3D12_RESOURCE_DESC desc = scene->GetDesc();
			if (desc.SampleDesc.Count != 1)
			{
				static std::atomic_bool told = false;
				if (!told.exchange(true))
					log::Info("ue5 particles: {} multisampled ({} samples): not copied", what, desc.SampleDesc.Count);
				return false;
			}
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
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			desc.Alignment = 0;
			desc.MipLevels = 1;
			desc.DepthOrArraySize = 1;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
			        IID_PPV_ARGS(&texture))))
			{
				texture = nullptr;
				return false;
			}
			log::Info("ue5 particles: copies of {} ({}x{}, format {})", what, desc.Width, desc.Height, static_cast<int>(desc.Format));
			return true;
		}

		bool Same(ID3D12Resource* a, ID3D12Resource* b)
		{
			const D3D12_RESOURCE_DESC x = a->GetDesc(), y = b->GetDesc();
			return x.Width == y.Width && x.Height == y.Height && x.Format == y.Format;
		}

		D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
		{
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
			return b;
		}

		// One of our lists copying the scene colour (in `state` where it goes in the queue) into `to`; its ring slot, or
		// -1 when none is free. (g_copyMutex held)
		int CopyScene(ID3D12Device* device, ID3D12Resource* scene, D3D12_RESOURCE_STATES state, ID3D12Resource* to)
		{
			if (!g_copyFence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copyFence))))
			{
				g_copyFence = nullptr;
				return -1;
			}
			const size_t slot = g_nextCopyList % kCopyLists;
			CopyList& c = g_copyLists[slot];
			if (c.done > g_copyFence->GetCompletedValue())
			{
				static std::atomic_bool told = false;
				if (!told.exchange(true))
					log::Info("ue5 particles: our copy lists all in flight; a copy left out");
				return -1;
			}
			t_busy = true;
			if (!c.allocator)
			{
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator))) ||
				    FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator, nullptr, IID_PPV_ARGS(&c.list))))
				{
					if (c.allocator)
						c.allocator->Release();
					c = CopyList{};
					t_busy = false;
					return -1;
				}
				c.list->Close();
			}
			c.allocator->Reset();
			c.list->Reset(c.allocator, nullptr);
			// (a state the copy can read from as it is needs no barrier)
			const bool readable = (state & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0;
			D3D12_RESOURCE_BARRIER barriers[2] = {Transition(to, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
			    Transition(scene, state, D3D12_RESOURCE_STATE_COPY_SOURCE)};
			c.list->ResourceBarrier(readable ? 1 : 2, barriers);
			D3D12_TEXTURE_COPY_LOCATION dst{to, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, src{scene, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
			dst.SubresourceIndex = src.SubresourceIndex = 0;
			c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
			std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
			c.list->ResourceBarrier(readable ? 1 : 2, barriers);
			const bool closed = SUCCEEDED(c.list->Close());
			t_busy = false;
			if (!closed)
				return -1;
			++g_nextCopyList;
			return static_cast<int>(slot);
		}

		ListState& StateOf(ID3D12GraphicsCommandList* list)
		{
			ListState* oldest = &t_lists[0];
			for (ListState& st : t_lists)
			{
				if (st.list == list)
				{
					st.age = ++t_age;
					return st;
				}
				if (st.age < oldest->age)
					oldest = &st;
			}
			*oldest = ListState{};
			oldest->list = list;
			oldest->frame = g_frame.load(std::memory_order_relaxed);
			oldest->age = ++t_age;
			return *oldest;
		}

		void Forget(ID3D12GraphicsCommandList* list)
		{
			for (ListState& st : t_lists)
				if (st.list == list)
					st = ListState{};
		}

		// Before a draw: decals into the G-buffer, noted.
		void BeforeDraw(ID3D12GraphicsCommandList* list)
		{
			if (t_busy)
				return;
			ListState& st = StateOf(list);
			if (st.kind == kDecal && !st.decals)
			{
				st.decals = true;
				// (the target they go into, learnt from the first; a list where it is new had its barriers on it unseen,
				// and is not copied about)
				ID3D12Resource* target = nullptr;
				DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
				{
					std::lock_guard lock(g_viewMutex);
					if (const auto found = g_views.find(st.target); found != g_views.end())
						target = found->second, format = g_viewFormats[st.target];
				}
				if (!target)
					st.decalSpan.odd = true;
				else if (ID3D12Resource* was = g_decalTarget.exchange(target); was != target)
				{
					st.decalSpan.odd = true;
					// (8 bits a channel hold a colour sRGB-encoded - linear, the darks would band: the game's B8G8R8A8_UNORM
					// does, measured - a decal's darkening on the screen is its ratio in the target to the power 2.2)
					const DXGI_FORMAT stored = target->GetDesc().Format;
					const auto eight = [](DXGI_FORMAT f) {
						return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
						       f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
					};
					g_decalSrgb = eight(format) || eight(stored);
					static std::atomic<int> told = 0;
					if (told.fetch_add(1) < 4)
						log::Info("ue5 particles: the game's decals go into {} (format {}, its view's {})", static_cast<const void*>(target), static_cast<int>(stored),
						    static_cast<int>(format));
				}
			}
		}

		// The particles' layer for the scene colour as it is now, made (again, when its size changed: the old one let go a
		// moment later, frames in flight may still use it) by the first list that draws particles into it; null when it
		// cannot be.
		ParticleLayer* LayerFor(ID3D12GraphicsCommandList* list, SIZE_T sceneView)
		{
			ID3D12Resource* scene = g_scene.load(std::memory_order_acquire);
			if (!scene)
				return nullptr;
			const D3D12_RESOURCE_DESC desc = scene->GetDesc();
			ParticleLayer* layer = g_layer.load(std::memory_order_acquire);
			if (layer && layer->width == desc.Width && layer->height == desc.Height)
				return layer;
			std::lock_guard lock(g_layerMutex);
			layer = g_layer.load(std::memory_order_acquire);
			if (layer && layer->width == desc.Width && layer->height == desc.Height)
				return layer;
			static UINT64 failedWidth = 0;
			static UINT failedHeight = 0;
			if (desc.SampleDesc.Count != 1 || (failedWidth == desc.Width && failedHeight == desc.Height))
				return nullptr;
			DXGI_FORMAT format = desc.Format;
			{
				std::lock_guard views(g_viewMutex);
				if (const auto found = g_viewFormats.find(sceneView); found != g_viewFormats.end() && found->second != DXGI_FORMAT_UNKNOWN)
					format = found->second;
			}
			ID3D12Device* device = nullptr;
			if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
				return nullptr;
			auto made = std::make_unique<ParticleLayer>();
			made->width = desc.Width;
			made->height = desc.Height;
			made->format = format;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC target = desc;
			target.Alignment = 0;
			target.MipLevels = 1;
			target.DepthOrArraySize = 1;
			target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			D3D12_CLEAR_VALUE black{format, {0, 0, 0, 0}}, white{format, {1, 1, 1, 1}};
			D3D12_DESCRIPTOR_HEAP_DESC views{};
			views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			views.NumDescriptors = 2;
			bool ok =
			    SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target, D3D12_RESOURCE_STATE_RENDER_TARGET, &black, IID_PPV_ARGS(&made->black))) &&
			    SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target, D3D12_RESOURCE_STATE_RENDER_TARGET, &white, IID_PPV_ARGS(&made->white))) &&
			    SUCCEEDED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&made->heap)));
			if (ok)
			{
				const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
				made->views[0] = made->heap->GetCPUDescriptorHandleForHeapStart();
				made->views[1].ptr = made->views[0].ptr + step;
				D3D12_RENDER_TARGET_VIEW_DESC view{};
				view.Format = format;
				view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				device->CreateRenderTargetView(made->black, &view, made->views[0]);
				device->CreateRenderTargetView(made->white, &view, made->views[1]);
			}
			device->Release();
			if (!ok)
			{
				failedWidth = desc.Width, failedHeight = desc.Height;
				log::Warn("ue5 particles: the particles' layer could not be made ({}x{}, format {})", desc.Width, desc.Height, static_cast<int>(format));
				return nullptr;
			}
			log::Info("ue5 particles: the particles' layer ({}x{}, format {})", desc.Width, desc.Height, static_cast<int>(format));
			layer = made.release();
			if (ParticleLayer* old = g_layer.exchange(layer))
				std::thread([old] {
					Sleep(3000);
					delete old;
				}).detach();
			return layer;
		}

		// After a particle's drawing into the scene colour: the same again into the layer, over black and over white, and
		// the list's own targets back.
		template <typename Draw>
		void Again(ID3D12GraphicsCommandList* list, Draw draw)
		{
			if (t_busy || !g_particlesOn.load(std::memory_order_relaxed))
				return;
			ListState& st = StateOf(list);
			if (st.kind != kParticle || !st.scene || !st.targetCount)
				return;
			ParticleLayer* layer = LayerFor(list, st.target);
			if (!layer)
				return;
			const D3D12_CPU_DESCRIPTOR_HANDLE* depth = st.hasDepth ? &st.depth : nullptr;
			for (const D3D12_CPU_DESCRIPTOR_HANDLE& view : layer->views)
			{
				g_omSetRenderTargets(list, 1, &view, FALSE, depth);
				draw();
			}
			g_omSetRenderTargets(list, st.targetCount, st.targets, st.range, depth);
			st.again = true;
			if (st.targetCount > 1)
			{
				static std::atomic_bool told = false;
				if (!told.exchange(true))
					log::Info("ue5 particles: particles drawn with {} render targets bound (the layer takes the first's)", st.targetCount);
			}
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
					const uint8_t kind = KindOf(reinterpret_cast<const void*>(e.value));
					what = std::format("pipeline {:#x} ps {:08x}{}", e.value, found == shaders.end() ? 0u : found->second,
					    kind == kParticle ? " particle" : kind == kDecal ? " decal" : "");
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
				Note(*out, &desc->PS, DescFacts(*desc));
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreatePipelineState(ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out)
		{
			const HRESULT hr = g_createPipelineState(device, desc, riid, out);
			if (SUCCEEDED(hr) && out)
			{
				Facts facts;
				const D3D12_SHADER_BYTECODE* ps = StreamPixelShader(desc, facts);
				Note(*out, ps, facts);
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(ID3D12PipelineLibrary* library, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
		{
			const HRESULT hr = g_loadGraphicsPipeline(library, name, desc, riid, out);
			if (SUCCEEDED(hr) && out && desc)
				Note(*out, &desc->PS, DescFacts(*desc));
			return hr;
		}

		HRESULT STDMETHODCALLTYPE LoadPipeline(ID3D12PipelineLibrary1* library, LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out)
		{
			const HRESULT hr = g_loadPipeline(library, name, desc, riid, out);
			if (SUCCEEDED(hr) && out)
			{
				Facts facts;
				const D3D12_SHADER_BYTECODE* ps = StreamPixelShader(desc, facts);
				Note(*out, ps, facts);
			}
			return hr;
		}

		void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Device* device, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE view)
		{
			{
				std::lock_guard lock(g_viewMutex);
				g_views[view.ptr] = resource;
				g_viewFormats[view.ptr] = desc ? desc->Format : resource ? resource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
				SetSceneView(view.ptr, resource && resource == g_scene.load(std::memory_order_relaxed));
			}
			g_createRenderTargetView(device, resource, desc, view);
		}

		HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* list)
		{
			if (!t_busy)
			{
				for (ListState& st : t_lists)
					if (st.list == list)
					{
						if (st.decals && !st.decalSpan.odd)
						{
							std::lock_guard lock(g_drawnMutex);
							g_decalLists[list] = {st.decalSpan.first, st.decalSpan.last, st.frame};
						}
						if (st.again)
						{
							std::lock_guard lock(g_againMutex);
							g_againLists[list] = st.frame;
						}
						st = ListState{};
					}
			}
			return g_close(list);
		}

		HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline)
		{
			Forget(list);
			Record(list, 'Z', 0);
			const HRESULT hr = g_reset(list, allocator, pipeline);
			if (!t_busy)
			{
				ListState& st = StateOf(list);
				st.pipeline = pipeline;
				st.kind = KindOf(pipeline);
			}
			return hr;
		}

		void STDMETHODCALLTYPE SetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
		{
			Record(list, 'S', reinterpret_cast<uintptr_t>(pipeline));
			if (!t_busy)
			{
				ListState& st = StateOf(list);
				st.pipeline = pipeline;
				st.kind = KindOf(pipeline);
			}
			g_setPipelineState(list, pipeline);
		}

		void STDMETHODCALLTYPE OMSetRenderTargets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL range,
		    const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
		{
			const SIZE_T first = count && targets ? targets[0].ptr : 0;
			Record(list, 'R', first);
			if (!t_busy)
			{
				ListState& st = StateOf(list);
				st.scene = IsSceneView(first);
				st.target = first;
				st.targetCount = targets ? std::min<UINT>(count, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) : 0;
				st.range = range;
				for (UINT i = 0; i < (range ? std::min<UINT>(st.targetCount, 1) : st.targetCount); ++i)
					st.targets[i] = targets[i];
				st.hasDepth = depth != nullptr;
				st.depth = depth ? *depth : D3D12_CPU_DESCRIPTOR_HANDLE{};
			}
			g_omSetRenderTargets(list, count, targets, range, depth);
		}

		void STDMETHODCALLTYPE ResourceBarrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
		{
			if (!t_busy && barriers)
			{
				// (the decals' target's states at the list's start and end, for the copies around it)
				if (ID3D12Resource* decal = g_decalTarget.load(std::memory_order_relaxed))
				{
					ListState* st = nullptr;
					for (UINT i = 0; i < count; ++i)
					{
						const D3D12_RESOURCE_BARRIER& b = barriers[i];
						if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Transition.pResource != decal)
							continue;
						if (!st)
							st = &StateOf(list);
						st->decalSpan.Note(b);
					}
				}
			}
			g_resourceBarrier(list, count, barriers);
		}

		void STDMETHODCALLTYPE DrawInstanced(ID3D12GraphicsCommandList* list, UINT a, UINT b, UINT c, UINT d)
		{
			BeforeDraw(list);
			CountDraw(list);
			g_drawInstanced(list, a, b, c, d);
			Again(list, [&] { g_drawInstanced(list, a, b, c, d); });
		}

		void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D12GraphicsCommandList* list, UINT a, UINT b, UINT c, INT d, UINT e)
		{
			BeforeDraw(list);
			CountDraw(list);
			g_drawIndexedInstanced(list, a, b, c, d, e);
			Again(list, [&] { g_drawIndexedInstanced(list, a, b, c, d, e); });
		}

		void STDMETHODCALLTYPE ExecuteIndirect(ID3D12GraphicsCommandList* list, ID3D12CommandSignature* signature, UINT most, ID3D12Resource* arguments,
		    UINT64 argumentsOffset, ID3D12Resource* counts, UINT64 countsOffset)
		{
			BeforeDraw(list);
			g_executeIndirect(list, signature, most, arguments, argumentsOffset, counts, countsOffset);
			Again(list, [&] { g_executeIndirect(list, signature, most, arguments, argumentsOffset, counts, countsOffset); });
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
			if (t_busy || !count || !lists)
				return g_executeCommandLists(queue, count, lists);
			// The lists in this submission that drew decals (and the state of their target about each) and that end a frame
			// (DLSS's and the composite's: the frame's number).
			std::vector<std::pair<UINT, Drawn>> decals;
			std::vector<std::pair<UINT, uint32_t>> ends;
			{
				std::lock_guard lock(g_drawnMutex);
				for (UINT i = 0; i < count; ++i)
				{
					if (const auto found = g_decalLists.find(lists[i]); found != g_decalLists.end())
					{
						decals.emplace_back(i, found->second);
						g_decalLists.erase(found);
					}
					if (const auto found = std::find_if(g_frameEnds.begin(), g_frameEnds.end(), [&](const auto& e) { return e.first == lists[i]; });
					    found != g_frameEnds.end())
					{
						ends.emplace_back(i, found->second);
						g_frameEnds.erase(found);
					}
				}
			}
			// (the lists that drew particles into the layer: each after the end of the frame before the one it was recorded in)
			{
				std::lock_guard lock(g_againMutex);
				size_t nextEnd = 0;
				for (UINT i = 0; i < count && (!g_againLists.empty() || nextEnd < ends.size()); ++i)
				{
					if (const auto found = g_againLists.find(lists[i]); found != g_againLists.end())
					{
						if (found->second > g_framesEnded)
						{
							static std::atomic<int> told = 0;
							if (told.fetch_add(1) < 4)
								log::Warn("ue5 particles: a list drew particles into the layer for frame {} before frame {} ended (the layer is read there)", found->second,
								    found->second - 1);
						}
						g_againLists.erase(found);
					}
					if (nextEnd < ends.size() && ends[nextEnd].first == i)
						g_framesEnded = std::max(g_framesEnded, ends[nextEnd++].second + 1);
				}
			}
			ID3D12Resource* decal = g_decalsOn ? g_decalTarget.load(std::memory_order_acquire) : nullptr;
			if (!decal || (decals.empty() && ends.empty()))
			{
				if (!ends.empty())
				{
					std::lock_guard lock(g_copyMutex);
					g_decalBeforeTaken = false;
				}
				return g_executeCommandLists(queue, count, lists);
			}
			std::lock_guard lock(g_copyMutex);
			std::vector<ID3D12CommandList*> out;
			std::vector<int> used;
			ID3D12Device* device = nullptr;
			const bool direct = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (direct && SUCCEEDED(decal->GetDevice(IID_PPV_ARGS(&device))))
			{
				const bool made = !decals.empty() && MakeTexture(device, decal, g_decalBefore, "the G-buffer about its decals") &&
				                  MakeTexture(device, decal, g_decalAfter, "the G-buffer about its decals");
				const auto copy = [&](ID3D12Resource* from, D3D12_RESOURCE_STATES state, ID3D12Resource* to) {
					if (const int slot = CopyScene(device, from, state, to); slot >= 0)
						out.push_back(g_copyLists[slot].list), used.push_back(slot);
				};
				size_t nextDecal = 0, nextEnd = 0;
				for (UINT i = 0; i < count; ++i)
				{
					const Drawn* e = nextDecal < decals.size() && decals[nextDecal].first == i ? &decals[nextDecal++].second : nullptr;
					const bool end = nextEnd < ends.size() && ends[nextEnd].first == i;
					nextEnd += end ? 1 : 0;
					if (e && made && !g_decalBeforeTaken)
					{
						g_decalBeforeTaken = true;
						copy(decal, e->first, g_decalBefore);
					}
					out.push_back(lists[i]);
					if (e && made)
						copy(decal, e->last, g_decalAfter);
					if (end)
					{
						g_decalBeforeTaken = false;
						// (the next frame's decals start from none: one that draws none shows none)
						if (g_decalBefore && g_decalAfter && Same(g_decalBefore, g_decalAfter))
							copy(g_decalBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g_decalAfter);
					}
				}
				device->Release();
			}
			else
			{
				out.assign(lists, lists + count);
				if (!ends.empty())
					g_decalBeforeTaken = false;
			}
			const uint64_t value = g_copyValue + 1;
			for (const int slot : used)
				g_copyLists[slot].done = value;
			t_busy = true; // (whatever runs the lists may submit too)
			g_executeCommandLists(queue, static_cast<UINT>(out.size()), out.data());
			t_busy = false;
			if (!used.empty() && SUCCEEDED(queue->Signal(g_copyFence, value)))
				g_copyValue = value;
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
				     Hook(l, kExecuteIndirect, &ExecuteIndirect, g_executeIndirect);
				ok = ok && Hook(Vtable(queue), kExecuteCommandLists, &ExecuteCommandLists, g_executeCommandLists);
				// (for the trace: enabled while tracing)
				if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))))
					Hook(Vtable(list4), kBeginRenderPass, &BeginRenderPass, g_beginRenderPass, false);
			} while (false);
			for (IUnknown* object : std::initializer_list<IUnknown*>{list4, list, allocator, queue, library, device1, device2, device})
				if (object)
					object->Release();
			if (ok)
				log::Info("ue5 particles: D3D12 hooked ({} particle and {} decal shaders known{})", std::size(kParticleShaders), std::size(kDecalShaders),
				    libraryHooked ? "" : "; no pipeline library");
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
		const uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed);
		{
			// (and the lists that drew decals or particles but were never submitted, let go)
			std::lock_guard lock(g_drawnMutex);
			if (list)
				g_frameEnds.emplace_back(list, frame);
			while (g_frameEnds.size() > 8)
				g_frameEnds.pop_front();
			std::erase_if(g_decalLists, [&](const auto& d) { return d.second.frame + 8 < frame; });
		}
		{
			std::lock_guard lock(g_againMutex);
			std::erase_if(g_againLists, [&](const auto& d) { return d.second + 8 < frame; });
		}
		// The particles' layer, to be read (a render target again, cleared, in FinishLayer).
		if (ParticleLayer* p = g_layer.load(std::memory_order_acquire); p && list && p->ready && g_particlesOn.load(std::memory_order_relaxed))
		{
			const D3D12_RESOURCE_BARRIER barriers[2] = {Transition(p->black, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			    Transition(p->white, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
			g_resourceBarrier(list, 2, barriers);
			layer.particles = p->black, layer.particlesWhite = p->white;
			layer.particleViews[0] = p->views[0], layer.particleViews[1] = p->views[1];
		}
		{
			std::lock_guard lock(g_copyMutex);
			if (g_decalsOn && g_decalBefore && g_decalAfter && Same(g_decalBefore, g_decalAfter))
				layer.decalBefore = g_decalBefore, layer.decalAfter = g_decalAfter, layer.decalSrgb = g_decalSrgb;
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

	void FinishLayer(ID3D12GraphicsCommandList* list, const Layer& layer)
	{
		if (!list)
			return;
		const float black[4] = {0, 0, 0, 0}, white[4] = {1, 1, 1, 1};
		if (layer.particles && layer.particlesWhite)
		{
			const D3D12_RESOURCE_BARRIER barriers[2] = {
			    Transition(layer.particles, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
			    Transition(layer.particlesWhite, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
			g_resourceBarrier(list, 2, barriers);
			list->ClearRenderTargetView(layer.particleViews[0], black, 0, nullptr);
			list->ClearRenderTargetView(layer.particleViews[1], white, 0, nullptr);
			return;
		}
		// (not read this frame: cleared all the same, and one just made cleared for the first time)
		if (ParticleLayer* p = g_layer.load(std::memory_order_acquire))
		{
			list->ClearRenderTargetView(p->views[0], black, 0, nullptr);
			list->ClearRenderTargetView(p->views[1], white, 0, nullptr);
			p->ready = true;
		}
	}

	void SetParticles(bool on)
	{
		g_particlesOn = on;
		log::Info("ue5 particles: the game's particles {}", on ? "over the renderer's picture" : "left out");
	}

	void SetDecals(bool on)
	{
		g_decalsOn = on;
		log::Info("ue5 particles: the game's decals {}", on ? "over the renderer's picture" : "left out");
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
