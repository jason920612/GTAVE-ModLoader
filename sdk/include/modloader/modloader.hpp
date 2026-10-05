// C++ convenience layer over modloader.h. Include this (and natives.hpp) in your mod.
#pragma once
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <type_traits>

#include "modloader.h"

namespace ml
{
	// Layout matches the game's scrVector (each component padded to 8 bytes).
	struct alignas(8) Vector3
	{
		float x;
		uint32_t _padX;
		float y;
		uint32_t _padY;
		float z;
		uint32_t _padZ;

		constexpr Vector3() : x(0), _padX(0), y(0), _padY(0), z(0), _padZ(0) {}
		constexpr Vector3(float x_, float y_, float z_) : x(x_), _padX(0), y(y_), _padY(0), z(z_), _padZ(0) {}
	};
	static_assert(sizeof(Vector3) == 24);

	namespace detail
	{
		inline const MLApi* g_api = nullptr;
		inline MLContext g_ctx{};

		template<class T>
		uint64_t ToSlot(T value)
		{
			static_assert(sizeof(T) <= sizeof(uint64_t), "native arguments must fit in 8 bytes");
			uint64_t slot = 0;
			std::memcpy(&slot, &value, sizeof(T));
			return slot;
		}

		template<class T>
		void Push(const T& value)
		{
			g_api->NativePush(ToSlot(value));
		}

		// Natives take a vector argument as three consecutive float slots.
		inline void Push(const Vector3& v)
		{
			g_api->NativePush(ToSlot(v.x));
			g_api->NativePush(ToSlot(v.y));
			g_api->NativePush(ToSlot(v.z));
		}
	}

	inline const MLApi& Api() { return *detail::g_api; }
	inline const MLContext& Context() { return detail::g_ctx; }

	// Call this first in MLOnLoad.
	inline void Init(const MLApi* api, const MLContext* ctx)
	{
		detail::g_api = api;
		detail::g_ctx = *ctx;
	}

	template<class Ret = void, class... Args>
	Ret Invoke(uint64_t hash, Args... args)
	{
		const auto& api = Api();
		api.NativeBegin(hash);
		(detail::Push(args), ...);
		uint64_t* result = api.NativeCall();
		if constexpr (!std::is_void_v<Ret>)
		{
			Ret value;
			std::memcpy(&value, result, sizeof(Ret));
			return value;
		}
	}

	inline void Wait(uint32_t ms) { Api().Wait(ms); }
	inline uint64_t TickMs() { return Api().GetTickMs(); }

	template<class... Args>
	void Log(std::format_string<Args...> fmt, Args&&... args)
	{
		Api().Log(ML_LOG_INFO, std::format(fmt, std::forward<Args>(args)...).c_str());
	}
	template<class... Args>
	void LogError(std::format_string<Args...> fmt, Args&&... args)
	{
		Api().Log(ML_LOG_ERROR, std::format(fmt, std::forward<Args>(args)...).c_str());
	}
}

// Declares MLGetModInfo for you: ML_MOD_INFO("My Mod", "1.0.0", "me", "What it does")
#define ML_MOD_INFO(name, version, author, description)                                           \
	extern "C" __declspec(dllexport) const MLModInfo* MLGetModInfo()                               \
	{                                                                                              \
		static const MLModInfo info{ML_API_VERSION, name, version, author, description};          \
		return &info;                                                                              \
	}
