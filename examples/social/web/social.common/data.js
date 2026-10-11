// Shared content of the Bleeter and Lifeinvader sites (written for this mod).
// PEOPLE: who posts. STORY: posts that appear once a story mission is passed (by the game's script name), each with
// `in` (game minutes after the mission) and either `b` (a Bleeter post) or `l` (a Lifeinvader post: `who`, `text`, and
// `on` = whose timeline, default the author). AMBIENT: everyday Bleeter posts; EVENTS: templates for what the player did.
window.PEOPLE = {
  weazel: { name: 'Weazel News', handle: 'WeazelNews', color: '#c8102e', mark: 'W', verified: true },
  lspd: { name: '洛聖都警察局', handle: 'LSPD', color: '#1c3f7a', mark: '警', verified: true },
  globe: { name: 'Daily Globe', handle: 'DailyGlobeLS', color: '#333', mark: 'G', verified: true },
  bawsaq: { name: 'BAWSAQ 行情快訊', handle: 'BAWSAQ_Ticker', color: '#0f3d7a', mark: 'B', verified: true },
  lifeinvader: { name: 'Lifeinvader', handle: 'Lifeinvader', color: '#d32f2f', mark: 'L', verified: true },
  jaynorris: { name: 'Jay Norris', handle: 'JayNorris', color: '#b71c1c', mark: 'J', verified: true },
  lazlow: { name: 'Lazlow', handle: 'LazlowLive', color: '#6a1b9a', mark: 'Z', verified: true },
  merryweather: { name: 'Merryweather 安全顧問', handle: 'MerryweatherSec', color: '#3e4a2f', mark: 'M', verified: true },
  devin: { name: 'Devin Weston', handle: 'DevinWeston', color: '#004d40', mark: 'D', verified: true },
  solomon: { name: 'Solomon Richards', handle: 'SolomonRichards', color: '#5d4037', mark: 'S', verified: true },
  epsilon: { name: 'Epsilon 計畫', handle: 'EpsilonProgram', color: '#2f6db5', mark: 'ε', verified: true },
  michael: { name: '麥可．迪聖塔', handle: 'MDeSanta', color: '#1565c0', mark: '麥' },
  franklin: { name: '富蘭克林．柯林頓', handle: 'FrankieC', color: '#2e7d32', mark: '富' },
  trevor: { name: '崔佛．菲利普', handle: 'TPIndustries', color: '#e65100', mark: '崔' },
  amanda: { name: '亞曼達．迪聖塔', handle: 'AmandaDS', color: '#ad1457', mark: '亞' },
  jimmy: { name: '吉米．迪聖塔', handle: 'JimmyBoy420', color: '#00838f', mark: '吉' },
  tracey: { name: '崔西．迪聖塔', handle: 'TraceyDS', color: '#ec407a', mark: '崔' },
  lamar: { name: '拉瑪．戴維斯', handle: 'LamarDavisLD', color: '#558b2f', mark: '拉' },
  tanisha: { name: '塔妮莎．傑克森', handle: 'TanishaJ', color: '#8e24aa', mark: '塔' },
  denise: { name: '丹妮絲．柯林頓', handle: 'AuntieDenise', color: '#f9a825', mark: '丹' },
  simeon: { name: '西門．葉特理恩', handle: 'SimeonPDM', color: '#4e342e', mark: '西' },
  ron: { name: '朗．傑考斯基', handle: 'RonTruthSeeker', color: '#827717', mark: '朗' },
  wade: { name: '韋德．赫伯特', handle: 'WadeH', color: '#6d4c41', mark: '韋' },
  friedlander: { name: '費藍德醫師', handle: 'DrFriedlander', color: '#455a64', mark: '費' },
  c_vinewood: { name: '好麥塢小姐 Tiffani', handle: 'VinewoodTiffani', color: '#f06292', mark: 'T' },
  c_sandy: { name: '沙岸地區的戴夫', handle: 'SandyShoresDave', color: '#a1887f', mark: '戴' },
  c_grove: { name: '格羅夫街的 Kev', handle: 'GroveStKev', color: '#43a047', mark: 'K' },
  c_dad: { name: '太平洋崖的老爸', handle: 'PacificBluffsDad', color: '#5c6bc0', mark: '爸' },
  c_yoga: { name: '羅克福德山瑜珈媽媽', handle: 'RockfordYogaMom', color: '#26a69a', mark: '瑜' },
  c_truth: { name: '真相只有一個', handle: 'WakeUpSheeple', color: '#616161', mark: '?' },
  c_driver: { name: '計程車司機老王', handle: 'DowntownCabWang', color: '#fbc02d', mark: '王' },
  c_surfer: { name: 'Vespucci 衝浪客', handle: 'VespucciSurf', color: '#039be5', mark: '浪' },
  c_gamer: { name: '電玩宅宅', handle: 'RighteousSlaughter_Fan', color: '#7e57c2', mark: '宅' },
  c_foodie: { name: '吃貨洛聖都', handle: 'LSFoodie', color: '#ff7043', mark: '吃' },
  c_hipster: { name: '鷹爪豆文青', handle: 'MirrorParkHipster', color: '#8d6e63', mark: '文' },
};

// Characters the pages log in as (0 Michael, 1 Franklin, 2 Trevor).
window.CHARACTERS = ['michael', 'franklin', 'trevor'];

window.STORY = [
  // ---- Franklin and Lamar, Simeon ----
  { after: 'armenian1', in: 40, b: { who: 'c_grove', text: '剛剛兩台車從福里斯特森林一路飆到市區，一台還是全新的 Bagger…南洛聖都的年輕人都不睡覺的嗎 #飆車', likes: 38 } },
  { after: 'armenian1', in: 90, b: { who: 'simeon', text: 'Premium Deluxe Motorsport 本週特惠！分期零頭期款，信用不好？沒關係，我們有「別的辦法」讓您還款。', likes: 4 } },
  { after: 'armenian1', in: 120, l: { who: 'lamar', text: '今天跟我兄弟 F 開了一整天的豪車，誰說回收員沒前途？我們是汽車界的催收天王 👑', likes: 21 } },
  { after: 'armenian2', in: 60, b: { who: 'c_grove', text: '有人在搶 Vagos 的機車？那條巷子現在一堆人在找人算帳，大家今晚別經過 #南洛聖都', likes: 52 } },
  { after: 'armenian3', in: 30, b: { who: 'weazel', text: '快訊：一輛轎車今晚撞穿 Premium Deluxe Motorsport 的展示窗，衝進店內。車行老闆 Simeon Yetarian 送醫，警方表示「沒有人願意作證」。', likes: 412 } },
  { after: 'armenian3', in: 70, b: { who: 'c_driver', text: '我剛開車經過 PDM，那面大玻璃整片沒了，一台車就停在展示間中間，旁邊還有一台全新的跑車被壓扁。洛聖都每天都比電影精彩', likes: 133 } },
  { after: 'armenian3', in: 200, l: { who: 'franklin', text: '有些工作做到一半，老闆就自己幫你辭職了。人生新的一頁。', likes: 9 } },
  { after: 'armenian3', in: 260, l: { who: 'lamar', text: '@FrankieC 你被開除了？？？Simeon 的店都被撞爛了你還在想工作？跟我一起混啦 💪', on: 'franklin', likes: 6 } },

  // ---- Michael's family ----
  { after: 'family1', in: 45, b: { who: 'weazel', text: '大洛聖都高速公路上演驚險一幕：一艘遊艇在行駛中被拖著狂飆，最後連同拖車翻落路邊。目擊者稱船上「站著一個人在揮手」。', likes: 287 } },
  { after: 'family1', in: 80, b: { who: 'jimmy', text: '我爸真的是全世界最不酷的人。我只是想把船「借」給朋友而已。#生活好難 #沒人懂我', likes: 3 } },
  { after: 'family1', in: 120, l: { who: 'jimmy', text: '我爸今天搞砸了一筆超讚的生意，然後還說要「好好談談」。救命。', on: 'michael', likes: 2 } },
  { after: 'family3', in: 50, b: { who: 'weazel', text: '好麥塢山莊一棟豪宅今午被整個拉下山坡，屋況全毀。據悉屋主與某位知名人士關係密切，警方暫未說明原因。', likes: 498 } },
  { after: 'family3', in: 90, b: { who: 'c_vinewood', text: '我家對面那棟房子…不見了？？我出門做個指甲回來山坡就空了 😳 #好麥塢山莊', likes: 76 } },
  { after: 'family3', in: 180, l: { who: 'amanda', text: '今天的網球課「提早結束」。有些人就是不懂什麼叫私人時間。', likes: 11 } },
  { after: 'family2', in: 60, b: { who: 'tracey', text: '本來今天在遊艇上要被製作人看見我的才華了！！！結果被我爸毀了。我爸是全宇宙最丟臉的人 😭😭😭', likes: 1204 } },
  { after: 'family2', in: 100, b: { who: 'c_surfer', text: '剛在 Vespucci 外海看到一個中年男人開水上摩托車追一艘遊艇，後座還載著一個尖叫的女生。這城市到底怎麼了', likes: 61 } },
  { after: 'family4', in: 30, b: { who: 'lazlow', text: '今天的 Fame or Shame 錄影發生「一點小意外」。我很好。我的褲子不太好。節目會照常播出。', likes: 2310 } },
  { after: 'family4', in: 80, b: { who: 'tracey', text: '我本來在 Fame or Shame 要紅了…然後我爸和一個長得像流浪漢的大叔衝上台。我要搬家。#FameOrShame', likes: 1877 } },
  { after: 'family4', in: 120, b: { who: 'weazel', text: '知名主持人 Lazlow 錄影後遭兩名男子追逐至好麥塢，事件影片在網路瘋傳。電視台表示「收視率創新高，我們不予置評」。', likes: 932 } },
  { after: 'family5', in: 200, l: { who: 'amanda', text: '我需要一點空間。孩子們，媽媽愛你們。', likes: 34 } },
  { after: 'family5', in: 240, l: { who: 'jimmy', text: '家裡現在只剩我跟我爸。他在泳池邊發呆了三個小時。好啦，我今天會洗碗。', on: 'michael', likes: 5 } },
  { after: 'family6', in: 120, l: { who: 'amanda', text: '一家人去了一趟家庭諮商。我們還是會吵架，但至少現在吵架有人收費。', likes: 27 } },
  { after: 'family6', in: 180, l: { who: 'tracey', text: '老爸請全家吃晚餐，沒有人哭，沒有人被綁。這是我們家的新紀錄 ❤️', on: 'michael', likes: 58 } },

  // ---- Lester's friend request: Lifeinvader ----
  { after: 'lester1', in: -600, b: { who: 'jaynorris', text: '明天，Lifeinvader 將發表一項會改變世界的產品。你的隱私？那是上個世紀的東西。 #LifeinvaderLive', likes: 15820 } },
  { after: 'lester1', in: 20, b: { who: 'weazel', text: '震驚！Lifeinvader 創辦人 Jay Norris 在新品發表直播中，手上的原型機突然爆炸，當場身亡。警方正調查是否為蓄意攻擊。', likes: 25411 } },
  { after: 'lester1', in: 40, b: { who: 'c_gamer', text: '我剛剛在看 Lifeinvader 直播…他拿起手機…然後…直播就斷了。我需要一個人靜一靜。', likes: 802 } },
  { after: 'lester1', in: 90, b: { who: 'bawsaq', text: 'LFI（Lifeinvader）盤後重挫。分析師：「執行長爆炸通常不是利多。」', likes: 412 } },
  { after: 'lester1', in: 120, b: { who: 'lifeinvader', text: '我們深切哀悼創辦人 Jay Norris。他相信每個人都應該被看見——每分每秒。服務將照常運作，您的資料一如往常地安全。', likes: 9120 } },
  { after: 'lester1', in: 400, b: { who: 'c_truth', text: '手機自己爆炸？在最大的發表會上？別傻了。醒醒吧各位 #JayNorris #都是安排好的', likes: 290 } },

  // ---- Assassinations ----
  { after: 'Assassin_Valet', in: 60, b: { who: 'weazel', text: '好麥塢一家飯店外發生槍擊，一名科技公司董事當場身亡。目擊者指出兇手「穿著飯店泊車員的制服」。', likes: 1120 } },
  { after: 'Assassin_Valet', in: 120, b: { who: 'c_vinewood', text: '以後去飯店我都要自己停車了。誰知道泊車小弟是不是殺手 😨', likes: 154 } },
  { after: 'Assassin_Multi', in: 60, b: { who: 'lspd', text: '本局正調查今日市內多起同時發生的槍擊案，死者皆為同一集團的董事。請民眾提供任何可疑資訊。', likes: 340 } },
  { after: 'Assassin_Hooker', in: 60, b: { who: 'globe', text: '某工會領袖於深夜遇害，現場位於一處「他太太大概不知道」的地點。', likes: 223 } },
  { after: 'Assassin_Bus', in: 60, b: { who: 'weazel', text: '市區公車上發生槍擊，一名商界人士遇害。公車公司表示「本路線將照常營運，敬請準時候車」。', likes: 287 } },
  { after: 'Assassin_Construction', in: 60, b: { who: 'weazel', text: '市中心工地發生意外，一名開發商從鋼樑上墜落身亡。警方不排除他殺可能。', likes: 312 } },

  // ---- The Jewel Store Job ----
  { after: 'jewelry_setup1', in: 300, b: { who: 'c_yoga', text: 'Vangelico 珠寶店今天來了一個戴怪眼鏡的男人，盯著天花板看了十分鐘。店員說他是「來檢查通風口的」。好喔。', likes: 19 } },
  { after: 'jewelry_heist', in: 15, b: { who: 'lspd', text: '羅克福德山 Vangelico 珠寶店發生持械搶案，嫌犯戴面具並使用催淚氣體，騎機車逃逸。附近民眾請避開該區。', likes: 1840 } },
  { after: 'jewelry_heist', in: 40, b: { who: 'weazel', text: '光天化日之下！Vangelico 珠寶店遭洗劫，損失估計達數百萬。嫌犯一路騎車衝進洛聖都河的排水道，警車根本追不進去。', likes: 3210 } },
  { after: 'jewelry_heist', in: 70, b: { who: 'c_driver', text: '剛剛一群機車從我旁邊衝進排水溝，後面跟著十台警車。我只是要去載客人去機場…', likes: 241 } },
  { after: 'jewelry_heist', in: 200, b: { who: 'c_truth', text: '#Vangelico 搶案發生時，對面大樓的 Bugstars 除蟲車剛好停在那裡一整天。巧合？我不這麼認為。', likes: 88 } },

  // ---- Trevor in Blaine County ----
  { after: 'trevor1', in: 40, b: { who: 'weazel', text: '沙岸地區一處拖車公園發生血腥衝突，Lost MC 飛車黨多名成員死傷。警方抵達時嫌犯早已離開，現場只留下一台被踩爛的…拖車。', likes: 876 } },
  { after: 'trevor1', in: 90, b: { who: 'c_sandy', text: '沙岸地區今天又是平凡的一天：槍聲、爆炸、有人在大吼。我的狗已經習慣了。', likes: 143 } },
  { after: 'trevor1', in: 160, b: { who: 'ron', text: '我只說一件事：這一帶的生意現在換人管了。還有，政府正在用路燈監視你。這是兩件事。', likes: 12 } },
  { after: 'chinese1', in: 60, b: { who: 'c_sandy', text: '有人在酒行外面跟一群 Aztecas 幹起來，用的是…一輛卡車？沙岸地區的週末真的不用看電影', likes: 77 } },
  { after: 'chinese2', in: 30, b: { who: 'weazel', text: '葡萄籽一處農場發生大爆炸，火光數公里外可見。消防人員在現場發現疑似製毒設備。', likes: 655 } },
  { after: 'chinese2', in: 80, b: { who: 'ron', text: '我們已經聽說了葡萄籽的意外。Trevor Philips Industries 致力於成為本郡最「可靠」的供應商。歡迎合作洽詢。', likes: 8 } },
  { after: 'trevor2', in: 50, b: { who: 'weazel', text: '沙岸機場附近一場衝突中，數架小型飛機被擊落。聯邦航空局表示「我們正在…呃…調查」。', likes: 501 } },
  { after: 'trevor2', in: 120, b: { who: 'ron', text: '朋友們，天上那些飛機不是普通飛機。今天少了幾架，你們可以安心睡覺了。不客氣。', likes: 22 } },
  { after: 'trevor3', in: 120, l: { who: 'trevor', text: '搬到洛聖都了。有地方睡，有老朋友，有一大堆老帳要算。這城市還不知道自己有多幸運。', likes: 3 } },
  { after: 'trevor3', in: 200, l: { who: 'wade', text: '崔佛帶我來城裡了！我住在表哥 Floyd 家，他說我可以待「幾天」。這裡好多高樓好漂亮', on: 'trevor', likes: 4 } },

  // ---- FIB ----
  { after: 'fbi1', in: 60, b: { who: 'weazel', text: '市立法醫中心今日發生槍戰，有人目擊一名中年男子從冷凍櫃區衝出。官方對此不予置評。', likes: 470 } },
  { after: 'fbi2', in: 40, b: { who: 'weazel', text: '有人從直升機垂降進入市中心某政府大樓，帶走一名「重要人士」。FIB 與 IAA 互相指責對方「根本不知道在幹嘛」。', likes: 1543 } },
  { after: 'fbi2', in: 100, b: { who: 'c_truth', text: '直升機從政府大樓把人帶走，新聞只播三十秒。你們真的覺得這是正常的嗎？？？ #IAA #FIB', likes: 310 } },
  { after: 'fbi4', in: 30, b: { who: 'lspd', text: '市中心一輛運鈔車遭垃圾車與拖吊車包夾搶劫，嫌犯身穿消防隊制服。本局呼籲民眾：消防員通常不拿步槍。', likes: 2210 } },
  { after: 'fbi4', in: 90, b: { who: 'merryweather', text: 'Merryweather 提醒您：如果您的貨物需要「真正的」保全，請別再把錢交給運鈔公司。', likes: 77 } },
  { after: 'fbi5A', in: 120, b: { who: 'globe', text: '海上一處化學研究設施昨夜遭入侵，業者 Humane Labs 表示「沒有任何危險物質外洩」，並請民眾不要喝附近的海水。', likes: 380 } },

  // ---- Franklin ----
  { after: 'franklin0', in: 60, b: { who: 'c_grove', text: '有隻羅威納犬一路追一個 Ballas 追到鐵道上，太猛了。那狗比我還會打架 🐶', likes: 211 } },
  { after: 'franklin0', in: 140, l: { who: 'lamar', text: '我說真的，Chop 是全洛聖都最強的保鑣。牠只是有時候會吃掉別人的鞋子。', on: 'franklin', likes: 17 } },
  { after: 'franklin1', in: 60, b: { who: 'lspd', text: '南洛聖都發生毒品交易衝突，現場多人駁火，嫌犯分乘機車與車輛逃逸。', likes: 199 } },
  { after: 'franklin2', in: 60, b: { who: 'weazel', text: '一座回收廠今日爆發大規模槍戰，警方與幫派火力交鋒，直升機徹夜在南洛聖都上空盤旋。', likes: 760 } },
  { after: 'lamar1', in: 60, b: { who: 'weazel', text: '洛聖都一處廢棄回收廠爆發槍戰，警方大批出動。據了解，衝突與兩個幫派的舊怨有關。', likes: 512 } },
  { after: 'lamar1', in: 120, l: { who: 'lamar', text: '今天差點掛了。不過你知道嗎，我拉瑪．戴維斯還活著，而且帥到不行。謝謝我兄弟 F。', on: 'franklin', likes: 25 } },

  // ---- The heists ----
  { after: 'docks_heistA', in: 60, b: { who: 'merryweather', text: '本公司的海上貨物於昨晚遭竊。Merryweather 將全力追查，必要時「採取適當手段」。我們從不手軟。', likes: 61 } },
  { after: 'docks_heistB', in: 60, b: { who: 'merryweather', text: 'Merryweather 一艘運輸船昨晚在外海出事。相關貨物去向不明，本公司保留一切追究權利。', likes: 58 } },
  { after: 'docks_heistA', in: 140, b: { who: 'c_surfer', text: '昨晚港口那邊一陣爆炸，今天海面上還漂著東西。衝浪客暫停一天，海太亂了', likes: 39 } },
  { after: 'agency_heist3A', in: 20, b: { who: 'weazel', text: '市中心 FIB 總部大樓發生火災，大批消防員進入灌救。有民眾指稱「有些消防員好像抱著硬碟出來」。', likes: 1980 } },
  { after: 'agency_heist3B', in: 20, b: { who: 'weazel', text: 'FIB 總部大樓遭人從屋頂入侵，直升機在大樓上空盤旋。聯邦探員表示資料「完全安全」。', likes: 1932 } },
  { after: 'rural_bank_heist', in: 10, b: { who: 'lspd', text: '緊急通報：佩立托灣 Blaine County 儲蓄銀行遭重裝嫌犯搶劫，嫌犯身穿全身防彈裝甲並持機槍。所有單位請支援。', likes: 4120 } },
  { after: 'rural_bank_heist', in: 40, b: { who: 'weazel', text: '佩立托灣陷入戰場！銀行搶匪與警方、甚至軍方交火，小鎮街道滿目瘡痍。嫌犯最後在一列火車上消失。', likes: 6650 } },
  { after: 'rural_bank_heist', in: 90, b: { who: 'c_sandy', text: '我表哥住佩立托灣，他說有一個穿鐵甲的大個子拿著機槍走在主街上，跟電玩一樣。我們這邊已經不用看新聞了，直接看窗外就好', likes: 520 } },
  { after: 'finale_heist2A', in: 30, b: { who: 'weazel', text: '史上最大劫案！聯合儲蓄所金庫遭洗劫，數噸金條不翼而飛。嫌犯手法專業，警方至今毫無頭緒。', likes: 18200 } },
  { after: 'finale_heist2B', in: 30, b: { who: 'weazel', text: '聯合儲蓄所金庫被人從地下打穿，數噸金條遭竊。專家：「這是本世紀最大膽的搶案。」', likes: 18010 } },
  { after: 'finale_heist2A', in: 120, b: { who: 'c_truth', text: '這麼大的搶案，到現在沒抓到半個人。你們不覺得有人「上面」罩著嗎', likes: 990 } },
  { after: 'finale_heist2B', in: 120, b: { who: 'bawsaq', text: '黃金搶案消息傳出，金價小幅上揚，保全類股同步下挫。', likes: 140 } },

  // ---- Michael's later life ----
  { after: 'michael4', in: 30, b: { who: 'weazel', text: '《Meltdown》首映會在好麥塢盛大舉行！製片 Solomon Richards 稱這是「他職業生涯最重要的作品」。', likes: 1430 } },
  { after: 'michael4', in: 60, b: { who: 'solomon', text: '感謝所有到場的朋友。特別感謝我的新夥伴——他對電影有一種…不按牌理出牌的熱情。', likes: 640 } },
  { after: 'michael4', in: 90, l: { who: 'michael', text: '我的名字上了大銀幕。製作人：麥可．迪聖塔。年輕時看電影的自己大概不會相信。', likes: 41 } },
  { after: 'solomon1', in: 120, l: { who: 'michael', text: '開始在 Richards Majestic 片場幫忙。電影業比我以前的工作還要黑。', likes: 18 } },
  { after: 'exile1', in: 60, b: { who: 'globe', text: '一架貨機在桑庫多河上空遭不明人士劫持，機上的「貨物」至今下落不明。', likes: 210 } },
  { after: 'carsteal1', in: 60, b: { who: 'lspd', text: '本局一輛警用跑車遭人假冒警察開走。提醒民眾：真正的警察不會叫你下車讓他「試開一下」。', likes: 670 } },
  { after: 'carsteal4', in: 60, b: { who: 'devin', text: '收藏是一種藝術。這週我又多了幾件「獨一無二」的作品。#DevinWestonCollection', likes: 255 } },

  // ---- Endings ----
  { after: 'finaleA', in: 60, b: { who: 'globe', text: '一名男子於羅斯特菲爾德附近的煉油廠墜落起火，身分尚待確認。警方表示「這人仇家應該不少」。', likes: 310 } },
  { after: 'finaleB', in: 60, b: { who: 'globe', text: '一名中年男子於好麥塢附近電廠高處墜落身亡。死者據信曾在電影界活動。', likes: 290 } },
  { after: 'finaleC2', in: 20, b: { who: 'weazel', text: '震驚！FIB 探員 Steve Haines 在德爾佩羅碼頭的摩天輪上遭狙擊身亡，事發時正在錄製節目。', likes: 12040 } },
  { after: 'finaleC2', in: 60, b: { who: 'weazel', text: '億萬富翁 Devin Weston 失蹤，其保鑣均遭殺害。另外，中國黑幫首腦 Wei Cheng 與南洛聖都幫派人物 Stretch 也在同日遇害。', likes: 9860 } },
  { after: 'finaleC2', in: 120, b: { who: 'c_truth', text: '同一天，一個 FIB 探員、一個億萬富翁、一個黑幫老大、一個幫派份子全都出事？醒醒吧，這是有人在「清理門戶」', likes: 2140 } },
  { after: 'finaleC2', in: 240, l: { who: 'franklin', text: '有些事做完了，就真的結束了。現在只想跟朋友好好吃頓飯。', likes: 77 } },
  { after: 'finaleC2', in: 260, l: { who: 'lamar', text: '@FrankieC 那頓飯你請。你現在有錢到可以把整間 Cluckin\' Bell 買下來了吧 🍗', on: 'franklin', likes: 33 } },
];

// Everyday Bleeter posts. `w` limits a post to weather kinds, `h` to hours [from, to).
window.AMBIENT = [
  { who: 'c_driver', text: '早上七點，Del Perro 高速公路已經變成停車場。我寧願去搶銀行也不想塞車', h: [6, 10] },
  { who: 'c_foodie', text: 'Cluckin\' Bell 新出的「三倍辣雞」吃完我的嘴巴已經離家出走了 🔥 #CluckinBell', h: [11, 22] },
  { who: 'c_foodie', text: 'Burger Shot 的薯條今天是熱的。今天是我的幸運日。', h: [11, 23] },
  { who: 'c_foodie', text: '有人推薦 Bean Machine 的拿鐵嗎？我喝了一杯之後到現在手還在抖 ☕', h: [7, 15] },
  { who: 'c_yoga', text: '今天的瑜珈課主題是「放下」。我放下了我的離婚律師的帳單。namaste 🙏', h: [7, 12] },
  { who: 'c_yoga', text: '羅克福德山的咖啡廳一杯冷萃要 14 塊。但你付的不是咖啡，是被看見的機會。', h: [9, 18] },
  { who: 'c_vinewood', text: '剛剛在好麥塢大道看到一個很像明星的人！結果他在發傳單。好麥塢就是這樣。', h: [10, 22] },
  { who: 'c_vinewood', text: '試鏡第 37 次失敗。導演說我「太有表情了」。到底要怎樣啦 😩 #演員人生', h: [12, 23] },
  { who: 'c_hipster', text: '我在鏡湖公園喝手沖咖啡，聽黑膠，讀紙本書。你們用手機看這篇的人應該很羨慕吧。', h: [9, 19] },
  { who: 'c_hipster', text: '新開的那家店很棒，可惜現在太多人知道了。我已經不去了。', h: [10, 22] },
  { who: 'c_gamer', text: '《Righteous Slaughter 7》玩了十二個小時，現在看到真的街道都想找掩體。', h: [0, 5] },
  { who: 'c_gamer', text: '凌晨三點，還在打排位。我媽說我需要曬太陽。太陽又沒有排位。', h: [1, 5] },
  { who: 'c_truth', text: '為什麼 Bleeter 的伺服器每天凌晨都要「維護」？他們在下載你的夢。', h: [0, 6] },
  { who: 'c_truth', text: '飛機尾巴那些白線是什麼你們真的知道嗎？我不知道，但我很確定那不是好東西。' },
  { who: 'c_truth', text: 'Epsilon 計畫的人今天在我家門口發傳單，我收下了然後燒掉。現在我被監視了嗎' },
  { who: 'c_dad', text: '兒子說要「創業」，結果是在車庫賣自己畫的 NFT 猴子。我開始考慮再生一個。' },
  { who: 'c_dad', text: '帶小孩去德爾佩羅碼頭坐摩天輪，花了 80 塊，孩子全程在看手機。', h: [10, 20] },
  { who: 'c_sandy', text: '沙岸地區的夜晚很安靜。除了槍聲、狗叫、和隔壁在煮某種很臭的東西。', h: [20, 24] },
  { who: 'c_sandy', text: '今天在亞拉摩海釣到一條三隻眼睛的魚。我決定不問。', h: [6, 18] },
  { who: 'c_grove', text: '格羅夫街今天很平靜。我說「今天」。', h: [8, 20] },
  { who: 'c_surfer', text: '浪不錯，水有點怪怪的顏色，但浪不錯 🏄', h: [6, 18], w: ['clear', 'sunny', 'clouds', 'clearing'] },
  { who: 'c_surfer', text: '霧太大了，海面上什麼都看不到。有一艘船差點撞到我，船上的人在罵髒話還是在唱歌我分不出來', w: ['fog', 'smog'] },
  { who: 'c_driver', text: '下雨天載客最煩，每個人上車都濕答答還嫌我開太快 ☔', w: ['rain', 'thunder'] },
  { who: 'c_vinewood', text: '雨下成這樣，我的頭髮已經放棄治療了。洛聖都不是一年只下三天雨嗎', w: ['rain', 'thunder'] },
  { who: 'c_dad', text: '打雷的時候我兒子說「爸，那是上帝在打排位」。我不知道該驕傲還是擔心。', w: ['thunder'] },
  { who: 'c_yoga', text: '今天陽光太好了，羅克福德山的每個人都在跑步，好像我們是在拍運動廣告 ☀️', w: ['sunny', 'clear'], h: [7, 18] },
  { who: 'c_foodie', text: '這種陰天最適合吃一碗熱騰騰的拉麵。小東京那家我推。', w: ['overcast', 'clouds', 'fog'] },
  { who: 'c_hipster', text: '霧霾讓整座城市看起來像一張底片照片。很有感覺，就是不太能呼吸。', w: ['smog', 'fog'] },
  { who: 'c_sandy', text: '下雪了？？？在聖安地列斯？？我把所有衣服都穿上還是好冷', w: ['snow'] },
  { who: 'epsilon', text: '今日金句：懷疑是一種疾病。好消息是，我們有療程，而且可以分期付款。#Kifflom' },
  { who: 'merryweather', text: 'Merryweather 正在招募！只要您身體健康、良心有彈性，歡迎加入我們的大家庭。' },
  { who: 'lifeinvader', text: '您知道嗎？您的 Lifeinvader 好友中，有 87% 正在偷看您的個人檔案。這是好事。' },
  { who: 'bawsaq', text: 'BAWSAQ 今日開盤平穩。分析師一致認為：「明天的事明天再說。」', h: [9, 16] },
  { who: 'weazel', text: '天氣預報：洛聖都今日陽光普照，空氣品質「還能呼吸」。', h: [6, 10], w: ['sunny', 'clear'] },
  { who: 'weazel', text: 'Weazel 民調：62% 的洛聖都市民認為交通是最大問題，另外 38% 正在塞車所以沒有回答。' },
  { who: 'globe', text: '社論：好麥塢標誌的翻修經費又追加了。我們的道路坑洞表示羨慕。' },
  { who: 'lspd', text: '提醒市民：在高速公路上倒車不是一種「抄捷徑」的方式。謝謝合作。' },
  { who: 'lspd', text: '本局今日共開出 412 張罰單，其中 3 張開給了真正的危險駕駛。其他的只是業績。' },
];

// What the player did -> Bleeter posts (and Lifeinvader statuses for the player). {zone} {vehicle} {shop} {amount} {name}
// {ticker} {company} {pct} {stars} are filled in.
window.EVENTS = {
  chase: [
    { who: 'lspd', text: '本局在{zone}一帶追捕一名危險嫌犯（{stars} 星通緝），嫌犯目前仍在逃。請民眾提供線索。', at: 0 },
    { who: 'c_driver', text: '剛剛在{zone}看到一台{vehicle}後面跟著一整排警車，我直接把車停到人行道上讓路 🚨', at: 6 },
    { who: 'c_grove', text: '{zone}那邊直升機在飛，好像在追人，最後好像讓他跑了？LSPD 又出包', at: 12 },
    { who: 'c_vinewood', text: '我在{zone}喝咖啡的時候，一台{vehicle}從我面前飛過去，後面跟著八台警車。今天值得了', at: 9 },
    { who: 'weazel', text: '警匪追逐！一名嫌犯駕駛{vehicle}在{zone}一帶甩開警方追捕，警方表示「正在檢討」。', at: 25 },
    { who: 'c_truth', text: '{zone}的追車又讓人跑了。LSPD 到底是抓不到，還是不想抓？', at: 40 },
  ],
  buyVehicle: [
    { self: true, text: '新玩具到手 🚗 感謝{shop}，錢包痛但心情好。', at: 5 },
    { self: true, text: '在{shop}刷了 {amount}。人生苦短，車要開好的。', at: 5 },
    { who: 'c_driver', text: '今天又有人在{shop}一口氣買了 {amount} 的東西。我開了二十年的計程車。', at: 30, bleeter: true },
  ],
  buyProperty: [
    { self: true, text: '剛在朝代 8 簽約買了新地方 🏠 {amount}，終於有自己的車庫了。', at: 10 },
    { self: true, text: '新的房地產入手。房價這麼高，我只能說：賺錢就是為了花掉。', at: 10 },
  ],
  hospital: [
    { self: true, text: '{name}的帳單 {amount}。我只是「稍微」受了點傷。美國醫療真棒。', at: 30 },
    { self: true, text: '今天在{name}醒來。護士說我的運氣很好，帳單說我的運氣很差（{amount}）。', at: 30 },
  ],
  arrest: [
    { self: true, text: '剛從{name}出來，保釋金 {amount}。我發誓那只是誤會。', at: 20 },
    { who: 'lspd', text: '本局今日逮捕一名嫌犯並移送{name}。嫌犯已繳交保釋金，本局提醒：遵守交通規則，從你我做起。', at: 5, bleeter: true },
  ],
  stockUp: [
    { who: 'bawsaq', text: '{ticker}（{company}）今日大漲 {pct}%，成交量放大。#BAWSAQ', at: 0 },
    { who: 'c_dad', text: '早知道就買 {ticker} 了。我每次都是「早知道」。', at: 50 },
  ],
  stockDown: [
    { who: 'bawsaq', text: '{ticker}（{company}）今日重挫 {pct}%，投資人恐慌性賣出。#BAWSAQ', at: 0 },
    { who: 'c_hipster', text: '{ticker} 跌了 {pct}%？我早就說它太主流了。', at: 45 },
  ],
};
