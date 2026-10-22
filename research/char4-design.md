# Officer as a Real 4th Protagonist: Design Document (revision 2)

Status: revision 2 plus the user's decisions of 2026-10-22 (§0.0); summary in Chinese in research/phase0.md §55. Survey data, analyzers and dumps: `D:\Projects\gtav-research\char4` (game-derived, never committed). Scope: the story-mode police officer (examples/police), played on `mp_m_freemode_01` (0x705E61F2) or `mp_f_freemode_01`.

Inputs:
- seven surveys: identity gates, per-character globals, saving, engine, gameplay inventory, script-rewrite technique, switching and integration;
- two critiques of revision 1. Every item in them was re-checked against the local disassembly, the globals dump, the exe dump and the repo. Accepted items are merged below. Rejected or corrected items are listed in the last appendix.
- spot checks made for this document, marked **(checked)**.

Evidence notation:
- `script@addr`: Enhanced disassembly in `D:/Projects/gtav-research/dis_enhanced`;
- `G<n>`: script global index;
- `0x…`: RVA in `GTA5_Enhanced.dump.exe`;
- `file:line`: repo `D:/Projects/GTAVE-ModLoader`;
- thread offsets follow `src/loader/game/scripts.cpp:23`: +0x10 program hash, +0x18 state, +0x1C ip, +0x60 stack size, +0x74 call depth, +0x154 name. Inside the interpreter, r12 = thread+8.

"Guess" marks anything not verified. "Unverified" marks a claim taken from a survey or critique that was not re-checked.

### 0.0 User decisions (2026-10-22)

All four are the recommended options; the sections below already reflect them.

| # | Question | Decision | Effect on the design |
|---|---|---|---|
| Q1 | Does the officer take part in the story? | **No.** He owns his economy, vehicles, phone, skills and wheel slot; missions, protagonist calls and texts, friends and family scenes stay with the three protagonists. | D3/D8 as written: story scripts stay officer-blind. |
| Q2 | Autosaves while the officer is played | **Deferred to the next switch to a protagonist.** No timed brokered autosave. The officer menu has "save now". | §3.9 autosave policy as written; the optional timed autosave is dropped. |
| Q3 | GTA Online while the officer slot is enabled | **Online is refused.** This already holds: the loader runs only with `-nobattleye` (`src/loader/dllmain.cpp:53-57`, otherwise idle, so no pack is mounted), and without BattlEye the game refuses Online (pause-menu tab 42 shows "BattlEye required"; research/phase0.md §2 line 80: the Online flow is blocked by BattlEye, tested). Playing Online = starting the game without `-nobattleye`, which leaves the loader and the generated pack out. | D9 simplified: the Online-equivalence work (S8 as a release gate, D4 differential, U31 Online session) is dropped. The transition-signal teardown stays as a safety net for a refused Online attempt (U31 rewritten). |
| Q4 | Paid activities (strip clubs, prostitutes, golf, tennis, darts, range, hunting, towing, races) | **All usable, paid from the officer's own account.** Taxi, car wash, vending, fairground, cinema, cable car and telescope likewise. | Every paid free-roam script found by U9 becomes officer-aware ("light": identity + payment wrapper + account 3); relationship data (stripper/prostitute "met" flags, PROSTITUTE_SAVED_DATA) stays NONE. U9 now only maps the sites. Later, the discipline system may treat off-duty offences (e.g. soliciting) as misconduct. |

---

## 0. Decisions at a glance

| # | Decision | Main reason |
|---|---|---|
| D1 | The officer's character value is **3** (`OFFICER = 3`). | It is the only value that fits every existing 4-wide structure: selector, switch struct, phone. Gates and loops can be changed with 1-byte edits. It does not collide with real contacts or with the "no character" sentinels. No G2349 entry has bank field 17 = 3 (checked), so account 3 is free. |
| D2 | **The officer's player identity is computed in each script and never stored.** In officer-aware scripts, `get_cur4` returns 3. G122220/1/2 (PP_CURRENT/PREVIOUS/LAST_KNOWN_PED, all saved) keep their vanilla meaning: 145 while the officer is played, the last protagonist in +4322/+4323. **Exception, already true today:** the phone owner G21666 is 3 in every script while the officer is played. This is a separate "owner" domain with its own 4-wide arrays (§3.5). | The vanilla `update_cur` writes the new character into +4322/+4323 and refreshes +4323 = cur. A naive patch would leak 3 into globals that 1,600+ sites in unpatched scripts read, and into the save (§0.1 item 6). |
| D3 | **Two classes of scripts plus safety patches.** *Officer-aware* scripts are rewritten and see the officer as 3: roughly 20–40 SP scripts at the end of the plan (shops, money, ATM, switching, respawn, services). *Officer-blind* scripts are all the others: story, missions, comms, friends, scenes, saving. They keep seeing 145, as today. Some blind scripts get **safety patches** only: they are not officer-aware, but they crash or leak today, for example appcontacts G24150[owner] and mission_triggerer_d's unguarded account read. | Identity suppression comes for free in blind scripts, because they already treat 145 as "not a protagonist". Work is limited to the systems the officer must own. |
| D4 | **Per-character data:** the vanilla 3-element storage is never moved or resized. The officer gets a **shadow 4th element** in a new script global block 13 (G3407872+). Index 3 reaches the shadow through a **detour on the VM's shared bounds-failure block**. The detour serves an access only when the opcode is an ARRAY opcode, **the thread belongs to an officer-aware program** (program hash at thread+0x10), and the base is whitelisted. The fallback is the per-site CALL stubs proposed by the technique survey. | The detour only acts where vanilla would have killed the thread. Thread gating keeps blind threads byte-for-byte vanilla, including their crashes (§3.3). One mechanism covers pointer parameters, nested arrays and copies of the shared library. |
| D5 | **Logic changes:** officer-aware scripts are rewritten at loader start-up into a generated replacement pack. Every modified function F gets an appended **F′ that starts with an `officer_mode()` test and falls back to an appended vanilla copy V**. The original body becomes a forwarder. In-place edits are allowed only when they cannot change behaviour on vanilla data. Every character-decision site in an officer-aware script gets an explicit, recorded decision, and the build refuses a script that has an undecided site. | This is the completeness argument (§3.5). With `officer_mode()` false (Online, block 13 not ready, officer disabled), an aware script behaves like vanilla. |
| D6 | **Engine:** while the officer is the player, protagonist stats slot k = 0 is aliased to an "SP3" character, using only data writes: model static, stat prefix, 187 stat-id objects. SP3_* stats are declared in our generated DLC pack. Whether they are SP-saved or mirrored into block 13 is decided by U6/U18 (§3.7). | HUD cash, pause header cash, street pickups, the respray charge, skills and the ability meter all follow the officer automatically, and no code patch is needed. CHANGE_PLAYER_PED's per-protagonist routine also runs for the officer under the alias (§2.3, U29). |
| D7 | **Persistence:** the officer's data is saved in its **own named save block**, owned by our registration script `ml_char4_reg`, inside the game's own save file. **Vanilla-clean invariant:** in v1 the engine never writes a save while the officer is the player or holds an officer value in a vanilla field. A save broker handles every save path, including the benchmark and mission-repeat saves. **Autosaves requested while the officer is the player are deferred and merged into one brokered save at the next switch to a protagonist** (§3.9). | The user's 100% save (SGTA50001, slot 03) and every other save stay loadable without the mod. Officer data follows each slot, autosave and cloud copy. No screen blackout on every purchase. |
| D8 | The officer **owns** economy and personal state, **shares** world services and collectibles, and has the **story identity suppressed** (§1, §4). | This is the user's goal. |
| D9 | **GTA Online is refused while the officer slot is enabled (user decision Q3), and this already holds.** The loader, and therefore the generated pack, only runs with `-nobattleye`, and without BattlEye the game refuses Online (§0.0). Kept as defence in depth: (a) every `*4` helper and every F′ tests `officer_mode()`, which is false in a network game; (b) in-place edits stay limited to ones that cannot be told apart from vanilla on vanilla data; (c) at the **first transition signal** (a refused Online attempt still starts the transition scripts) the loader tears the officer session down: restores G2349[3], G2349[145], G143050[145] and removes the alias. | The loader's Online pause (`core.cpp:199-207`, `:236-240`, `:277-278`) stops mods and native overrides, not rewritten bytecode; BattlEye being off is what keeps the pack out of Online. |
| D10 | **SDK compatibility:** `ml::game::CurrentCharacter()` / `CharacterIndex()` keep returning −1..2. Character 3 is visible only through a new opt-in API that is version-checked through MLApi. Officer payments run an officer-aware copy of the bank function compiled into `ml_char4_reg`. | Existing mods index fixed `[3]` C++ arrays with the character (23 call sites). Returning 3 would make them write out of bounds inside the game process (§3.9). |
| D11 | **Failure policy for aware threads:** in development builds an unserved index 3 kills the thread and logs the details, as in vanilla. In release builds it reads and writes a zeroed per-array scratch element, logs an ERROR, turns `officer_mode()` off for the rest of the session (the officer falls back to 145 behaviour) and shows a warning. | respawn_controller is restarted by main whenever the player is injured or arrested. Killing it inside its own sequence risks a kill/relaunch loop (guess). Degrade-with-warning policy. |

### 0.1 Contradictions between surveys, resolved

| # | Claim | Finding | Evidence |
|---|---|---|---|
| 1 | The phone owner G21666 falls back to 0 (Michael) for a non-protagonist (per-character globals survey). | **Only in Director Mode.** On the normal path, get_cur maps 145→3, G80362→3 and >3→3, so the officer is phone owner 3 today. | gunclub_shop@998687 `PUSH_U8 14; CALL @46577; JZ @998800` (mode 14 branch at @998709–@998794, fallback 0 at @998793); normal path @998800–@998838 **(checked)** |
| 2 | The hospital fee as the officer goes to Michael (inventory survey). | **No fee is charged for 145.** Fee = cash stat(char)/100×5, clamped to 0..5000, and @48484 returns 0 when the SWITCH has no case. The charge call @48074 runs only when the fee is > 0. | respawn_controller@20594–@20677, @48484 SWITCH 0/1/2 → default `PUSH 0` **(checked)** |
| 3 | The Michael-account leak through entry 145 is real for fixed-price services. | **Confirmed.** Car wash: get_cur → wrapper → account = G2349[c].17. Entry 145 has field 17 = 0, which is Michael's account. Entries 162 and 222 do too. | carwash1@40944–@40989; gread G6572 (=2349+1+145·29+17) = 0, G7065 ([162]) = 0, G8805 ([222]) = 0 **(checked)** |
| 4 | respawn_controller wipes WEAPONS[char] on death (inventory survey). | **On arrest.** The loop runs after `CALL @48025`, which is true when the last arrest is newer than the last death. With 145 it indexes WEAPONS[145] and the thread dies (unverified in game). | respawn_controller@20681–@20800, @48025 **(checked)** |
| 5 | G80145 [3]×14 is a per-character outfit array (identity survey). | **A scratch buffer for query results.** Elements 0 and 1 are identical in the dump, and code fills it by STORE_N after a query call. It is excluded from the catalog. | gread G80146..G80173; yoga@4374 `GLOBAL_U24 80145; ARRAY_U8 14; STORE_N` after `CALL @49063` **(checked)** |
| 6 | Patching `is_playable` (<3 → <4) plus the ped_to_char loop bound is enough for identity (identity and technique surveys). | **Not enough.** update_cur writes +4322 = cur and +4323 = new when they are playable, and its "model unchanged" path writes +4323 = cur whenever cur ≠ 145. With 3 counted as playable, 3 leaks into the saved PP_PREVIOUS/LAST_KNOWN_PED, which blind scripts read. | gunclub_shop@46380–@46503 and @46512–@46553 **(checked)** |
| 7 | The officer value: 3 (identity survey), keep 145 (inventory survey), 162/222 (switching survey), "decide by experiment" (technique survey). | **3.** 162 and 222 are in the "no character" set {144,145,161,162,222,223} (player_controller_b@634088). The VM's IS_BIT_SET is `bt reg` (bit index mod 32), so 162 would test bit 2, which is Trevor's bit. Entry 3 is the MP placeholder ("多人遊戲", model 0x7E0961B8, bank 4, 118[3].8 = 198). No owner's phonebook lists entry 3 (its fields 13–16 = 0). No script indexes G2349 or G118 *entries* with a constant 3. **Correction:** constant owner index 3 does occur inside per-entry owner arrays (appinternet@2582102–2582140; see §0.2 item 10). | gread G2437–G2465, G149–G158 **(checked)**; rg over all 1,151 scripts: 0 `PUSH 3; GLOBAL_U16 2349/118; ARRAY_U8` **(checked)**; exe 0x920720 |
| 8 | The 2349 field layout: "12–16 int[4]". | Fields 12, 19 and 24 are the **count slots** of int[4] arrays; the elements are at 13–16, 20–23 and 25–28. The bank account is field 17. | gread entry 3: G2449 = 4 (count), G2450–2453 = 0, G2454 = 4 (bank) **(checked)** |
| 9 | Data access must be rewritten at each of about 204k sites (technique survey). | **Not required.** All six ARRAY handlers fall into one failure block with the base, index, ip and VM stack still in registers. One detour can serve the shadow element (§3.3). **Correction:** the block is shared with two non-array failures (0x920418, 0x920466), so the detour must dispatch on the opcode byte. | 0x91ee10: `mov rax,[r15]; mov ecx,[r15-8]; cmp ecx,[rax]; jnb 0x91ee3d`; continuation 0x91ee1b computes `rax+(idx·size+1)·8`; 0x92076e sets `[r12+0x10]=2` (= thread+0x18) **(checked)** |
| 10 | Save layout counts: 33,638 rows (per-character globals survey), 30,804 values (saving survey), 24,024 fields (inventory survey). | These are different units (scopes counted, values counted, emulator coverage). This does not affect the design. Use the saving survey's `names.tsv` as the reference. | — |

### 0.2 Corrections after review

| # | Revision-1 claim | Correction | Evidence |
|---|---|---|---|
| 1 | "G2349[145].17 := 4 … every blind payment path skips … ships immediately." | All 119 reads of G2349[c].17 in the 1,151 scripts are the one-line account getter. Its callers are guarded (`==3`/`==4` → skip) except flow_controller@585224, whose character is first checked by `is_playable` (@585133 → @5463, so it is never 145), and **mission_triggerer_d@502327 / @506478**. Those two pass the account straight to `G62254[acct]`. With 145.17 = 4 the read becomes G62254[4] (count 3), and the story script mission_triggerer_d is killed. The ship-now fix is therefore made conditional (§3.2). | mission_triggerer_d@502314–502344 (`PLAYER_PED_ID; CALL @4770; CALL @506526; STATIC_U16_STORE 1310; …; CALL @35981 = G62254[x]`), reached from @501163 / @507011 / @507099 when the player sits in a driveable Gauntlet (@501151 `IS_VEHICLE_MODEL -1800170043`) and G80626 ∉ {80,81,82} (@501064–501095) **(checked)**; scan script `D:\Projects\gtav-research\char4\rev\acct2.py` **(checked)** |
| 2 | The officer owns his bank account and log through 2349[3].17 = 3; the 145 hardening fixes the Michael leak. | The ATM and the phone's Internet bank page get the account from the **saved** sheet **G143050[c].17**. These are the only 2 such reads in all scripts. Today the officer sees Michael's balance and log there (G143050[145].17 = 0). With account 3 they would refuse to open (G143050[3].17 = 4). | atm_trigger@9721–9755 (get_cur @12293; `GLOBAL_U24 114990; IOFFSET_S16 28060; ARRAY_U8 29; IOFFSET_U8_LOAD 17`; returns on 3/4), balance G62254[acct] @9950, log G135796[acct] @9990+, account-name SWITCH 0/2/1 @9788; appinternet@3077235–3077304 same; gread G147273 = 0, G143155 = 4, G143050 = 162 **(checked)** |
| 3 | "The value 3 for the player exists only inside officer-aware scripts." | True for the *player character*. But the phone owner G21666 is already 3 in blind phone scripts, and 1,206 sites use `GLOBAL_U16_LOAD 21666` directly as an array index. 1,205 of them index 4-wide arrays. One indexes **G24150 [3]**: appcontacts writes G24150[owner] when the first contact entry is confirmed. That crash exists today (prediction). | appcontacts@41028–41041; indexed arrays: G10341 ×781, CELLPHONE_SETTINGS G129050 (=114990+14060) ×417, G10310 ×2, G10212 ×2, G10217, G10195 (all [4]), G24150 [3] ×1 (its only use anywhere); gread G24150 = 3 **(checked)** |
| 4 | D9: "patched scripts must be inert … the loader already pauses mods in Online." | The pause covers mod code and native overrides only. The replacement pack is a persistent overlay item, and no Online check exists in the pack code. See D9 and §3.4. | `src/loader/core.cpp:203-207` SetOnline, `:236-240` freemode thread, `:277-278` `ApplyNativeOverrides(!state::online)`; `convert/replace.cpp:559-566` (`overlay`, `persistent`) **(checked)** |
| 5 | ml::save v2: "`story::Character()` returns 3 when the officer is the player." | No `story::Character()` exists. The SDK has `ml::game::CurrentCharacter()` / `CharacterIndex()` (by model, −1..2). Mods index `[3]` C++ arrays with them. `ml::game::Pay` runs appinternet's (blind) bank function. See D10 and §3.9. | `sdk/include/modloader/game.hpp:118-178`; `examples/bank/main.cpp:158 g_accounts[3]`, `examples/property/main.cpp:122-123`, `examples/property/garage.cpp:229-230`, `examples/social/main.cpp:122`, `examples/social/gen.cpp:59`; 23 `CharacterIndex()/CurrentCharacter()` sites **(checked)** |
| 6 | Hash-triple example: "bank spending stats: the account SWITCHes in @854401". | The first account SWITCH is not a pure triple: case 0 also calls `@231930(99,1)`. The cash sync at function entry (@856918) is unrolled with constant indices 0/1/2: it has no SWITCH and no loop. Both need explicit rules (new UNROLLED class, §3.4). The real cash triples are the write-back @856578 and the getter @857075. | gunclub_shop@854444 SWITCH (SP0_MONEY_TOTAL_SPENT = −656546900); @856918–857032 (runs only if NETWORK_IS_SIGNED_IN); @855915 `CALL @856578` (G62254[acct] → SPn_TOTAL_CASH, SWITCH 0/1/2); @857075 STAT_GET_INT TOTAL_CASH SWITCH **(checked)** |
| 7 | Broker: overrides on "SET_SAVE_MENU_ACTIVE, DO_AUTO_SAVE and the mission-repeat save native", i.e. "every save native". | The benchmark writes a repeat save through **QUEUE_MISSION_REPEAT_SAVE_FOR_BENCHMARK_TEST**. This is a different native, used only in benchmark. The benchmark then forces Michael's model and reloads the repeat save. | benchmark@11978 (gated by G114990+9094 = G124084, which is 1 in the dump; meaning unknown); @13570–13621 `SET_PLAYER_MODEL(225514697)` when the model is not a protagonist (@13658); QUEUE_MISSION_REPEAT_LOAD @501391 **(checked)** |
| 8 | Making shop_controller aware opens Ammu-Nation, while clothes, barber and tattoo shops stay NONE and LS Customs waits for M4. | shop_controller has a **single** character gate for every shop type. With every `is_playable` retargeted, all SP shops would start for the officer. | shop_controller@96425–96447 inside @96110 (params: shop index, flags pointer): `STATIC_U16_LOAD 369; CALL @81566 (c<3)`; "not SP character"; no shop-type term **(checked)** |
| 9 | §5.4: never save into slot 02 (autosaves), while the plan tests autosaves as the officer. | These contradict each other. §5.4 now has an explicit backup/restore procedure. | — |
| 10 | "The owner-3 phonebook column is 0 for all 223 runtime and 162 saved entries." | True for field 16 (phonebook) and field 23. In the third per-owner array (count field 24, elements 25–28), entry 96 has owner-3 = 1 in both the runtime and saved sheets. appinternet has owner-3 contact operations on contacts 147 and 82. Whether that code runs in SP or MP was not determined. | gread G5159–G5162 = 1,1,1,1; G5147–5150 = 2,2,2,0; full scan of G2349/G143050 **(checked)**; appinternet@2582102–2582140 `PUSH_U8 147/82; PUSH 3; CALL @2354471/@2353725/@2587335` **(checked)** |
| 11 | "The six ARRAY handlers fall into one failure path"; U16 continuations "in 0x91f140/0x91f180/0x91f1c0". | The failure block 0x91ee3d is also reached from 0x920418 and 0x920466 (guess: THROW and CALLINDIRECT with a null target). The U16 continuations are 0x91f14f / 0x91f18f / 0x91f1cf. The block's first instruction is a 6-byte RIP-relative `mov eax,[rip+…]`. There is a single interpreter (the pattern `49 8B 07 41 8B 4F F8 3B 08` has 6 hits, all in 0x91dd30). | `relocate func/pattern` output `D:\Projects\gtav-research\char4\rev\vmfunc.txt` **(checked)** |
| 12 | The bank function's first parameter is "character". | It is the **account**: wrappers pass `getter(c)` = G2349[c].17, and the function uses LOCAL 0 as the account (`LOCAL_U8_STORE 9` at @855416, then G62254[local9]). For protagonists account = character, so nothing changes for them. For the officer, account 3 must be passed. | carwash1@40977–40989; gunclub_shop@855414–855416 **(checked)** |

---

## 1. Goal and definition

**"Real 4th protagonist"** means that the game's own systems serve the officer as a character with his own state, the same way they serve Michael, Franklin and Trevor:

- his own money, account and HUD cash, including the ATM and the bank web page;
- his own purchases and weapons, persisted across switches and saves;
- his own skills and stats, his own slot on the wheel, and his own last-known state;
- **no** protagonist identity: no protagonist calls, texts, missions, friends, phone contacts, scenes, safehouses or businesses.

**Enum value:** `OFFICER = 3`. Entry 3 of the runtime character sheet G2349 (and G118) is taken over at runtime. The saved character sheet G143050[3] is never modified (§3.2). G143050[145].17 is changed temporarily and restored before every save (§3.2).

**What the officer owns, shares or has suppressed:**

| OWN (game systems serve him, with officer-specific storage) | SHARED (world-level, no per-character state) | NONE / suppressed (he stays "no character" there) |
|---|---|---|
| cash and HUD cash, bank account and log (including the ATM and the bank web page), stock portfolio; Ammu-Nation purchases and unlocks, weapons, ammo and tints; health, armour, wanted level, last-known position; outfit indices (VARIATIONS); per-character SP stats and skills (as SP3_*); fees (hospital, arrest); LS Customs; personal vehicles (mod garage plus the game's vehicle records); phone handset, settings and contact list (existing slot 3); wheel slot 3; his entries in our own mods (bank, property, stocks, social: ml::save slot 3, opt-in) | paid world services (taxi, car wash, vending, cable car, fairground, cinema, telescope; also strip club dances and drinks, prostitutes, golf, tennis, darts, shooting range fees, hunting, towing: each decided by U9), charged to *his* account; character-neutral collectibles; world flags; respawn locations | story flow and missions, mission triggers, Strangers & Freaks, random events (as content), friends and booty calls, per-character relationship data of strippers and prostitutes, family and timetable scenes, protagonist safehouses, beds and garages, SP businesses, protagonist calls, texts and e-mails, Lifeinvader and car-app pages, completion % and achievements, minigame records, Director Mode, mission replay and snapshots, peyote animal trips (v1), Rockstar Editor and benchmark sessions (the officer session ends cleanly, §3.11), pause-menu Skills page (3 hard-coded columns) |

Appearance is mostly **OWN through the mod**: head blend, overlays, hair, tattoos and the freemode clothing catalog. The game's SP shop catalogs are per protagonist model, and the freemode branch is the MP path backed by MP stats (§4). The vanilla clothes, barber and tattoo shops must **not** be started for the officer (§0.2 item 8).

---

## 2. Inventory

### 2.1 Identity: who the scripts think the player is

There is no native that returns the SP character. Every script carries compiled copies of the same helpers (gunclub_shop addresses shown, all **(checked)**):

| Helper | gunclub_shop | Copies | Behaviour |
|---|---|---|---|
| `is_playable(c)` | @46287 | 765 scripts, 6,790 calls (5,337 SP) | `LOCAL_U8_LOAD 0; PUSH 3; ILT`, the only `<3` compare anywhere; 12 bytes |
| `get_cur()` | @46299 | 734 scripts, 6,734 calls | `CALL update_cur`; load +4321 |
| `update_cur()` | @46324 | 734 scripts | Recomputes G122220 from GET_ENTITY_MODEL(PLAYER_PED_ID). Writes +4322 (previous), +4323 (last) and +4321 (current). Director Mode (G44938 == 14) gives 145 unless G113936 is set. |
| `ped_to_char(ped)` | @46591 | 774 scripts | loop c = 0..2 (`PUSH 2; ILE_JZ`) comparing with `char_model(c)`, else 145 |
| `char_model(c)` | @46652 → @46689 | 777 scripts | G2349[c].0 when `is_playable(c)`, else 0 |
| phone owner | @998682 | 612 scripts | G21666 = get_cur, with 145→3, G80362→3, >3→3 (Director Mode: model vs G143050[c].0, fallback 0) |
| account getter | carwash1@46526 etc. | 119 scripts (one body each) | `G2349[c].17`. These are all 119 reads of field 17 in all scripts **(checked)**. Callers are guarded (3/4 → skip) except flow_controller@585224 (guarded by is_playable) and mission_triggerer_d@502327/@506478 (unguarded, §0.2 item 1). |

Other gate forms in SP scripts (identity survey; the technique survey's counts are within ±25% because it used a different method):

| Pattern | Sites (unique) | Treatment in officer-aware scripts |
|---|---|---|
| `== / != 145` | 972 (50) | review: 3 passes through these |
| `> 3` (already accepts 3) | 84 (8) | review |
| invalid set {144,145,161,162,222,223} | 36 | none (3 not in set) |
| compares with 0/1/2 | 7,088 (2,907), 326 scripts | default: leave as is (identity) |
| SWITCH cases {0,1,2} | 2,478 (611) + 270 partial (201) | per site; hash-triple auto-rule (§3.4) |
| char vs non-constant (mission and contact char) | 471 (195) | leave |
| loops 0..2 over characters | 591 loops (108 unique functions) | per function: extend or leave |
| **unrolled constant-index access to per-character arrays** | e.g. `PUSH 2; GLOBAL_U16 62254; ARRAY_U8*`: 261 sites in 123 scripts **(checked)**; gunclub_shop@856918 (cash sync), main@465751 (cash), main@569143–569227 (VARIATIONS fix-up), pb_prostitute@11379–11461 | new UNROLLED class: per site, extend or decide (§3.4) |
| IS_BIT_SET(x, char) | 1,074 (94); SET/CLEAR_BIT 25 (4) | check that bit 3 is free |
| recipient masks 1..7 | 110 (14) | leave (keeps the officer out of protagonist comms) |
| model-hash compares | 526 direct (45), 6,169 via parameter (314), 3,802 SWITCH (288) | leave (freemode → MP/default branches) |
| "3 = MP" | about 50 (stats_controller@6125/@6278, completionpercentage_controller@2784, main@35155 skill case 3 → MP stat enums 64–71, player_controller_b@577 switch target 3 → current) | rewrite only if the script becomes officer-aware |
| **character passed to natives** | e.g. gunclub_shop: SETUP_SHOP_PED_APPAREL_QUERY_TU ×6, GET_NUM_TATTOO_SHOP_DLC_ITEMS ×3, _GET_STAT_HASH_FOR_CHARACTER_STAT ×4, GET/SET_PACKED_STAT_*_CODE ×4; similar sets in shop_controller, respawn_controller, selector, player_controller(_b), carmod_shop **(checked counts)** | new NATIVE-ARG class: per site; the engine's meaning of 3 is unknown (guess: MP male catalog / MP slot), U32 |
| direct reads of G122220 outside helpers | 24 (chop, code_controller, epsdesert, epsrobes, family6, family_scene_*…) | stay blind; G122220 never holds 3 under D2 |
| **phone owner used as an index** | 1,206 `GLOBAL_U16_LOAD 21666` → array sites; 1 into a [3] array (G24150, appcontacts) **(checked)** | owner domain; G24150 gets a safety patch (§3.5) |
| **"force a protagonist model" checks** | main@14801 (once per second: if G153585 == 0 and the model is not one of the three hard-coded hashes, @465152, it switches to +4321/+4323/1 and sets G153585); main@569229 (Editor cleanup); benchmark@13574; mission_repeat_controller@665234 | the alias does not satisfy them (they compare hashes, not statics); §3.11 |

**Character enum:**
- 0, 1, 2: protagonists;
- 3: MP placeholder;
- 4–144, 146–161, 163–221: contacts;
- 145, 162, 222: empty (zero-filled, so **bank field 17 = 0 = Michael**);
- ≥ 223: out of bounds for G2349.

Field 17 across the 223 runtime entries: 4 ×217, 0 ×4 (Michael, 145, 162, 222), 1 ×1, 2 ×1. **No entry has 3** **(checked)**.

### 2.2 Per-character script data

Totals (curated catalog from the per-character globals survey, cross-checked against the saving and technique surveys):

- Catalog: 89 entries / 137 arrays / 34,618 slots / 193,909 access sites. Count-3 arrays at high or medium confidence: **76 entries / 122 arrays / 20,258 slots / 184,237 sites**, in 845 scripts but only **2,362 distinct function bodies** (shared library code).
- One extra character needs about **6,650 slots**. About 3,250–3,600 of them belong to saved data. About 1,900 belong to the two snapshot structs, which the officer does not need.
- The saved block G114990 (38,594 slots, standard_global_reg@146–@365) holds 133 outer 3-element arrays (10,090 slots). One character's full mirror is 3,319 slots / 3,183 values.
- Proven to be indexed by a character: 71 arrays (identity survey), or 40 global + 11 static (technique survey). The candidate ceiling is 327–356 count-3 global arrays and 561 static arrays in SP code. The M1 analyzer closes this gap (§3.5).
- Per-character data can also be selected without any index, through explicit `c == 0/1/2` compares with constant indices (PROSTITUTE_SAVED_DATA). The analyzer must catch these too (UNROLLED class).

Important arrays:

| Global | Name (save name) | Shape | Saved | Sites | Officer |
|---|---|---|---|---|---|
| 117360 | COMPONENTS_ARRAY | 3×164 (inner [3] bit words) | yes | 75,643 / 601 scripts | shadow exists; unused for freemode |
| 117853 | TATTOOS_ARRAY | 3×15 | yes | 524 | shadow; tattoos handled by the mod |
| 117899 | PP_INFO_STRUCT (4,324 slots, passed whole by pointer 154×) | struct | yes | 66,087 / 830 scripts | inner arrays below |
| 117899 | VARIATIONS | 3×65 | yes | 17,943 | OWN |
| 118189 / 118193 | HEALTH_PERCENTAGE / ARMOUR | [3] | yes | 388 / 367 | OWN |
| 118197 | WEAPONS (WEAPON_INFO 44×5 + DLC_INFO 51×5) | 3×477 | yes | 2,803 | OWN |
| 119629 | WEAPON_TO_SHOW | [8][3] | yes | 745 | OWN |
| 120195..120227 | LAST_TIME_ACTIVE, LAST_KNOWN_XCOORD/HEADING/ROOM_KEY/VELOCITY/WANTED | [3] / 3×3 | yes | ≈3,000 | OWN |
| 120247 | AVAILABLE | [3] (all 1 in the 100% save) **(checked)** | yes | 77 | OWN (wheel); also read by the Phase A wheel (§3.10) |
| 120254..120278, 120146 | STAT_OFFSET_1..7, STAT_UPDATES [8][3] | [3] | yes | 254 | review |
| 120282 / 120286 | SPECIAL_ABILITY / SPEC_AB_UNLOCK | [3] | yes | 24 | optional |
| 120306 | STORED_VEH_DATA (CAR/BIK/GAR/MOD) | [4][3]×98 | yes | 20,194 | OWN |
| 121487 / 121508 | VEH_POS / VEH_HEAD | [2][3] | yes | 1,654 | OWN |
| 121715 | WARDROBE | 3×166 (arithmetic inner index) | yes | 308 | mod |
| 122220–2 | PP_CURRENT/PREVIOUS/LAST_KNOWN_PED | scalars | yes | 7,292 / 808 / 1,603 | **never hold 3** (D2) |
| 62254 | cash (mirror of SPn_TOTAL_CASH; indexed by **account**) | [3] | no | 2,192 / 235 | OWN (account 3). Bank function writes G62254[acct], then @856578 writes it back to SPn_TOTAL_CASH by SWITCH 0/1/2; main's copy is @704920. Entry sync @856918 is unrolled. |
| 135796 | FINANCE PLR_ACNT_LGS (bank log) (=114990+20573+233) | 3×69 | yes | 2,910 | OWN |
| 135563…135651 | stocks M_/F_/T_ (*three-way named* groups of 33) | 9×[10] | yes | 3,775 / 204 | OWN (4th group) |
| 135117 / 135247 | CAR_APP_DATA / ORDER | 3×43 / 3×41 | yes | 4,241 | NONE |
| 136036 / 55121 / 46175–7 | EMI_AR / runtime inbox / unread counters | 3×103 / 3×120 / 3 scalars | yes / no | 1,591 | NONE |
| 123606 | COMM_CONTROL_CHAR_PRIORITY | [3] | yes | 699 | NONE |
| 135555 / 135559 | FLOW_HELP priority / displayed | [3] | yes | 1,416 | NONE |
| 133532 | g_ePlayerLastScene | [3] | yes | 1,763 | NONE (scenes) |
| 125049 | PROSTITUTE_SAVED_DATA (8 × 11; per-character fields iMetMichael/Franklin/…) | 8×11 | yes | selected by `c == 0/1/2` (pb_prostitute@11379–11461) **(checked)** | NONE (relationship) |
| 125138 | PROSTITUTE_SERVICES_ARRAY | [3] | yes | — | NONE, or a shadow if pb_prostitute (aware, Q4) needs a per-character counter (U9) |
| 149731 | WEB_VEHICLES | 3×939 | yes | 26 | OWN (our shops) |
| 152787 / 153349 / 153341 | IMPOUND_VEHS / SWITCH_IMP / IMPOUND_SLOTS | 3×157 / 3×78 / [3] | yes | 2,292 | OWN later |
| 115736 / 115752 | CARMOD_DATA / VIEWED_MODS (flattened 3×408) | 3×5 / [1224] | yes | 78 | OWN |
| 134146 / 116985 | SHOOTING_RANGE / PILOT_SCHOOL | 3×271 / 3×121 | yes | 126+ | NONE |
| 99876, 101915, 102210/14/18/28, 4525216, 99865, 102236, 102432, 100622, 99799/99803/99807/99811/99851 | switch, placement, left-behind vehicle, outfit backup, mission badges | [3]… | no | ≈8,000 | OWN for switch state; review the rest |
| 105547 / 108531 | snapshot structs (mission replay/repeat, Director Mode) | 2,984 each, 11 per-character arrays | no | 30,115 | NONE (officer never in missions) |
| 34212 | shop and blip table, +0[3] / +12[3] | 263×23 | no | 163 | OWN for shops (meaning medium confidence) |
| 24150 | contacts-app flag indexed by phone owner | [3] | no | 1 (appcontacts@41041) | safety patch (owner 3 crashes it) |

**Existing 4-wide structures (index 3 already exists):**
- 99964 (switch struct: peds [4], flags [4], +44/+45/+46 request);
- 102436 (ambient protagonist peds);
- 24447 (wheel UI);
- CELLPHONE_SETTINGS 129050 [4×20] (slot 3 = "cellphone_ifruit");
- cellphone block 10341 [4×2811];
- 10310, 10212, 10217, 10195 [4] (indexed by the phone owner) **(checked)**;
- TEXT_MESSAGES 129140 PhonePresence +99 [4];
- 2349 and 143050 per-entry owner arrays (+12/+19/+24 counts, elements 13–16, 20–23, 25–28);
- 10195 / 10212 / 10217.

Index 3 is used by constant mostly in MP or transition scripts. Owner-3 values in the per-entry owner arrays: fields 16 and 23 are 0 for all 223 runtime and 162 saved entries. Field 28 is **1 for entry 96** in both sheets **(checked)**. appinternet has explicit owner-3 contact operations (contacts 147 and 82; SP/MP context undetermined).

**Phone palette for owner 3:** 11 scripts do `G21666 == 3 → SWITCH G4525199 → SET_PLAYER_PHONE_PALETTE_IDX`: main, cellphone_controller, cellphone_flashhand, appcontacts, appemail, apptextmessage, appsettings, appcamera, dialogue_handler, freemode, appmpemail **(checked)**. G4525199 lives in global block 17 (MP side) and is 1 in the dump. The officer's phone theme therefore comes from that global.

**Full-character-enum tables:**

| Table | Shape | Notes |
|---|---|---|
| G2349 | 223×29 | 3,932 sites / 782 scripts; not saved; rebuilt by standard_global_reg@555944 |
| G118 | 223×10 | |
| G143050 | 162×29 | saved CHARSHEET; written by the setters @556364… only when c < 162. Field 17 is read by atm_trigger and appinternet (the bank screens) **(checked)** |
| G45490 | [222] | |
| G21672 | [223] | |

**Per-character bitsets:**
- COMM Player_Char_Bitset (in 5 queues; comms_controller@31841 `IS_BIT_SET(bitset, cur)`);
- FLOW_HELP iCharBitset G135409[16]+11 (770 sites);
- G97513 (711);
- G93274[34]+11 (70);
- G99419[17]+8;
- OWVEC_IND G140181.

**Character-valued globals** (audit list for §3.2):
- 122220–2;
- **owner domain:** 21666 (phone owner), 10222/10223, 80362 (forces owner 3 during transitions: main_persistent@2529528 writes 1, @540 writes 0) **(checked)**;
- 153339 IMPOUND_CHAR (saved), 153340 VEHGEN_CHAR (saved), 80139;
- 135409[16]+11 (mission-trigger char);
- 100009 / 99964+45 (switch target);
- 98220, 79734, 8820, 10146[6]+5;
- 99827[1], 102532, 122224 OVERRIDE_PED, 98317, 21896;
- 139903[16] property owner (145 = none);
- animal_controller static 149 (current character for the peyote return model).

**Session flags with character semantics:** G153585 ("player model settled"; main@14801 forces a protagonist when it is 0; set by selector@31464, main_persistent@2529593 (called with 1 at @2529532) and main @891434 (unverified)); value 1 in the dump **(checked)**.

### 2.3 Engine-side per-character systems

| System | Location | Shape | Effect on the officer today |
|---|---|---|---|
| protagonist model statics | 0x4714e50 / e60 / e70 = {0x0D7114C9, 0x9B22DBAF, 0x9B810FA2} **(checked)** | 3; 100 compare sites (21 against static 0 **(checked)**), 9 init writes | not recognised |
| **CHANGE_PLAYER_PED / SET_PLAYER_MODEL per-protagonist dispatch** | CHANGE_PLAYER_PED 0xc84a10: 0xc8537c skips when the model equals the last-model static 0x42EBD08 (written at 0xc85411); 0xc85388–0xc853ce: model == static k → `0xB5F970(&0x415ECF0, k)`; else 0xc85405 → `0xB600E0(&0x415ECF0)`. Same pattern in SET_PLAYER_MODEL 0xc843d0 at 0xc84985–0xc849c6 **(checked)**. Effect of 0xB5F970 unknown (guess: per-protagonist audio or vehicle setup). | 3 | the non-protagonist routine runs; under the alias, Michael's routine would run (U29) |
| stats character index | u32 0x481DC38, set only by 0x1c3bf90 (from SET_PLAYER_MODEL 0xc843d0, CHANGE_PLAYER_PED 0xc84a10, save PostLoad 0xc84100, STAT_SET_INT) | — | stays on the last protagonist (when the MP flags [0x47AA0D0]/[0x47AA0DC] &1 are set) or goes to MP0/MP1 |
| stat prefix table | 0x2944870 = [SP0, SP1, SP2, MP0, MP1, −1] **(checked)**, used by the `<prefix>_<name>` builder 0x1c3c180 (550 calls, no bounds check) | 5 | — |
| StatId5 objects | 187 built by 0x1eb44d0 (stride 0x14) + 9 at runtime; getters 0x1ea5650 / 0x1ea5620 / 0x1ea5610 (index > 4 → 0). TOTAL_CASH at 0x4eef0d4 = 0x0324C31D / 0x44BD6982 / 0x8D75047D / 0x810E680B / 0x42232706 **(checked)** | 5 | — |
| characterStat expansion | 0x1eaa030, unrolled for SP0..2 (+ MP0/1); stats are data files of type 0x23 (SP) / 0x24 (MP), parsed by 0x1ea99d0 | 3 | no SP3_* exists |
| HUD cash, pause header, street cash pickups, garage respray | 0x54a550, 0x5d2110, 0x1030390, 0x13ace70 @0x13add0c ($100) | via current index | charges and shows the last protagonist |
| special ability | type from CPedModelInfo+0x280 (peds.meta +0xfc); gates = model static k + unlock byte 0x43506d0+k (d3 unused, d4 MP); about 20 inline gates; input immediates for player_zero/player_two at 0xd7be0d/0xd7be2a | 3 | none |
| DO_AUTO_SAVE | 0x1d613d0 (refuses non-protagonist model); called only by autosave_controller **(checked)** | 3 | refused |
| CPlayerPedSaveStructure | PreSave 0x222c2a0 / PostLoad 0x222ba40 (PostLoad re-applies the model → 0x1c3bf90) | — | — |
| DLCScriptSaveData registration | 0xd2eba0: compares the program hash ([prog+0x30]) with [0x434915C] = 0xB08F5BE3 (matches no script name among 1,154) and [0x4349160] = 0x77714C97 = joaat("standard_global_reg"); references "startup" **(checked)**. Semantics of the special case are not analysed. | — | — |
| ped type (Pedtype) | [ped+0x1098] bits 14–20; about 42 protagonist tests; pain bank 0x9d1720 (fallback is Michael's); tuning blocks 0x434a290 | — | Michael's pain bank, default tuning |
| lens flare | 0xb101d0 | 3 slots | random other slot |
| pilot and bike helmets | 0x168f3e0, 0x16d9240, 0x1b450f0, 0x165a480 | — | — |
| inertia ×2.5 | 0xe0a6e0 (F/T) | — | — |
| buddy-death speech | 0x1b20910 | — | — |
| other immediates | 0xa1a640, 0x1b41b90, 0x7dadd0 | — | — |
| pause Skills page, JSON metadata, SCADMIN cash | 0x6b6600 / 0x6094e0; 0xeb6a0; 0x1c3df60 | 3 hard-coded columns | — |
| switch effect | 0xc61a60 / 0xc62d60 by ped type | — | neutral style |
| phone models | 0x3deb3c0 | 5 slots; not character-aware | — |
| **script VM: bounds failure** | 0x91ee3d, shared by the 6 ARRAY handlers (jnb at 0x91ee19 / 0x91ee89 / 0x91eeb9 / 0x91f149 / 0x91f189 / 0x91f1c9) and by 0x920418, 0x920466; first instruction 6-byte RIP-relative; stores ip at thread+0x1C, then 0x92076e sets state 2 **(checked)** | — | — |
| **script VM: unregistered global block** | GLOBAL_U24 0x91f3f0: `block = [r13]>>26`; `test rcx,rcx; jz 0x91f42a` → thread killed; _LOAD 0x91f470 and _STORE 0x91f4b0 same shape **(checked)** | — | reading block 13 before it exists kills the reader |
| **script VM: call depth and stack** | ENTER 0x91e653–0x91e6a9: depth byte thread+0x74, failure when ≥ 0x80; stack check against thread+0x60 **(checked)** | — | limits forwarder and helper depth |

### 2.4 Save data

- **Script SP block:** START_SAVE_DATA(&G114990, 38594, 1) at standard_global_reg@146. The size is checked against GET_SIZE_OF_SAVE_DATA(1) at @378. fSaveVersion is 1.7 and must never change (startup@502–@545 runs an upgrade routine @11176 when it differs).
- **The format is name-keyed with type checks** (loader 0xd2dfe0):
  - a member missing from the file keeps its in-memory value;
  - a saved member that nobody registers is ignored;
  - names are `[A-Za-z0-9._:]` and shorter than 64 characters, and a duplicate name in an open scope is silently skipped.
- **Additional per-script blocks:** sp_dlc_registration (G2883692, block 11, one int "aFakeSavedInt", sets G4525781 bit 0) and sp_pilotschool_reg (G3145728, block 12, one int "PILOT_SCHOOL_DUMMY_DATA", bit 1) each own a block **(checked)**. They are stored under `DLCScriptSaveData/<script-name hash>` (0xd2eba0, 0xd1fc10, write 0xd2abd0). Pending data is applied at STOP_SAVE_DATA (0xd30140 → 0xd30290). **Arrays, structs or large blocks in a DLC save block have no precedent** (U4).
- **Engine sections:** stats (SPn_*), and the player ped (model, components, decorations).
- **Save and load natives in scripts** **(checked)**:
  - SET_SAVE_MENU_ACTIVE: savegame_bed, save_anywhere;
  - DO_AUTO_SAVE: autosave_controller;
  - QUEUE_MISSION_REPEAT_SAVE: mission_repeat_controller@571897;
  - QUEUE_MISSION_REPEAT_SAVE_FOR_BENCHMARK_TEST: benchmark@11978;
  - QUEUE_MISSION_REPEAT_LOAD: mission_repeat_controller@145/@496, benchmark@501391;
  - SHUTDOWN_AND_LOAD_MOST_RECENT_SAVE: maintransition ×4.
- **Autosave requests:** the `G102550+10 += 1` request pattern appears in **218 scripts** **(checked)**: shops, carmod, services, every re_*, collectibles, Strangers & Freaks, minigames. Example: gunclub_shop@968054 → @332787 → @332844–@332856.
- **Files:** SGTA50000 / 50001 (the user's 100% save, slot 03) / 50015 (autosave, slot 02), MISREP0000, `.bak` copies. The body is encrypted, so files cannot be edited offline.
- **Mod data today:** ml::save keeps one JSON per mod for all slots (`mods.cpp:758-880`). Slots are 0..2 only (`mods.cpp:795,821`). A commit happens on an SGTA5 mtime change, and a revert on any loading screen. The police mod keeps:
  - `officer`: active flag and position; applied only when the loaded save's game clock is within ±30 game minutes (`examples/police/persist.cpp:22-29,71-73`) **(checked)**;
  - `officer_look` and `officer_reports`: these leak across slots.

### 2.5 Coverage gaps

- pilot_school, launcher_epsilon and ob_mp_shower_med did not parse.
- dlcpacks script archives were not scanned.
- Character values carried in struct fields defeat the taint analysis: about 82k sites are classified only as "parameter".
- Local (frame) per-character arrays have not been counted.
- Only 69 of the 187 StatId5 names are resolved.
- **New in revision 2:**
  - character values passed to natives are not yet classified (U32);
  - unrolled constant-index access is counted only for G62254;
  - the stats section of the save has not been analysed: capacity, unknown stat hashes on a vanilla load (U6/U18);
  - the creation timing of a script-owned global block after loads and transitions is unknown (U28).

---

## 3. Architecture

```
            ┌─────────── loader start-up ───────────┐
 game .ysc ─► rule engine (fingerprints, catalog) ─► generated replacement pack (officer-aware scripts + safety-patched blind scripts + ml_char4_reg.ysc)
            └───────────────────────────────────────┘
 runtime:
   officer-aware script ──officer_mode()? ─no──► vanilla copy V (Online, block 13 not ready, officer off)
                          └─yes─► get_cur4()=3 ──► ARRAY op idx 3 ──► VM bounds fail 0x91ee3d ──► DETOUR: ARRAY opcode ∧ aware thread ∧ whitelisted? ─yes─► shadow element in block 13
                                                                                                     └─no──► vanilla thread kill + LOG (dev) / scratch + ERROR + officer_mode off (release)
   officer-blind script ──get_cur()=145──► vanilla behaviour (+ 145 hardening, + safety patches)
   phone scripts (blind) ──G21666=3──► existing 4-wide owner arrays (+ G24150 safety patch)
   engine ──(alias slot k while officer is player)──► SP3_* stats (HUD, pickups, respray, skills)
   save path ──► SAVE BROKER (defer autosaves; swap to protagonist, sanitize, nonce) ──► game save incl. ml_char4_reg block
   system paths (Editor, benchmark, replay, Online transition) ──► TEARDOWN (§3.11)
```

### 3.1 Script classes

- **Officer-aware**:
  - target end state: shop_controller (with a per-shop-type GUARD), gunclub_shop, carmod_shop (M4), respawn_controller, blip_controller (shop blips, same shop-type filter), **atm_trigger**, the public-service scripts (carwash1/2, ob_vend1/2, taxiservice, cablecar, bigwheel, rollercoaster, act_cinema, ob_telescope, plus every paid script U9 lists, all of them aware by user decision Q4: stripclub, stripclub_drinking, pb_prostitute, golf, tennis, darts, range_modern, hunting_ambient, towing…), selector, player_controller_b, player_controller (switch-related parts), and later stock_controller / appinternet if our browser does not replace them;
  - `ml_char4_reg` (ours) also carries an officer-aware copy of the bank function for the SDK (D10);
  - rule: a script is aware only if every character-decision site in it has a recorded decision and the residual check passes (§3.5). Otherwise it is automatically demoted to blind, with a warning.
- **Officer-blind**: everything else, including main, flow_controller, mission_triggerer_a–d, comms_controller, friends_*, family_scene_*, player_timetable_scene, startup_positioning, autosave_controller, savegame_bed, save_anywhere, stats_controller, completionpercentage_controller, animal_controller, director_mode, benchmark, mission_repeat_controller, all re_*/launcher_*/RC scripts, and clothes_shop_sp / hairdo_shop_sp / tattoo_shop (never started for the officer).
- **Blind with safety patches** (behaviour change only where vanilla crashes or leaks):
  - appcontacts: guard the G24150[G21666] store when the owner is ≥ 3;
  - mission_triggerer_d: guard the two unguarded account reads (skip the cash test when the account is ≥ 3). Needed only if the G2349[145].17 hardening stays on while that script runs.
- **Owner-domain scripts:** cellphone_controller, cellphone_flashhand, the app* scripts and dialogue_handler stay blind for player identity. They already serve owner 3 through 4-wide arrays (§3.5).

### 3.2 Identity layer

**Rewrite applied to every officer-aware script.** The helpers are identical copies, so they are found by fingerprint:

1. Append these functions:
   - `officer_mode()`: a call to a loader-served native. The rewriter appends one entry to the script's native table, and the loader serves it through OverrideScriptNative in aware programs only. The native choice is made in M1 and needs an S7 check. It returns true only when all of these hold:
     - block 13 is registered (the loader checks the block pointer table);
     - the officer feature is enabled for this save;
     - no network game is in progress and no transition signal has been seen (§3.11);
     - not Director Mode (G44938 ≠ 14);
     - no teardown is in progress.
     
     Block 13 is touched only after this returns true, because reading an unregistered block kills the thread (§2.3).
   - `is_char4(c)`: `c < 3 || (c == 3 && officer_mode())`.
   - `char_model4(c)`: G2349[c].0 if is_char4(c), else 0.
   - `ped_to_char4(ped)`: the vanilla result when it is not 145; otherwise 3 if `officer_mode()` and `ped == G[OFFICER_PED]` (a handle slot in block 13); otherwise 145. Identifying the officer by handle stops other freemode peds (Director Mode, other mods, selector's freemode path at selector@520435/@520752) from being taken for the officer.
   - `get_cur4()`: `r = get_cur()`; if `r == 145`, `officer_mode()` and `PLAYER_PED_ID() == G[OFFICER_PED]`, return 3; otherwise return r.
2. Retarget calls in place (3-byte CALL target):
   - every `CALL get_cur` → `get_cur4`;
   - every `CALL is_playable` / `char_model` / `ped_to_char` → the `*4` versions, **except the calls inside update_cur and its private helpers**, which stay vanilla so G122220/1/2 never receive 3.
   - These retargets cannot be told apart from vanilla when `officer_mode()` is false, so they are allowed in place (D5).
3. Direct `GLOBAL_U24 114990; IOFFSET_S16 2370; IOFFSET_S16 539; IOFFSET_S16_LOAD 4321` loads outside the helpers (13 bytes) become `CALL get_cur4` (4 bytes) plus NOPs.
4. The phone-owner function needs no change. It calls get_cur, which is retargeted, and keeps owner 3 either way.

**OFFICER_PED binding.** The loader writes OFFICER_PED and re-applies the look whenever the officer's ped is replaced:
- after the mod's own CHANGE_PLAYER_PED / SET_PLAYER_MODEL (gender change);
- after respawn;
- in Phase B, after the game's own switch code creates the officer ped (§3.10).

A per-frame check compares PLAYER_PED_ID with OFFICER_PED while the officer is the logical player. Whether PLAYER_PED_ID changes after SET_PLAYER_MODEL is unverified (guess: it does; U26). Paths that put a protagonist model on the player (Editor, benchmark, replay) end the officer session instead (§3.11).

**Runtime ownership of entry 3** (loader service, re-applied after every session start and every load, because standard_global_reg rebuilds G2349):

- G2349[3]: +0 = the officer's current model (follows gender), +3..6 name label and +7..10 picture label (through the loader's text override), +11 = 7 (not a friend), **+17 = 3 (officer bank account)**. The per-owner arrays stay as they are.
- G118[3] stays unchanged: 118[3].8 = 198 keeps him off contact lists.
- **Never use the standard_global_reg setters** (@556364 etc.): they mirror into the saved G143050 when c < 162. Write G2349 directly.
- The broker restores vanilla values of G2349[3] around each save and at the first transition signal. G143050[3] is checked against its session-start image.

**Hardening while the officer is played (blind scripts):**

- **G2349[145].17 := 4**, so that blind payment paths (`account = 2349[c].17`, guarded wrapper) skip instead of charging Michael.
  - It is applied only while **no mission_triggerer_d thread exists**. The loader checks with EnumScripts every frame and restores 0 at once when the thread appears.
  - Reason: mission_triggerer_d reads `G62254[2349[c].17]` without the guard (§0.2 item 1). The window is small but not zero: the thread could reach @502314 on its first frames if the player already sits in a Gauntlet (guess; U35).
  - From M1, the mission_triggerer_d safety patch removes the condition.
  - The condition was chosen over the alternative "145.17 := 3 with G62254[3] served to all threads" because the detour does not exist before M0 ends.
- **G143050[145].17 := 4** while the officer is the player. The ATM and the phone's bank page then refuse to show an account for the officer, instead of showing Michael's.
  - G143050 is saved, so the broker restores 0 before every save and the value is in the vanilla-clean invariant (§3.9). R5 watches it.
  - If it ever leaked into a save, the value would mean "no account" for an entry that vanilla never plays (benign; guess).
  - From M2, atm_trigger is aware and reads G2349[c].17 at its single site, with CASE3 for the account-name SWITCH (an officer label through text override). appinternet's site follows in M5 or is replaced by our browser.
- Until the broker exists, the police mod's current save routine (officer.cpp) must restore both hardening values before it saves.
- Character-valued globals audit (list in §2.2):
  - the analyzer reports every write of a character-typed value (possibly 3) into a global from an officer-aware script;
  - each target must be read only by officer-aware scripts, be proven safe, or be sanitized by the broker;
  - saved ones (153339, 153340, 139903, 135409[].11) must never hold 3 at save time.

### 3.3 Data layer: shadow elements through the bounds-failure detour

**Mechanism** (registers verified at 0x91ee10–0x91f1e1 **(checked)**):

- All six ARRAY handlers (opcodes 52/53/54 = ARRAY_U8 / _LOAD / _STORE, 73/74/75 = ARRAY_U16 / _LOAD / _STORE) execute `mov rax,[r15]; mov ecx,[r15-8]; cmp ecx,[rax]; jnb 0x91ee3d`.
- **0x91ee3d is shared.** It is also reached from 0x920418 (handler at 0x920410) and 0x920466 (handler at 0x920460, null target; guess: THROW and CALLINDIRECT). The detour therefore dispatches on the opcode byte at `[r13]` and passes every other opcode to the original code untouched.
- Registers live at 0x91ee3d:
  - `rax` = array base (count slot address), `ecx` = index;
  - `r13` = current instruction (element size at `[r13+1]`, u8 or u16), `r15` = VM stack top, `r8` = stack base, `r9` = frame;
  - `r10` = code-page table, `r11` = handler table, `r12` = thread+8, `r14` = code base for ip;
  - plus rbx/rsi/rdi/rbp.
- **Patch:** the first instruction at 0x91ee3d is a 6-byte RIP-relative `mov eax,[rip+…]` (TLS index). Replace it with a 5-byte `jmp rel32` plus a NOP to a thunk allocated within ±2 GB of the module. MinHook is not used here, because it only hooks function entries. The thunk's fall-through re-executes the original load with its displacement re-based, then jumps to 0x91ee43.
- **Thunk contract:** preserve every register listed above.
  - The served path uses only rax/rcx/rdx after saving them, and does its lookups in assembly against two small open-addressing tables: aware program hashes, and whitelisted bases.
  - The log and miss path saves all volatile registers and aligns rsp to 16 before calling C++.
- **Gating, in order:**
  1. opcode ∈ ARRAY set;
  2. the thread's program hash (thread+0x10, thread from r12−8) is in the aware set;
  3. the base `rax` is in the whitelist (a hash map built at session start from the catalog; global index → absolute address through the block pointer table 0x3ED15A8);
  4. the rule matches the index.
- **Served:** set `rax = shadow − 8·(1 + idx·size)` and jump to the handler's own continuation after its `jnb`, chosen by opcode: U8 0x91ee1b / 0x91ee8b / 0x91eebb, U16 0x91f14f / 0x91f18f / 0x91f1cf **(checked)**.
- **Not served:** log (script name from thread+0x154, ip, global or static index, idx, count) and either run the original failure code (dev) or apply the D11 release policy (aware threads only).
- Because the handler's own arithmetic then lands on the shadow element, LOAD_N/STORE_N of one element, address escapes into natives and pointer-parameter callees all work unchanged.

**Zero-regression property:** the detour only runs where vanilla would have set the thread state to 2 (thread killed). For every vanilla execution that does not crash, behaviour is byte-for-byte identical. With thread gating, blind threads keep even their vanilla crashes, so blind scripts never silently read officer shadows: for example the phone scripts through G21666 = 3, or the "3 = MP" paths. Protagonist play cannot be affected by the data layer.

**Whitelist rule kinds:**

| Kind | Rule | Examples |
|---|---|---|
| E (element) | `idx == count` (3) → shadow element of `size` slots. Each inner base of a nested array is listed separately. | VARIATIONS, WEAPONS, 62254, 135796, STORED_VEH_DATA (4 inner bases), VEH_POS (2), 34212[263]+0/+12 (526 inner bases) |
| R (range) | `count ≤ idx < count+extra` → `shadow[idx−count]` | VIEWED_MODS [1224] (extra 408), WARDROBE arithmetic index (to confirm) |
| S (static) | per-thread static array, keyed by (script hash, static index); base relative to the thread's static area. Shadow in loader memory, not saved. | 561 candidates, 11 confirmed |
| L (local) | (script, function, local offset relative to the frame pointer thread+0x20) | count unknown; implemented only if M1 finds any |
| N (sink, optional) | explicit (blind script, array) exception: `idx == 145` → per-array scratch element, logged as a warning | only where a blind script provably crashes on 145 today and cannot be guarded otherwise; none planned |

**Policy:**
- Whitelist only catalogued per-character arrays. Missing entries fail loudly in development (thread kill plus log), and the log tells exactly what to add. Release builds follow D11.
- Over-whitelisting a non-character array cannot corrupt vanilla data, but it hides bugs, so it is avoided.

**Shadow storage:**
- Global block 13, G3407872 + offsets. 13 = 3,407,872 ≥ 2^18·13, within the 24-bit GLOBAL_U24 operand. Blocks 13–15 and 21–63 are unowned (technique survey header scan of 1,151 scripts). Block 11 = sp_dlc_registration, block 12 = sp_pilotschool_reg, block 17 = MP side (G4525199, G4525781) **(checked)**.
- Layout, generated:
  - header: layout version, per-save nonce, OFFICER_ACTIVE, OFFICER_PED, ready flag (informational only: readiness is decided by `officer_mode()`, never by reading block 13);
  - saved shadows (≈3.3–3.6k slots);
  - non-saved shadows;
  - officer copies of the *three-way named* groups: 4th stock group of 33 slots, 4th e-mail counter, OWVEC colour column [312];
  - stat mirror;
  - scratch sinks for D11;
  - reserve.
- Size: 16,384 slots (one initial-values page of 0x4000).
- Initial values come from the vanilla **initial image** of element 0 in standard_global_init's global pages (not runtime data), so nested count slots are correct. The officer's real defaults (no weapons, 100% health…) are written by the mod when the officer is created.

**Cases the detour does not cover** (enumerated by the analyzer, each with a recorded decision):
- whole-array or whole-struct copies that include a [3] array (6 known, for example main@506381 copying 292 slots of SOCIAL_SAVED_DATA): the shadow element is not part of the copy;
- loops bounded by an array's own count slot (95–274 reads): they never reach 3;
- loops `i<3` / `i<=2` (15,192 sites): handled by the logic layer, not the data layer;
- **unrolled constant-index code** (§2.1): it never forms index 3, so the officer's element silently goes stale. Handled by the logic layer (UNROLLED);
- three-way named groups (stocks, email counters, OWVEC_COL_M/F/T): selected by SWITCH, so the logic layer adds case 3.

**Fallback if the detour is rejected in M0** (integrity check, instability): per-site `CALL stub` replacement, as in the technique survey. Every global chain is at least 5 bytes and a CALL is 4. It uses the same catalog and the same block 13, but needs points-to analysis for about 13.5k pointer-parameter accesses and 252 base escapes.

### 3.4 Logic layer: how scripts are rewritten

**Where:**
- At loader start-up, the original script is read from the game archive with the game's own decryption (`dlcpacks.cpp:482-561`). Rules are applied, and the result goes into the generated replacement pack, using the overlay mechanism already used for converted .ysc files (`replace.cpp:559-566`, `ml_stream.rpf`). Encryption type 0 (§26 lesson).
- Cache key: CRC of the original script + rule-set version + catalog version.
- If another mod replaces an officer-aware script, the rules are applied to that copy. If fingerprints do not match, the script becomes blind with a warning.

**How** (interpreter constraints from the technique survey, consistent with the handler code above):
- **In-place, same-size edits.** Allowed only when they cannot change behaviour on vanilla data, because they stay active in Online:
  - CALL target retargeting to `*4` helpers, which return vanilla results when `officer_mode()` is false;
  - wrapper fix: in the payment wrapper (gunclub_shop@854360, carwash1@40955) `PUSH 3`→`PUSH 4`, so account 3 is no longer "none" and only 4 is. This cannot be told apart from vanilla while no entry holds account 3 (§2.1). The loader restores G2349[3].17 = 4 at the first transition signal.
- **Function copies.** Everything else, including constant edits to loop bounds and gates, new SWITCH cases, extra loop iterations and guards:
  - **F′** (modified) and **V** (vanilla copy, relocated) are appended in new code pages.
  - F′ begins with `if !officer_mode(): reload params; CALL V; LEAVE`.
  - The original body is overwritten in place with a **forwarder** (`ENTER p; LOCAL_LOAD 0..p−1; CALL F′; LEAVE p r`). Function-pointer callers (CALLINDIRECT through PUSH_U24 constants, 121,908 sites) then reach F′ too, without telling code pointers apart from constants.
  - Direct CALLs are also retargeted to F′ to save a hop.
  - A forwarder needs 12 + 2p bytes. A function shorter than that (for example is_playable, 12 bytes) gets no forwarder. Its direct calls are retargeted, and the build verifies that no `PUSH_U24 <its address>` exists. Otherwise the script is demoted.
- **Assembler and writer rules:**
  - append only, never move existing code;
  - no instruction straddles a 0x4000 page, and every page ends with NOP or a control transfer (0 straddles today; 85% of page ends are NOP);
  - 16-bit branch relaxation with islands inside F′ over 32 KiB (46 SP functions are larger than 32 KiB);
  - resolve CALL→J→ENTER chains (250 sites);
  - code stays under 16 MiB (largest SP script 6.57 MB);
  - **native table re-encoding:** entries are stored rotated by `(code size + index) & 63` (`tools/ysc_dis.py:27`), so appending pages changes every entry's encoding. New natives (officer_mode) are appended to the table. `convert/ysc.cpp:142-165` only rewrites global operands in place and never changes the size, so this is new code;
  - code page table and resource layout grow (resource writer in M1);
  - **call depth and stack:** depth must stay < 128 (thread+0x74). Per-thread stack use must fit thread+0x60. Example: respawn_controller is started with a 1,424-slot stack (main@789371–789379) **(checked)**. Forwarder hops and `*4` helpers add frames; S7 computes the worst case per thread entry point.

**Rule types:**

| Rule | Effect |
|---|---|
| IDENTITY | the identity rewrite of §3.2 |
| CONST | loop bounds and gate constants (inside F′) |
| CASE3 | add case 3 to a SWITCH |
| EXTEND-LOOP | iterate 0..3 |
| UNROLLED | add the officer counterpart next to constant-index 0/1/2 code (for example the cash sync @856918), or record "stale is fine" |
| NATIVE-ARG | decide what a character-taking native receives for 3: keep 3, map it to a protagonist slot, or skip the call |
| GUARD | skip a block when c == 3, or allow only listed sub-cases (shop_controller@96425: gun shops only until M4) |
| GROUP4 | three-way named group → officer group in block 13 |
| DEFAULT | an explicit "falls through, intended" decision |

Every rule is keyed by **function fingerprint**: instruction shape with CALL targets and function-pointer pushes masked, native indices replaced by native hashes. Shared library copies therefore get one decision.

**Hash-triple auto-rule:**
- Pattern: a SWITCH {0,1,2} whose case bodies differ only in one pushed constant equal to joaat("SP0_X") / ("SP1_X") / ("SP2_X").
- Effect: add case 3 with joaat("SP3_X").
- Examples **(checked)**:
  - cash getter gunclub_shop@857075; cash write-back @856578 (G62254[acct] → SPn_TOTAL_CASH, called at @855915 at the end of the bank function); TOTAL_CASH_EARNED @855565;
  - respawn_controller@48484 (so the officer pays his own hospital fee);
  - 177 Ammu-Nation unlock triples.
- **Not triples** (manual rules):
  - the MONEY_TOTAL_SPENT SWITCH @854444, where case 0 also calls `@231930(99,1)`: CASE3 by hand;
  - the unrolled cash sync @856918–857032 (only when NETWORK_IS_SIGNED_IN): UNROLLED, which adds `SP3_TOTAL_CASH → G62254[3]`.
- Names are resolved from the extracted SP stats definition list. Unresolved triples get DEFAULT plus a warning.

**"3 = MP" sites:** in an officer-aware script, a branch that sends character 3 to MP stats or MP behaviour is rewritten to the officer behaviour. In blind scripts they never see 3 as the *player* character. The phone owner is a separate case (§3.5).

### 3.5 Completeness proof (what "all sites handled" means and how it is checked)

1. **Identity containment:**
   - The value 3 for the *player character* exists only inside officer-aware scripts (get_cur4, ped_to_char4).
   - G122220/1/2 provably never receive 3: update_cur is untouched and only reads vanilla helpers.
   - The value can cross into other scripts only through globals. The analyzer lists every global store of a character-typed value in aware scripts, and each must be signed off (§3.2).
   - Static check: no blind script contains a retargeted helper, and no aware script loads +4321 directly.
   - **Owner domain:** the phone owner G21666 (and 10222/10223, G80362-derived values) is a second taint source. It already holds 3 in blind scripts while the officer is played.
     - S4 lists every array indexed by an owner-typed value: 1,205 sites into [4] arrays are safe; G24150 [3] gets a safety patch.
     - Owner-typed values must never reach player-character arrays.
2. **Data safety:**
   - Every ARRAY op in any script, executed with index 3, lands in one of four cases:
     - a whitelisted shadow (aware thread);
     - an in-bounds 4+-element array;
     - a logged thread kill (blind thread, or dev build);
     - a D11 scratch sink plus officer_mode off (aware thread, release build).
   - No other outcome exists, because of the bounds check in every handler.
   - For aware scripts, the analyzer's character-type inference proves that every ARRAY op whose index may be character-typed has a base that resolves to a whitelisted array or to an array with count ≥ 4.
   - Bases that cannot be resolved statically (pointer parameters) are listed for runtime coverage.
   - Block 13 is read only behind `officer_mode()` (§3.2).
3. **Logic completeness:**
   - For each aware script, the analyzer enumerates every character-decision site: compares with 0/1/2/145, `<3 <=2 >2 >=3 >3`, SWITCH on a character, IS_BIT_SET/SET_BIT/CLEAR_BIT with a character, 1..7 masks, loops over 0..2, count-slot loops, literal character arguments, **character values passed to natives**, **constant-index access to every catalogued per-character array (including IOFFSET chains into G114990)**, whole-array copies, three-way groups, reads of +4322/+4323, **and SET_PLAYER_MODEL / CHANGE_PLAYER_PED calls**.
   - Each must have a decision in the rule file. The build refuses a script with an undecided site and demotes it to blind with a warning.
4. **Runtime confirmation:**
   - the OOB logger shows zero unserved index-3 hits over the test plan;
   - coverage counters show every CASE3 / EXTEND-LOOP / UNROLLED / shadow rule exercised;
   - differential protagonist runs show no data difference (§5).

### 3.6 Global placement and relocation

- **No vanilla global is relocated or resized.** The save layout, existing saves and all IOFFSET chains stay exactly as they are.
- Officer data lives only in block 13, whose indices are ours and stay stable across game builds.
- Vanilla indices in the catalog and the rules move between game builds. They are re-derived through:
  - save names (standard_global_reg is emulated in C++ at start-up, the same idea as the saving survey's `reg.py`);
  - learn_ysc-style function pairing (as for `ysc_tables.inc`, 5,223 pairs);
  - the globals signature (header +0x18, 0xA2BE51AC).
- **Block 13 registration:** a generated script `ml_char4_reg.ysc` (joaat 0x4F0DCB34):
  - header +0x28 = (13<<18)|N, plus initial-value pages;
  - main resets block 13 to defaults, then START_SAVE_DATA(&G3407872, Nsaved, 1), REGISTER_* for the saved part, STOP_SAVE_DATA, ready flag;
  - it also contains the officer-aware bank-function copy used by the SDK (D10).
- It is started by startup's registration list: F′ of startup@143246 adds case 14, and startup@143036 changes `PUSH_U8 14`→`15` **(checked: list of 14 cases at @143253 ending with sp_dlc_registration (12) and sp_pilotschool_reg (13); loop @143017–143145; yields after cases 8 and 12 @143151; dependencies for 12/13 @143186)**.
  - Boot order is favourable: startup@238 runs the registration loop before standard_global_init (@317) and standard_global_reg (@384), and so before main (checked).
  - That the block is created when the program loads, and is re-created correctly after loads and SP↔MP transitions, is unverified (U28). This is why readiness goes through `officer_mode()`.
  - Fallback: the loader starts it at the same point.
- The DLC save path special-cases two program hashes (0xd2eba0: standard_global_reg and the unidentified 0xB08F5BE3). Saving under a never-seen script name is part of U4.

### 3.7 Engine changes

**Slot alias, k = 0 by default.** Applied when the officer becomes the player, removed before any other ped becomes the player, before any save is written, before any load begins, and at any teardown (§3.11):

1. Model static k (0x4714e50 + 0x10·k) := officer model hash.
2. prefix[k] (0x2944870 + 8·k, in .data) := pointer to our "SP3" string.
3. For each of the 187 StatId5 objects: dword k := joaat("SP3_" + name). Unresolved names get 0 (no stat) plus a warning, never left on the protagonist. The 9 runtime-built objects follow the prefix (verify).
4. Optional: unlock byte 0x43506d0+k := the officer's ability-unlocked flag (save and restore the protagonist's value).
5. Re-trigger 0x1c3bf90 through the CHANGE_PLAYER_PED in progress (index → k → SP3).

**Side effects of the alias that need checking (U29):**
- CHANGE_PLAYER_PED and SET_PLAYER_MODEL call `0xB5F970(&0x415ECF0, k)` when the new model equals static k, so under the alias Michael's routine runs for the officer. Its effect is unknown.
- All 21 engine compares against static 0 (helmet 0x168f629, buddy line 0x1b2099c, ability 0xd7xxxx …) treat **any** ped with the officer's freemode model as Michael while the alias is on, not only the player. Handle-based identity only protects scripts.
- Visible leftovers (guess, not verified in the exe because the strings are hashed): pause-menu header name and portrait, HUD/minimap blip colour, default radio station on entering a vehicle.

**Apply and remove points:**
- a loader wrapper around CHANGE_PLAYER_PED / SET_PLAYER_MODEL, using OverrideScriptNative in all calling scripts (60 scripts) plus direct use for the mod's own calls. SP SET_PLAYER_MODEL callers **(checked)**: main@569271 (Editor cleanup), benchmark@13612, mission_repeat_controller@665272, animal_controller@3275/@485624, selector@520435/@520752, maintransition@507065/@1516358; main_install@277 is install-time;
- a MinHook on the SP load handler 0xd1ef80 (or player PostLoad 0x222ba40) that forces removal before the save is applied;
- the teardown signals of §3.11.
- MinHook is already used for engine functions (`script.cpp:48`, `fixes.cpp:79`).

**Why k = 0 (Michael):**
- no ×2.5 inertia tweak (F/T only, 0xe0a6e0);
- no Trevor helmet rules (0x165a480).
- Known leftovers under the alias: Michael's lens flare, pilot-helmet prop 27 (the mod re-applies its own helmet), the BUDDY_SEES_MICHAEL_DEATH line, Michael's AI ped not recognised as a protagonist by the engine while the alias is active, the pause Skills page highlighting column 0 while showing Michael's real SP0 numbers, the 0xB5F970 routine, and freemode NPCs treated as Michael by the engine.
- Alternative (M0 decides): alias MP slot 4 instead and write 0x481DC38 := 4 after the switch. No protagonist leftovers and no ability, but possible MP-specific behaviour keyed on index ≥ 3 (unknown).

**SP3 stats:**
- An extra stats definition file of data-file type 0x23 in our generated DLC pack. The mounter vtable 0x27186a8 → 0x1eab1d0 → 0x1ea7d40 registers SP stats at DLC mount.
- It declares explicit `SP3_<name>` (characterStat = false) for every characterStat name in the extracted SP stats list.
- **Save category is decided by U6/U18.** Saved SP3 stats would be written into *every* SGTA5 save, protagonist saves included. That raises three unknowns: the capacity of the stats section (690 triples plus 187 StatId5 names), a vanilla load of unknown stat hashes, and leftovers after the mod is removed.
  - Preferred if any of these is bad: declare SP3_* without SP-save persistence (if the format allows it; guess) and have the broker mirror SP3_* values into block 13 before saves and restore them after load. The stats section then stays vanilla.

**Not reached by the alias (accept or optional patch):**
- the ability-input immediates 0xd7be0d / 0xd7be2a (on-foot activation);
- 0xa1a640 (audio), 0x1b41b90, 0x7dadd0;
- Pedtype-based pain and voice (Michael's pain bank fallback at 0x9d1720; the mod sets the voice through natives).

**MP safety:** the alias is never active in a network session. Removal is forced at the first transition signal (§3.11).

### 3.8 Data files

- **Extract** (through `ModLoader/debug_convert.txt` `extractall`):
  - content.xml / setup2.xml (data-file type names for 0x23, 0x24);
  - the SP and MP stats definition XMLs (characterStat names: the StatId5 name mapping and the hash-triple rule; save-category attributes for §3.7);
  - peds.meta / peds.ymt (freemode Pedtype, ability init field +0xfc);
  - common data lensflare_m/f/t, playerswitch*, pausemenu XML;
  - pilot_school.ysc, launcher_epsilon.ysc, ob_mp_shower_med.ysc;
  - every dlcpacks script_rel archive (to check for alternative copies of SP scripts, for owners of blocks 13–15, and to re-run the account-getter scan `D:\Projects\gtav-research\char4\rev\acct2.py`).
- **Generate:** the SP3 stats file plus its content.xml entry in our DLC pack; optionally peds.meta for an ability type (M7); text-label entries for the officer's name and bank account name (ATM CASE3).

### 3.9 Save and persistence

**Vanilla-clean invariant.** At the instant the engine serializes a save:
- the player ped is a protagonist;
- the alias is off;
- G2349[3] and G2349[145] hold vanilla values;
- **G143050[145].17 = 0** (vanilla);
- G122220/1/2 are in 0..2;
- no audited character-valued saved global holds 3;
- G143050[3] equals its session-start image;
- SPn stats, G62254[0..2] and PP_INFO[0..2] are untouched by officer activity (guaranteed by the shadow design and asserted);
- if SP3_* stats are SP-saved (U6), they are the only stats-section addition, and their effect on vanilla loads has passed D3.

**Officer save block:** registered by `ml_char4_reg` (§3.6), stored under DLCScriptSaveData inside the game save.
- Contents: the header (nonce, layout version, active flag, position, heading, game clock), all saved shadows (the officer counterparts of the vanilla saved arrays) and the stat mirror.
- Member names are `ML4_<vanilla save path>`. Loading is name-keyed, so the layout can grow without breaking older officer data.
- Bulky mod data (reports, records) goes in side files keyed by the nonce: `data\save\<nonce>.json`, pruned.
- Arrays and structs inside a DLC save block, and a block this size, are unproven (U4/U18). If they fail, D7 falls back to a minimal in-save header (nonce plus active flag) with everything else in side files.

**Save broker** (loader service):
- Interception (OverrideScriptNative in the calling scripts) **(checked list, §2.4)**:
  - SET_SAVE_MENU_ACTIVE (savegame_bed, save_anywhere);
  - DO_AUTO_SAVE (autosave_controller);
  - QUEUE_MISSION_REPEAT_SAVE (mission_repeat_controller);
  - **QUEUE_MISSION_REPEAT_SAVE_FOR_BENCHMARK_TEST** (benchmark).
- While the officer is the player, the call is swallowed, the sequence runs, and then the call is re-issued.
- Quick save (G9506, save_anywhere) is watched too.

**Autosave policy** (new):
- While the officer is the player, DO_AUTO_SAVE in autosave_controller is swallowed and recorded as *pending*. The 218 request sites keep incrementing G102550+10 as usual.
- How autosave_controller proceeds when the native does nothing (save icon, retry, counter reset) is checked in U22.
- The pending autosave is written once, by the broker, **right after the next switch to a protagonist completes**. The officer's state is flushed at switch-out, so this costs no extra swap or fade.
- Menu, bed and quick saves as the officer still run the full swap sequence below.
- The officer menu gets a "save now" item.
- The player is told once per session: "As the officer, autosaves happen when you switch to a protagonist."
- Trade-off: officer progress since the last switch is lost on a crash. No timed brokered autosave (user decision Q2); "save now" in the officer menu covers it.

Sequence (explicit saves as the officer):

1. Fade out and let the officer's state settle.
2. Flush engine-held officer values into block 13: inventory (into the WEAPONS shadow layout), SP3 stat mirror, position, heading, clock, look reference.
3. Remove the alias. CHANGE_PLAYER_PED to the protagonist that was left behind, using the game's take-control bookkeeping (§3.10).
4. Run the game's pre-save routine (save_anywhere@789 or autosave_controller@2234 through CallScriptFunction), so e-mails, ped info and the safehouse override are fresh.
5. Restore G2349[3], G2349[145] and G143050[145]. Sanitize (invariant check; abort and warn on failure). Write a new nonce and the active flag.
6. Trigger the save and wait for completion (GAME_SAVED, IS_AUTO_SAVE_IN_PROGRESS clear, SGTA5/MISREP mtime). Commit side files under the nonce.
7. Swap back, re-apply the alias, entry 3 and the hardening, fade in.

**Load:**
- The block is reset to defaults by ml_char4_reg before its STOP applies saved data. Without the reset, a save that lacks the block would inherit the previous session's officer.
- Then the mod reads the nonce:
  - officer active → create the officer and switch to him (today's Restore path), bind OFFICER_PED;
  - no block (old save) → legacy import (below);
  - otherwise defaults.

**Legacy import (decided):**
- Today's `officer` record (active flag and position) is imported only into the save whose game clock matches it within ±30 game minutes. This is the rule persist.cpp already applies.
- `officer_look` and `officer_reports` are shared by all slots today. They are **copied into every legacy save the first time that save is loaded**, so each save keeps what the player sees today. A notice says so.
- After import the legacy JSON is kept read-only as a backup and is never written again.

**ml::save v2 and SDK** (for every mod; D10):
- Existing API unchanged: `ml::game::CurrentCharacter()` / `CharacterIndex()` stay −1..2 (by model), so the existing `[3]` arrays (`examples/bank/main.cpp:158`, `examples/property/main.cpp:122-123`, `examples/property/garage.cpp:229-230`, `examples/social/main.cpp:122`, `examples/social/gen.cpp:59`; 23 call sites) stay in bounds.
- New opt-in API, available only when the loader's MLApi has the new members (`detail::Has` check):
  - `ml::game::Slot()` returns 0..3, where 3 is the officer;
  - `Character::Officer`;
  - ml::save slot 3 for mods that declare 4 slots;
  - per-save scoping by nonce;
  - events "save about to happen" and "save loaded";
  - `CharacterElement(global, character)` returns the vanilla element or the shadow, so our bank, stocks, property and browser pages read officer data uniformly.
- `Cash/AddCash/Pay` for the officer: `Pay` currently runs appinternet's bank function, which is blind and has no case 3 (§0.2 items 5, 6, 12). For `Officer` it runs the aware copy in `ml_char4_reg` (`RunFunction("ml_char4_reg", …)`) with **account 3**. That copy updates G62254[3] (shadow), the bank log shadow and SP3_TOTAL_CASH through its CASE3/UNROLLED rules. `Cash(Officer)` reads SP3_TOTAL_CASH.
- All `examples/*` are audited before slot 3 is enabled for them.

**Optional native officer saves (M7):** the engine saves with the officer as the player. This requires:
- the main@779336 gate, autosave_controller@314, startup_positioning;
- DO_AUTO_SAVE accepted under the alias;
- a load ordering in which the alias is active before PostLoad;
- main@14801 not firing: it forces a protagonist model when G153585 = 0 and compares hard-coded hashes, so the alias does not satisfy it.

It is decided only after U4/U5 show that vanilla recovers such saves (startup_positioning@0:233–332 falls back to the first available protagonist).

### 3.10 Interplay with the police mod (switching, saving, life)

**Phase A** (now; independent of the toolchain; follows the switching survey):
- In `SwitchTo`, before CHANGE_PLAYER_PED, do the game's bookkeeping for the protagonist being left:
  - store PP_INFO health (+290), armour (+294), weapons (+298 x477, wheel slots +1730/+1763), position, heading and room (+2300/+2310/+2314), velocity (+2318), wanted (+2328), time (+2296) (player_controller_b@28969 family);
  - G45490[c] = now + 20000;
  - 99964: [slot] = old ped, +5 = slot, +6 = 3, +7 = 4.
- Hand the protagonist over to player_controller (no mission-entity ownership; it adopts peds owned by no script or by player_controller_b, @253693–253885) instead of KeepProtagonist.
- Officer-relevant equivalents of take-control:
  - relationship groups (cops friendly; reset gangs, @26867);
  - SET_NEXT_PLAYER_TCMODIFIER("") (verify);
  - blip name, RESET_PLAYER_STAMINA;
  - SPECIAL_ABILITY_DEACTIVATE_FAST on the protagonist being left;
  - health floor.
- **Availability (new):**
  - Officer slot: shown only where the game's own wheel can open. It is greyed when wanted, on a mission, during the prologue or story segments, and when no protagonist could be switched back to.
  - The mod's own wheel (`wheel.cpp:146-147` today sets every protagonist slot to state 1 unconditionally **(checked)**) must mirror the game's rules: AVAILABLE G120247[c], and the selector's per-slot state G24447+20..23 when valid (meaning unverified, U34). A protagonist that the flow has made unavailable is greyed.
  - `officer.cpp` SwitchFrom (writes 99964+45/46/44 directly, around line 305) must refuse unavailable targets.
- The left-behind officer is managed by the mod: wander while visible, then record position, heading, time and patrol car and despawn. This replaces the immediate Drop().
- **Left-behind injuries (new):**
  - If the left-behind officer is killed, he is "hospitalised": the next switch to him starts at the nearest hospital, the hospital fee is charged to his account, and the last-known position is reset.
  - Damage to left-behind protagonists by the officer is left to the game, after U30 shows how player_controller reacts (PP_INFO health, AVAILABLE, script deaths).
- Re-apply relationships, uniform props and look after every respawn (respawn_controller@50930 resets the group to PLAYER at 51733).
- Guard arrest while the officer is the player (respawn_controller arrest loop, until it becomes aware in M2).
- Peyote (animal_controller) stays NONE.
  - For the officer, state 0 waits for `HAS_MODEL_LOADED(char_model(145)) = HAS_MODEL_LOADED(0)` (@621–@644, @518943) **(checked)**.
  - U33 checks whether a peyote picked up as the officer starts a deferred trip after the next switch. If it does, the mod blocks peyote pickups for the officer.

**Phase B** (M3, scripts become aware):
- selector@24088 opens the wheel for the officer;
- @517478 computes the slot-3 state;
- @511936 maps slot 3 → 3;
- the @513707 → @512023 "windfall" (Online/upsell) branch is replaced by the officer switch;
- player_controller_b: the scene for 3 (@614750 currently returns 318 "no scene" for c ≥ 3), @256 572–604 (target 3 → current), the model preload loop @634237, the stage-0 start-up choice;
- **ped creation for character 3:** the switch target is created by player_controller_b (CREATE_PED @108325, CREATE_PED_INSIDE_VEHICLE @637219; roles unverified). A rule after the creating call writes OFFICER_PED and calls the mod's "officer ped created" hook (look, uniform, relationships) before the ped is visible. Alternative: the mod pre-creates the ped and hands it over through 99964[3];
- the switch request API (99964+45/+46/+44) then works for the officer;
- the game's store and restore functions keep the officer's weapons, health, armour, VARIATIONS and last-known state in the shadows;
- the mod's own wheel (`wheel.cpp` OwnWheel) and switch are retired.
- Saving still goes through the broker; startup_positioning stays blind because loads always start as a protagonist.

**life.cpp / persist.cpp:**
- keep the schedule, and add "return where he was left" when the switch-back is recent (copy of the +2296/+2300 rule);
- persist.cpp moves to the officer save block plus nonce side files.

### 3.11 System paths that replace the player or the session

Each path tears the officer session down cleanly. Teardown means:
- record the officer state into block 13;
- remove the alias;
- restore G2349[3], G2349[145] and G143050[145];
- set `officer_mode()` false;
- mark OFFICER_PED stale.

The officer is restored only through a normal load of a brokered save.

| Path | Evidence | Signal and treatment |
|---|---|---|
| Rockstar Editor (pause menu) | main@465762: REPLAY_SYSTEM_HAS_REQUESTED_A_SCRIPT_CLEANUP → G80141 = 1, G104707 = 1 → @568590(1): FORCE_CLEANUP 8/64, CLEAR_AREA, then @569229–@569280 loops SET_PLAYER_MODEL(player_zero) until the model is a protagonist (@465152) **(checked)** | The loader polls the same native, or observes it through an override in main, and tears down first. What happens on return is unverified (guess: SP reloads from a save; U23). |
| Director Mode | main@784857: HAS_DIRECTOR_MODE_BEEN_LAUNCHED_BY_CODE → SET_DIRECTOR_MODE_LAUNCHED_BY_SCRIPT; availability @785577(1,0,119) → SET_DIRECTOR_MODE_AVAILABLE at @784904/@784912, **called only when the value changes** **(checked)**; director_mode restores the player with CHANGE_PLAYER_PED @909159 (also @627589) | OverrideScriptNative on SET_DIRECTOR_MODE_AVAILABLE in main forces 0 while the officer is active. The loader remembers main's last requested value and re-issues it when the officer is no longer active. If Director Mode is launched anyway, tear down (U24). |
| Benchmark (settings) | benchmark@11952–12025 repeat save (gated by G124084; meaning unknown, 1 in the dump), @13570–13621 forces player_zero, QUEUE_MISSION_REPEAT_LOAD @501391; command-line path RESTART_GAME @1075 / QUIT_GAME @1082 **(checked)** | The broker swaps to a protagonist before the benchmark's repeat save. The reload then restores the officer from the block (U25). |
| Pause menu → Replay Mission | mission_repeat_controller QUEUE_MISSION_REPEAT_SAVE @571897, SET_PLAYER_MODEL(player_zero) @665272 when the model is not a protagonist **(checked)**; in pausemenu_sp_repeat only the shared helper copies refer to the character (no gate found) | The broker handles the repeat save; teardown before the model change (U25). |
| Peyote trip | animal_controller@3275 SET_PLAYER_MODEL(char_model(static 149)) on return **(checked)** | NONE in v1 (§3.10; U33). |
| Going to GTA Online and back | main_persistent@2529528 sets G80362 = 1 (phone owner → 3) during the transition **(checked)**; maintransition SHUTDOWN_AND_LOAD_MOST_RECENT_SAVE ×4 on return | Teardown at the first transition signal (G80362 rising or maintransition starting; their meaning as transition flags is a guess), before freemode exists. The return loads the most recent save, which is clean. Officer progress since the last save is lost, as for vanilla progress (guess; U31). |
| main's forced-protagonist check | main@14801 (every second, when G153585 = 0) **(checked)** | Not reached while G153585 = 1. Relevant for M7 and for load ordering. |

---

## 4. Per-system treatment table

| System | Where it lives | Officer | Work |
|---|---|---|---|
| Who the player is | helpers in 734–777 scripts; G122220–2 | OWN (3 in aware scripts; 145 in blind) | §3.2 identity rewrite with officer_mode(); G122220–2 untouched |
| Character sheet entry | G2349[3], G118[3] (runtime); G143050[3] (saved) | OWN runtime entry; saved entry untouched | loader service; broker restore and check |
| Cash and HUD cash | G62254 (by account), SPn_TOTAL_CASH, StatId5 0x4eef0d4, HUD 0x54a550 | OWN | shadow 62254[3]; alias k; SP3_TOTAL_CASH; CASE3 on @857075 / @856578; UNROLLED on @856918 |
| Bank account and payments | wrapper gunclub_shop@854349 / carwash1@40944; bank function @854401 (param 0 = account); log G135796; G2349[c].17 | OWN, account 3 | 2349[3].17 = 3; wrapper `PUSH 3`→`PUSH 4` in aware scripts; CASE3 (manual @854444) in the bank function (F′); log shadow |
| ATM and phone bank page | atm_trigger@9716, appinternet@3077230: account from **G143050[c].17**; account-name SWITCH 0/1/2 | OWN | until M2: G143050[145].17 := 4 (refuse, broker restores); M2: atm_trigger aware (read 2349[c].17, CASE3 label); appinternet M5 or our browser |
| Michael-account leak through 145 | G2349[145].17 = 0 | fixed | 145.17 := 4 while the officer is the player and mission_triggerer_d is not running; M1: mission_triggerer_d safety patch |
| Hospital / arrest fees | respawn_controller@20594–@20677, @48484, @48074 | OWN | respawn_controller aware; hash-triple rule |
| Arrest weapon loss | respawn_controller@20681–@20800 | OWN (vanilla rule) | aware: WEAPONS shadow; until then guard arrest |
| Stocks | M_/F_/T_ groups G135563…135651; stock_controller@7748; appinternet@24146 | OWN | GROUP4 (33 slots); our browser page through CharacterElement |
| Weapons, ammo, tints, parachute | ped inventory; WEAPONS G118197; WEAPON_TO_SHOW G119629; store player_controller_b@29307 | OWN | shadows; game store and restore (M3); interim mod store in the same layout |
| Ammu-Nation | shop_controller (@96425 single gate for all shop types), gunclub_shop (177 triples, dual-mode) | OWN | both aware; **GUARD by shop type at @96425 (gun shops only until M4)**; SP3 stats; shop blips (same filter in blip_controller) |
| Per-character SP stats | 690 script triples; 187 StatId5 | OWN (SP3_*) | stats file + alias + triple rule; save category by U6/U18 |
| Skills applied by the engine (stamina, lung, shooting, flying, wheelie) | StatId5 by index | OWN | alias |
| Skills applied by scripts (strength, stealth) | main@34972–@35152 (main@35155 case 3 → MP enums) | OWN | main stays blind; the mod applies the melee and noise modifiers from SP3 values |
| Special ability | engine gates, unlock bytes, model info +0x280, input 0xd7bb70 | optional | M7 |
| Health and armour | G118189 / G118193 | OWN | shadows |
| Outfit indices | VARIATIONS G117899, backup G102236 | OWN | shadows; the mod re-applies head blend, overlays and hair tint |
| Clothes, barber, tattoo shops | COMPONENTS G117360, WARDROBE G121715, TATTOOS G117853; clothes_shop_sp / hairdo_shop_sp / tattoo_shop (freemode = MP path: STAT_SET_INT 29/26/26, packed-stat setters; whether these run in SP for 145 is unverified) | OWN through the mod (v1); vanilla shops NONE and **never started for the officer** | shop_controller GUARD; mod shop UI on freemode data, stored in block 13 (officer section) |
| Personal vehicles | STORED_VEH_DATA G120306, VEH_POS/HEAD, 101915/102210…, vehicle gens G147749 | OWN | shadows; property-mod garage key 3; vehicle_gen_controller: its one payment site is made aware with the other paid scripts (Q4, U9) |
| Impound | G152787 / 153341 / 153349, IMPOUND_CHAR G153339 (saved) | OWN later | M4 decision (game vs mod) |
| LS Customs | carmod_shop; CARMOD_DATA G115736; VIEWED_MODS G115752 | OWN | aware (M4); R-rule for VIEWED_MODS; shop_controller GUARD opened for it in M4 |
| Vehicle websites | WEB_VEHICLES G149731; our shop mods | OWN | our pages, key 3 |
| Pay 'n' Spray (engine charge) | 0x13ace70 | OWN | alias |
| Street cash pickups | 0x1030390 | OWN | alias |
| Phone handset and settings | G21666 = 3; CELLPHONE_SETTINGS G129050[3]; cellphone_flashhand@33357 default branch; **palette from G4525199 (block 17) in 11 scripts** | OWN (existing slot 3) | verify that SP code never writes slot 3 (U5); theme and ringtone; palette: set G4525199 while the officer is the player and restore on exit (unverified that it is MP-side runtime only), or patch the 11 sites if they become aware |
| Phone contacts | owner-3 columns (2349[x] +16, +23, +28; entry 96 has +28 = 1); appinternet owner-3 ops on contacts 147/82 | OWN | write the runtime column only, re-applied after load (keeps the save vanilla); U5 watches all three owner-3 fields |
| Contacts app crash | appcontacts@41041 G24150[owner] [3] | fix | safety patch (M1); until then: known crash when the officer confirms the first contact entry (prediction, U27) |
| Officer texts | PhonePresence +99[3] | OWN later | M7 |
| Protagonist calls, texts, e-mails | comms_controller Player_Char_Bitset (bits 0–2), masks 1..7, EMI_AR | NONE | blind (145 → tests bit 17): static check that no comm uses bit 17 |
| Story flow, missions, triggers | flow_controller, mission_triggerer_a–d, code_controller | NONE | blind; mission_triggerer_d safety patch for its account read |
| Random events | 38 re_* | NONE as content (rewards no longer reach Michael) | blind; optional suppression |
| Strangers & Freaks | randomchar_controller, 32 launchers | NONE | blind |
| Friends, booty calls | friends_*, bootycallhandler | NONE | blind |
| Strip clubs | stripclub (@4109 SWITCH 0/1/2), stripclub_drinking (payment sites) | SHARED service, OWN money; relationship data NONE | aware ("light"), Q4; U9 maps the sites |
| Prostitutes | pb_prostitute (payment site; PROSTITUTE_SAVED_DATA G125049 / SERVICES G125138 per character) | SHARED service, OWN money; relationship data NONE | aware ("light"), Q4; relationship arrays NONE |
| Minigame fees (golf, tennis, darts, range, hunting, towing, races) | debit sites in golf, tennis, darts, range_modern, hunting_ambient, towing, offroad_races, launcher_racing | fees OWN; records NONE | aware ("light"), Q4 |
| Family, timetable, switch scenes | player_timetable_scene, family_scene_* | NONE | blind (the officer's day is life.cpp) |
| Safehouses and beds | building_controller, savegame_bed | NONE (officer home = property 20) | — |
| SP properties and businesses | PROPERTIES G139903 owner enum | NONE | audit: 3 never written |
| Completion %, achievements, minigame records | completionpercentage_controller, achievement_controller, range, golf, pilot school | NONE | blind |
| Collectibles | global flags | SHARED | none (their autosave requests are deferred, §3.9) |
| Public services | taxiservice, carwash1/2, ob_vend1/2, cablecar, bigwheel, rollercoaster, act_cinema, ob_telescope | SHARED service, OWN money | aware ("light": wrapper + identity), Q4 |
| Peyote animal trips | animal_controller | NONE (v1) | blind; block pickups if U33 shows a deferred trip |
| Director Mode | director_mode; main@784857 availability | NONE | SET_DIRECTOR_MODE_AVAILABLE override in main (§3.11) |
| Rockstar Editor, benchmark, mission replay | main@465762, benchmark, mission_repeat_controller | NONE | teardown + broker (§3.11) |
| Mission replay / snapshots | G105547 / G108531; replay_controller, mission_repeat_controller | NONE | blind; never active with the officer |
| Character wheel | selector, G24447 [4] slot 3 | OWN slot 3 | Phase A overlay with availability rules → Phase B aware |
| Switching | player_controller_b; G99964 [4] | OWN | Phase A bookkeeping → Phase B aware (OFFICER_PED on creation) |
| Left-behind world | player_controller@252794 (cycles slots 0–2; vehicle tracking @252985–253153) | protagonists: the game; officer: the mod | hand-over (Phase A); injury policy (§3.10, U30) |
| Last-known state, wanted level | LAST_KNOWN_* G120199…G120227 | OWN | shadows |
| Relationship groups, TC modifier, blip name | player_controller_b@26867 / @3982 / @27303 | OWN | the mod (Phase A) |
| Saving | main@779336, autosave_controller, save_anywhere, savegame_bed, mission_repeat_controller, benchmark, MISREP | OWN data, own block | broker (§3.9); autosave deferral |
| GTA Online | generated pack mounted | inert | officer_mode() false; transition teardown; fallback refuse (D9) |
| Pause-menu Skills page, stats columns | 0x6b6600 / 0x6094e0 | NONE | officer stats in the mod UI |
| Lens flare, helmet, buddy-death line, CHANGE_PLAYER_PED routine 0xB5F970 | engine | inherited from k under the alias | accept; the mod sets the helmet; U29 |
| Pain, voice, Pedtype | 0x9d1720, tuning 0x434a290 | default / Michael fallback | voice natives; peds.meta later |
| Switch effect | 0xc61a60 | neutral | none |
| Our mods' per-character data | ml::save slots 0..2 | OWN slot 3 (opt-in) | ml::save v2, SDK v2 (D10) |

---

## 5. Verification strategy and game updates

### 5.1 Static checks (build-time; failing a check demotes the script to blind with a warning)

- **S1 Rewriter verifier:**
  - re-decode the output;
  - stack depth consistent where branches meet;
  - same stack effect for each edit;
  - page-end rule; 16-bit ranges;
  - ENTER/LEAVE parameter counts;
  - forwarders match the original signature;
  - F′ begins with the officer_mode() fallback to V.
- **S2 Decision completeness** for every aware script (§3.5 item 3), including NATIVE-ARG, UNROLLED and SET_PLAYER_MODEL sites.
- **S3 Index safety:** character-typed ARRAY indexes resolve to whitelisted or 4+-element arrays.
- **S4 Identity containment:**
  - no direct +4321 loads in aware scripts; no retargeted helpers in blind scripts;
  - audit of character-valued global writes;
  - **owner-domain report**: every array indexed by an owner-typed value has count ≥ 4 or a safety patch;
  - no block-13 access outside an officer_mode() region.
- **S5 Save block:** unique `ML4_` names, all names `[A-Za-z0-9._:]` and shorter than 64 characters, registered size equals the layout, vanilla registration unchanged.
- **S6 Bit-3 report:** every bitset reachable with 3 in aware scripts, with values from the 100% dump (gread). Bit 3 must be free or the site must be guarded.
- **S7 Writer report (new):**
  - native-table round trip: decode with the new code size, compare hashes;
  - the officer_mode native resolves and is not otherwise used in the program;
  - maximum call depth (< 128) and stack use per thread entry point against its start stack size;
  - forwarder fit (12 + 2p bytes), or proof that no function pointer to the function exists;
  - in-place edits limited to the allowed kinds (D5).
- **S8 Fallback equivalence:** for every modified function, F′ with officer_mode() = false is V, and every in-place edit is on the allowed list. (Not an Online gate any more, Q3: it guarantees vanilla behaviour whenever the officer is off or block 13 is not ready.)

### 5.2 Runtime asserts and logs

- **R1 OOB logger** (part of the detour): an unserved index 3 in an aware thread is an error (script, ip, global or static, idx, count). An index 3 or 145 in a blind thread is a warning (finds today's officer crashes, for example appcontacts). Count summary on the debug pipe.
- **R2 Thread-death watchdog:** threads reaching state 2 (thread+0x18), with the op decoded at thread+0x1C, plus kills from the null-block path (0x91f42a).
- **R3 Invariant checks:**
  - the vanilla-clean check before each save;
  - at every player change, the alias state must match the player model, and OFFICER_PED must equal the player while the officer is the logical player.
- **R4 Coverage counters** per rule and per shadow (block 13 debug area).
- **R5 Watches** on G2349[3], G2349[145].17, G143050[3], **G143050[145].17**, the owner-3 phone fields (+16, +23, +28 of every entry), G21666, G4525199 and the saved character-valued globals.
- **R6 Readiness log (new):** for each thread, the time of the first get_cur4 call relative to block 13 creation and to ml_char4_reg's start, at boot, after each load and after an SP↔MP round trip.

### 5.3 Differential tests

- **D1 Protagonist regression:**
  - same save copy; vanilla build vs patched build; scripted route through `tools/gtactl.py` (window off-screen, no focus);
  - block 0 dumps with `gdump.py` at fixed points; diff against a noise baseline taken from ≥ 3 vanilla runs;
  - expected: no difference in protagonist data.
- **D2 Officer isolation:** M/F/T cash, SPn stats, bank logs, PP_INFO[0..2] and G143050 identical before and after an officer session that buys, pays, uses the ATM, dies, gets arrested and saves.
- **D3 Vanilla-load check:** each test save is loaded with the loader disabled. The story must load, G122220–2 must be in 0..2, and the models, cash and weapons must match the pre-session values. A vanilla re-save followed by a modded load must show what happens to the officer block and to SP3_* stats (U4, U6).
- **D4 Officer-off equivalence:** with the pack mounted and officer_mode() forced false, gunclub_shop, shop_controller, carmod_shop and selector behave identically to vanilla in an SP differential run. (No Online session: Online is refused, Q3; U31 only checks a refused attempt.)

### 5.4 In-game test plan (per milestone)

Every scenario runs on **≥ 3–4 launches**, with success rates reported (memory note: behaviour is flaky).

**Rules:**
- **Never save into slot 03 (SGTA50001, the 100% save).**
- Copy the whole profile folder before each session (precedent: `Profiles_backup_2026-10-20`).
- Manual test saves go to new slots and are deleted afterwards.
- **Autosave tests** necessarily write slot 02 (SGTA50015). Before the run, record its hash and keep the profile copy. After the run, restore SGTA50015 and SGTA50015.bak from the copy and verify the hashes. Never run an autosave test unless the copy has been verified.

**Scenarios:**
- officer purchase at Ammu-Nation; **clothes, barber, tattoo and LS Customs entries as the officer (must not open before their milestone)**;
- car wash, vending, taxi; **strip club, prostitute, golf/tennis/darts/range fees (U9)**;
- **ATM and the phone's Internet bank page as the officer**, before and after the M2 fix;
- **phone contacts as the officer: confirm the first entry (appcontacts G24150)**;
- death, arrest;
- Pay 'n' Spray, cash pickup;
- switches officer↔M/F/T in all directions, including short switches; **switching into an unavailable protagonist (must be refused)**;
- **left-behind deaths: kill the left-behind officer as a protagonist; hurt a left-behind protagonist as the officer**;
- menu, bed, quick and autosave as the officer; **deferred autosave written at the next switch**;
- load of each save type;
- loading an old save (legacy import with clock match and without);
- mission start as a protagonist right after officer play;
- **Rockstar Editor round trip, Director Mode attempt, benchmark, pause menu → Replay Mission, peyote pickup then switch, GTA Online and back, all as the officer**;
- **sitting in a Gauntlet as the officer while mission_triggerer_d runs (hardening condition)**, if a suitable save exists;
- one hour of free roam with the OOB logger.

### 5.5 Game updates

- Everything is derived at loader start-up from the user's own game files: no shipped patched binaries.
- Script rules are keyed by function fingerprints. Vanilla global indices are re-derived from save names and learn_ysc pairing. Engine RVAs come from relocate signatures (`relocate sig/match`), for:
  - the detour site 0x91ee3d, its six continuations and the opcode list;
  - the GLOBAL_U24 null-block path;
  - the model statics, the prefix table, StatId5;
  - CHANGE_PLAYER_PED's dispatch;
  - the load handler and 0x1c3bf90.
- **Degradation rules (degrade-with-warning policy):**

| Failure | Result |
|---|---|
| an aware script's fingerprints do not match | that script becomes blind; warning |
| the detour site or alias data are not found | officer = 145 everywhere (today's behaviour); warning |
| a catalogued array has no save-name match | its shadow is disabled and the scripts touching it become blind |
| a stats name cannot be resolved | that StatId5 entry is set to 0 |
| block 13 not registered at runtime | officer_mode() stays false (officer = 145); warning |
| native table re-encoding fails S7 | the pack is not generated for that script; it stays vanilla |
| an unserved index 3 in a release build | D11: scratch sink, officer_mode() off for the session; warning |

- The cache is invalidated by the script CRC and the build hash. The per-update workflow: re-run the analyzer, review unmatched rules (estimate 1–3 days).

---

## 6. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| A save written with officer values in vanilla fields (PP_*_PED = 3, freemode player ped, G143050[3] or [145] changed, officer cash in SPn) | corrupts or confuses saves, including the 100% save | Computed identity (D2); the shadows never touch vanilla storage; the broker plus invariant check aborts a non-clean save; never use slot 03 in tests; slot 02 restore procedure; D3 vanilla-load test |
| A save path not intercepted (benchmark repeat save, Editor, a future path) | engine saves with the officer as the player | overrides on all four save natives (§3.9); G9506 / G102550 watches; §3.11 teardowns; R3 assert; DO_AUTO_SAVE also refuses non-protagonists (0x1d613d0) as a second line of defence unless the alias is on, so the alias is removed first |
| Autosave swaps on every purchase or collectible | screen blackouts, streaming stalls | deferred, merged autosave at the next switch (§3.9); U22 measures cost |
| Alias still active when a save loads | the loaded protagonist is not recognised; stats index wrong | forced removal in a hook on 0xd1ef80 / PostLoad; R3 assert at every player change |
| Alias side effects (0xB5F970 for the officer, freemode NPCs treated as Michael by 21 engine compares, pause header or blip colour) | odd behaviour or wrong UI | U29/U6 checks; the MP-slot alternative if they are bad |
| Stats falling to MP0/MP1 in memory | Online character data touched (flush behaviour unknown) | the alias keeps the index on k; never let it fall to 3/4 unless the MP-slot alternative is chosen deliberately; inert in Online (D9) |
| SP3_* stats in every save | stats-section overflow, vanilla load issues, leftovers after uninstall | U6/U18; non-saved SP3 stats + block-13 mirror if needed |
| Story content broken | lost progress | story scripts stay blind and unchanged; the data detour cannot change non-crashing executions; D1 differential runs |
| The 145 hardening kills mission_triggerer_d | story trigger lost until reload | hardening only while mission_triggerer_d is not running; safety patch in M1; U35 |
| Thread deaths from missed index-3 sites | a controller stops (respawn_controller etc.); possible kill/relaunch loop (main@789315–789383 restarts respawn_controller while the player is injured or arrested) | dev: fail loud; release: D11 scratch + officer_mode off; OOB logger; S3; coverage |
| Reading block 13 before it exists | the reader is killed (null-block path) | officer_mode() gate; boot order (§3.6); R6; U28 |
| Integrity checks around interpreter code (obfuscated .text), or 0x13D24E0 (walks all global blocks with sampling and an imported call; guess: integrity check) | crash or detection | M0 soak tests; fallback to per-site stubs (no .text patch) and a block-11 extension; SP only |
| Detour thunk corrupts interpreter state | random crashes | assembly fast path that preserves every live register; C++ only on the log path with full save and aligned rsp; dispatch by opcode (shared block); M0 soak |
| Entry 3 or owner-3 data really used in SP (MP placeholder, entry 96 field 28, appinternet owner-3 ops, palette G4525199) | officer data visible in odd places | no constant-3 indexing of 2349/118 entries exists (checked); runtime watch U5 on all three owner-3 fields; the saved copy is never written |
| Owner-domain crashes (G24150[3]) | phone app dies for the officer today | safety patch; R1 warnings in blind threads; U27 |
| Bit-3 collisions in per-character bitsets | wrong flags for protagonists | S6 report; guard or GROUP4 where bit 3 is used |
| Shared-global leakage (aware script writes 3; blind or saved readers) | crash or bad save | S4 audit; broker sanitizer for saved character-valued globals |
| Whole-array copies, snapshot restores and unrolled code skipping the shadow | officer data lost or stale | enumerated (6 struct copies, UNROLLED class); the officer is never in missions; Director Mode blocked |
| Another mod replaces an aware script | patch mismatch | apply rules to the active copy; otherwise blind plus warning |
| Rewritten dual-mode scripts and startup running in GTA Online | ban / instability | cannot happen in practice: the loader and pack need `-nobattleye`, which keeps the game out of Online (Q3). Defence in depth: officer_mode() false in network games; teardown at the first transition signal (U31) |
| Existing SDK mods see character 3 | out-of-bounds writes in the game process | D10: old API unchanged; opt-in v2 with a version check; audit examples/* |
| Officer identity lost when the game replaces the player ped | officer silently becomes 145, default looks | OFFICER_PED re-bind and look re-apply in the wrapper; per-frame check (R3); teardown for protagonist-model paths |
| Officer used to get around story locks | story breaks | Phase A availability rules (§3.10) |
| Save size limit exceeded by the officer block, or DLC block cannot hold arrays/structs | save failure | U4/U18 probes; keep the in-save core small (≈3.6k slots); fallback to header + side files |
| An unclaimed DLCScriptSaveData block dropped by a vanilla re-save | officer data lost for that save | nonce side-file backup; restore with a warning |
| Performance of F′ / the forwarder hop / extended loops | frame time | OOB path is rare (index 3 only); measure in shop menus (wardrobe hits 33,981 COMPONENTS sites) |
| Another freemode-model player treated as the officer | identity leak | officer identified by ped handle plus officer_mode(), not by model |
| An entry-145 crash today (arrest as the officer) | respawn_controller dies | guard now (Phase A); respawn_controller aware in M2 |

---

## 7. Unknowns and the experiments that settle them

| ID | Unknown | Experiment | Decides | When |
|---|---|---|---|---|
| U1 | Is a detour at 0x91ee3d (5-byte jmp to a near thunk, opcode dispatch, jumps back into the six continuations) stable, and free of integrity reactions? | Phase 1: log-only detour (including blind-thread warnings); play as the officer and as protagonists for 60 min over 3–4 launches. Phase 2: serve G62254[3] from block 13 for one aware test program; read and write it from that program; confirm a blind thread is still killed. | D4 primary vs the stub fallback | M0 |
| U2 | Can replacement .ysc code grow (appended pages, F′ + V, in-place forwarder, retargeted CALL, appended native with re-encoded table)? | blip_controller (already proven to load from a pack): append a page, move one function to F′/V with a forwarder, add one native entry, add a function that forces a NOP-padded page boundary; confirm the code size through GetScriptCode, native resolution, and that no thread dies | rewrite technique | M0 |
| U3 | Does a script-registered global block 13 work: reset per session, survives switches and mission replay, tolerated by 0x13D24E0? | ml_char4_reg with header (13<<18)\|N, started from the patched startup list; read and write G3407872+k from scripts and ScriptGlobal; 1-hour soak | storage location (fallback: extend block 11 / sp_dlc_registration) | M0 |
| U4 | Own save block: round trip; **arrays and structs (START_SAVE_ARRAY_WITH_SIZE / START_SAVE_STRUCT_WITH_SIZE) inside a DLC block; a never-seen script name (0xd2eba0 special-cases two hashes)**; a vanilla load of such a save; a vanilla re-save (kept or dropped?); quick save and MISREP coverage; old saves keep the defaults | START_SAVE_DATA + REGISTER_INT64 "ML4_TEST_i" + one array + one struct; save to a spare slot; reload; load with the loader disabled; re-save in vanilla; reload with the loader | D7, side-file need | M0 |
| U5 | Is entry 3 or any owner-3 field used by SP code while protagonists are played? | hardware read and write watches on G2349[3] .0/.3/.17, G118[3], G143050[3], G129050[3], owner-3 fields +16/+23/+28 of all entries (entry 96 especially), during play as M/F/T (phone, contacts, texts, the Internet app, a mission) | D1 confirmation (fallback: officer phone in a separate block area) | M0 |
| U6 | Engine alias: does the HUD refresh on a 0→0 index change? Are SP3_* stats from a type-0x23 DLC file registered and saved by the stats section, and what do they do to a vanilla load and re-save? Values of the [0x47AA0D0]/[0x47AA0DC] flags in SP. **Alias leftovers: pause header name and portrait, minimap blip colour, default radio station.** | register SP3_TOTAL_CASH in a test stats file; alias k = 0; switch officer↔Michael and officer↔Trevor; check HUD, pickups, respray; screenshots with the alias on and off; save, reload, STAT_GET_INT; load with the loader disabled, re-save, compare | D6, k choice, SP3 save category, stat mirror need | M0 |
| U7 | Tracer bullet: what officer-aware shop_controller + gunclub_shop touch | apply the identity rewrite to these two only (with the shop-type GUARD); detour in log mode; enter Ammu-Nation as the officer; list arrays; then serve them | M2 sizing; catalog validation | M0 |
| U8 | Arrest as the officer today | let police arrest the officer; EnumScripts afterwards (prediction: respawn_controller gone) | Phase A guard urgency | M0 |
| U9 | Service payment results with account "none" (refused vs free), for **all** paid free-roam scripts: taxi, car wash, vending, cable car, fairground, cinema, telescope, strip club and drinks, prostitutes, golf, tennis, darts, range, hunting, towing, races, vehicle_gen_controller | with 2349[145].17 = 4, the officer uses each service; read the cash and logs of M/F/T; list every payment site per script | 145 hardening side effects until M2; the full list of scripts made aware in M2 (all of them, user decision Q4) | M0 |
| U10 | Is G2349 (entry 145/3) rebuilt from G143050 at load or init? | write markers, load a save, read them back | re-apply frequency | M0 |
| U11 | Bit 3 free in Player_Char_Bitset, FLOW_HELP, G97513, G93274[].11? | S6 static report plus setting bit 3 on a queued comm or help entry and observing | logic decisions | M1 |
| U12 | Character type of the 254 unproven count-3 arrays; local arrays; pointer-parameter bases; unrolled constant-index sites for all catalogued arrays | C++ analyzer with type inference plus save-name review | final catalog | M1 |
| U13 | Roles of snapshots G105547/G108531; G102432, G100622, G34212 +0/+12 meanings | dumps before a mission, after a fail, after a replay; diffs | catalog decisions | M1 |
| U14 | The selector slot-3 "windfall" path (natives 0x317989BB72A2EBFB, 0xA6E586C3BD772B7B; selector's freemode SET_PLAYER_MODEL @520435/@520752) | OverrideScriptNative logging on selector | Phase B design | M3 |
| U15 | Does player_controller adopt a protagonist handed over by the mod? Does the game keep the officer in 99964[3] when switching away? | hand-over test (switching survey) | Phase A/B design | M1b |
| U16 | The officer's TC modifier, short switches, relationship group after respawn | the experiments listed in the switching survey | Phase A details | M1b |
| U17 | Freemode Pedtype and ability type; helmet behaviour under the alias | GET_PED_TYPE; peds.meta extraction; aircraft and quad bike | M7, helmet handling | M2 |
| U18 | Maximum script save size; **stats-section capacity with SP3_* stats** | 1k / 8k / 32k / 64k-member probe blocks; saves with N SP3 stats, file sizes compared | in-save core size; SP3 save category | M0 |
| U19 | Missing scripts and dlcpacks versions | extractall: the three scripts plus every dlcpacks script_rel; re-run the scans (including acct2.py) | completeness | M0 (extraction), M1 (scan) |
| U20 | StatId5 names (118 unresolved) and the 690 triple names | extract the stats XMLs; match joaat("SPk_" + name) | alias and triple rules | M0 |
| U21 | Does the engine credit SPn stats while the officer is played *without* the alias (today's leakage)? | snapshot all SPn stats before and after 10 min of officer play | urgency of the alias | M0 |
| U22 | Cost of a broker swap (protagonist near / far); how autosave_controller behaves when DO_AUTO_SAVE is swallowed (icon, retry, G102550+10 reset) | timed swaps over 3 launches; OverrideScriptNative swallow test with logging of G102550 | autosave policy details | M0 |
| U23 | Rockstar Editor round trip as the officer: what state comes back (reload? stale OFFICER_PED? alias?) | enter the Editor as the officer, return, dump state | §3.11 Editor teardown | M0 |
| U24 | Does forcing SET_DIRECTOR_MODE_AVAILABLE(0) in main block Director Mode, and is the re-issue on officer exit correct? | override test; try launching from the Editor | Director Mode block | M0 |
| U25 | Benchmark and pause-menu Replay Mission as the officer through the broker | run both as the officer with the broker; check the reloaded state | §3.11 | M2 |
| U26 | Does PLAYER_PED_ID change after SET_PLAYER_MODEL (and after the mod's gender change)? | log the handle before and after | OFFICER_PED re-bind | M0 |
| U27 | Today's phone as the officer: does appcontacts die on G24150[3]? | confirm the first contact entry as the officer; EnumScripts | safety-patch urgency | M0 |
| U28 | When is block 13 created (program load vs first run), and is it present after a load and after an SP↔MP round trip before any aware thread runs? | R6 readiness log over boots and loads | officer_mode() readiness rules | M0 |
| U29 | CHANGE_PLAYER_PED under the alias: effect of 0xB5F970(…, 0) for the officer; a freemode NPC nearby while the alias is on | switch to the officer with the alias on; compare ped flags, ability state and audio with the alias off | k choice | M0 |
| U30 | Left-behind deaths: the officer killed by a protagonist; a left-behind protagonist killed by the officer | kill each kind; read PP_INFO health, AVAILABLE; watch for script deaths | left-behind policy | M1b |
| U31 | A refused Online attempt as the officer (wheel MP slot if still reachable, pause-menu tab 42, invites): do the transition scripts start, which signal fires first (G80362, maintransition), and does story mode come back sane? | try each entry point as the officer with `-nobattleye`; transition timing log; EnumScripts and invariant check afterwards | D9 teardown trigger | M2 |
| U32 | What character-taking natives do with 3 (SETUP_SHOP_PED_APPAREL_QUERY_TU, GET_NUM_TATTOO_SHOP_DLC_ITEMS, _GET_STAT_HASH_FOR_CHARACTER_STAT, packed stats) | call each with 0..4 from a test script; compare results | NATIVE-ARG decisions | M1 |
| U33 | Peyote picked up as the officer: does a trip start after the next switch? | pick up, switch, observe animal_controller state | peyote policy | M1b |
| U34 | Selector per-slot state (G24447+20..23) meaning; story locks on protagonists; prologue | dumps of G24447 and G120247 in locked and unlocked states | Phase A availability | M1b |
| U35 | Is mission_triggerer_d running in the 100% save, and when? | EnumScripts over a session | hardening condition | M0 |

---

## 8. Phased plan

Effort is in engineer-weeks for one experienced engineer, with ranges. M1b runs in parallel.

**M0: De-risk (4–5 weeks).** Starts with the biggest unknowns.
- Work:
  - extraction (U19, U20);
  - U1 detour (log, then serve; opcode dispatch, thread gating);
  - U2 code growth (including native re-encoding);
  - U3 block 13, U28 readiness;
  - U4 save block (arrays, structs, script name) and vanilla compatibility, U18 size and stats section;
  - U5 entry-3 and owner-3 watches;
  - U6 alias with SP3_TOTAL_CASH and leftovers, U29 alias dispatch;
  - U7 tracer bullet (Ammu-Nation as officer = 3);
  - U8 arrest, U9 services (full list), U10 rebuild, U21 stat leakage;
  - U22 autosave cost, U23 Editor, U24 Director Mode, U26 handle change, U27 contacts crash, U35 mission_triggerer_d.
- Immediately shippable safety fixes (3–4 days inside M0; loader and mod only, no toolchain):
  - G2349[145].17 := 4 while the officer is the player **and no mission_triggerer_d thread exists**;
  - G143050[145].17 := 4 while the officer is the player, restored by the mod's current save routine and on exit;
  - arrest guard;
  - SET_DIRECTOR_MODE_AVAILABLE override in main;
  - Editor teardown (restore the hardening values).
- Done when:
  - each experiment has a result over ≥ 3 launches;
  - a written go/no-go exists for D1 (value 3), D4 (detour vs stubs), D6 (alias k = 0 vs MP slot; SP3 save category), D7 (own save block vs side file; autosave policy);
  - the tracer bullet buys a weapon as the officer with no thread deaths, Trevor's phone, calls and blips are unchanged, and the clothes shop does not open for the officer.

**M1: Toolchain and catalog (7–9 weeks).**
- Work:
  - C++ script library: CFG, stack analysis, assembler with page and branch rules, resource writer for code, statics, globals pages **and native table (re-encoding, append)**;
  - fingerprint library shared with learn_ysc / relocate;
  - analyzer: character-type and owner-type inference, site enumeration (including NATIVE-ARG, UNROLLED, SET_PLAYER_MODEL), S1–S8 checks, residual scan;
  - catalog v1 (all per-character arrays with rule kinds and save names);
  - block 13 layout generator and the ml_char4_reg generator;
  - officer_mode() native and the F′/V/forwarder scheme;
  - production detour (assembly thunk, opcode dispatch, thread gating, whitelist, logger, D11 release policy); coverage counters;
  - safety patches: appcontacts G24150, mission_triggerer_d account read;
  - rule-file format and build pipeline into the generated pack, with cache.
- Done when:
  - the pipeline regenerates and verifies the identity rewrite for every candidate script;
  - D1 differential runs (≥ 3 launches) with all candidate scripts rewritten but the officer inactive show zero protagonist data differences;
  - S8 passes for every rewritten script;
  - the catalog covers 100% of U7's logged arrays.

**M1b: Police-mod switching hygiene, Phase A (2–3 weeks, parallel).**
- Work:
  - everything listed in §3.10 Phase A, including the availability rules (U34), the left-behind injury policy (U30) and peyote (U33);
  - OFFICER_PED re-bind and look re-apply on player-ped replacement;
  - per-save officer records through a nonce side file (an interim persist.cpp).
- Done when:
  - switching officer↔each protagonist ×10 per launch, ×3 launches leaves correct PP_INFO for the protagonist that was left;
  - player_controller owns and despawns the protagonist;
  - no stale weapons or position on switch-back;
  - unavailable protagonists are refused;
  - the arrest guard holds.

**M2: Economy slice (6–8 weeks).**
- Work:
  - aware: shop_controller (shop-type GUARD), gunclub_shop, respawn_controller, blip_controller (shop blips, same filter), **atm_trigger**, and every paid free-roam script listed by U9 (Q4);
  - account 3 (wrapper edits, bank function manual CASE3 and UNROLLED sync, log shadow);
  - cash shadow and sync; the hash-triple rule;
  - alias k plus the SP3 stats file (save category from U6/U18);
  - save broker v1 (menu, bed, quick save, deferred autosave, mission repeat, benchmark) plus the officer save block v1 (header, cash mirror, weapons, health, armour, last-known);
  - §3.11 teardowns (Editor, Director Mode, benchmark, replay, Online transition);
  - refused-Online check and transition teardown (D9, U31);
  - legacy import (decided rule);
  - SDK v2 (D10): opt-in slot API, ml::save slot 3 and events, officer Pay through ml_char4_reg's aware bank function; audit examples/*.
- Done when, over ≥ 3 launches each:
  - an officer purchase at Ammu-Nation is charged to the officer's account, the HUD shows SP3 cash, and the unlocks persist;
  - the ATM shows the officer's account and log;
  - the chosen services charge the officer;
  - the hospital fee comes from the officer;
  - D2 isolation holds (M/F/T cash, stats and logs unchanged);
  - saves from every path reload the officer state, deferred autosaves are written at the next switch, and D3 vanilla loads are clean;
  - D4 officer-off equivalence holds;
  - existing example mods behave unchanged with the officer active.

**M3: Switching and personal state through the game, Phase B (5–7 weeks).**
- Work:
  - aware: selector, player_controller_b, and the switch-related parts of player_controller;
  - officer scene choice;
  - OFFICER_PED binding at the game's ped creation (§3.10);
  - the game wheel opens for the officer, slot 3 is selectable, and the request API switches to the officer;
  - weapons, health, armour, VARIATIONS and last-known state through the game's store and restore functions;
  - retire OwnWheel and the mod-driven switch.
- Done when:
  - the vanilla wheel is used in both directions;
  - short and long switches work;
  - the officer never appears with default looks;
  - no OOB misses in 60 min of play over 3 launches;
  - D1 shows no protagonist differences.

**M4: Vehicles (4–6 weeks).**
- Work:
  - carmod_shop aware (CARMOD_DATA, VIEWED_MODS R-rule) and the shop_controller GUARD opened for it;
  - STORED_VEH_DATA and the left-behind vehicle arrays for the officer;
  - property-mod garage and vehicle shops keyed 3;
  - impound decision (game vs mod).
- Done when: the officer mods a car at LS Customs from his own account; his personal car persists across switch, save and load; protagonist garages are unaffected.

**M5: Phone, bank site, stocks (3–5 weeks).**
- Work:
  - officer contacts in the owner-3 column (runtime only, re-applied after load);
  - settings, ringtone and palette (G4525199);
  - stock GROUP4 and our browser bank and stock pages through CharacterElement;
  - decide whether appinternet (bank page G143050 read, owner-3 contact ops) / stock_controller become aware or are fully replaced by our browser.
- Done when: the officer's phone shows only his contacts; trades and bank history are his own; the U5 watches stay silent for protagonists.

**M6: Appearance (3–4 weeks).**
- Work: mod-driven freemode clothing shop, barber and tattoo parlour; storage in block 13; re-apply after switch, respawn and load.
- Done when: purchases persist per save slot and do not leak across slots.

**M7: Optional extensions (3–6 weeks, each go/no-go).**
- Special ability (model info type, unlock byte under the alias, input immediates patch);
- native officer saves (§3.9, including main@14801);
- officer stats page in the mod UI;
- officer texts and calls through slot 3.

**M8: Hardening and update drill (3 weeks).**
- Work:
  - full regression suite through gtactl, including the §5.4 system-path scenarios;
  - an update drill: re-derive all rules, RVAs and globals against a different build or the legacy-converted set, verifying degrade-with-warning paths;
  - documentation in research/phase0.md;
  - release.
- Done when:
  - all static checks are green;
  - the D1/D2/D3/D4 suites pass on 4/4 launches;
  - an injected rule mismatch demotes exactly one script, with a visible warning.

**Total:** about 35–47 engineer-weeks for M0–M6 plus M8 (M1b in parallel, M7 optional).

---

## Appendix: survey artefacts to reuse (all absolute paths)

- Save layout (emulated registration): `D:\Projects\gtav-research\char4\svdim_q7\names.tsv`
- Per-character save arrays: `D:\Projects\gtav-research\char4\svdim_q7\per_char_arrays.txt`
- Curated per-character catalog: `D:\Projects\gtav-research\char4\catalog.txt`
- All global array shapes: `D:\Projects\gtav-research\char4\r3_arrays.txt`
- Save registrations with indices: `D:\Projects\gtav-research\char4\savereg.tsv`
- Identity taint scanner: `D:\Projects\gtav-research\char4\scan\scan.py`
- Rewrite-technique analyzers: `D:\Projects\gtav-research\char4\tech4\an2.py`
- Engine helpers: `D:\Projects\gtav-research\char4\exedim\xr.py`
- Payment call sites: `D:\Projects\gtav-research\char4\w\debit_calls.txt`
- Per-character hash triples: `D:\Projects\gtav-research\char4\w\alltriples.json`
- **Revision 2 checks:**
  - account-getter caller scan (all 119 scripts): `D:\Projects\gtav-research\char4\rev\acct2.py`
  - address-range viewer for the disassembly: `D:\Projects\gtav-research\char4\rev\at.sh`
  - G2349 / G143050 dumps: `D:\Projects\gtav-research\char4\rev\g2349.txt`, `D:\Projects\gtav-research\char4\rev\g143050.txt`
  - interpreter disassembly (0x91dd30): `D:\Projects\gtav-research\char4\rev\vmfunc.txt`
  - CHANGE_PLAYER_PED (0xc84a10): `D:\Projects\gtav-research\char4\rev\c85.txt`
  - DLC save registration (0xd2eba0): `D:\Projects\gtav-research\char4\rev\d2eba0.txt`
- Repo tools: `D:\Projects\GTAVE-ModLoader\tools\gtactl.py`, `D:\Projects\GTAVE-ModLoader\tools\globals.py`, `D:\Projects\GTAVE-ModLoader\tools\ysc_dis.py`, `D:\Projects\GTAVE-ModLoader\build\Release\relocate.exe`, `D:\Projects\gtav-research\globals\gread.py`, `D:\Projects\gtav-research\globals\gdump.py`

## Appendix: Critique items rejected

No critique item was rejected outright. The parts below were rejected or corrected after re-checking. Everything else was verified and merged.

| Critique item (part) | Decision | Reason / evidence |
|---|---|---|
| clothes_shop_sp, hairdo_shop_sp and tattoo_shop "call … STAT_SAVE" | **Rejected (detail only)** | None of them calls STAT_SAVE. They call STAT_SAVE_PENDING_OR_REQUESTED, STAT_SET_INT (29 / 26 / 26) and the packed-stat setters (checked). The main point (single shop_controller gate; MP stat code in those scripts) stands, and whether that MP code runs in SP for 145 is marked unverified. |
| "DO_AUTO_SAVE is called only by autosave_controller (and benchmark)" | **Corrected** | benchmark does not call DO_AUTO_SAVE. It calls QUEUE_MISSION_REPEAT_SAVE_FOR_BENCHMARK_TEST and QUEUE_MISSION_REPEAT_LOAD (checked). |
| Account-getter scan "in all 55 scripts that contain it" | **Count corrected, conclusion kept** | The critique's scanner (`D:\Projects\gtav-research\char4\crit\acct.py` (not kept)) anchors addresses at line start, but lines below address 100000 are space-padded, so it missed most functions. The corrected scan finds 119 scripts and 119 getter bodies, and these are all reads of G2349[c].17. The unguarded callers are still only flow_controller@585224 (guarded by is_playable at @585133, never 145) and mission_triggerer_d (2 sites). |
| "124 `PUSH 2; GLOBAL_U16 62254; ARRAY_U8*` sites in 57 scripts" | **Count corrected** | 261 sites in 123 scripts with a CRLF-tolerant regex (checked). The point (unrolled constant-index code needs a rule class) stands. |
| Detour: "call depth thread+0x6C", "stack size thread+0x58" | **Corrected** | These offsets are r12-relative and r12 = thread+8 (0x92076e writes state at [r12+0x10] = thread+0x18). The thread offsets are +0x74 (depth) and +0x60 (stack size), matching `scripts.cpp:23`. Mechanism accepted. |
| "Gating on the calling thread is what makes the zero-regression claim true" | **Partly rejected** | The zero-regression property (no change to non-crashing vanilla executions) holds without gating, because the detour only acts where vanilla kills the thread. Gating was adopted anyway: it keeps blind threads' vanilla crashes visible and stops them from silently reading officer shadows (phone-owner and "3 = MP" paths). |
| "Nothing shows that aware scripts cannot call get_cur4 before block 13 exists" | **Partly rejected** | At boot the order is favourable: startup@238 runs the registration loop (which would start ml_char4_reg) before standard_global_init (@317), standard_global_reg (@384) and main (checked). Block creation timing and re-creation after loads and transitions remain unverified, so the officer_mode() guard and U28 were adopted. |
| "The loader's Online detection fires only once the freemode thread exists" | **Corrected** | It also checks NETWORK_IS_SESSION_STARTED every tick (`core.cpp:277-278`). Both signals still come after the transition scripts start, so the recommendation (teardown at the first transition signal) stands. |
| "selector@31464 and main_persistent@2529593 can also clear G153585" | **Unverified, kept as a note** | Both write their parameter into G153585. The one main_persistent call inspected passes 1 (@2529532). A call that passes 0 was not found. |
| "MP transition scripts may already have read G2349[3] / [145].17 = 4 … an MP payment for character 3 would reach G62254[3]" | **Kept as a guess** | Plausible but not demonstrated. It is covered by the teardown at the first transition signal and by U31. |
| G80626 ∉ {80,81,82} and the Gauntlet condition for mission_triggerer_d | **Accepted (verified)** | The conditions precede the call (@501064–501095, @501151). Reaching this code also needs mission_triggerer_d to be running (U35). |