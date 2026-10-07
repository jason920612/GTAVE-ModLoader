// High-level story-mode helpers built on natives: the player's character and money, game events, and streamed
// resources that release themselves. Include after (or instead of) natives.hpp.
#pragma once
#include <format>
#include <functional>
#include <string>
#include <utility>

#include "natives.hpp"

namespace ml
{
	// ---- streamed resources --------------------------------------------------------------------
	// Requested on construction, released (no longer needed) when the object goes away:
	//   if (auto model = ml::LoadModel(hash)) { VEHICLE::CREATE_VEHICLE(hash, ...); }   // released at the end of the block
	class Streamed
	{
	public:
		enum class Kind : uint8_t { None, Model, TextureDict, AnimDict, PtfxAsset };

		Streamed() = default;
		Streamed(const Streamed&) = delete;
		Streamed& operator=(const Streamed&) = delete;
		Streamed(Streamed&& o) noexcept { *this = std::move(o); }
		Streamed& operator=(Streamed&& o) noexcept
		{
			if (this != &o)
			{
				Release();
				m_kind = std::exchange(o.m_kind, Kind::None);
				m_hash = o.m_hash;
				m_name = std::move(o.m_name);
				m_loaded = o.m_loaded;
			}
			return *this;
		}
		~Streamed() { Release(); }

		bool Loaded() const { return m_loaded; }
		explicit operator bool() const { return m_loaded; }
		// Keeps the resource loaded after this object is gone.
		void Keep() { m_kind = Kind::None; }

		// Requests the resource and waits for it (MLMain, a callback or a task); Loaded() tells whether it came in time.
		static Streamed Load(Kind kind, Hash hash, std::string name, uint32_t timeoutMs)
		{
			Streamed s;
			s.m_kind = kind;
			s.m_hash = hash;
			s.m_name = std::move(name);
			s.Request();
			s.m_loaded = WaitUntil([&] { return s.IsIn(); }, timeoutMs);
			return s;
		}

	private:
		void Request() const
		{
			switch (m_kind)
			{
			case Kind::Model: STREAMING::REQUEST_MODEL(m_hash); break;
			case Kind::TextureDict: GRAPHICS::REQUEST_STREAMED_TEXTURE_DICT(m_name.c_str(), false); break;
			case Kind::AnimDict: STREAMING::REQUEST_ANIM_DICT(m_name.c_str()); break;
			case Kind::PtfxAsset: STREAMING::REQUEST_NAMED_PTFX_ASSET(m_name.c_str()); break;
			default: break;
			}
		}
		bool IsIn() const
		{
			switch (m_kind)
			{
			case Kind::Model: return STREAMING::HAS_MODEL_LOADED(m_hash) != 0;
			case Kind::TextureDict: return GRAPHICS::HAS_STREAMED_TEXTURE_DICT_LOADED(m_name.c_str()) != 0;
			case Kind::AnimDict: return STREAMING::HAS_ANIM_DICT_LOADED(m_name.c_str()) != 0;
			case Kind::PtfxAsset: return STREAMING::HAS_NAMED_PTFX_ASSET_LOADED(m_name.c_str()) != 0;
			default: return false;
			}
		}
		void Release()
		{
			switch (std::exchange(m_kind, Kind::None))
			{
			case Kind::Model: STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(m_hash); break;
			case Kind::TextureDict: GRAPHICS::SET_STREAMED_TEXTURE_DICT_AS_NO_LONGER_NEEDED(m_name.c_str()); break;
			case Kind::AnimDict: STREAMING::REMOVE_ANIM_DICT(m_name.c_str()); break;
			case Kind::PtfxAsset: STREAMING::REMOVE_NAMED_PTFX_ASSET(m_name.c_str()); break;
			default: break;
			}
		}

		Kind m_kind = Kind::None;
		Hash m_hash = 0;
		std::string m_name;
		bool m_loaded = false;
	};

	inline Streamed LoadModel(Hash model, uint32_t timeoutMs = 5000)
	{
		if (!STREAMING::IS_MODEL_IN_CDIMAGE(model))
			return Streamed();
		return Streamed::Load(Streamed::Kind::Model, model, {}, timeoutMs);
	}
	inline Streamed LoadTextureDict(const char* name, uint32_t timeoutMs = 5000)
	{
		return Streamed::Load(Streamed::Kind::TextureDict, 0, name, timeoutMs);
	}
	inline Streamed LoadAnimDict(const char* name, uint32_t timeoutMs = 5000)
	{
		return Streamed::Load(Streamed::Kind::AnimDict, 0, name, timeoutMs);
	}
	inline Streamed LoadPtfxAsset(const char* name, uint32_t timeoutMs = 5000)
	{
		return Streamed::Load(Streamed::Kind::PtfxAsset, 0, name, timeoutMs);
	}

	// ---- the story game ------------------------------------------------------------------------
	namespace game
	{
		enum class Character : int32_t { None = -1, Michael = 0, Franklin = 1, Trevor = 2 };

		// The story character the player is (by the player's model).
		inline Character CurrentCharacter()
		{
			const Hash model = ENTITY::GET_ENTITY_MODEL(PLAYER::PLAYER_PED_ID());
			static const Hash models[3] = {MISC::GET_HASH_KEY("player_zero"), MISC::GET_HASH_KEY("player_one"), MISC::GET_HASH_KEY("player_two")};
			for (int32_t i = 0; i < 3; ++i)
				if (model == models[i])
					return static_cast<Character>(i);
			return Character::None;
		}
		// 0 Michael, 1 Franklin, 2 Trevor, -1 none: handy as a save slot (ml::save).
		inline int32_t CharacterIndex() { return static_cast<int32_t>(CurrentCharacter()); }

		// Money of a character (default: the current one); 0 when there is none.
		inline int32_t Cash(Character who = CurrentCharacter())
		{
			if (who == Character::None)
				return 0;
			int value = 0;
			STATS::STAT_GET_INT(MISC::GET_HASH_KEY(std::format("SP{}_TOTAL_CASH", static_cast<int32_t>(who)).c_str()), &value, -1);
			return value;
		}
		// Adds (or with a negative amount takes) money; false when there is not enough or no character.
		inline bool AddCash(int32_t amount, Character who = CurrentCharacter())
		{
			if (who == Character::None)
				return false;
			const int32_t now = Cash(who);
			if (amount < 0 && now < -amount)
				return false;
			return STATS::STAT_SET_INT(MISC::GET_HASH_KEY(std::format("SP{}_TOTAL_CASH", static_cast<int32_t>(who)).c_str()), now + amount, true) != 0;
		}

		inline bool IsLoadingScreen() { return DLC::GET_IS_LOADING_SCREEN_ACTIVE() != 0; }

		// Asks story mode to autosave (it does unless autosave is off in the settings). False when one is already waiting.
		inline bool RequestAutosave()
		{
			return detail::Has(&MLApi::RequestAutosave) && Api().RequestAutosave() != 0;
		}

		enum class Event : int32_t
		{
			CharacterChanged = ML_EVENT_CHARACTER_CHANGED,
			Saved = ML_EVENT_GAME_SAVED,             // the game wrote a story save (ml::save data was written too)
			SaveLoading = ML_EVENT_SAVE_LOADING,     // a loading screen began: unsaved changes (and ml::save changes) are gone
		};
		// Runs `fn` on the mod's callback fiber when `event` happens. MLOnLoad or MLMain.
		inline bool On(Event event, std::function<void()> fn)
		{
			if (!detail::Has(&MLApi::OnGameEvent))
				return false;
			return Api().OnGameEvent(static_cast<int32_t>(event), detail::Run, detail::Keep(std::move(fn))) != 0;
		}
	}
}
