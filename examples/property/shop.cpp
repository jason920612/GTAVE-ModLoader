// The vehicle shop websites (Legendary Motorsport, Southern San Andreas Super Autos, Elitás Travel, Dock Tease,
// Pedal and Metal, Warstock Cache & Carry) on the loader's browser. Cars, bikes and bicycles are delivered to a
// garage of the player's properties; aircraft and boats to the character's own hangar, helipad or marina slip, the way
// the game's own websites do it (research/phase0.md §33).
//
// Pages call:
//   shop.list(site) -> { character, cash, ready, items: [ { item, name, maker, price, kind, photo } ],
//                        garages: [ { id, name, free } ], storage: { hangar, marina, helipad } }
//   shop.buy(item, garage) -> { ok, error, cash }
#define NOMINMAX
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "property.hpp"

namespace property::shop
{
	namespace
	{
		// ---- the catalogue: the game's story-mode web shop list (appinternet @2962860 item -> model, @2910122 kind,
		// @2830178 price), read offline. kind: 0 road vehicle, 1 plane, 2 boat, 3 helicopter.
		struct Item
		{
			int item;
			Hash model;
			int price;
			int kind;
		};
		const Item kItems[] = {
		    {0, 758895617u, 10000000, 0},
		    {1, 1545842587u, 1000000, 0},
		    {2, 1051415893u, 475000, 0},
		    {3, 2983812512u, 650000, 0},
		    {4, 3003014393u, 795000, 0},
		    {5, 3078201489u, 1000000, 0},
		    {6, 3861591579u, 490000, 0},
		    {7, 330661258u, 185000, 0},
		    {10, 3080461301u, 1150000, 1},
		    {11, 2172210288u, 250000, 1},
		    {12, 3650256867u, 240000, 1},
		    {13, 970356638u, 275000, 1},
		    {14, 621481054u, 1625000, 1},
		    {15, 744705981u, 1300000, 3},
		    {16, 2634305738u, 780000, 3},
		    {17, 782665360u, 1500000, 1},
		    {18, 1981688531u, 2000000, 0},
		    {19, 4244420235u, 1790000, 3},
		    {20, 788747387u, 1750000, 3},
		    {21, 321739290u, 225000, 1},
		    {22, 3471458123u, 450000, 1},
		    {24, 3251507587u, 413990, 2},
		    {25, 861409633u, 299000, 2},
		    {27, 400514754u, 196621, 2},
		    {29, 290013743u, 22000, 2},
		    {30, 3264692260u, 16899, 2},
		    {32, 4012021193u, 25160, 2},
		    {33, 1131912276u, 500, 0},
		    {34, 4108429845u, 1000, 0},
		    {35, 1127861609u, 2500, 0},
		    {36, 3061159916u, 2500, 0},
		    {37, 3894672200u, 2500, 0},
		    {38, 448402357u, 3000, 0},
		    {39, 3548084598u, 80000, 0},
		    {40, 3172678083u, 50000, 0},
		    {41, 2494797253u, 32000, 0},
		    {42, 3469130167u, 21000, 0},
		    {43, 3117103977u, 18000, 0},
		    {44, 418536135u, 440000, 0},
		    {45, 384071873u, 99000, 0},
		    {46, 338562499u, 240000, 0},
		    {47, 1032823388u, 120000, 0},
		    {48, 3249425686u, 85000, 0},
		    {49, 3253274834u, 90000, 0},
		    {50, 2299640309u, 145000, 0},
		    {51, 1126868326u, 16000, 0},
		    {52, 3105951696u, 45000, 0},
		    {53, 1909141499u, 24000, 0},
		    {54, 3164157193u, 25000, 0},
		    {55, 1123216662u, 250000, 0},
		    {56, 4289813342u, 205000, 0},
		    {57, 142944341u, 90000, 0},
		    {58, 2006918058u, 60000, 0},
		    {59, 2136773105u, 85000, 0},
		    {60, 3903372712u, 100000, 0},
		    {61, 3783366066u, 80000, 0},
		    {62, 4180675781u, 10000, 0},
		    {63, 1672195559u, 9000, 0},
		    {64, 3401388520u, 10000, 0},
		    {65, 4154065143u, 9000, 0},
		    {66, 2166734073u, 8000, 0},
		    {67, 3385765638u, 9000, 0},
		    {68, 788045382u, 7000, 0},
		    {69, 55628203u, 5000, 0},
		    {70, 2598821281u, 150000, 0},
		    {71, 2072687711u, 195000, 0},
		    {72, 108773431u, 55000, 0},
		    {73, 2833484545u, 130000, 0},
		    {74, 2360515092u, 118000, 0},
		    {75, 1737773231u, 136000, 0},
		    {76, 2196019706u, 1000000, 0},
		    {77, 2672523198u, 80000, 0},
		    {78, 837858166u, 1825000, 0},
		    {79, 2548391185u, 300000, 0},
		    {80, 2621610858u, 450000, 0},
		    {81, 2164484578u, 1000000, 0},
		    {82, 1283517198u, 550000, 0},
		    {83, 3581397346u, 500000, 0},
		    {84, 2222034228u, 525000, 0},
		    {85, 4174679674u, 15000, 0},
		    {86, 904750859u, 27000, 0},
		    {87, 3196165219u, 30000, 0},
		    {88, 2333339779u, 30000, 0},
		    {89, 4278019151u, 30000, 0},
		    {90, 2623969160u, 12000, 0},
		    {91, 4205676014u, 95000, 0},
		    {92, 301427732u, 15000, 0},
		    {93, 3101863448u, 65000, 0},
		    {94, 3403504941u, 10000, 0},
		    {95, 3728579874u, 95000, 0},
		    {96, 544021352u, 100000, 0},
		    {97, 37348240u, 90000, 0},
		    {98, 11251904u, 40000, 0},
		    {99, 3945366167u, 75000, 0},
		    {100, 92612664u, 40000, 0},
		    {101, 1488164764u, 50000, 0},
		    {102, 231083307u, 325000, 2},
		    {103, 2859047862u, 12000, 0},
		    {104, 2633113103u, 20000, 0},
		    {105, 3087195462u, 7000, 0},
		    {106, 3695398481u, 35000, 0},
		    {107, 2841686334u, 8000, 0},
		    {108, 989381445u, 45000, 0},
		    {109, 117401876u, 1150000, 0},
		    {110, 767087018u, 150000, 0},
		    {111, 2997294755u, 240000, 0},
		    {112, 408192225u, 500000, 0},
		    {113, 1341619767u, 950000, 1},
		    {114, 4152024626u, 275000, 0},
		    {115, 2891838741u, 725000, 0},
		    {116, 486987393u, 195000, 0},
		    {121, 1836027715u, 75000, 0},
		    {122, 80636076u, 35000, 0},
		    {123, 3703357000u, 80000, 0},
		    {124, 499169875u, 36000, 0},
		    {125, 3917501776u, 24000, 0},
		    {126, 1349725314u, 60000, 0},
		    {127, 873639469u, 60000, 0},
		    {128, 2485144969u, 12000, 0},
		    {129, 2391954683u, 26000, 0},
		    {130, 1069929536u, 23000, 0},
		    {131, 3505073125u, 70000, 0},
		    {132, 2519238556u, 35000, 0},
		    {133, 3005245074u, 9000, 0},
		    {134, 886934177u, 16000, 0},
		    {135, 3984502180u, 30000, 0},
		    {136, 2411098011u, 10000, 0},
		    {137, 2643899483u, 32000, 0},
		    {138, 1645267888u, 9000, 0},
		    {139, 3627815886u, 6000, 0},
		    {140, 2817386317u, 10000, 0},
		    {141, 1723137093u, 10000, 0},
		    {142, 1777363799u, 15000, 0},
		    {143, 3089165662u, 160000, 0},
		    {144, 1373123368u, 125000, 0},
		    {145, 75131841u, 200000, 0},
		    {146, 841808271u, 100000, 0},
		    {147, 3863274624u, 85000, 0},
		    {148, 3057713523u, 249000, 0},
		    {149, 1078682497u, 400000, 0},
		    {150, 1507916787u, 9000, 0},
		    {151, 4280472072u, 8000, 0},
		    {152, 699456151u, 11000, 0},
		    {153, 65402552u, 16000, 0},
		    {154, 3025077634u, 69000, 0},
		    {155, 2249373259u, 22000, 0},
		    {156, 3144368207u, 9000, 0},
		    {157, 3990165190u, 35000, 0},
		    {158, 736902334u, 96000, 0},
		    {159, 3286105550u, 55000, 0},
		    {160, 3449006043u, 742000, 0},
		    {161, 743478836u, 120000, 0},
		    {162, 165154707u, 1750000, 1},
		    {163, 1824333165u, 658000, 1},
		    {164, 3955379698u, 1500000, 3},
		    {165, 1011753235u, 395000, 0},
		    {167, 4135840458u, 92500, 0},
		    {168, 1265391242u, 82000, 0},
		    {169, 3205927392u, 448000, 0},
		    {171, 444171386u, 45000, 0},
		    {172, 941800958u, 680000, 0},
		    {173, 509498602u, 125000, 2},
		    {174, 1753414259u, 48000, 0},
		    {175, 296357396u, 65000, 0},
		    {176, 2186977100u, 375000, 0},
		    {177, 970385471u, 3000000, 1},
		    {178, 2434067162u, 1350000, 0},
		    {179, 2071877360u, 675000, 0},
		    {180, 2922118804u, 95000, 0},
		    {181, 410882957u, 525000, 0},
		    {182, 640818791u, 750000, 0},
		    {183, 2242229361u, 32500, 0},
		    {184, 2287941233u, 550000, 0},
		    {185, 4212341271u, 1950000, 3},
		    {186, 2198148358u, 950000, 0},
		    {187, 2694714877u, 2850000, 3},
		    {188, 1077420264u, 995000, 1},
		    {189, 2751205197u, 29000, 0},
		    {190, 3670438162u, 60000, 0},
		    {191, 1269098716u, 58000, 0},
		    {192, 2230595153u, 87000, 0},
		    {193, 3660088182u, 12000, 0},
		    {194, 1348744438u, 82000, 0},
		    {195, 1162065741u, 13000, 0},
		    {196, 3039514899u, 65000, 0},
		    {197, 1221512915u, 30000, 0},
		    {198, 2400073108u, 38000, 0},
		    {199, 3393804037u, 500000, 1},
		    {200, 1233534620u, 250000, 1},
		    {201, 3228633070u, 1325000, 2},
		    {202, 1039032026u, 42000, 0},
		    {203, 1923400478u, 71000, 0},
		    {204, 723973206u, 62000, 0},
		    {205, 3968823444u, 279000, 0},
		    {206, 3893323758u, 277000, 0},
		    {207, 3379262425u, 315000, 0},
		    {208, 349315417u, 230000, 0},
		    {209, 237764926u, 535000, 0},
		    {210, 729783779u, 49500, 0},
		    {211, 3705788919u, 37500, 0},
		    {212, 3188613414u, 350000, 0},
		    {213, 3663206819u, 385000, 0},
		    {214, 2728226064u, 975000, 0},
		    {215, 3080673438u, 10000000, 1},
		    {216, 1987142870u, 1950000, 0},
		    {217, 1075432268u, 5150000, 3},
		    {218, 3796912450u, 195000, 0},
		    {219, 1581459400u, 845000, 0},
		    {220, 2815302597u, 715000, 0},
		    {221, 349605904u, 225000, 0},
		    {222, 784565758u, 695000, 0},
		    {223, 1663218586u, 2200000, 0},
		    {224, 1070967343u, 1750000, 2},
		    {225, 2941886209u, 630000, 0},
		    {226, 3612755468u, 29000, 0},
		    {227, 2175389151u, 36000, 0},
		    {228, 525509695u, 32500, 0},
		    {230, 523724515u, 5500, 0},
		    {231, 3463132580u, 550000, 0},
		    {232, 2068293287u, 650000, 0},
		    {233, 1878062887u, 149000, 0},
		    {234, 634118882u, 247000, 0},
		    {235, 906642318u, 154000, 0},
		    {236, 2264796000u, 254000, 0},
		    {237, 4180339789u, 1650000, 0},
		    {238, 2634021974u, 995000, 0},
		    {239, 2351681756u, 585000, 0},
		    {240, 2809443750u, 116000, 0},
		    {241, 1489967196u, 208000, 0},
		    {242, 1102544804u, 695000, 0},
		    {243, 710198397u, 1667800, 3},
		    {244, 2623428164u, 2218000, 3},
		    {246, 972671128u, 375000, 0},
		    {247, 970598228u, 12000, 0},
		    {248, 3692679425u, 982000, 0},
		    {249, 2609945748u, 15000, 0},
		    {250, 464687292u, 30000, 0},
		    {253, 2449479409u, 2295000, 1},
		    {254, 1621617168u, 1995000, 3},
		    {255, 1475773103u, 130000, 0},
		    {256, 3989239879u, 555000, 0},
		    {257, 2999939664u, 1900000, 1},
		    {258, 2194326579u, 1250000, 2},
		    {259, 2364918497u, 900000, 0},
		    {260, 2123327359u, 2700000, 0},
		    {261, 1426219628u, 1750000, 0},
		    {262, 1274868363u, 610000, 0},
		    {263, 1203490606u, 253000, 0},
		    {264, 2537130571u, 695000, 0},
		    {265, 2465164804u, 1135000, 0},
		    {266, 234062309u, 1595000, 0},
		    {267, 3062131285u, 2475000, 0},
		    {268, 3517794615u, 701000, 0},
		    {269, 1887331236u, 816000, 0},
		    {270, 1549126457u, 155000, 0},
		    {271, 101905590u, 550000, 0},
		    {272, 3631668194u, 695000, 0},
		    {273, 683047626u, 250000, 0},
		    {274, 390201602u, 225000, 0},
		    {275, 86520421u, 95000, 0},
		    {276, 2191146052u, 1300000, 0},
		    {277, 3223586949u, 995000, 0},
		    {278, 741090084u, 120000, 0},
		    {279, 2067820283u, 1830000, 0},
		    {280, 482197771u, 841000, 0},
		    {281, 819197656u, 1097000, 0},
		    {282, 2154536131u, 16000, 0},
		    {283, 2035069708u, 264000, 0},
		    {284, 2688780135u, 100000, 0},
		    {285, 822018448u, 412000, 0},
		    {286, 2179174271u, 116000, 0},
		    {287, 3285698347u, 99000, 0},
		    {288, 3724934023u, 122000, 0},
		    {289, 6774487u, 210000, 0},
		    {290, 2890830793u, 145000, 0},
		    {291, 1873600305u, 48000, 0},
		    {292, 3889340782u, 2225000, 0},
		    {293, 3620039993u, 648000, 0},
		    {294, 4039289119u, 976000, 0},
		    {295, 3685342204u, 356000, 0},
		    {296, 3854198872u, 81000, 0},
		    {297, 1491277511u, 1995000, 0},
		    {298, 2771538552u, 67000, 0},
		    {299, 2736567667u, 378000, 0},
		    {300, 1026149675u, 195000, 0},
		    {301, 3676349299u, 95000, 0},
		    {302, 3005788552u, 55000, 0},
		    {303, 2452219115u, 47500, 0},
		    {304, 3982671785u, 3192000, 0},
		    {305, 2645431192u, 2553600, 0},
		    {306, 1180875963u, 1489600, 0},
		    {307, 2704629607u, 953360, 0},
		    {308, 682434785u, 1300000, 0},
		    {309, 2382949506u, 658350, 0},
		    {310, 941494461u, 3750000, 0},
		    {311, 989294410u, 3830400, 0},
		    {312, 2536829930u, 880000, 0},
		    {313, 272929391u, 1329000, 0},
		    {314, 2246633323u, 1189000, 0},
		    {315, 1034187331u, 1440000, 0},
		    {316, 4055125828u, 169000, 0},
		    {317, 627535535u, 135000, 0},
		    {318, 1886268224u, 599000, 0},
		    {319, 1234311532u, 1260000, 0},
		    {320, 2889029532u, 915000, 0},
		    {321, 719660200u, 430000, 0},
		    {322, 3312836369u, 705000, 0},
		    {325, 223240013u, 865000, 0},
		    {326, 1504306544u, 998000, 0},
		    {327, 1939284556u, 1535000, 0},
		    {328, 917809321u, 1670000, 0},
		    {329, 562680400u, 2325000, 0},
		    {330, 1897744184u, 850000, 0},
		    {331, 4262731174u, 1695000, 0},
		    {332, 884483972u, 2067669, 0},
		    {333, 3084515313u, 1585000, 0},
		    {334, 2413121211u, 1400000, 0},
		    {335, 159274291u, 1150000, 0},
		    {336, 433954513u, 1245000, 0},
		    {337, 3013282534u, 6500000, 0},
		    {338, 2531412055u, 665000, 0},
		    {339, 3545667823u, 2275300, 0},
		    {340, 3319621991u, 1596000, 0},
		    {341, 2594093022u, 3657500, 0},
		    {342, 3902291871u, 1130500, 0},
		    {343, 1043222410u, 4100000, 0},
		    {344, 2908775872u, 4455500, 0},
		    {345, 1565978651u, 4788000, 0},
		    {346, 1036591958u, 2653350, 0},
		    {347, 4262088844u, 4750000, 0},
		    {348, 4252008158u, 4123000, 0},
		    {349, 2310691317u, 2300900, 0},
		    {350, 3287439187u, 1296750, 0},
		    {351, 2771347558u, 2121350, 0},
		    {352, 1392481335u, 1034000, 0},
		    {353, 3296789504u, 2250000, 0},
		    {354, 1841130506u, 615000, 0},
		    {355, 2049897956u, 885000, 0},
		    {356, 3052358707u, 3750000, 0},
		    {358, 1483171323u, 5750000, 0},
		    {359, 886810209u, 2500000, 0},
		    {360, 2601952180u, 3125500, 0},
		    {361, 3602674979u, 1500000, 0},
		    {362, 2859440138u, 3850350, 0},
		    {363, 1181327175u, 4500000, 0},
		    {364, 1489874736u, 2500000, 0},
		    {365, 4081974053u, 2121350, 0},
		    {366, 447548909u, 3724000, 0},
		    {367, 1561920505u, 710000, 0},
		    {368, 2445973230u, 1500000, 0},
		    {369, 1741861769u, 500000, 0},
		    {370, 1104234922u, 650000, 0},
		    {371, 1871995513u, 485000, 0},
		    {372, 1352136073u, 761800, 0},
		    {373, 3981782132u, 1955000, 0},
		    {374, 2215179066u, 785000, 0},
		    {375, 600450546u, 625000, 0},
		    {376, 3884762073u, 866000, 0},
		    {377, 867799010u, 1420000, 0},
		    {378, 2765724541u, 975000, 0},
		    {379, 903794909u, 990000, 0},
		    {380, 2762269779u, 380000, 0},
		    {381, 15219735u, 535000, 0},
		    {382, 661493923u, 1145000, 0},
		    {383, 838982985u, 900000, 0},
		    {384, 3903371924u, 875000, 0},
		    {385, 4173521127u, 345000, 0},
		    {386, 1909189272u, 940000, 0},
		    {387, 1617472902u, 335000, 0},
		    {388, 3027423925u, 565000, 0},
		    {389, 931280609u, 360000, 0},
		    {390, 1046206681u, 1225000, 0},
		    {391, 3035832600u, 1675000, 0},
		    {392, 1115909093u, 830000, 0},
		    {393, 1031562256u, 2825000, 0},
		    {394, 3918533058u, 2515000, 0},
		    {395, 3308022675u, 725000, 0},
		    {396, 3160260734u, 1980000, 0},
		    {397, 2174267100u, 2305000, 0},
		    {398, 4080061290u, 790000, 0},
		    {399, 3306466016u, 145000, 0},
		};

		// What the scan found about an item: the shop (from the dictionary holding its picture), names, picture.
		struct Info
		{
			std::string site, name, maker, photo; // photo "dictionary/texture"
		};
		std::map<int, Info> g_info; // by item
		bool g_ready = false;

		// Every store dictionary with vehicle pictures (from the game files; research/phase0.md §33). Earlier entries win, so a
		// vehicle keeps the store that sells it in story mode; Warstock (candc_) comes last.
		const char* const kDictionaries[] = {"candc_default", "dock_default", "elt_default", "lgm_default", "pandm_default", "sssa_default", "dock_dlc_executive1",
		    "dock_dlc_heist4", "elt_dlc_2024_2", "elt_dlc_apartments", "elt_dlc_assault", "elt_dlc_battle", "elt_dlc_business",
		    "elt_dlc_executive1", "elt_dlc_heist", "elt_dlc_luxe", "elt_dlc_pilot", "elt_dlc_smuggler", "elt_dlc_sum2", "lgm_dlc_2023_01",
		    "lgm_dlc_2023_2", "lgm_dlc_2024_1", "lgm_dlc_2024_2", "lgm_dlc_2025_1", "lgm_dlc_2025_2", "lgm_dlc_2026_1", "lgm_dlc_apartments",
		    "lgm_dlc_arena", "lgm_dlc_assault", "lgm_dlc_battle", "lgm_dlc_biker", "lgm_dlc_business", "lgm_dlc_business2",
		    "lgm_dlc_casinoheist", "lgm_dlc_executive1", "lgm_dlc_gunrunning", "lgm_dlc_heist", "lgm_dlc_heist4", "lgm_dlc_importexport",
		    "lgm_dlc_lts_creator", "lgm_dlc_luxe", "lgm_dlc_pilot", "lgm_dlc_security", "lgm_dlc_smuggler", "lgm_dlc_specialraces",
		    "lgm_dlc_stunt", "lgm_dlc_sum2", "lgm_dlc_summer2020", "lgm_dlc_tuner", "lgm_dlc_valentines", "lgm_dlc_valentines2",
		    "lgm_dlc_vinewood", "lgm_dlc_xmas2017", "lgm_dlc_xmas2022", "pandm_dlc_2023_01", "sssa_dlc_2023_01", "sssa_dlc_2023_2",
		    "sssa_dlc_2024_1", "sssa_dlc_2024_2", "sssa_dlc_2025_1", "sssa_dlc_2025_2", "sssa_dlc_2026_1", "sssa_dlc_arena", "sssa_dlc_assault",
		    "sssa_dlc_battle", "sssa_dlc_biker", "sssa_dlc_business", "sssa_dlc_business2", "sssa_dlc_casinoheist", "sssa_dlc_christmas_2",
		    "sssa_dlc_christmas_3", "sssa_dlc_executive_1", "sssa_dlc_halloween", "sssa_dlc_heist", "sssa_dlc_heist4", "sssa_dlc_hipster",
		    "sssa_dlc_independence", "sssa_dlc_lts_creator", "sssa_dlc_mp_to_sp", "sssa_dlc_security", "sssa_dlc_smuggler", "sssa_dlc_stunt",
		    "sssa_dlc_sum2", "sssa_dlc_summer2020", "sssa_dlc_tuner", "sssa_dlc_valentines", "sssa_dlc_vinewood", "sssa_dlc_xmas2017",
		    "sssa_dlc_xmas2022", "sssa_mp_to_sp", "candc_2023_01", "candc_2023_2", "candc_apartments", "candc_assault", "candc_battle",
		    "candc_casinoheist", "candc_chopper", "candc_dlc_2024_1", "candc_dlc_2024_2", "candc_dlc_2025_1", "candc_dlc_2025_2",
		    "candc_dlc_2026_1", "candc_executive1", "candc_gunrunning", "candc_hacker", "candc_heist4", "candc_importexport", "candc_smuggler",
		    "candc_sub", "candc_truck", "candc_xmas2017", "candc_xmas2022"};

		std::string SiteOfDictionary(const std::string& d)
		{
			static const std::pair<const char*, const char*> sites[] = {
			    {"lgm_", "legendary"}, {"sssa_", "ssasa"}, {"elt_", "elitas"}, {"dock_", "docktease"}, {"pandm_", "pandm"}, {"candc_", "warstock"}};
			for (const auto& [prefix, site] : sites)
				if (d.starts_with(prefix))
					return site;
			return {};
		}
		// Items without a picture: by what they are.
		std::string SiteOfKind(const Item& i)
		{
			if (i.kind == 1 || i.kind == 3)
				return "elitas";
			if (i.kind == 2)
				return "docktease";
			switch (VEHICLE::GET_VEHICLE_CLASS_FROM_NAME(i.model))
			{
			case 13: return "pandm";
			case 19: return "warstock";
			case 6:
			case 7: return "legendary";
			default: return "ssasa";
			}
		}

		// Pictures named differently from their model.
		std::string Alias(const std::string& label)
		{
			static const std::map<std::string, std::string> aliases = {
			    {"sentinel2", "sentinel_convertable"}, {"stalion", "stallion"}, {"submers2", "sub2"}};
			const auto it = aliases.find(label);
			return it == aliases.end() ? std::string() : it->second;
		}

		std::filesystem::path CacheFile()
		{
			return std::filesystem::path(ml::Context().dataDir) / "shop_catalog.txt";
		}
		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		void Publish(std::map<int, std::pair<std::string, std::string>>& found);

		// Which dictionary holds a picture of each model: every dictionary is loaded once and its pictures looked up by
		// model name. Cached in data\shop_catalog.txt: "v4 <items>", "retry <dictionaries that did not load>", then
		// "item site dictionary/texture" lines. Dictionaries that did not load (some belong to packs story mode never
		// mounts) are tried again on the next start for the items still without a picture. Runs as a task: its waiting
		// holds nothing else up, and a cached catalogue is usable before the retry ends.
		void Scan()
		{
			std::map<int, std::pair<std::string, std::string>> found; // item -> site, "dictionary/texture" ("" = none)
			std::vector<std::string> order;
			{
				std::ifstream in(CacheFile());
				std::string header, retry;
				if (std::getline(in, header) && header == std::format("v4 {}", std::size(kItems)) && std::getline(in, retry))
				{
					std::istringstream words(retry.substr(std::min<size_t>(retry.size(), 6)));
					for (std::string dict; words >> dict;)
						order.push_back(dict);
					for (int item; in >> item;)
						if (std::string site, photo; in >> site >> photo)
							found[item] = {site, photo == "-" ? "" : photo};
				}
			}
			const bool full = found.size() != std::size(kItems);
			if (full)
			{
				found.clear();
				order.assign(std::begin(kDictionaries), std::end(kDictionaries));
			}
			else
				Publish(found);
			if (order.empty())
				return;

			// Model names (base-game models have none in the loader's list; their display label is used instead).
			std::map<Hash, std::string> names;
			ml::WaitUntil([&] {
				bool ready = false;
				for (const auto& m : ml::Models(ML_MODEL_VEHICLE, &ready))
					names[m.hash] = Lower(m.name);
				return ready;
			}, 300000, 500);

			// A dictionary that does not load in time is tried again at the end with a longer wait (large ones such as
			// lgm_default can take a while right after the game loads).
			std::vector<std::string> failed;
			const size_t first = order.size();
			for (size_t n = 0; n < order.size(); ++n)
			{
				const std::string name = order[n];
				const bool retry = n >= first;
				const auto dict = ml::LoadTextureDict(name.c_str(), retry ? 30000 : 5000);
				if (!dict)
				{
					(retry ? failed : order).push_back(name);
					continue;
				}
				for (const Item& i : kItems)
				{
					if (found.contains(i.item) && !found[i.item].second.empty())
						continue;
					// The picture is named after the model.
					const std::string model = names.contains(i.model) ? names[i.model] : std::string(),
					                  label = Lower(VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(i.model));
					for (const std::string& texture : {model, label, label + "_tless", Alias(label)})
						if (!texture.empty())
							if (const Vector3 size = GRAPHICS::GET_TEXTURE_RESOLUTION(name.c_str(), texture.c_str()); size.x > 0)
							{
								found[i.item] = {SiteOfDictionary(name), name + "/" + texture};
								break;
							}
				}
			}

			for (const Item& i : kItems)
				if (!found.contains(i.item))
					found[i.item] = {SiteOfKind(i), ""};
			std::ofstream out(CacheFile(), std::ios::trunc);
			out << std::format("v4 {}\nretry", std::size(kItems));
			for (const std::string& dict : failed)
				out << ' ' << dict;
			out << '\n';
			for (const auto& [item, sp] : found)
				out << item << ' ' << sp.first << ' ' << (sp.second.empty() ? "-" : sp.second) << '\n';
			const auto missing = std::ranges::count_if(found, [](const auto& f) { return f.second.second.empty(); });
			ml::Log("shop: catalogue scanned ({}, {} items, {} without a picture, {} dictionaries did not load)", full ? "full" : "retry",
			    found.size(), missing, failed.size());
			Publish(found);
		}

		void Publish(std::map<int, std::pair<std::string, std::string>>& found)
		{
			for (const Item& i : kItems)
			{
				Info info;
				info.site = found[i.item].first;
				info.photo = found[i.item].second;
				const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(i.model);
				info.name = Text(label);
				if (info.name.empty())
					info.name = label ? label : "";
				info.maker = Text(VEHICLE::GET_MAKE_NAME_FROM_VEHICLE_MODEL(i.model));
				g_info[i.item] = info;
			}
			g_ready = true;
		}

		// ---- storage of the character's own (hangar, marina, helipad) -------------------------------------

		// The save's vehicle generators: hangar 12..14, marina 15..17, helipad 18..20, by character (appinternet @2957967).
		bool HasStorage(int kind, int c)
		{
			const int first = kind == 1 ? 12 : kind == 2 ? 15 : kind == 3 ? 18 : -1;
			const ml::Global gens = ml::Global(114990) + 32759;
			// Bit 5: the character owns the property (vehicle_gen_controller sets it from the property's owner).
			return first >= 0 && c >= 0 && first + c < gens.Size() && gens.At(first + c).Bit(5);
		}

		// The game's own "store a bought vehicle": appinternet @2941169(item, character, &Global 77590, -1), found by its
		// first bytes.
		constexpr const char* kStorePattern = "2d 04 6f 00 00 38 03 70 58 09 00 43 25 01 71 71";
		std::string StoreOwnVehicle(int item, int c)
		{
			const int64_t result = reinterpret_cast<int64_t>(ml::Global(77590).Ptr());
			switch (ml::scripts::RunFunction("appinternet", kStorePattern, {item, c, result, -1}, 4000))
			{
			case ml::scripts::RunResult::Ran:
				ml::Log("shop: item {} stored for character {} by the game", item, c);
				return {};
			case ml::scripts::RunResult::Timeout:
				return "unsupported";
			default:
				ml::LogError("shop: the game's vehicle storage function was not found (game update?); aircraft and boats cannot be bought");
				return "unsupported";
			}
		}

		const Item* Find(int item)
		{
			for (const Item& i : kItems)
				if (i.item == item)
					return &i;
			return nullptr;
		}

		ml::Json List(const std::string& site)
		{
			const int c = ml::game::CharacterIndex();
			ml::Json items = ml::Json::Array(), garages = ml::Json::Array();
			if (g_ready)
				for (const Item& i : kItems)
					if (const Info& info = g_info[i.item]; info.site == site)
						items.Push({{"item", i.item}, {"name", info.name}, {"maker", info.maker}, {"price", i.price}, {"kind", i.kind}, {"photo", info.photo}});
			for (const int id : Owned(c))
				garages.Push({{"id", id}, {"name", Name(id)}, {"free", garage::FreeSlots(id)}});
			return {{"character", c}, {"cash", ml::game::Cash()}, {"ready", g_ready}, {"items", items}, {"garages", garages},
			    {"storage", {{"hangar", HasStorage(1, c)}, {"marina", HasStorage(2, c)}, {"helipad", HasStorage(3, c)}}}};
		}

		ml::Json Buy(int item, int garageId)
		{
			const auto fail = [](const char* error) { return ml::Json{{"ok", false}, {"error", error}, {"cash", ml::game::Cash()}}; };
			const Item* i = Find(item);
			const int c = ml::game::CharacterIndex();
			if (!i)
				return fail("unknown");
			if (c < 0)
				return fail("character");
			if (ml::game::Cash() < i->price)
				return fail("money");
			if (i->kind == 0)
			{
				if (!Owned(c).contains(garageId) || garage::FreeSlots(garageId) <= 0)
					return fail("garage");
				if (!garage::Deliver(garageId, i->model))
					return fail("model");
			}
			else
			{
				if (!HasStorage(i->kind, c))
					return fail("storage");
				if (const std::string error = StoreOwnVehicle(item, c); !error.empty())
					return fail(error.c_str());
			}
			ml::game::AddCash(-i->price);
			NotePurchase();
			ml::Log("shop: character {} bought item {} ({:08X}) for ${}", c, item, i->model, i->price);
			return {{"ok", true}, {"error", ""}, {"cash", ml::game::Cash()}};
		}
	}

	void Start()
	{
		ml::web::Function("shop.list", [](std::string site) { return List(site); });
		ml::web::Function("shop.buy", [](int item, int garage) { return Buy(item, garage); });
		ml::StartTask([] {
			ml::WaitUntil([] { return ml::game::CharacterIndex() >= 0; }, UINT32_MAX, 500);
			Scan();
		});
	}
}
