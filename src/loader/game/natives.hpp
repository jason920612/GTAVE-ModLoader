#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <type_traits>

namespace loader::game::natives
{
	// rage::scrVector: each component padded to 8 bytes.
	struct ScrVector
	{
		float x, _px, y, _py, z, _pz;
	};

	// rage::scrNativeCallContext (0x80 bytes, verified against handler code).
	struct CallContext
	{
		uint64_t* returnValue;       // 0x00
		uint32_t argCount;           // 0x08
		uint64_t* args;              // 0x10
		int32_t vectorRefCount;      // 0x18
		ScrVector* vectorRefs[4];    // 0x20 out-params to copy back into
		struct alignas(16) { float x, y, z, w; } vectorSources[4]; // 0x40
	};
	static_assert(sizeof(CallContext) == 0x80);

	using Handler = void (*)(CallContext*);

	// One in-flight native call. Each mod owns one so calls never interleave.
	struct Invocation
	{
		static constexpr uint32_t kMaxArgs = 32;

		CallContext ctx{};
		uint64_t args[kMaxArgs]{};
		uint64_t result[4]{};
		uint64_t hash = 0;
		Handler handler = nullptr;
		bool overflow = false;

		void Begin(uint64_t publicHash);
		void Push(uint64_t value);
	};

	enum class CallStatus { Ok, UnknownNative, TooManyArgs, Crashed };

	// Loads "<public>,<runtime>" hash pairs. Missing file = identity mapping only.
	bool LoadCrossmap(const std::filesystem::path& file);
	// Resolves every known native to its handler. Must run after the game registered natives.
	bool ResolveHandlers();
	Handler FindHandler(uint64_t publicHash);

	// Runs the call; on a crash inside the game, returns Crashed instead of propagating.
	CallStatus Call(Invocation& inv);

	// Loader-side call of a native by public hash (game thread, script context). Arguments are copied into 8-byte
	// slots (pointers, integers, floats). Returns R{} when the native is unknown or crashed.
	template<class R = uint64_t, class... A>
	R Invoke(uint64_t publicHash, A... args)
	{
		Invocation inv;
		inv.Begin(publicHash);
		(
		    [&] {
			    uint64_t slot = 0;
			    std::memcpy(&slot, &args, sizeof(A));
			    inv.Push(slot);
		    }(),
		    ...);
		const bool ok = Call(inv) == CallStatus::Ok;
		if constexpr (!std::is_void_v<R>)
		{
			R value{};
			if (ok)
				std::memcpy(&value, inv.result, sizeof(R));
			return value;
		}
	}
}
