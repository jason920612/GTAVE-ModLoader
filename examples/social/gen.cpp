// Posts and replies written by the loader's local text model (ml::ai), when the player has turned it on.
// One request at a time, most wanted first: replies to the player's own posts, posts about what the player just did,
// replies to posts on the open page, and a few everyday Bleeter posts a game day. Results are checked (Traditional
// Chinese, short, nothing about games or AI); a failed one is asked again once, then dropped. They are kept in the
// save ("social_ai"); the pages fall back to the written content for anything not generated.
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <set>

#include "social.hpp"

namespace social::gen
{
	namespace
	{
		// ---- who writes ----------------------------------------------------------------------------

		const std::map<std::string, std::pair<const char*, const char*>> kPeople = {
		    {"weazel", {"Weazel News", "你是 Weazel News 新聞台的官方帳號，聳動、八卦，愛用「快訊」「震驚」。"}},
		    {"lspd", {"洛聖都警察局", "你是洛聖都警察局（LSPD）的官方帳號，語氣官方制式，偶爾不小心透露出自己的無能。"}},
		    {"globe", {"Daily Globe", "你是 Daily Globe 報社的帳號，老派報紙口吻，帶點冷嘲熱諷。"}},
		    {"bawsaq", {"BAWSAQ 行情快訊", "你是 BAWSAQ 股市快訊帳號，簡短像行情機器人，偶爾講冷笑話。"}},
		    {"lifeinvader", {"Lifeinvader", "你是 Lifeinvader 社群網站的官方帳號，科技公司的假親切口吻，毫無隱私觀念。"}},
		    {"merryweather", {"Merryweather", "你是 Merryweather 私人保全公司的帳號，冷酷、軍事化、帶威脅感的公關口吻。"}},
		    {"epsilon", {"Epsilon 計畫", "你是 Epsilon 計畫的官方帳號，說話神祕兮兮，愛講 Kifflom 和 Kraff，總想要人捐錢。"}},
		    {"michael", {"麥可", "你是麥可．迪聖塔，住羅克福德山的退休中年富人，家庭關係很糟，愛老電影，厭世、愛冷嘲熱諷。"}},
		    {"franklin", {"富蘭克林", "你是富蘭克林．柯林頓，來自南洛聖都的年輕人，想往上爬，冷靜、直接，帶點街頭口吻。"}},
		    {"trevor", {"崔佛", "你是崔佛．菲利普，暴躁瘋狂的罪犯，經營 Trevor Philips Industries，講話粗魯、愛罵髒話、充滿威脅。"}},
		    {"amanda", {"亞曼達", "你是亞曼達．迪聖塔，麥可的太太，熱衷瑜珈、購物和自我成長，對丈夫滿腹怨氣。"}},
		    {"jimmy", {"吉米", "你是吉米．迪聖塔，麥可的兒子，二十歲的懶惰宅男，愛打電動、自以為是饒舌歌手，講話幼稚。"}},
		    {"tracey", {"崔西", "你是崔西．迪聖塔，麥可的女兒，一心想紅的網紅，戲劇化，愛用表情符號。"}},
		    {"lamar", {"拉瑪", "你是拉瑪．戴維斯，富蘭克林的好兄弟，講話很嗆、愛吹牛、滿口街頭俚語，愛亂取綽號。"}},
		    {"tanisha", {"塔妮莎", "你是塔妮莎，富蘭克林的前女友，成熟務實，對街頭那一套很不耐煩。"}},
		    {"denise", {"丹妮絲", "你是丹妮絲，富蘭克林的阿姨，熱衷自我成長小組，嘴巴很毒，愛抱怨姪子。"}},
		    {"simeon", {"西門", "你是西門．葉特理恩，Premium Deluxe Motorsport 車行老闆，滑頭、愛推銷，三句不離分期付款。"}},
		    {"ron", {"朗", "你是朗．傑考斯基，崔佛的跟班，神經質的陰謀論者，深信政府和外星人在監視所有人。"}},
		    {"wade", {"韋德", "你是韋德，崔佛的跟班，天真、反應很慢，講話像小孩。"}},
		    {"friedlander", {"費藍德醫師", "你是費藍德醫師，貪財的心理醫師，滿口心靈雞湯，句句暗示要收費。"}},
		    {"solomon", {"Solomon Richards", "你是 Solomon Richards，自大的老牌好麥塢製片。"}},
		    {"lazlow", {"Lazlow", "你是 Lazlow，過氣又油膩的電視主持人，總愛講自己有多紅。"}},
		    {"devin", {"Devin Weston", "你是 Devin Weston，傲慢的億萬富翁投資人，看不起所有人。"}},
		    {"c_vinewood", {"Tiffani", "你是好麥塢一個一直試鏡失敗、很想紅的年輕女演員。"}},
		    {"c_sandy", {"戴夫", "你是住在沙岸地區拖車裡的鄉下大叔，見怪不怪。"}},
		    {"c_grove", {"Kev", "你是格羅夫街的年輕人，街頭口吻。"}},
		    {"c_dad", {"老爸", "你是太平洋崖的中年老爸，愛抱怨兒子和物價。"}},
		    {"c_yoga", {"瑜珈媽媽", "你是羅克福德山的有錢瑜珈媽媽，愛炫富、愛講身心靈。"}},
		    {"c_truth", {"真相只有一個", "你是陰謀論網友，什麼事都覺得背後有黑幕。"}},
		    {"c_driver", {"老王", "你是市中心的計程車司機，愛抱怨塞車和奇怪的客人。"}},
		    {"c_surfer", {"衝浪客", "你是 Vespucci 海灘的衝浪客，很放鬆、很隨便。"}},
		    {"c_gamer", {"電玩宅宅", "你是整天窩在家打電動的宅宅。"}},
		    {"c_foodie", {"吃貨", "你是到處吃美食、愛評論餐廳的網友。"}},
		    {"c_hipster", {"文青", "你是鏡湖公園的文青，看不起所有主流的東西。"}},
		};
		const std::vector<std::string> kCitizens = {"c_vinewood", "c_sandy", "c_grove", "c_dad", "c_yoga", "c_truth", "c_driver", "c_surfer", "c_gamer",
		    "c_foodie", "c_hipster"};
		constexpr const char* kCharacters[3] = {"michael", "franklin", "trevor"};

		const char* Persona(const std::string& who)
		{
			const auto it = kPeople.find(who);
			return it == kPeople.end() ? "你是洛聖都的一名網友。" : it->second.second;
		}
		std::string NameOf(const std::string& who)
		{
			const auto it = kPeople.find(who);
			return it == kPeople.end() ? "一名網友" : it->second.first;
		}

		std::mt19937 g_rng{std::random_device{}()};
		template<class T>
		const T& Pick(const std::vector<T>& v)
		{
			return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(g_rng)];
		}

		// Up to `n` different people from `pool`, not the author.
		std::vector<std::string> Repliers(std::vector<std::string> pool, const std::string& author, int n)
		{
			std::erase(pool, author);
			std::shuffle(pool.begin(), pool.end(), g_rng);
			if (pool.size() > static_cast<size_t>(n))
				pool.resize(static_cast<size_t>(n));
			return pool;
		}

		// ---- what was written ------------------------------------------------------------------------

		struct GenPost
		{
			std::string id;
			int64_t t;
			std::string who, text, site, on, ev, kind; // kind: event, daily, mine
		};
		struct Reply
		{
			std::string who, text;
			int64_t t;
		};
		struct Store
		{
			std::vector<GenPost> posts;
			std::map<std::string, std::vector<Reply>> replies; // "<site>:<post id>"
			std::set<std::string> eventsDone;                  // event keys already asked about
			int64_t next = 1;
		} g;

		constexpr size_t kMaxPosts = 600, kMaxReplied = 800;

		void Save()
		{
			ml::Json posts = ml::Json::Array();
			for (const GenPost& p : g.posts)
				posts.Push(ml::Json::Array({p.id, p.t, p.who, p.text, p.site, p.on, p.ev, p.kind}));
			ml::Json replies = ml::Json::Object();
			for (const auto& [key, list] : g.replies)
			{
				ml::Json a = ml::Json::Array();
				for (const Reply& r : list)
					a.Push(ml::Json::Array({r.who, r.text, r.t}));
				replies[key] = a;
			}
			ml::save::Set("social_ai", {{"posts", posts}, {"replies", replies}, {"events", std::vector<std::string>(g.eventsDone.begin(), g.eventsDone.end())},
			                               {"next", g.next}});
		}

		// ---- requests ------------------------------------------------------------------------------------

		enum Priority { Mine = 0, EventPost = 1, PageReply = 2, Daily = 3 };
		struct Task
		{
			Priority priority;
			bool reply;      // a reply to `key`, else a new post
			std::string key; // reply: "<site>:<post id>"
			std::string who, site, on, ev, kind;
			int64_t t;       // the post's time
			std::string prompt;
			int attempt = 0;
		};
		std::deque<Task> g_queue;
		bool g_busy = false;
		int g_generation = 0; // bumped on reload: late answers are dropped

		bool Queued(const std::string& key)
		{
			return std::any_of(g_queue.begin(), g_queue.end(), [&](const Task& t) { return t.reply && t.key == key; });
		}
		void Queue(Task t)
		{
			const auto at = std::find_if(g_queue.begin(), g_queue.end(), [&](const Task& q) { return q.priority > t.priority; });
			g_queue.insert(at, std::move(t));
		}

		std::string System(const std::string& site)
		{
			return std::format("你在為諷刺、黑色幽默的犯罪都市「洛聖都」（聖安地列斯州）裡的社群網站 {} 撰寫內容。內容可以粗俗、誇張、帶犯罪題材。"
			                   "不要提到遊戲、電玩、GTA、AI 或「虛構」，就當這是真實的城市。只輸出內容本身一則，使用台灣繁體中文，"
			                   "不要加引號、署名、標籤說明或任何解釋。",
			    site == "l" ? "Lifeinvader（類似臉書）" : "Bleeter（類似推特）");
		}

		size_t Chars(const std::string& s)
		{
			return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) != 0x80; }));
		}

		// Cleans a generated text; empty when it is not usable.
		std::string Check(std::string s)
		{
			const auto trim = [&] {
				while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r'))
					s.erase(0, 1);
				while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r'))
					s.pop_back();
			};
			trim();
			for (size_t at; (at = s.find('\n')) != std::string::npos;)
				s.replace(at, 1, " ");
			// Quotes around the whole text, a "name：" in front.
			for (const auto& [open, close] : std::vector<std::pair<std::string, std::string>>{{"「", "」"}, {"“", "”"}, {"\"", "\""}, {"『", "』"}})
				if (s.starts_with(open) && s.ends_with(close) && s.size() > open.size() + close.size())
					s = s.substr(open.size(), s.size() - open.size() - close.size());
			// A speaker's name in front ("名字：…"): a short run without punctuation before a colon.
			for (const char* colon : {"：", ":"})
				if (const size_t at = s.find(colon); at != std::string::npos && Chars(s.substr(0, at)) <= 12 &&
				    s.substr(0, at).find_first_of(" ，。！？,.!?") == std::string::npos)
				{
					s.erase(0, at + std::strlen(colon));
					break;
				}
			trim();
			static const char* kBad[] = {"抱歉", "我不能", "無法協助", "无法", "作為一個", "AI", "人工智慧", "語言模型", "遊戲", "GTA", "俠盜", "虛構", "#GTA"};
			for (const char* bad : kBad)
				if (s.find(bad) != std::string::npos)
					return {};
			// Simplified-only characters: ask again rather than show them.
			static const char* kSimplified[] = {"们", "这", "说", "时", "会", "对", "发", "经", "过", "还", "为", "没", "从", "见", "问", "应", "让", "该", "车", "门",
			    "开", "关", "长", "东", "风", "书", "买", "卖", "头", "钱", "实", "现", "国", "动", "学", "当", "点", "样", "边", "认", "识", "设", "话", "请", "谢",
			    "讲", "变", "别", "际", "吗", "给", "觉", "带", "处", "难", "爱", "乐", "听", "网", "视", "讯", "业", "务", "产", "刚", "帮", "机", "鸡", "钟", "杀",
			    "枪", "个", "么", "里", "后", "气", "们", "热", "脑", "饭", "银", "货", "贵", "赚", "输", "赢", "疯", "龙", "战", "终", "录", "红", "传"};
			for (const char* ch : kSimplified)
				if (s.find(ch) != std::string::npos)
					return {};
			const size_t n = Chars(s);
			return n >= 4 && n <= 120 ? s : std::string();
		}

		void Send()
		{
			if (g_busy || g_queue.empty() || ml::ai::GetState() != ml::ai::State::Ready)
				return;
			Task task = std::move(g_queue.front());
			g_queue.pop_front();
			g_busy = true;
			const int generation = g_generation;
			const ml::ai::Request request{System(task.site), std::format("{}\n{}", Persona(task.who), task.prompt), 100, 0.9f};
			const bool sent = ml::ai::Generate(request, [task, generation](std::optional<std::string> text) mutable {
				if (generation != g_generation)
					return;
				g_busy = false;
				const std::string ok = text ? Check(*text) : std::string();
				if (ok.empty())
				{
					if (text && ++task.attempt < 2)
						Queue(std::move(task));
					return;
				}
				if (task.reply)
				{
					auto& list = g.replies[task.key];
					list.push_back({task.who, ok, std::max(task.t, GameMinute())});
					while (g.replies.size() > kMaxReplied)
						g.replies.erase(g.replies.begin());
				}
				else
				{
					g.posts.push_back({std::format("g{}", g.next++), task.t, task.who, ok, task.site, task.on, task.ev, task.kind});
					if (g.posts.size() > kMaxPosts)
						g.posts.erase(g.posts.begin());
				}
				Save();
			});
			if (!sent)
			{
				g_busy = false;
				g_queue.push_front(std::move(task));
			}
		}

		// ---- what to ask for -------------------------------------------------------------------------------

		const char* WeatherText(const std::string& w)
		{
			static const std::map<std::string, const char*> kText = {{"clear", "晴朗"}, {"sunny", "大太陽"}, {"clouds", "多雲"}, {"overcast", "陰天"},
			    {"rain", "下雨"}, {"clearing", "雨後放晴"}, {"thunder", "雷雨"}, {"smog", "霧霾"}, {"fog", "起霧"}, {"snow", "下雪"}};
			const auto it = kText.find(w);
			return it == kText.end() ? "晴朗" : it->second;
		}
		std::string Money(int64_t n)
		{
			std::string s = std::to_string(n < 0 ? -n : n);
			for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
				s.insert(static_cast<size_t>(i), ",");
			return "$" + s;
		}

		void QueueEvent(const Event& e)
		{
			const std::string key = EventKey(e);
			if (!g.eventsDone.insert(key).second)
				return;
			const auto post = [&](const std::string& who, const std::string& site, int64_t at, std::string prompt) {
				Queue({EventPost, false, "", who, site, site == "l" ? who : "", key, "event", e.t + at, std::move(prompt)});
			};
			const std::string self = e.c >= 0 && e.c < 3 ? kCharacters[e.c] : "";
			const std::string vehicle = e.b.empty() ? "" : std::format("（嫌犯開的是一台{}）", e.b);
			if (e.type == "chase")
			{
				post("lspd", "b", 0, std::format("發一則官方貼文：本局在「{}」一帶追捕一名 {} 星通緝的嫌犯{}，嫌犯逃逸，呼籲民眾提供線索。60 字以內。", e.a, e.n, vehicle));
				post(Pick(kCitizens), "b", 8,
				    std::format("你剛剛在「{}」附近親眼看到警察大陣仗追捕一名嫌犯{}，最後警察跟丟了。發一則目擊貼文，50 字以內。", e.a, vehicle));
				post("weazel", "b", 25, std::format("發一則新聞快訊：一名嫌犯在「{}」一帶甩開警方追捕{}。60 字以內。", e.a, vehicle));
			}
			else if (e.type == "buy" && !self.empty())
				post(self, "l", 5,
				    std::format("你剛在「{}」花了 {} 買了{}。發一則 Lifeinvader 動態，50 字以內。", e.a, Money(e.n), e.b == "property" ? "新的房地產" : "一台新載具"));
			else if (e.type == "hospital" && !self.empty())
				post(self, "l", 30, std::format("你剛從「{}」出院，醫療帳單 {}。發一則 Lifeinvader 動態，50 字以內。", e.a, Money(e.n)));
			else if (e.type == "arrest" && !self.empty())
			{
				post(self, "l", 20, std::format("你剛被警察抓去「{}」，付了 {} 保釋金。發一則 Lifeinvader 動態，堅稱自己是冤枉的，50 字以內。", e.a, Money(e.n)));
				post("lspd", "b", 5, std::format("發一則官方貼文：本局剛逮捕一名嫌犯並移送「{}」，嫌犯已交保。50 字以內。", e.a));
			}
			else if (e.type == "stock")
			{
				const std::string move = std::format("{}{:.1f}%", e.n >= 0 ? "大漲 " : "重挫 ", static_cast<double>(e.n < 0 ? -e.n : e.n) / 100.0);
				post("bawsaq", "b", 0, std::format("{}（{}）今日股價{}。發一則行情快訊，40 字以內。", e.a, e.b, move));
				post(Pick(kCitizens), "b", 45, std::format("你看到 {}（{}）今天股價{}。發一則貼文說說你的反應，40 字以內。", e.a, e.b, move));
			}
		}

		// A few everyday Bleeter posts each game day (after 7:00), one at a time.
		uint64_t g_nextDaily = 0;
		void QueueDaily()
		{
			if (ml::TickMs() < g_nextDaily)
				return;
			g_nextDaily = ml::TickMs() + 10000;
			const int64_t now = GameMinute(), today = now / 1440;
			const int hour = static_cast<int>(now % 1440 / 60);
			if (hour < 7)
				return;
			if (std::any_of(g_queue.begin(), g_queue.end(), [](const Task& t) { return t.priority == Daily; }))
				return;
			const auto count = std::count_if(g.posts.begin(), g.posts.end(), [&](const GenPost& p) { return p.kind == "daily" && p.t / 1440 == today; });
			if (count >= 4)
				return;
			std::string recent;
			for (auto it = Events().rbegin(); it != Events().rend() && recent.empty(); ++it)
				if (it->type == "chase" && it->t > now - 1440)
					recent = std::format("今天稍早有人在「{}」甩掉了警察的追捕。", it->a);
			Queue({Daily, false, "", Pick(kCitizens), "b", "", "", "daily", now,
			    std::format("現在是{}的 {} 點。{}發一則日常生活的 Bleeter 貼文（不一定要提到最近的事），40 字以內。", WeatherText(Weather()), hour, recent)});
		}
	}

	void Reload()
	{
		++g_generation;
		g_busy = false;
		g_queue.clear();
		g = {};
		const ml::Json j = ml::save::Get("social_ai");
		for (const ml::Json& p : j["posts"].Items())
			g.posts.push_back({p[0].Str(), p[1].Int64(), p[2].Str(), p[3].Str(), p[4].Str(), p[5].Str(), p[6].Str(), p[7].Str()});
		for (const auto& [key, list] : j["replies"].Members())
			for (const ml::Json& r : list.Items())
				g.replies[key].push_back({r[0].Str(), r[1].Str(), r[2].Int64()});
		for (const ml::Json& k : j["events"].Items())
			g.eventsDone.insert(k.Str());
		g.next = std::max<int64_t>(1, j["next"].Int64());
	}

	void OnEvent(const Event& e)
	{
		if (ml::ai::Usable())
			QueueEvent(e);
	}

	void Tick()
	{
		if (!ml::ai::Usable())
			return;
		QueueDaily();
		Send();
	}

	void Want(const ml::Json& list)
	{
		if (!ml::ai::Usable())
			return;
		for (const ml::Json& p : list.Items())
		{
			const std::string key = p["key"].Str(), site = p["site"].Str(), author = p["who"].Str();
			if (key.empty() || g.replies.contains(key) || Queued(key))
				continue;
			std::vector<std::string> pool;
			for (const ml::Json& id : p["pool"].Items())
				pool.push_back(id.Str());
			if (pool.empty())
				pool = kCitizens;
			const auto repliers = Repliers(pool, author, 1 + static_cast<int>(g_rng() % 2));
			for (const std::string& who : repliers)
				Queue({PageReply, true, key, who, site, "", "", "", p["t"].Int64(),
				    std::format("{}在 {} 上發了：「{}」\n用一句話{}，30 字以內。", NameOf(author), site == "l" ? "Lifeinvader" : "Bleeter", p["text"].Str(),
				        site == "l" ? "留言" : "回覆")});
		}
	}

	ml::Json Post(const std::string& site, const std::string& text)
	{
		const int c = ml::game::CharacterIndex();
		if (c < 0 || text.empty() || Chars(text) > 200)
			return {{"ok", false}};
		const std::string who = kCharacters[c];
		GenPost post{std::format("m{}", g.next++), GameMinute(), who, text, site == "l" ? "l" : "b", site == "l" ? who : "", "", "mine"};
		g.posts.push_back(post);
		Save();
		if (ml::ai::Usable())
		{
			const std::string key = post.site + ":" + post.id;
			const int count = 2 + static_cast<int>(g_rng() % 2);
			std::vector<std::string> pool = kCitizens;
			if (post.site == "l")
				pool = c == 0 ? std::vector<std::string>{"amanda", "jimmy", "tracey", "franklin", "trevor"}
				     : c == 1 ? std::vector<std::string>{"lamar", "tanisha", "denise", "michael", "trevor"}
				              : std::vector<std::string>{"ron", "wade", "michael", "franklin"};
			for (const std::string& replier : Repliers(pool, who, count))
				Queue({Mine, true, key, replier, post.site, "", "", "", post.t,
				    std::format("{}剛在 {} 上發了：「{}」\n用一句話{}，30 字以內。", NameOf(who), post.site == "l" ? "Lifeinvader" : "Bleeter", text,
				        post.site == "l" ? "留言" : "回覆")});
		}
		return {{"ok", true}, {"id", post.id}};
	}

	ml::Json ToJson()
	{
		ml::Json posts = ml::Json::Array();
		for (const GenPost& p : g.posts)
			posts.Push({{"id", p.id}, {"t", p.t}, {"who", p.who}, {"text", p.text}, {"site", p.site}, {"on", p.on}, {"ev", p.ev}, {"kind", p.kind}});
		ml::Json replies = ml::Json::Object();
		for (const auto& [key, list] : g.replies)
		{
			ml::Json a = ml::Json::Array();
			for (const Reply& r : list)
				a.Push({{"who", r.who}, {"text", r.text}, {"t", r.t}});
			replies[key] = a;
		}
		static const char* kStates[] = {"off", "starting", "ready", "failed"};
		return {{"state", kStates[static_cast<int>(ml::ai::GetState()) & 3]}, {"queued", static_cast<int64_t>(g_queue.size())}, {"posts", posts},
		    {"replies", replies}};
	}
}
