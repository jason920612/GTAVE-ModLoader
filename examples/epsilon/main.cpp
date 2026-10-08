// Epsilon mod: the Epsilon Program website of the loader's browser (web\www.epsilonprogram.com; research/phase0.md §36).
//
// Everything that counts is the game's own, run through appinternet's functions: the questionnaire (part of the first
// Epsilon mission), donations of $500 / $5,000 (Michael's money; the donated total moves the Epsilon missions on at
// $500, $5,000 and $10,000 while they ask for it) and the $25,000 robes (only while the robes mission runs).
//
// Pages call:
//   epsilon.status()        -> { michael, cash, donated, robes, robesPrice }
//   epsilon.donate(amount)  -> { ok, error, cash, donated }      amount 500 or 5000
//   epsilon.robes()         -> { ok, error, cash, donated }
//   epsilon.questionnaire() -> { ok, error }
#include <modloader/game.hpp>

ML_MOD_INFO("Epsilon", "0.1.0", "ModLoader", "The Epsilon Program website")

namespace
{
	// appinternet: the Epsilon site's transactions (@3163085(action), the site in Global 77532: 20 donations, 21 robes;
	// errors in Global 79115) and its page actions (@3129700(movie, page), page 12 = questionnaire sent).
	constexpr const char* kTransactionPattern = "2d 01 04 00 00 62 dc 2e 01 65 02 14 00 00 00";
	constexpr const char* kPagePattern = "2d 02 05 00 00 38 01 65 06 01 00 00 00 21 00 0c 00 00 00";
	const ml::Global kSite{77532};
	const ml::Global kError{79115};
	const ml::Global kVisited{79114};
	const ml::Global kDonated = ml::Global(114990) + (18583 + 381);
	constexpr int kRobesPrice = 25000;

	bool RobesOnSale()
	{
		return SCRIPT::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH(MISC::GET_HASH_KEY("epsrobes")) > 0;
	}

	ml::Json Status()
	{
		// What the site's own front page does (page 14): the mission checks that the site was visited.
		if (kVisited.Int() == -1)
			kVisited.Set(1);
		return {{"michael", ml::game::CurrentCharacter() == ml::game::Character::Michael},
		    {"cash", ml::game::Cash(ml::game::Character::Michael)}, {"donated", kDonated.Int()}, {"robes", RobesOnSale()},
		    {"robesPrice", kRobesPrice}};
	}

	ml::Json Result(const char* error)
	{
		return {{"ok", !*error}, {"error", error}, {"cash", ml::game::Cash(ml::game::Character::Michael)}, {"donated", kDonated.Int()}};
	}

	// One of the site's transactions; "" or an error.
	const char* Transaction(int site, int action)
	{
		if (ml::game::CurrentCharacter() != ml::game::Character::Michael)
			return "character";
		const int previous = kSite.Int();
		kSite.Set(site);
		kError.Set(-1);
		const auto ran = ml::scripts::RunFunction("appinternet", kTransactionPattern, {action}, 4000);
		kSite.Set(previous);
		if (ran != ml::scripts::RunResult::Ran)
		{
			ml::LogError("the game's Epsilon transaction function was not found (game update?)");
			return "unsupported";
		}
		switch (kError.Int())
		{
		case -1: return "";
		case 1: return "closed";
		case 2: return "money";
		default: return "failed";
		}
	}

	ml::Json Donate(int amount)
	{
		if (amount != 500 && amount != 5000)
			return Result("unknown");
		const int before = kDonated.Int();
		const char* error = Transaction(20, amount == 500 ? 2 : 3);
		if (!*error)
			ml::Log("donated ${} (total ${} -> ${})", amount, before, kDonated.Int());
		return Result(error);
	}

	ml::Json Robes()
	{
		if (!RobesOnSale())
			return Result("closed");
		const char* error = Transaction(21, 1);
		if (!*error)
			ml::Log("robes bought");
		return Result(error);
	}

	ml::Json Questionnaire()
	{
		if (ml::game::CurrentCharacter() != ml::game::Character::Michael)
			return Result("character");
		if (ml::scripts::RunFunction("appinternet", kPagePattern, {0, 12}, 4000) != ml::scripts::RunResult::Ran)
		{
			ml::LogError("the game's Epsilon page function was not found (game update?)");
			return Result("unsupported");
		}
		ml::Log("questionnaire sent");
		return Result("");
	}
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	if (!ml::web::Available())
	{
		ml::LogError("this loader has no web browser; the Epsilon mod needs it");
		return 0;
	}
	ml::web::Function("epsilon.status", [] { return Status(); });
	ml::web::Function("epsilon.donate", [](int amount) { return Donate(amount); });
	ml::web::Function("epsilon.robes", [] { return Robes(); });
	ml::web::Function("epsilon.questionnaire", [] { return Questionnaire(); });
	return 1;
}
