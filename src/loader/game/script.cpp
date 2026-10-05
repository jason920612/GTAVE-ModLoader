#include "script.hpp"

#include <Windows.h>
#include <intrin.h>

#include <MinHook.h>

#include "../log.hpp"
#include "pointers.hpp"

namespace loader::game::script
{
	namespace
	{
		// Verified on build stamp 0x6aa45f10 (see research/phase0.md).
		constexpr size_t kTlsCurrentThread = 0x7A0;
		constexpr size_t kTlsThreadActive = 0x7A8;
		constexpr size_t kThreadId = 0x08;
		constexpr size_t kThreadNameHash = 0x150;

		using RunScriptThreadsFn = bool (*)(uint32_t ops);
		RunScriptThreadsFn g_origRunScriptThreads = nullptr;
		TickCallback g_onTick = nullptr;

		template<class T>
		T& Field(void* base, size_t offset)
		{
			return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
		}

		uint8_t* TlsContext()
		{
			const auto slots = reinterpret_cast<uint8_t**>(__readgsqword(0x58));
			return slots[*g_pointers.TlsIndex];
		}

		bool HookRunScriptThreads(uint32_t ops)
		{
			const bool result = g_origRunScriptThreads(ops);
			g_onTick();
			return result;
		}
	}

	bool InstallHooks(TickCallback onTick)
	{
		g_onTick = onTick;
		if (MH_CreateHook(g_pointers.RunScriptThreads, reinterpret_cast<void*>(&HookRunScriptThreads),
		        reinterpret_cast<void**>(&g_origRunScriptThreads)) != MH_OK ||
		    MH_EnableHook(g_pointers.RunScriptThreads) != MH_OK)
		{
			log::Error("could not hook RunScriptThreads");
			return false;
		}
		return true;
	}

	scrThread* FindThread(uint32_t nameHash)
	{
		scrThread** threads = *g_pointers.ScriptThreads;
		const uint16_t count = *g_pointers.ScriptThreadCount;
		for (uint16_t i = 0; i < count; ++i)
		{
			scrThread* thread = threads[i];
			if (thread && Field<uint32_t>(thread, kThreadId) != 0 && Field<uint32_t>(thread, kThreadNameHash) == nameHash)
				return thread;
		}
		return nullptr;
	}

	ScopedThread::ScopedThread(scrThread* thread)
	    : m_tls(TlsContext()),
	      m_prevThread(Field<scrThread*>(m_tls, kTlsCurrentThread)),
	      m_prevGlobal(*g_pointers.ActiveThread),
	      m_prevActive(Field<bool>(m_tls, kTlsThreadActive))
	{
		Field<scrThread*>(m_tls, kTlsCurrentThread) = thread;
		*g_pointers.ActiveThread = thread;
		Field<bool>(m_tls, kTlsThreadActive) = true;
	}

	ScopedThread::~ScopedThread()
	{
		Field<scrThread*>(m_tls, kTlsCurrentThread) = m_prevThread;
		*g_pointers.ActiveThread = m_prevGlobal;
		Field<bool>(m_tls, kTlsThreadActive) = m_prevActive;
	}
}
