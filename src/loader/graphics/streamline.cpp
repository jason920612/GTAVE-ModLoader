#include "streamline.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include <MinHook.h>

#include "../log.hpp"

namespace loader::graphics::streamline
{
	namespace
	{
		// Streamline 2.9 (include/sl_struct.h, sl.h, sl_consts.h): every struct starts with a 32-byte BaseStructure
		// (next, 16-byte type GUID, version).
		//   sl::Resource:    u32 type @32, void* native @40 (the ID3D12Resource), memory @48, view @56, u32 state @64 ...
		//   sl::ResourceTag: Resource* @32, u32 BufferType @40, u32 lifecycle @44, Extent (u32 top, left, width, height) @48: 64 bytes
		//   sl::Constants:   five float4x4 @32 (cameraViewToClip, clipToCameraView, clipToLensClip, clipToPrevClip,
		//                    prevClipToClip), float2 jitterOffset @352, mvecScale @360, cameraPinholeOffset @368,
		//                    float3 cameraPos @376, cameraUp @388, cameraRight @400, cameraFwd @412, float cameraNear @424,
		//                    cameraFar @428, cameraFOV @432, cameraAspectRatio @436
		//   sl::FrameToken:  an interface whose first virtual function is operator uint32_t (the frame index)
		constexpr size_t kTagSize = 64, kTagResource = 32, kTagType = 40, kResourceNative = 40;
		constexpr size_t kViewToClip = 32, kCameraPos = 376, kCameraNear = 424;

		using SetTagFn = int (*)(const void* viewport, const void* tags, uint32_t count, void* commandList);
		using SetTagForFrameFn = int (*)(const void* frame, const void* viewport, const void* tags, uint32_t count, void* commandList);
		using SetConstantsFn = int (*)(const void* constants, const void* frame, const void* viewport);
		using EvaluateFeatureFn = int (*)(uint32_t feature, const void* frame, const void** inputs, uint32_t count, void* commandList);
		SetTagFn g_setTag = nullptr;
		SetTagForFrameFn g_setTagForFrame = nullptr;
		SetConstantsFn g_setConstants = nullptr;
		EvaluateFeatureFn g_evaluateFeature = nullptr;

		std::atomic_bool g_hooked = false;
		std::atomic<AfterDlss> g_afterDlss = nullptr;
		std::atomic<OnCamera> g_onCamera = nullptr;
		// The output colour tagged last on this thread (the game tags, then evaluates, on one render thread).
		thread_local DlssFrame t_frame;
		constexpr uint32_t kBufferDepth = 0, kBufferScalingInputColor = 3, kBufferScalingOutputColor = 4, kBufferBiasCurrentColorHint = 29,
		                   kFeatureDlss = 0;

		void RememberOutput(const void* tags, uint32_t count)
		{
			const auto* t = static_cast<const uint8_t*>(tags);
			for (uint32_t i = 0; t && i < count; ++i, t += kTagSize)
			{
				uint32_t type = 0;
				const uint8_t* resource = nullptr;
				std::memcpy(&type, t + kTagType, 4);
				std::memcpy(&resource, t + kTagResource, 8);
				if (!resource)
					continue;
				void** target = type == kBufferScalingOutputColor ? &t_frame.output : type == kBufferScalingInputColor ? &t_frame.input
				              : type == kBufferDepth                ? &t_frame.depth
				              : type == kBufferBiasCurrentColorHint ? &t_frame.bias
				                                                    : nullptr;
				uint32_t* state = type == kBufferScalingOutputColor ? &t_frame.outputState : type == kBufferScalingInputColor ? &t_frame.inputState
				                : type == kBufferDepth                ? &t_frame.depthState
				                : type == kBufferBiasCurrentColorHint ? &t_frame.biasState
				                                                      : nullptr;
				if (!target)
					continue;
				std::memcpy(target, resource + kResourceNative, 8);
				std::memcpy(state, resource + 64, 4);
				if (type == kBufferScalingInputColor)
				{
					std::memcpy(&t_frame.renderWidth, t + 56, 4); // (the extent's width and height)
					std::memcpy(&t_frame.renderHeight, t + 60, 4);
				}
			}
		}
		bool g_trace = false;
		std::mutex g_mutex;
		Camera g_camera;
		// Tracing: one frame's calls every few seconds.
		uint32_t g_traceFrame = ~0u;
		ULONGLONG g_nextTrace = 0;

		uint32_t FrameIndex(const void* frame)
		{
			if (!frame)
				return ~0u;
			using IndexFn = uint32_t (*)(const void*);
			return reinterpret_cast<IndexFn>((*reinterpret_cast<void* const* const*>(frame))[0])(frame);
		}

		bool Tracing(uint32_t frame)
		{
			if (!g_trace)
				return false;
			std::lock_guard lock(g_mutex);
			if (frame == g_traceFrame)
				return true;
			if (GetTickCount64() < g_nextTrace || frame == ~0u)
				return false;
			g_traceFrame = frame;
			g_nextTrace = GetTickCount64() + 5000;
			return true;
		}

		// (research: memory that may not be what it is taken for)
		bool SafeRead(const void* from, void* to, size_t size)
		{
			__try
			{
				std::memcpy(to, from, size);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		std::string Hex(const void* p, size_t size)
		{
			uint8_t bytes[128] = {};
			if (size > sizeof(bytes) || !SafeRead(p, bytes, size))
				return "?";
			std::string out;
			for (size_t i = 0; i < size; i += 8)
			{
				uint64_t q;
				std::memcpy(&q, bytes + i, 8);
				out += std::format("{}{:x}", i ? " " : "", q);
			}
			return out;
		}

		std::string Describe(const uint8_t* tags, uint32_t count)
		{
			std::string out;
			for (uint32_t i = 0; i < count && i < 8; ++i)
			{
				const uint8_t* t = tags + i * kTagSize;
				uint32_t type = 0, lifecycle = 0, extent[4] = {};
				const uint8_t* resource = nullptr;
				SafeRead(t + kTagType, &type, 4);
				SafeRead(t + kTagType + 4, &lifecycle, 4);
				SafeRead(t + 48, extent, 16);
				SafeRead(t + kTagResource, &resource, 8);
				out += std::format(" [type {} life {} extent {},{} {}x{}", type, lifecycle, extent[0], extent[1], extent[2], extent[3]);
				// sl::Resource: native @40, u32 state @64, width @68, height @72, nativeFormat @76
				void* native = nullptr;
				uint32_t r[4] = {};
				if (resource && SafeRead(resource + kResourceNative, &native, 8) && SafeRead(resource + 64, r, 16))
					out += std::format(" resource {} state {:#x} {}x{} format {}", native, r[0], r[1], r[2], r[3]);
				out += "]";
			}
			return out;
		}

		int SetTag(const void* viewport, const void* tags, uint32_t count, void* commandList)
		{
			if (Tracing(g_traceFrame))
				log::Info("streamline: thread {} slSetTag cmd {}{}", GetCurrentThreadId(), commandList, Describe(static_cast<const uint8_t*>(tags), count));
			RememberOutput(tags, count);
			return g_setTag(viewport, tags, count, commandList);
		}

		int SetTagForFrame(const void* frame, const void* viewport, const void* tags, uint32_t count, void* commandList)
		{
			const uint32_t index = FrameIndex(frame);
			if (Tracing(index))
				log::Info("streamline: thread {} frame {} slSetTagForFrame cmd {}{}", GetCurrentThreadId(), index, commandList,
				    Describe(static_cast<const uint8_t*>(tags), count));
			RememberOutput(tags, count);
			return g_setTagForFrame(frame, viewport, tags, count, commandList);
		}

		int SetConstants(const void* constants, const void* frame, const void* viewport)
		{
			const auto* c = static_cast<const uint8_t*>(constants);
			const uint32_t index = FrameIndex(frame);
			{
				std::lock_guard lock(g_mutex);
				g_camera.frame = index;
				std::memcpy(g_camera.viewToClip, c + kViewToClip, sizeof(g_camera.viewToClip));
				std::memcpy(g_camera.position, c + kCameraPos, 48);
				std::memcpy(&g_camera.nearZ, c + kCameraNear, 16);
				g_camera.valid = true;
				if (const OnCamera on = g_onCamera.load(std::memory_order_relaxed))
					on(g_camera);
			}
			if (Tracing(index))
			{
				const Camera& k = g_camera;
				log::Info("streamline: thread {} frame {} slSetConstants pos {:.2f} {:.2f} {:.2f} fwd {:.3f} {:.3f} {:.3f} up {:.3f} {:.3f} {:.3f} near {} far {} fov {:.4f} "
				          "aspect {:.4f}; viewToClip {:.4f} {:.4f} {:.4f} {:.4f} / {:.4f} {:.4f} {:.4f} {:.4f} / {:.4f} {:.4f} {:.4f} {:.4f} / {:.4f} {:.4f} {:.4f} {:.4f}",
				    GetCurrentThreadId(), index, k.position[0], k.position[1], k.position[2], k.forward[0], k.forward[1], k.forward[2], k.up[0], k.up[1],
				    k.up[2], k.nearZ, k.farZ, k.fov, k.aspect, k.viewToClip[0], k.viewToClip[1], k.viewToClip[2], k.viewToClip[3], k.viewToClip[4],
				    k.viewToClip[5], k.viewToClip[6], k.viewToClip[7], k.viewToClip[8], k.viewToClip[9], k.viewToClip[10], k.viewToClip[11],
				    k.viewToClip[12], k.viewToClip[13], k.viewToClip[14], k.viewToClip[15]);
			}
			return g_setConstants(constants, frame, viewport);
		}

		int EvaluateFeature(uint32_t feature, const void* frame, const void** inputs, uint32_t count, void* commandList)
		{
			const uint32_t index = FrameIndex(frame);
			if (Tracing(index))
				log::Info("streamline: thread {} frame {} slEvaluateFeature {} cmd {} inputs {}", GetCurrentThreadId(), index, feature, commandList, count);
			const int result = g_evaluateFeature(feature, frame, inputs, count, commandList);
			if (feature == kFeatureDlss && result == 0 && t_frame.output && commandList)
				if (const AfterDlss after = g_afterDlss.load(std::memory_order_relaxed))
				{
					t_frame.commandList = commandList;
					after(t_frame);
				}
			if (feature == kFeatureDlss)
				t_frame.bias = nullptr; // (an optional tag: each frame's own or none)
			return result;
		}

		template <typename Fn>
		bool HookExport(HMODULE module, const char* name, Fn detour, Fn& original)
		{
			void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
			if (!target)
				return false;
			return MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) == MH_OK && MH_EnableHook(target) == MH_OK;
		}
	}

	void Hook(HMODULE interposer, bool trace)
	{
		if (!interposer || g_hooked.exchange(true))
			return;
		g_trace = trace;
		const bool tag = HookExport(interposer, "slSetTag", &SetTag, g_setTag);
		const bool tagForFrame = HookExport(interposer, "slSetTagForFrame", &SetTagForFrame, g_setTagForFrame);
		const bool constants = HookExport(interposer, "slSetConstants", &SetConstants, g_setConstants);
		const bool evaluate = HookExport(interposer, "slEvaluateFeature", &EvaluateFeature, g_evaluateFeature);
		log::Info("streamline: hooks slSetTag {} slSetTagForFrame {} slSetConstants {} slEvaluateFeature {}", tag, tagForFrame, constants, evaluate);
	}

	Camera LastCamera()
	{
		std::lock_guard lock(g_mutex);
		return g_camera;
	}

	void SetAfterDlss(AfterDlss callback)
	{
		g_afterDlss = callback;
	}

	void SetOnCamera(OnCamera callback)
	{
		g_onCamera = callback;
	}
}
