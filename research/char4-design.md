# Officer as a Real 4th Protagonist: Design Document (revision 4)

Status: revision 4 = revision 3 plus the third review round (§0.5). Revision 3 = revision 2, the user's decisions of 2026-10-22 (§0.0) and the second review round (§0.3; questions answered in §0.4); summary in Chinese in research/phase0.md §55. Survey data, analyzers and dumps: `D:\Projects\gtav-research\char4` (game-derived, never committed). Scope: the story-mode police officer (examples/police), played on `mp_m_freemode_01` (0x705E61F2) or `mp_f_freemode_01`.

Inputs:
- seven surveys: identity gates, per-character globals, saving, engine, gameplay inventory, script-rewrite technique, switching and integration;
- two critiques of revision 1. Every item in them was re-checked against the local disassembly, the globals dump, the exe dump and the repo. Accepted items are merged below. Rejected or corrected items are listed in the last appendix.
- a second review round on revision 2 (journey, mechanism and state lenses). Its verified items are merged and listed in §0.3; the parts rejected or corrected are in the last appendix.
- a third review round on revision 3 (journey, mechanism and state lenses; 15 verified items, two of them state-lens duplicates). They are merged and listed in §0.5; the parts rejected or corrected are in the last appendix.
- spot checks made for this document, marked **(checked)**. For revision 3 the main evidence of every round-2 item was re-checked again (autosave_controller, the launchers, selector@24088, save_anywhere, startup's registration loop, the loader and police-mod sources, natmap). For revision 4 the main evidence of the round-3 items was spot-checked again: Reset 0x921930's argument copy, the placement guard 0x91bb64–0x91bb91 and its table in the dump, startup@143017–@143145, the repeat-save loops, player_controller's keep test and deletes, taxilauncher@35914, taxiservice's G114037 stores, pb_prostitute@15489, appcontacts@45289, autosave_controller@915–@951, range_modern static 90, respawn_controller@30387, and the cited lines of guard.cpp, officer.cpp, wheel.cpp, replace.cpp, dlcpacks.cpp, natives.cpp, scripts.cpp, dllmain.cpp and natmap.

Evidence notation:
- `script@addr`: Enhanced disassembly in `D:/Projects/gtav-research/dis_enhanced`;
- `G<n>`: script global index;
- `0x…`: RVA in `GTA5_Enhanced.dump.exe`;
- `file:line`: repo `D:/Projects/GTAVE-ModLoader`;
- thread offsets follow `src/loader/game/scripts.cpp:23`: +0x10 program hash, +0x18 state, +0x1C ip, +0x60 stack size, +0x74 call depth, +0xC8 first argument static (static count − argument count, set by Reset 0x921930), +0x154 name. Inside the interpreter, r12 = thread+8.
- program offsets (`scripts.cpp:29` and §2.3): +0x18 globals signature, +0x1C code size, +0x20 argument count, +0x24 static count, +0x28 globals block (>> 18) and size, +0x2C native count, +0x30 static initial values, +0x40 native table, +0x58 name hash.

"Guess" marks anything not verified. "Unverified" marks a claim taken from a survey or critique that was not re-checked.

### 0.0 User decisions (2026-10-22)

All four are the recommended options; the sections below already reflect them.

| # | Question | Decision | Effect on the design |
|---|---|---|---|
| Q1 | Does the officer take part in the story? | **No.** He owns his economy, vehicles, phone, skills and wheel slot; missions, protagonist calls and texts, friends and family scenes stay with the three protagonists. | D3/D8 as written: story scripts stay officer-blind. |
| Q2 | Autosaves while the officer is played | **Deferred to the next switch to a protagonist.** No timed brokered autosave. The officer menu has "save now". | §3.9 autosave policy; the optional timed autosave is dropped. Revision 3: autosave_controller already drops the officer's requests itself, so the loader records them and raises one vanilla request after the next switch (§0.3 item 1). "Save now" goes through the vanilla quick-save entry G9506 and its checks (§0.3 item 4). |
| Q3 | GTA Online while the officer slot is enabled | **Online is refused.** This already holds: the loader runs only with `-nobattleye` (`src/loader/dllmain.cpp:53-57`, otherwise idle, so no pack is mounted), and without BattlEye the game refuses Online (pause-menu tab 42 shows "BattlEye required"; research/phase0.md §2 line 80: the Online flow is blocked by BattlEye, tested). Playing Online = starting the game without `-nobattleye`, which leaves the loader and the generated pack out. | D9 simplified: the Online-equivalence work (S8 as a release gate, D4 differential, U31 Online session) is dropped. The transition-signal teardown stays as a safety net for a refused Online attempt (U31 rewritten). |
| Q4 | Paid activities (strip clubs, prostitutes, golf, tennis, darts, range, hunting, towing, races) | **All usable, paid from the officer's own account.** Taxi, car wash, vending, fairground, cinema, cable car and telescope likewise. | Every paid free-roam script found by U9 becomes officer-aware ("light": identity + payment wrapper + account 3); relationship data (stripper/prostitute "met" flags, PROSTITUTE_SAVED_DATA) stays NONE. U9 now only maps the sites. Later, the discipline system may treat off-duty offences (e.g. soliciting) as misconduct. Revision 3: the **launchers** of these activities (launcher_*; also controller_towing, but towing stays NONE by the §0.4 Q8 answer) refuse non-protagonists and read protagonist cash, so they become aware too; launch paths are mapped by U9 (§0.3 item 2). |

---

## 0. Decisions at a glance

| # | Decision | Main reason |
|---|---|---|
| D1 | The officer's character value is **3** (`OFFICER = 3`). | It is the only value that fits every existing 4-wide structure: selector, switch struct, phone. Gates and loops can be changed with 1-byte edits. It does not collide with real contacts or with the "no character" sentinels. No G2349 entry has bank field 17 = 3 (checked), so account 3 is free. |
| D2 | **The officer's player identity is computed in each script and never stored.** In officer-aware scripts, `get_cur4` returns 3. G122220/1/2 (PP_CURRENT/PREVIOUS/LAST_KNOWN_PED, all saved) keep their vanilla meaning: 145 while the officer is played, the last protagonist in +4322/+4323. **Exception, already true today:** the phone owner G21666 is 3 in every script while the officer is played. This is a separate "owner" domain with its own 4-wide arrays (§3.5). | The vanilla `update_cur` writes the new character into +4322/+4323 and refreshes +4323 = cur. A naive patch would leak 3 into globals that 1,600+ sites in unpatched scripts read, and into the save (§0.1 item 6). |
| D3 | **Two classes of scripts plus safety patches.** *Officer-aware* scripts are rewritten and see the officer as 3: roughly 20–40 SP scripts at the end of the plan (shops, money, ATM, switching, respawn, services). *Officer-blind* scripts are all the others: story, missions, comms, friends, scenes, saving. They keep seeing 145, as today. Some blind scripts get **safety patches** only: they are not officer-aware, but they crash or leak today, for example appcontacts G24150[owner] and mission_triggerer_d's unguarded account read. | Identity suppression comes for free in blind scripts, because they already treat 145 as "not a protagonist". Work is limited to the systems the officer must own. |
| D4 | **Per-character data:** the vanilla 3-element storage is never moved or resized. The officer gets a **shadow 4th element** in a new script global block 13 (G3407872+). Index 3 reaches the shadow through a **detour on the VM's shared bounds-failure block**. The detour serves an access only when the opcode is an ARRAY opcode, block 13 is live (its current pointer is non-null), **the thread belongs to an officer-aware program** (program hash at thread+0x10), the loader's serve byte is set, and the base is whitelisted (§3.3). The fallback is the per-site CALL stubs proposed by the technique survey. | The detour only acts where vanilla would have killed the thread. Thread gating keeps blind threads byte-for-byte vanilla, including their crashes (§3.3). One mechanism covers pointer parameters, nested arrays and copies of the shared library. |
| D5 | **Logic changes:** officer-aware scripts are rewritten at loader start-up into a generated replacement pack. Every modified function F gets an appended **F′ that starts with an `officer_mode()` test and falls back to an appended vanilla copy V**. A modified function that can yield (WAIT) or never returns tests `officer_mode()` at each decision site instead, because a choice made once at entry would go stale (§3.4). The original body becomes a forwarder. In-place edits are allowed only when they cannot change behaviour on vanilla data. One documented exception: startup's registration patch is unconditional (§3.6). Every character-decision site in an officer-aware script gets an explicit, recorded decision, and the build refuses a script that has an undecided site. | This is the completeness argument (§3.5). With `officer_mode()` false (Online, block 13 not ready, officer disabled), an aware script behaves like vanilla. |
| D6 | **Engine:** while the officer is the player, protagonist stats slot k = 0 is aliased to an "SP3" character, using only data writes: model static, stat prefix, 187 stat-id objects. SP3_* stats are declared in our generated DLC pack. Whether they are SP-saved or mirrored into block 13 is decided by U6/U18 (§3.7). | HUD cash, pause header cash, street pickups, the respray charge, skills and the ability meter all follow the officer automatically, and no code patch is needed. CHANGE_PLAYER_PED's per-protagonist routine also runs for the officer under the alias (§2.3, U29). |
| D7 | **Persistence:** the officer's data is saved in its **own named save block**, owned by our registration script `ml_char4_reg`, inside the game's own save file. **Vanilla-clean invariant:** in v1 the engine never writes a save while the officer is the player or holds an officer value in a vanilla field. A save broker handles every save path (the benchmark and mission-repeat saves are refused while the officer is the player, §3.11) and writes a new nonce before **every** save, whoever the player is, carrying forward the side files of data producers that are not loaded (no rotation when none is loaded, §3.9). **Autosaves requested while the officer is the player are deferred:** autosave_controller drops them itself (autosave_controller@306–@375), the loader records them, and it raises one vanilla autosave request after the next switch to a protagonist (§3.9). | The user's 100% save (SGTA50001, slot 03) and every other save stay loadable without the mod. Officer data follows each slot, autosave and cloud copy. No screen blackout on every purchase. |
| D8 | The officer **owns** economy and personal state, **shares** world services and collectibles, and has the **story identity suppressed** (§1, §4). | This is the user's goal. |
| D9 | **GTA Online is refused while the officer slot is enabled (user decision Q3), and this already holds.** The loader, and therefore the generated pack, only runs with `-nobattleye`, and without BattlEye the game refuses Online (§0.0). Kept as defence in depth: (a) every `*4` helper and every F′ tests `officer_mode()`, which is false in a network game; (b) in-place edits stay limited to ones that cannot be told apart from vanilla on vanilla data; (c) at the **first transition signal** (a refused Online attempt still starts the transition scripts) the loader tears the officer session down: restores G2349[3], G2349[145], G143050[145] and removes the alias. | The loader's Online pause (`core.cpp:199-207`, `:236-240`, `:277-278`) stops mods and native overrides, not rewritten bytecode; BattlEye being off is what keeps the pack out of Online. |
| D10 | **SDK compatibility:** `ml::game::CurrentCharacter()` / `CharacterIndex()` keep returning −1..2. Character 3 is visible only through a new opt-in API that is version-checked through MLApi. Officer payments run an officer-aware copy of the bank function compiled into a second generated script, `ml_char4_bank`, which owns no global block (§3.6). | Existing mods index fixed `[3]` C++ arrays with the character (23 call sites). Returning 3 would make them write out of bounds inside the game process (§3.9). |
| D11 | **Failure policy for aware threads (revised in revision 4):** in development builds an unserved index kills the thread and logs the details, as in vanilla. In release builds **any** unserved index in an aware thread (3, 145 or another value) reads and writes a **per-array scratch sink** in loader memory, sized from the opcode's element-size operand and initialised from element 0 so inner counts stay valid (never zero-filled; nested sinks with a cap; above the cap the vanilla kill runs and is logged, §3.3). The failure path only records the event; on the next tick the loader logs an ERROR, turns `officer_mode()` off for the rest of the session (the officer falls back to 145 behaviour) and shows a warning. The serve byte stays set, so aware threads that already hold 3 keep reaching the catalogued shadows (§3.3 gate 4). The session state becomes "degraded": the alias, entry 3, hardening, arrest guard and broker stay on while the officer is the logical player, so he stays isolated (§3.2 session states). A torn-down session stays torn-down. The latch is cleared only at restart (§0.4 Q7 answer). | respawn_controller is restarted by main whenever the player is injured or arrested. Killing it inside its own sequence risks a kill/relaunch loop (guess). Degrade-with-warning policy. |

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

### 0.3 Corrections after review round 2

Each item was verified by the review against the local disassembly, the exe dump and the repo; its main evidence was re-checked for this revision. Items 15, 18 and 22 are the state-lens versions of items 1, 4 and 11 and are merged with them.

| # | Lens | Correction | Sections |
|---|---|---|---|
| 1 | journey | The Q2 deferral never fired. autosave_controller consumes a request made while the player is not a protagonist at @306–@375 (get_cur → `<3` → G102550+4 = 1) and the next pass decrements +10 and terminates; DO_AUTO_SAVE (@915) is never reached. The loader now records such requests every frame and raises one vanilla request (+10 = 1) after the next switch to a protagonist. Patching autosave_controller to hold is only an alternative. | D7, §2.4, §3.9, §6, U22, §5.4, M0/M2 |
| 2 | journey | The Q4 activities are started by blind launchers whose can-launch routine refuses 145 (launcher_golf@1150 → @7453–@7509 and copies) and whose cash gates read SP0..2_TOTAL_CASH. controller_towing has a Franklin-only gate (@43143–@43163). These launchers become aware ("light"); launcher_tonya and the other RC launchers stay blind; the §3.1/§4 launcher_racing contradiction is removed; launcher_stunts is a user question. | §0.0 Q4, §3.1, §4, U9, §5.4, M2, §0.4 Q6/Q8 |
| 3 | journey | The Phase A wheel and SwitchFrom had no global "may switch now" gate. They now mirror selector@24088 without its character term, including the block flags G24447+5/+6 that activity scripts set (range_modern, act_cinema), and read only IS_CONTROL_PRESSED(0,19). | §3.10, U34, §5.4, M1b |
| 4 | journey | Explicit saves as the officer had no cancel path, no preconditions and no vehicle handling. "Save now" now enters through G9506 (save_anywhere's own checks; silent refusal in free roam → mod message); the swap stops the vehicle first; completion is polled with GET_STATUS_OF_MANUAL_SAVE, and a cancel rolls the nonce back. | §3.9, U22, §5.4 |
| 5 | journey | SHOOTING_RANGE (G134146) was "NONE", but aware scripts index it with the character (gunclub_shop@95342 → @96206; range_modern caches get_cur in static 90). It becomes an E-rule shadow (saved or non-saved: user question). "Records NONE" in §4 now means "not shared with protagonists". | §1, §2.2, §3.3, §3.5, §4, U7, §0.4 Q5 |
| 6 | journey | Block 13 could be claimed by a future update or another pack, and the readiness test could not tell. Start-up ownership scan over all script headers; relocation to a free block or officer off; readiness = block present **and** our magic and layout version. | §3.2, §3.6, §5.5, M8 |
| 7 | mechanism | officer_mode() cannot be served through OverrideScriptNative (64-slot pool, patching by handler pointer, applied one tick late). The loader now writes its handler into the appended slot by index from a MinHook on InitNativeTables (0x91bf20). The save and player-model natives are hooked once at their own handlers and filtered by program hash. | §3.2, §3.7, §3.9, §3.11, §5.1 S7, U2 |
| 8 | mechanism | As written, the startup patch would hang boot: an officer_mode() F′ needs block 13, which only case 14 creates. The startup patch is now a documented unconditional exception; case 14 skips itself when ml_char4_reg does not exist; startup and ml_char4_reg are generated as one unit. | D5, §3.6, §5.1, §5.4, §5.5 |
| 9 | mechanism | Every new placement of ml_char4_reg's program re-creates block 13 from its initial values (0x91bde0), so the SDK's RunFunction path and the loader fallback could reset it silently. ml_char4_reg now stays resident; the SDK bank copy moves to ml_char4_bank (no block); the fallback start needs a null block; officer_mode() requires the ready flag of the current placement. | D10, §3.2, §3.3, §3.6, §3.9, R7, U3, U38 |
| 10 | mechanism | The detour cached absolute block addresses with no lifecycle and had no block-presence gate (a null block 13 → write near address 0). Gate 0 (live block pointer), a serve byte, live classification of bases, shadow computed from the live pointer. | §3.3, §5.2 R2/R6, U28 |
| 11 | mechanism | S (static) and L (local) shadow kinds were keyed so that concurrent threads, recursion and escaped frame pointers share or miss them. S → appended shadow statics per program (per thread by construction); L → frame growth in F′. | §3.3, §3.4, §5.1 S3/S4/S7 |
| 12 | mechanism | F′/V is chosen once per invocation, so yielding functions ignore later officer_mode() changes. Yielding or never-returning functions get site-level guards; officer_mode() is split from a session-stable pack_active(). | D5, §3.2, §3.4, §5.1 S1, §5.2 R4 |
| 13 | mechanism | Step 3 of the identity rewrite added update_cur's stores at raw +4321 loads that had none (selector@31586/@31610). New side-effect-free load_cur4(); S8 compares global stores. | §3.2, §5.1 S8 |
| 14 | mechanism | Forwarders and V copies break or silently redirect the SDK's byte-pattern lookups (RunScriptFunction, ml::game::Pay, stocks, property, epsilon). Patterns are resolved against the vanilla image and redirected through the rewriter's manifest; copies get a named ENTER. | §3.4, §3.9, M2 audit, M5 |
| 15 | state | Same finding as item 1. Adds: the hold alternative's side effects (G102550+1 kept at 1 turns off every re_* through the shared launch gate, Director Mode reports 8, phone and flow readers); the broker restores the hardening before CHANGE_PLAYER_PED and defers DO_AUTO_SAVE during its own swap. | §3.9, U22 |
| 16 | state | Saves made as a protagonist kept the old nonce, so two slots could share one set of side files. The broker now runs a pre-save step (flush, new nonce, write-ahead side files, SDK event) for every save native, whoever the player is. | D7, §3.9, §5.2 R3 |
| 17 | state | Side files were committed after the save, non-atomically, and pruned without a slot map. Now write-ahead immutable generations, a slot-file → nonce index over SGTA5xxxx and MISREP files, a prune rule, and a temporary-file-and-rename write for ml::save. | §2.4, §3.9, U37 |
| 18 | state | Same finding as item 4. Adds: the mod's 存檔 action and bedside save use the same G9506 entry; mod-level gates (pursuit, custody, driving or piloting, pending switch, wanted). | §3.9, §5.4 |
| 19 | state | Officer-off latches had no reset point and "torn down" was not enforced. Loader-owned session states (inactive, active, degraded, torn-down); latches cleared only in the load hook (U36); torn-down refuses switching to the officer. | D11, §3.2, §3.10, §3.11, U36, §5.4 |
| 20 | state | A police-mod fault left the officer session armed with nothing to drive it. Fault teardown by the loader; the char4-critical overrides move into the loader; ml_char4_reg stays registered even with the mod absent or off. | §3.6, §3.11, §5.5, §6 |
| 21 | state | The hardening condition was dropped "from M1" with no fallback if the mission_triggerer_d safety patch is not the copy being served. The condition now follows the served copy (build record), with a degradation row. | §3.1, §3.2, §4, §5.5, §6 |
| 22 | state | Same finding as item 11 for statics. Adds: SP3_* stats are reset at the load hook before saved or mirrored values are applied, for every load. | §3.3, §3.7, U6 |

### 0.4 Questions from review round 2 (answered by the user, 2026-10-22)

| # | Question | Options | Recommendation |
|---|---|---|---|
| | **Answers:** Q5 = (b) saved; Q6 = yes; Q7 = (a) only at restart; Q8 = neither: the vanilla towing job is Franklin's work and stays NONE for the officer. For a police officer, "towing" means calling a tow truck to remove a vehicle (impound, abandoned or crashed cars), a police-mod feature that has nothing to do with the vanilla job. controller_towing and towing therefore stay blind, and Q4 does not cover them. | | |
| Q5 | The officer's minigame records (SHOOTING_RANGE G134146 medals; golf, tennis and darts records once M1 lists them). Needed before M0 U7, because gunclub_shop reads SHOOTING_RANGE[c] during the tracer bullet. | (a) **non-saved** E shadow: records live for the session only; range-medal items in Ammu-Nation stay locked for him (guess). (b) **saved** E shadow: he keeps his own medals in block 13 (+271 slots for SHOOTING_RANGE). Either way, completion % and achievements stay protagonist-only. | (b): it matches "his own skills and purchases" and costs little block space. |
| Q6 | Do Q4's "races" include the stunt plane races (launcher_stunts names "stunt_plane_races" at @14340 and has an is_playable copy at @6784 **(checked)**; whether that copy gates the launch is unverified)? | yes: launcher_stunts becomes aware too; no: it stays blind | yes, if U9 finds a character gate or a fee on its path (Q4 covers "all paid free-roam activities"). |
| Q7 | After D11 degrades the officer for a session: when is the latch cleared? | (a) only at restart, with the warning repeated at each load; (b) at the next load | (a): the same unserved site would fire again after a load, and a restart re-runs the build checks. |
| Q8 | If U9 shows that the tow job is offered through a protagonist call or text (would conflict with Q1; guess): should the mod offer towing to the officer itself? | mod-side offer (phone notification or a world marker) that starts the same towing script; or no towing for the officer | a mod-side offer, so Q4 holds without touching the story comms. |

### 0.5 Corrections after review round 3

Each item was verified by the review against the local disassembly, the exe dump and the repo; its main evidence was spot-checked again for this revision. Items 14 and 15 are the state-lens versions of items 9 and 2 and are merged with them.

| # | Lens | Correction | Sections |
|---|---|---|---|
| 1 | journey | A protagonist handed over to player_controller (Phase A) is deleted unless he is within 200 m with collision loaded, visible within 40 m, or one of three flags holds (keep test player_controller@254246; DELETE_PED @253637/@253666), and he is re-created only near his last-known position. An officer save could then find no ped (SaveForOfficer refuses, `officer.cpp:380-384`). New step 2b "materialise the protagonist" (switch request preferred, CREATE_PED from PP_INFO as the alternative); the fault teardown uses the switch request, which needs no ped. | §3.9, §3.10, §3.11, §4, §5.4, §6, U15, U22, M1b |
| 2 | journey | Saves made with the police mod off, or in a session without the loader (no `-nobattleye`, Q3), orphaned the officer's side files or sent the slot to the legacy import. Producer-aware rotation (carry forward; no rotation when no producer is loaded); ml_char4_reg and the SP3 stats file always mounted together; no officer flush when the officer was not active for the loaded save; the write-ahead side file mirrors the saved block; the slot index records {nonce, mtime, size}; a block-less load of a recorded slot offers a restore, and the legacy import runs only for never-recorded slots. | D7, §3.6, §3.7, §3.9, §3.11, §5.3 D3, §5.4, §6, U36, M2 |
| 3 | journey | Aware scripts register the player as a conversation speaker with a protagonist voice (pb_prostitute@15489 SWITCH 0/1/2 → "MICHAEL"/"FRANKLIN"/"TREVOR", no case 3; taxilauncher@35703), and no policy existed. One recorded policy: never a protagonist voice; DEFAULT (no player speaker) if U39 shows that such lines are skipped without blocking, otherwise skip only the player lines. Consistency: towing and controller_towing are taken out of the aware lists, following the §0.4 Q8 answer. | §0.0, §1, §3.1, §3.4, §4, §5.1 S2, §5.4, §6, U9, U39, M0, M2 |
| 4 | journey | The wheel rule "refuses while any officer-aware activity or shop thread is running" never passes after M2 (shop_controller, object brains and world-point brains run all the time), and the save gates had no activity term. Replaced by the defined "may switch now" predicate (the selector@24088 mirror, plus per-script engaged states only where U34/U9 find a gap), which now also gates "save now", 存檔 and the bedside save; it refuses during a taxi ride (G114037, set only by taxiservice). | §3.9, §3.10, §5.4, U34, M0 |
| 5 | journey | A taxi cannot be called from the officer's phone (taxilauncher@35804 accepts 0..2 only through contact 131's owner column), and taxilauncher was not in the aware list. Calling a contact whose phonebook value is 2, or contact 130, mirrors into the saved G143050 (appcontacts@45289 → @46149). taxilauncher becomes aware with case 3; the service contacts (at least 131) move to M2 with value 1 only, never contact 130; the G143050 owner-3 image joins the vanilla-clean invariant. | §3.1, §3.9, §4, §5.2 R5, §5.4, §6, M2, M5 |
| 6 | mechanism | **Critical.** The engine copies a started script's arguments into its last +0x20 statics (Reset 0x921930). Appended S-kind shadow statics would receive the arguments and leave the parameter statics at their initial values, which breaks every aware program that takes arguments, for every character (593 of 1,151 scripts take arguments; atm_trigger 1, gunclub_shop 4). Shadows now go before the argument block, operands ≥ s−a are renumbered (static map in the manifest; the SDK's Static() translates through it); v1 allows the S kind only for programs without arguments unless that layout passes S7/S8. | §2.3, §2.5, §3.3, §3.4, §5.1 S7/S8, §5.5, §6, U2, M1 |
| 7 | mechanism | D11's "zeroed per-array scratch element" had no size (an element can be up to 65,535 slots, and only the operand at serve time gives it), sat in block B and broke nested arrays (inner counts 0). Sinks now live in loader memory, sized from the operand, initialised from element 0, nested with a cap, with the vanilla kill above the cap; the failure path only records, and OnTick logs and warns. | D11, §3.3, §3.5, §5.4, §5.5, §6, M1 |
| 8 | mechanism | Clearing the serve byte at teardown and at D11 stranded aware threads that already hold 3 as a plain index (range_modern static 90; respawn_controller local 179 in the yielding @29480). The byte is now set when officer_mode() first becomes true and cleared only on the block-free paths, an invalid block, the Online transition signal and the load hook; torn-down is final until the load hook. | D11, §3.2, §3.3, §3.11, §5.4, §6 |
| 9 | mechanism | The InitNativeTables hook wrote natives[slot] into any program with a matching name hash, including another mod's copy (out of bounds when slot ≥ its native count). It now checks code size, native count and the placeholder handler first; otherwise it writes nothing, runs the program blind and warns. The char4 pack's overlay precedence is stated. | §3.2, §3.3, §3.4, §5.1 S7, §5.2 R3, §5.5, §6, U41, M1, M8 |
| 10 | mechanism | A present but unloadable ml_char4_reg still hangs startup's registration loop (no frame counter) or raises a fatal error on every boot, and the cache reproduces the defect. Boot sentinel with a fallback pack; frame budget for case 14 (ENTER @143017 locals 6 → 7); header rules for +0x18 (ml_char4_reg 0; ml_char4_bank 0 or 0xA2BE51AC). | §2.3, §3.6, §5.1 S7, §5.4, §5.5, §6, U40, M0, M1 |
| 11 | state | mission_repeat_controller and benchmark poll GET_STATUS_OF_MISSION_REPEAT_SAVE in a WAIT loop right after the queue native, and mission_repeat_controller has already captured the player (position, snapshot, safehouse override) before it. Repeat saves as the officer are refused through a status hook (2 → the vanilla abort). DO_AUTO_SAVE during the broker's swap is passed through after the restore instead of swallowed (a swallowed call is lost with a SAVE_OFF message). | D7, §1, §2.4, §3, §3.9, §3.11, §4, §5.4, §5.5, §6, U25, U36, M2 |
| 12 | state | The arrest guard was a police-mod override retired at M2, so the degraded, faulted and torn-down-while-player states brought back the verified arrest softlock and the Michael-account leaks. It moves into the loader's char4 service and stays active whenever respawn_controller cannot serve the officer as 3; the teardown restores the hardening only once the officer is no longer the player; the fault teardown requests a switch only when it may switch; D11's sink covers any unserved index. | D11, §3.2, §3.10, §3.11, §4, §5.4, §5.5, §6, M0 |
| 13 | state | Saves and switches were not mutually exclusive: during the broker's swap a protagonist is the physical player and nothing blocks the wheels, and a switch into the officer could start while an autosave is in flight. New loader-owned "save busy" state: every switch path refuses, control 19 is disabled, and a switch into the officer waits for IS_AUTO_SAVE_IN_PROGRESS to clear. | §3.7, §3.9, §3.10, §4, §5.4, §6, U22, M0 |
| 14 | state | Same finding as item 9. Adds: R3 logs the check result; the build lists every other overlay of each aware script and either makes the char4 pack win or drops that file from the other pack, recorded in the build record. | as item 9 |
| 15 | state | Same finding as item 2. Adds: each nonce records its parent (fallback and pruning follow the chain); a slot re-saved outside the loader keeps its old nonce as a restore candidate; U36 logs which slot file the load handler reads. | as item 2 |

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
| cash and HUD cash, bank account and log (including the ATM and the bank web page), stock portfolio; Ammu-Nation purchases and unlocks, weapons, ammo and tints; health, armour, wanted level, last-known position; outfit indices (VARIATIONS); per-character SP stats and skills (as SP3_*); fees (hospital, arrest); LS Customs; personal vehicles (mod garage plus the game's vehicle records); phone handset, settings and contact list (existing slot 3); wheel slot 3; his entries in our own mods (bank, property, stocks, social: ml::save slot 3, opt-in) | paid world services (taxi, car wash, vending, cable car, fairground, cinema, telescope; also strip club dances and drinks, prostitutes, golf, tennis, darts, shooting range fees, hunting, races: all usable by user decision Q4 (the vanilla towing job stays NONE, §0.4 Q8), with U9 mapping each launch path), charged to *his* account; character-neutral collectibles; world flags; respawn locations | story flow and missions, mission triggers, Strangers & Freaks, random events (as content), friends and booty calls, per-character relationship data of strippers and prostitutes, family and timetable scenes, protagonist safehouses, beds and garages, SP businesses, protagonist calls, texts and e-mails, Lifeinvader and car-app pages, completion % and achievements, minigame records as shared or completion data (aware Q4 scripts read the officer's own records from E shadows, §2.2 row 134146, §0.4 Q5), Director Mode, mission replay and snapshots, peyote animal trips (v1), Rockstar Editor sessions (the officer session ends cleanly) and benchmark runs (refused as the officer), §3.11, pause-menu Skills page (3 hard-coded columns) |

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
| 134146 | SHOOTING_RANGE (=114990+19156; count 3 in the dump **(checked)**) | 3×271 | yes | 126+; 81 sites in range_modern use the get_cur cached in static 90 (@2865 `CALL @601698; STATIC_U8_STORE 90`); gunclub_shop@95342 (get_cur) → @96206 reads SHOOTING_RANGE[c].3[x].3 (a range medal value; that it gates an unlock is a guess) **(checked)** | **E-rule shadow** (non-saved or saved, §0.4 Q5): aware scripts index it with the character, so "NONE" cannot apply |
| 116985 | PILOT_SCHOOL | 3×121 | yes | — | NONE, unless M1 finds an aware script that indexes it with a character (then an E shadow like SHOOTING_RANGE) |
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
| **script global block registration (new)** | 0x91bde0, called from both program-load paths (streaming placement 0x91b9f0 @0x91bba3 when [prog+0x28] ≠ 0; 0x91c380 @0x91d51f–0x91d526): stores the size in 0x3ED17A8[block] (@0x91be0c), allocates a new buffer (@0x91be16), copies the 0x20000-byte initial pages (@0x91be50–0x91be62), stores the pointer in 0x3ED15A8[block]; never reads or frees the old pointer **(checked: allocation, size store and copy loop)**. Blocks are freed and zeroed only when the counter 0x3ED1BD0 reaches 0 (0x91c290 @0x91c2a3, 0x920f10 @0x920fdb, 0x922a40 @0x922b14) | 64 | every placement of an owner re-creates its block (§3.6) |
| **native table fill (new)** | InitNativeTables 0x91bf20 (`game/pointers.cpp:26`), called by both load paths (0x91b9f0 @0x91bbab, 0x91c380 @0x91d52e); unresolved hashes get the default handler 0x50CB60 (`CC C3`) | — | serving point for officer_mode() / pack_active() (§3.2) |
| **script VM: thread start (Reset; revision 4)** | Reset 0x921930 (caller 0x13cf920; rdi = program, rsi = thread): @0x921aa6 argument count = prog+0x20; @0x921aa9–0x921ab8 a start with arguments must pass exactly +0x20·8 bytes, otherwise "Arg size mismatch" (0x921baf); @0x921ac5–0x921acf the static count +0x24 must be below the stack size thread+0x60; @0x921ae2–0x921ae7 thread+0xC8 = statics − arguments; @0x921d07 `test ebp,ebp; jz` skips the argument copy for a start without arguments; @0x921d0b–0x921d23 memcpy of the arguments to statics + [thread+0xC8]·8 **(checked)**; initial statics copied at 0x921c97–0x921d05. So the parameter statics are the **last** +0x20 statics: gunclub_shop@173 `STATIC_U16_LOAD 1407` (1,411 statics, 4 arguments), atm_trigger@230 `STATIC_U16_LOAD 649` (650/1), ob_vend1@149 `STATIC_U8_LOAD 105` (106/1) **(checked)**. 593 of 1,151 scripts take arguments; shop_controller, respawn_controller, selector and player_controller(_b) take none (review, headers read with tools/ysc.py) | — | shadow statics appended after the last static would receive the arguments (§3.3 S row) |
| **placement globals-signature guard (revision 4)** | 0x91b9f0 @0x91bb64–0x91bb91: skip when prog+0x18 == 0; otherwise compare it with the table 0x3ED1AD0 indexed by the globals block of +0x28 (`movzx ecx, word [r12+0x2A]; and ecx, ~3`); skip when the entry is 0 or equal; otherwise `int3` unless the byte 0x3ED1AC0 is set. Dump: table[0] = 0xA2BE51AC (the standard globals signature), [1] = 0x5AC12D65, [5] = 0xB71DC512, [11] = 0xA6C8B83F, [12]..[15] = 0; 0x3ED1AC0 = 0 **(checked)** | 64 | a program with +0x28 = 0 is compared with table[0]; header rules in §3.6 / S7 |

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
  - **GET_STATUS_OF_MISSION_REPEAT_SAVE (revision 4):** mission_repeat_controller@571902–@571931 and benchmark@11983–@12007 WAIT while it is 1, right after their queue native; 2 → abort: mission_repeat_controller `CALL @509(1)` (unpause, fade in, clear the G80611 bits, SET_PLAYER_IS_REPEATING_A_MISSION(0), terminate; review), benchmark TERMINATE_THIS_THREAD @12007 **(checked: both loops)**. Its handler 0x1d615f0 serves only this native (natmap **(checked)**). It and the queue handlers 0x1d615b0 / 0x1d615d0 are obfuscated jump stubs, so the idle value and whether the engine refuses a non-protagonist repeat save are unknown (U25). Before the queue native, mission_repeat_controller has already captured the player into the saved repeat block: @571867 → @669275 → @203639 (coordinates, heading, parachute state, wanted level into G114990+25093), @193247 (G105547 snapshot), @571893 → @665533 (OVERRIDE_SAVE_HOUSE; clears G102550+8) (review). On success it goes on to @572111 → @664591, whose loop @665230–@665281 calls SET_PLAYER_MODEL(player_zero) until the model is a protagonist;
  - QUEUE_MISSION_REPEAT_LOAD: mission_repeat_controller@145/@496, benchmark@501391;
  - SHUTDOWN_AND_LOAD_MOST_RECENT_SAVE: maintransition ×4;
  - GET_STATUS_OF_MANUAL_SAVE: savegame_bed@3347–3375 calls SET_SAVE_MENU_ACTIVE(0) and advances when the status is != 1, which ends on success and on cancel alike. save_anywhere does not poll: its state 1 (@269) cleans up and clears G9506.
- **Quick save entry:** cellphone_flashhand@4471–4472 sets G9506 = 1; main@793257–793288 starts save_anywhere (hash −845649458) while it is set. save_anywhere's can-save routine @468268–@468537 has no character gate. A refusal shows AM_H_SAVEAONM (@597) only when G44938 ∉ {15, 5} (@239–@255), so it is silent in free roam (G44938 = 15 in the dump) **(checked)**.
- **Autosave requests:** the `G102550+10 += 1` request pattern appears in **218 scripts** **(checked)**: shops, carmod, services, every re_*, collectibles, Strangers & Freaks, minigames. Example: gunclub_shop@968054 → @332787 → @332844–@332856. Request sites refuse when +10 > 1, or when +8 is set and +10 > 0 (main@506142–506179, gunclub_shop@332804–332841). Engine requests: main@794317 HAS_CODE_REQUESTED_AUTOSAVE → @506125 (same cap, +10 += 1). main@794333–794364 starts autosave_controller (hash 922362507, stack 1424) when +10 > 0 **(checked)**.
- **autosave_controller drops requests made while the player is not a protagonist** **(checked)**: static 38 state 1 (@446529) sets +1 = 1 and goes to state 2; state 2 (@140) → @306 get_cur (@446480 = update_cur @22081 + load +4321) → @314 `<3` false → G44938 == 13 or 14 → @368; == 17 → @365 LEAVE (vanilla wait); otherwise @368–@375 `G102550+4 = 1; LEAVE`. The next pass (@145–@233) clears +1..+5, decrements +10, and at 0 clears G102550 and terminates (state 3 → @446606). DO_AUTO_SAVE (@915) comes after many later gates and is never reached for 145 (§3.9).
- **Files:** SGTA50000 / 50001 (the user's 100% save, slot 03) / 50015 (autosave, slot 02), MISREP0000, `.bak` copies. The body is encrypted, so files cannot be edited offline.
- **Mod data today:** ml::save keeps one JSON per mod for all slots (`mods.cpp:758-880`). Slots are 0..2 only (`mods.cpp:795,821`). A commit happens on an SGTA5 mtime change, and a revert on any loading screen. The write truncates the file in place (`mods.cpp:854`, `std::ofstream(..., std::ios::trunc)`, no temporary file) **(checked)**, and invalid JSON reads as "starting empty" (`mods.cpp:776-781`). story.cpp polls the newest SGTA5* mtime over all profiles and slots once a second (`game/story.cpp:29-46`, `:75-80`): it cannot tell which slot was written, and MISREP files are not watched **(checked)**. The police mod keeps:
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
- **New in revision 3:**
  - the launch paths of the Q4 activities (launcher_*, controller_towing, launcher_carwash) were not mapped (U9);
  - per-character arrays indexed by Q4 scripts and their launchers were not listed (SHOOTING_RANGE was missed); the M1 analyzer lists them, including cached statics such as range_modern static 90;
  - the 11 confirmed character-indexed static arrays have not been matched against multi-instance programs (object brains, shop_controller instances; S3/S4).
- **New in revision 4:**
  - script arguments were not modelled: the S-kind statics growth ignored the engine's argument copy into the last statics (§2.3 Reset row);
  - player-speaker registration in aware scripts (conversation helpers given a protagonist voice) was not listed (§3.4, U39);
  - the deletion rules for left-behind protagonists (player_controller@254246) were not part of the Phase A design (§3.10);
  - the overlay order between the char4 pack and other replacement packs that replace the same script is not known (U41).

---

## 3. Architecture

```
            ┌─────────── loader start-up ───────────┐
 game .ysc ─► rule engine (fingerprints, catalog, block-ownership scan) ─► generated replacement pack (officer-aware scripts + safety-patched blind scripts + startup patch + ml_char4_reg.ysc + ml_char4_bank.ysc) + manifest (slot indices, address map, identity data, static map)
            └───────────────────────────────────────┘
 program load ─► InitNativeTables hook ─► identity check (code size, native count, placeholder handler) ─► officer_mode()/pack_active() handlers written into the appended slots (by index, from the manifest); mismatch ─► no write, program blind, warning
 runtime:
   officer-aware script ──officer_mode()? ─no──► vanilla copy V (Online, block 13 not ready, officer off)   [yielding functions: site guards instead]
                          └─yes─► get_cur4()=3 ──► ARRAY op idx 3 ──► VM bounds fail 0x91ee3d ──► DETOUR: ARRAY opcode ∧ block live ∧ aware thread ∧ serve byte ∧ whitelisted? ─yes─► shadow element in block 13 (or shadow static)
                                                                                                     └─no──► vanilla thread kill + LOG (dev) / sized scratch sink; ERROR + officer_mode off on the next tick (release)
   officer-blind script ──get_cur()=145──► vanilla behaviour (+ 145 hardening, + safety patches)
   phone scripts (blind) ──G21666=3──► existing 4-wide owner arrays (+ G24150 safety patch)
   engine ──(alias slot k while officer is player)──► SP3_* stats (HUD, pickups, respray, skills)
   save path ──► SAVE BROKER ("save busy"; pre-save step for every save: flush, new nonce or carry-forward, write-ahead side files with a block mirror; officer: materialise and swap to the protagonist, sanitize) ──► game save incl. ml_char4_reg block
   repeat save as the officer (Replay Mission, benchmark) ──► refused: status hook returns 2 ──► vanilla abort
   autosave request as the officer ──► dropped by autosave_controller@368 ──► loader records "pending" ──► one vanilla request after the next switch
   system paths (Editor, Director Mode, Online transition, police-mod fault) ──► TEARDOWN (§3.11) ──► session state torn-down until the next load (after a mod fault: until restart)
```

### 3.1 Script classes

- **Officer-aware**:
  - target end state: shop_controller (with a per-shop-type GUARD), gunclub_shop, carmod_shop (M4), respawn_controller, blip_controller (shop blips, same shop-type filter), **atm_trigger**, the public-service scripts (carwash1/2, ob_vend1/2, taxiservice, cablecar, bigwheel, rollercoaster, act_cinema, ob_telescope, plus every paid script U9 lists, all of them aware by user decision Q4: stripclub, stripclub_drinking, pb_prostitute, golf, tennis, darts, range_modern, hunting_ambient, offroad_races…; not towing, §0.4 Q8), **taxilauncher** together with taxiservice (the phone booking path, §4; revision 4), selector, player_controller_b, player_controller (switch-related parts), and later stock_controller / appinternet if our browser does not replace them;
  - **the entry scripts of the Q4 activities (new, "light")**: launcher_golf, launcher_range, launcher_tennis, launcher_darts, launcher_hunting_ambient, launcher_offroadracing and launcher_racing; launcher_stunts only if Q4's "races" covers the stunt plane races (§0.4 Q6); launcher_carwash if U9 finds a character gate on the car-wash path. controller_towing and towing stay blind (§0.4 Q8 answer). Reason: each launcher's can-launch routine refuses a non-protagonist and its cash gate reads protagonist cash (§4 "Minigame fees" row) **(checked: launcher_golf@1150 → @7453–@7509, cash getter @6349; controller_towing@43143–@43163)**. Treatment: the identity rewrite (the can-launch routine's is_playable and get_cur are retargeted), CASE3 `SP3_TOTAL_CASH` in the cash getters through the hash-triple rule;
  - **activity rule (new):** an activity the user made usable (Q4) is aware along its whole launch path: world point → launcher → can-launch routine → cash gate → activity script. U9 lists each path and every character gate on it;
  - `ml_char4_reg` (ours) owns block 13 and the officer save block; `ml_char4_bank` (ours, header +0x28 = 0, no global block) carries the officer-aware copy of the bank function for the SDK (D10, §3.6);
  - rule: a script is aware only if every character-decision site in it has a recorded decision and the residual check passes (§3.5). Otherwise it is automatically demoted to blind, with a warning.
- **Officer-blind**: everything else, including main, flow_controller, mission_triggerer_a–d, comms_controller, friends_*, family_scene_*, player_timetable_scene, startup_positioning, autosave_controller (stays blind: the autosave deferral is loader-only, §3.9), savegame_bed, save_anywhere, stats_controller, completionpercentage_controller, animal_controller, director_mode, benchmark, mission_repeat_controller, all re_* and RC scripts, controller_towing and towing (§0.4 Q8), every launcher_* not listed as aware above (explicitly **launcher_tonya**, which has the same can-launch gate but is a Strangers & Freaks launcher, and the other Strangers & Freaks launchers), and clothes_shop_sp / hairdo_shop_sp / tattoo_shop (never started for the officer).
- **Blind with safety patches** (behaviour change only where vanilla crashes or leaks):
  - appcontacts: guard the G24150[G21666] store when the owner is ≥ 3;
  - mission_triggerer_d: guard the two unguarded account reads (skip the cash test when the account is ≥ 3). Needed only if the G2349[145].17 hardening stays on while that script runs. The hardening relies on it only when the copy actually served is ours (§3.2).
  - If a safety patch cannot be applied (another mod's copy, fingerprint mismatch), the runtime workaround stays and a warning is shown (§5.5).
- **Owner-domain scripts:** cellphone_controller, cellphone_flashhand, the app* scripts and dialogue_handler stay blind for player identity. They already serve owner 3 through 4-wide arrays (§3.5).

### 3.2 Identity layer

**Rewrite applied to every officer-aware script.** The helpers are identical copies, so they are found by fingerprint:

1. Append these functions:
   - `officer_mode()`: a call to an appended native-table entry whose handler the loader supplies. **How it is served (revised):**
     - A MinHook on InitNativeTables (0x91bf20, already found by `game/pointers.cpp:26`). Both program-load paths call it: streaming placement 0x91b9f0 @0x91bbab, and 0x91c380 @0x91d52e. After the original returns, the hook looks the program hash up in the rewriter's manifest (program hash → slot indices, code size, native count) and **checks that the placed program is our generated copy before it writes anything (revision 4)**: code size (+0x1C) and native count (+0x2C) equal the manifest's values, each slot is < +0x2C (checked before reading), and `natives[slot]` after the original fill equals the unique handler of the chosen placeholder native (the same test for pack_active's slot). Only then does it write both handlers by index and add the program hash to the detour's aware set for this placement (§3.3 gate 3). On any mismatch it writes nothing, removes the hash from the aware set (the program runs blind) and warns once (`another copy of <script> is active; officer features off for it`). The test runs on every placement, so a later placement of our copy adds the hash back. Reason: InitNativeTables fills prog+0x2C/+0x40 in place (`game/natives.cpp:22-31` FakeProgram static_asserts, `pointers.hpp:24-25`), placement calls it with r12 = the program (0x91b9f0 @0x91bba8–@0x91bbab) **(checked)**, and the name hash cannot tell copies apart (hardening below). Without the check another mod's copy of an aware script would get one of its real natives replaced by officer_mode() (slot < its native count) or a heap write past its native table (slot ≥ its native count). The loader's own fake-program calls (`game/natives.cpp:110`, `:144`) are skipped.
     - Not through OverrideScriptNative. Its pool has 64 slots (`game/scripts.cpp:248`), and the design's own uses add up to about 117 before any mod takes one (review count: one slot per aware program, 77 CHANGE_PLAYER_PED/SET_PLAYER_MODEL script/native pairs, the broker and Director Mode). Its Patch() swaps every slot whose pointer equals the original handler (`scripts.cpp:277-287`), and handlers are shared (natmap: 80 natives on 0x1f90, 46 on 0x1c05eb0, 20 on 0x1be3f00). It is applied from OnTick, which runs after the scripts and only once main_persistent exists (`core.cpp:248-262`, `:278`), so a freshly placed program first runs with the original handler **(checked)**. If OverrideScriptNative is kept for anything char4-related, kOverrideSlots must be raised and the slots budgeted.
     - The appended entry is chosen so that an unpatched slot is harmless: a registered native whose handler no other native shares (checked against `D:\Projects\gtav-research\char4\exedim\natmap.txt`, not only by hash), 0 arguments, returning 0/false in story mode, and not used elsewhere in the program. S7 checks all four; R3 checks `natives[slot]` after each load.
     
     It returns true only when all of these hold:
     - block 13 (block B, §3.6) is registered, its header holds our magic and layout version, and ml_char4_reg's ready flag for the current placement is set. The loader checks these in C++: reading a registered block from C++ is safe; the "never read block 13" rule is for script threads before registration;
     - the officer feature is enabled for this save (read only after the ready flag is set);
     - no network game is in progress and no transition signal has been seen (§3.11);
     - not Director Mode (G44938 ≠ 14);
     - the session state is "active" (not degraded by D11, not torn down, no teardown in progress; session states below).
     
     Block 13 is touched only after this returns true, because reading an unregistered block kills the thread (§2.3).
   - `pack_active()` (new): a second appended entry, served the same way. It is session-stable: block B present and valid, ready flag set, no network game. It selects F′ over V at the entry of modified functions that can yield or never return; their decision sites test `officer_mode()` (§3.4). Reason: a choice made once at entry would keep a WAIT loop in V after the officer is enabled mid-session, or in F′ after a teardown.
   - `is_char4(c)`: `c < 3 || (c == 3 && officer_mode())`.
   - `char_model4(c)`: G2349[c].0 if is_char4(c), else 0.
   - `ped_to_char4(ped)`: the vanilla result when it is not 145; otherwise 3 if `officer_mode()` and `ped == G[OFFICER_PED]` (a handle slot in block 13); otherwise 145. Identifying the officer by handle stops other freemode peds (Director Mode, other mods, selector's freemode path at selector@520435/@520752) from being taken for the officer.
   - `get_cur4()`: `r = get_cur()`; if `r == 145`, `officer_mode()` and `PLAYER_PED_ID() == G[OFFICER_PED]`, return 3; otherwise return r.
   - `load_cur4()` (new): side-effect free. A raw +4321 load that returns 3 only when it reads 145, `officer_mode()` is true and `PLAYER_PED_ID() == G[OFFICER_PED]`; otherwise the value read. It adds no global stores (step 3).
2. Retarget calls in place (3-byte CALL target):
   - every `CALL get_cur` → `get_cur4`;
   - every `CALL is_playable` / `char_model` / `ped_to_char` → the `*4` versions, **except the calls inside update_cur and its private helpers**, which stay vanilla so G122220/1/2 never receive 3.
   - These retargets cannot be told apart from vanilla when `officer_mode()` is false, so they are allowed in place (D5).
3. Direct `GLOBAL_U24 114990; IOFFSET_S16 2370; IOFFSET_S16 539; IOFFSET_S16_LOAD 4321` loads outside the helpers (13 bytes) become a 4-byte CALL plus NOPs. The analyzer classifies each raw load:
   - **preceded by an update_cur call** (get_cur clones and helpers: player_controller_b @27327, @633981, @634185; respawn_controller @22219; selector @33400, @511919): `CALL get_cur4`. The extra update_cur only repeats work;
   - **raw cache read** (no update_cur just before): `CALL load_cur4`. Example: selector @31586/@31610 read +4321 after GET_ENTITY_MODEL (@31570 → @478832) and a non-protagonist test (@478780) with no update_cur call **(checked)**. `get_cur4` there would add update_cur's store of +4321 = 145 (path @46406 → @46509 → @46559) and stop a stale protagonist in +4321 from being preferred over +4323. Rare, but not provably vanilla (D5).
   - S8 compares global stores, not only return values, for every in-place edit.
4. The phone-owner function needs no change. It calls get_cur, which is retargeted, and keeps owner 3 either way.

**OFFICER_PED binding.** The loader writes OFFICER_PED and re-applies the look whenever the officer's ped is replaced:
- after the mod's own CHANGE_PLAYER_PED / SET_PLAYER_MODEL (gender change);
- after respawn;
- in Phase B, after the game's own switch code creates the officer ped (§3.10).

A per-frame check compares PLAYER_PED_ID with OFFICER_PED while the officer is the logical player. Whether PLAYER_PED_ID changes after SET_PLAYER_MODEL is unverified (guess: it does; U26). Paths that put a protagonist model on the player end the officer session instead (Editor), or are refused while the officer is the player (benchmark, Replay Mission; §3.11).

**Runtime ownership of entry 3** (loader service, re-applied after every session start and every load, because standard_global_reg rebuilds G2349):

- G2349[3]: +0 = the officer's current model (follows gender), +3..6 name label and +7..10 picture label (through the loader's text override), +11 = 7 (not a friend), **+17 = 3 (officer bank account)**. The per-owner arrays stay as they are.
- G118[3] stays unchanged: 118[3].8 = 198 keeps him off contact lists.
- **Never use the standard_global_reg setters** (@556364 etc.): they mirror into the saved G143050 when c < 162. Write G2349 directly.
- The broker restores vanilla values of G2349[3] around each save and at the first transition signal. G143050[3] is checked against its session-start image.

**Hardening while the officer is played (blind scripts):**

- **G2349[145].17 := 4**, so that blind payment paths (`account = 2349[c].17`, guarded wrapper) skip instead of charging Michael.
  - It is applied only while **no mission_triggerer_d thread exists**. The loader checks with EnumScripts every frame and restores 0 at once when the thread appears.
  - Reason: mission_triggerer_d reads `G62254[2349[c].17]` without the guard (§0.2 item 1). The window is small but not zero: the thread could reach @502314 on its first frames if the player already sits in a Gauntlet (guess; U35).
  - From M1, the mission_triggerer_d safety patch removes the condition, **but only when the loader's build record shows that the mounted mission_triggerer_d is our patched output and no later overlay replaced it** (alternative: a marker native appended by the patch, verified at each placement by the same identity test as the officer_mode slot above). The program hash at thread+0x10 is the name hash and cannot tell the copies apart. Otherwise the EnumScripts condition stays, with a warning (§5.5).
  - The condition was chosen over the alternative "145.17 := 3 with G62254[3] served to all threads" because the detour does not exist before M0 ends.
- **G143050[145].17 := 4** while the officer is the player. The ATM and the phone's bank page then refuse to show an account for the officer, instead of showing Michael's.
  - G143050 is saved, so the broker restores 0 before every save and the value is in the vanilla-clean invariant (§3.9). R5 watches it.
  - If it ever leaked into a save, the value would mean "no account" for an entry that vanilla never plays (benign; guess).
  - From M2, atm_trigger is aware and reads G2349[c].17 at its single site, with CASE3 for the account-name SWITCH (an officer label through text override). appinternet's site follows in M5 or is replaced by our browser.
- **Arrest guard (loader service, revision 4; today the mod's override `examples/police/guard.cpp:67-72`, checked).** A MinHook on GET_TIME_SINCE_LAST_ARREST's own handler 0x1d20950 (one native per handler in natmap **(checked)**), filtered to respawn_controller's program hash (also read at @49927 per the guard.cpp comment; review), returns −1 while the guard is active. The loader also does the post-arrest fade-in and control release that the mod does today (`guard.cpp:110-114`).
  - Active while the physical player is the officer's ped **and** respawn_controller cannot serve him as 3: before M2, degraded, torn-down, or after a police-mod fault. After M2 it stays for those states, at least in development builds.
  - Reason: the arrest softlock is verified in game (phase0.md §53.16: respawn_controller dies after about 3 s, black screen with the loading icon for more than 75 s). A mod-side override disappears with a mod fault, and revision 3 retired it at M2 although degraded and torn-down sessions still leave the officer as the player.
- Until the broker exists, the police mod's current save routine (officer.cpp) must restore both hardening values before it saves.
- Character-valued globals audit (list in §2.2):
  - the analyzer reports every write of a character-typed value (possibly 3) into a global from an officer-aware script;
  - each target must be read only by officer-aware scripts, be proven safe, or be sanitized by the broker;
  - saved ones (153339, 153340, 139903, 135409[].11) must never hold 3 at save time.

**Session states (new; loader-owned):** inactive → active → {degraded, torn-down}.
- **active:** the officer feature is enabled for this save and nothing has failed.
- **degraded (D11):** `officer_mode()` is false. The officer can still be played. Catalogued shadows stay served to aware threads (serve byte kept, §3.3 gate 4, revision 4).
- **torn-down (§3.11):** `officer_mode()` is false, and the loader reports `OfficerAvailable() == false` to the mod: the wheel's officer slot is greyed, and SwitchTo/Restore are refused until the next load (after a mod fault: until restart). Today GameWheel (`examples/police/wheel.cpp:96-101`, closing on the officer slot → `officer::SwitchTo`) and SwitchTo (`officer.cpp:286-303`) have no such check **(checked)**.
- **Transitions (revision 4):** torn-down is final until the load hook. A D11 that fires after a teardown (for example in a thread that still holds a cached 3) only logs; it never moves torn-down to degraded and never re-enables `OfficerAvailable()`.
- The alias, entry 3, the hardening, the arrest guard and the broker follow **"the officer is the logical player"**, not `officer_mode()`. A degraded officer therefore stays isolated: payments skip through 145.17 = 4, and the alias keeps street pickups and the respray charge (§2.3) off SP0.
- **Latches** (transition signal seen, teardown, torn-down, and D11 if §0.4 Q7 says so) are cleared in one place only: the load hook (§3.7), once U36 shows that it fires for a normal load, a MISREP load (benchmark, Replay Mission) and SHUTDOWN_AND_LOAD_MOST_RECENT_SAVE.

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
  - The served path uses only rax/rcx/rdx after saving them, and does its lookups in assembly against two small open-addressing tables: aware program hashes, and whitelisted global indices (rax is first classified against the live block tables, gate 5 below).
  - The log and miss path saves all volatile registers and aligns rsp to 16 before calling C++.
- **Gating, in order:**
  1. opcode ∈ ARRAY set;
  2. **(new, mandatory) the live block pointer `0x3ED15A8[B]` is non-null** (B = 13 by default, §3.6), loaded on every serve. If it is null, the original failure path runs (dev) or D11 applies without serving. Without this gate a missing block 13 would make the handler write near address 0 and crash the whole game instead of losing one thread;
  3. the thread's program hash (thread+0x10, thread from r12−8) is in the aware set. **(revision 4)** The set holds a hash only while the program placed under it passed the InitNativeTables identity check (§3.2); another mod's copy runs blind;
  4. **the loader-owned serve byte is set (revised in revision 4).** It is set when `officer_mode()` first becomes true in a session, and cleared only on the block-free paths, on an invalid block (magic, generation or owner check), at the Online transition signal and in the load hook. D11 and teardown turn `officer_mode()` off (no new identity decisions) but keep serving catalogued shadows to aware threads.
     - Reason: aware threads cache the character as a plain index. range_modern@2865 `CALL @601698; STATIC_U8_STORE 90` (get_cur, get_cur4 once aware) feeds 87 `STATIC_U8_LOAD 90` sites, for example @420079 `STATIC_U8_LOAD 90; GLOBAL_U24 114990; IOFFSET_S16 19156; ARRAY_U16 271` (SHOOTING_RANGE[c]); respawn_controller@30387 `CALL @3294; LOCAL_U8_STORE 179` sits inside the yielding @29480 **(checked)**. A cached 3 is not a decision site (§3.4), so with the byte cleared every later access to a correctly whitelisted array would be unserved: a thread kill in development (its cleanup skipped), lost purchases and medals in release.
     - In sessions where the officer was never active the byte is never set, so vanilla kill behaviour is kept there (D5).
     - Variant, if gate 4 must stay strict: at a D11 or teardown, snapshot the aware threads alive at that moment (thread id plus program hash) and keep serving only those until they end;
  5. the base `rax` is in the whitelist. **Vanilla bases are not cached as absolute addresses (revised):** 0x91bde0 gives a block a new buffer at every placement of its owner (allocation @0x91be16, initial pages copied @0x91be50–0x91be62, pointer stored @0x91bedb) and never frees the old one, and all 64 block pointers are freed and zeroed when the counter 0x3ED1BD0 reaches 0 (0x91c290 @0x91c2a3, 0x920f10 @0x920fdb, 0x922a40 @0x922b14). So `rax` is classified at serve time against the live pointers 0x3ED15A8 and sizes 0x3ED17A8 for the few blocks that hold catalogued arrays, and the whitelist is keyed by global index. Alternative: rebuild an address map from hooks on 0x91bde0 and on the three free paths;
  6. the rule matches the index.
- **Served:** compute the shadow from the live block-B pointer plus the layout offset (a few instructions on a path that otherwise kills a thread), set `rax = shadow − 8·(1 + idx·size)` and jump to the handler's own continuation after its `jnb`, chosen by opcode: U8 0x91ee1b / 0x91ee8b / 0x91eebb, U16 0x91f14f / 0x91f18f / 0x91f1cf **(checked)**. Static shadows (kind S) are served relative to r8 instead (below).
- **Not served:** record (script name from thread+0x154, ip, global or static index, idx, count) and either run the original failure code (dev) or apply the D11 release policy (aware threads only; **any** unserved index, 3, 145 or another value, revision 4).
- **D11 scratch sinks (revised in revision 4):**
  - Allocated in loader memory, not in block B, with guard pages around the sink area. A sink is keyed by (program hash, base) and sized at serve time from the opcode's element-size operand: ARRAY_U8 reads a byte (0x91ee1f `movzx edx, byte [r13+1]`), ARRAY_U16 a word (0x91f153 `movzx edx, word [r13+1]`), then `imul; inc; lea rax,[rax+rcx*8]`, so an element can be up to 65,535 slots (review). Above a fixed cap (the largest catalogued element) or when the sink area is exhausted, the vanilla kill runs and is logged; the thunk never computes a pointer whose element passes the end of its sink.
  - Initialised so that inner count slots are valid, never zero-filled: from the initial image of element 0 when the base classifies to a global block (the owner's prog+0x38 pages) or to a static (prog+0x30); otherwise from element 0's live contents (counts valid; reads may show protagonist data; writes stay in the sink). Reason: a zeroed element has count 0 in every inner array, so the next inner ARRAY op fails for any index, which defeats D11 for nested per-character structs (WEAPONS, STORED_VEH_DATA, PP_INFO inner arrays).
  - A failing ARRAY op in an aware thread whose base lies inside a sink (for example an inner per-character [3] indexed with 3) is served from a nested sink, with a depth and total-size limit.
  - The failure-block path only records the event and flips the flags (`officer_mode()` off, degraded). ERROR logging, the latch, the session-state change and the warning notice run from the loader's OnTick.
- Because the handler's own arithmetic then lands on the shadow element, LOAD_N/STORE_N of one element, address escapes into natives and pointer-parameter callees all work unchanged.

**Zero-regression property:** the detour only runs where vanilla would have set the thread state to 2 (thread killed). For every vanilla execution that does not crash, behaviour is byte-for-byte identical. With thread gating, blind threads keep even their vanilla crashes, so blind scripts never silently read officer shadows: for example the phone scripts through G21666 = 3, or the "3 = MP" paths. Protagonist play cannot be affected by the data layer.

**Whitelist rule kinds:**

| Kind | Rule | Examples |
|---|---|---|
| E (element) | `idx == count` (3) → shadow element of `size` slots. Each inner base of a nested array is listed separately. The shadow can sit in the saved area or in the non-saved area of block B (session-only data). | VARIATIONS, WEAPONS, 62254, 135796, STORED_VEH_DATA (4 inner bases), VEH_POS (2), 34212[263]+0/+12 (526 inner bases); SHOOTING_RANGE (saved or non-saved, §0.4 Q5) |
| R (range) | `count ≤ idx < count+extra` → `shadow[idx−count]` | VIEWED_MODS [1224] (extra 408), WARDROBE arithmetic index (to confirm) |
| S (static) | **revised:** shadow statics in the rewritten program, not loader memory. The rewriter raises the header static count (+0x24) by K and inserts initial values (+0x30) copied from element 0's static initial values, so nested count slots are correct. **Argument block last (revision 4):** the engine copies a started script's arguments into its last +0x20 statics (Reset 0x921930, §2.3), so shadows appended after the last static would receive the arguments and leave the parameter statics at their initial values, for every character (atm_trigger would lose its ATM in static 649, gunclub_shop its shop index in 1407). Layout: [0, s−a) unchanged, [s−a, s−a+K) shadows, [s−a+K, s+K) arguments (s = static count, a = +0x20). +0x20 never changes (the engine checks argument bytes = +0x20·8 at 0x921aad). Every STATIC_U8/U16/U24 operand ≥ s−a is renumbered by +K in all code of the program, unmodified bodies included (a relocation, §3.4); where a STATIC_U8 operand would pass 255 the instruction widens, so that function becomes an F′/V copy. The static map goes into the manifest, and the SDK's `ml::script::Static(id, index)` (MLApi::ScriptStatic, `sdk/include/modloader/modloader.hpp:312`) translates vanilla indices through it. v1 allows the S kind only for programs with +0x20 = 0 (shop_controller, respawn_controller, selector, player_controller(_b)); a program with arguments (atm_trigger, ob_vend1/2, gunclub_shop, carmod_shop, the Q4 launchers…) gets this layout once S7/S8 check it, otherwise the per-site stub fallback (end of §3.3), otherwise it is demoted. No "copy at address 0" prologue: a start without arguments skips the copy (0x921d07), so such a prologue would copy shadow initial values into the parameter statics. Statics are r8-relative (STATIC_U8 0x91ef40 `lea rax,[r8+rax*8]`; `mov r8, rcx` at 0x91ddac), so the detour serves `rax = r8 + 8·shadowStart − 8·(1 + idx·size)`. The shadow is per thread, initialised by the engine at thread start and discarded with the thread, with no thread tracking; concurrent instances (object brains such as atm_trigger, registered several times by standard_global_init@42747–42887; shop_controller's second instance, START_NEW_SCRIPT_WITH_ARGS @87730/@87771) each get their own. Where vanilla initialises the element in code at thread start (shop_controller@5–98), the rewriter adds the same stores for the shadow in main's F′. S7 checks the layout and that the statics plus the worst-case frames still fit each start stack size. Not saved. | 561 candidates, 11 confirmed; programs with arguments: 593 of 1,151 scripts (review count) |
| L (local) | **revised: no detour kind.** In F′ the frame grows (ENTER u16), the locals after the array are renumbered, and the count initialiser becomes 4. This covers arrays passed by pointer to callees, recursion and concurrent threads, which a frame-relative key would share or miss. | count unknown; implemented only if M1 finds any |
| N (sink, optional) | explicit (blind script, array) exception: `idx == 145` → per-array scratch element, logged as a warning | only where a blind script provably crashes on 145 today and cannot be guarded otherwise; none planned |

**Policy:**
- Whitelist only catalogued per-character arrays. Missing entries fail loudly in development (thread kill plus log), and the log tells exactly what to add. Release builds follow D11.
- Over-whitelisting a non-character array cannot corrupt vanilla data, but it hides bugs, so it is avoided.

**Shadow storage:**
- Global block 13, G3407872 + offsets (block B: 13 unless the start-up ownership scan picks another free block, §3.6; "block 13" in this document means block B). 13 = 3,407,872 ≥ 2^18·13, within the 24-bit GLOBAL_U24 operand. Blocks 13–15 and 21–63 are unowned (technique survey header scan of 1,151 scripts). Block 11 = sp_dlc_registration, block 12 = sp_pilotschool_reg, block 17 = MP side (G4525199, G4525781) **(checked)**.
- Layout, generated:
  - header: magic, layout version, per-placement generation number, per-save nonce, OFFICER_ACTIVE, OFFICER_PED, ready flag. ml_char4_reg writes the magic, generation and ready flag after STOP_SAVE_DATA. The loader reads them in C++ for `officer_mode()` (§3.2); script threads never read block 13 before `officer_mode()` is true;
  - saved shadows (≈3.3–3.6k slots);
  - non-saved shadows;
  - officer copies of the *three-way named* groups: 4th stock group of 33 slots, 4th e-mail counter, OWVEC colour column [312];
  - stat mirror;
  - no D11 scratch sinks: they live in loader memory (revision 4, "Not served" above);
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
- Cache key: CRC of the original script + rule-set version + catalog version + the chosen block number B (§3.6).
- **Manifest (new):** for each generated program the rewriter records the program hash, the slot indices of the appended `officer_mode()`/`pack_active()` entries (served by the InitNativeTables hook, §3.2), an address map original address → forwarder / F′ / V (used by pattern lookups, below), **and (revision 4) the generated program's code size and native count with the chosen placeholder handler (the identity check, §3.2), and the static map of the argument-last layout (§3.3 S row)**.
- If another mod replaces an officer-aware script, the rules are applied to that copy. If fingerprints do not match, the script becomes blind with a warning.
- **Overlay precedence (revision 4).** Every generated replacement pack carries `<order value="999" />` and an overlay ml_stream.rpf (`convert/replace.cpp:559-581` **(checked)**), the loader resolves conflicts between replacement mods only as "first mod by name wins" (`game/dlcpacks.cpp:363-380` **(checked)**), and packs under platform:/dlcPacks/ are processed too (`dlcpacks.cpp:40`). Which pack wins a tie is a guess (U41). So:
  - the char4 pack gets an order above every other generated pack;
  - at build time the rewriter lists every other overlay of each aware script (other replacement packs; dlcpacks once U19 has scanned them) and, after consuming that copy as input, either makes the char4 pack win or drops the file from the other pack's overlay; the choice is recorded in the build record (same mechanism as mission_triggerer_d, §3.2);
  - the log names the copy each aware program came from, and the runtime identity check (§3.2) catches anything that slips through.

**How** (interpreter constraints from the technique survey, consistent with the handler code above):
- **In-place, same-size edits.** Allowed only when they cannot change behaviour on vanilla data, because they stay active in Online:
  - CALL target retargeting to `*4` helpers, which return vanilla results when `officer_mode()` is false;
  - wrapper fix: in the payment wrapper (gunclub_shop@854360, carwash1@40955) `PUSH 3`→`PUSH 4`, so account 3 is no longer "none" and only 4 is. This cannot be told apart from vanilla while no entry holds account 3 (§2.1). The loader restores G2349[3].17 = 4 at the first transition signal.
  - **static renumbering (revision 4):** STATIC_* operands ≥ s−a shifted by +K for the argument-last layout (§3.3 S row). It is a relocation, equivalent by construction under the static map; S8 compares the thread-start image with the map applied.
- **Function copies.** Everything else, including constant edits to loop bounds and gates, new SWITCH cases, extra loop iterations and guards:
  - **F′** (modified) and **V** (vanilla copy, relocated) are appended in new code pages.
  - F′ begins with `if !officer_mode(): reload params; CALL V; LEAVE` (functions that do not yield; yielding ones below).
  - The original body is overwritten in place with a **forwarder** (`ENTER p; LOCAL_LOAD 0..p−1; CALL F′; LEAVE p r`). Function-pointer callers (CALLINDIRECT through PUSH_U24 constants, 121,908 sites) then reach F′ too, without telling code pointers apart from constants.
  - Direct CALLs are also retargeted to F′ to save a hop.
  - A forwarder needs 12 + 2p bytes. A function shorter than that (for example is_playable, 12 bytes) gets no forwarder. Its direct calls are retargeted, and the build verifies that no `PUSH_U24 <its address>` exists. Otherwise the script is demoted.
- **Yielding and never-returning functions (new).** S1 uses the call graph to mark every modified function that can yield (WAIT or any yielding native, directly or through callees) or never returns. Example: respawn_controller@29480 (params 3, 233 locals) runs to LEAVE @32100 with WAIT at @29594, @29785, @30033, @31680 and @31971, a get_cur clone call at @30387 and a character SWITCH at @30402 **(checked)**. For these:
  - F′ is entered on `pack_active()` (session-stable), not on `officer_mode()`;
  - every decision site inside F′ is guarded so the test is re-evaluated at every execution: `c == 3 && officer_mode()` for CASE3 / EXTEND-LOOP / UNROLLED / GUARD, and `officer_mode() ? new : old` for CONST. Alternative: re-test at loop heads after a yield, provided F′ and V keep identical frame layouts;
  - with `officer_mode()` false every guarded site takes the vanilla branch, so S8's equivalence still holds;
  - D11 and teardown change only the site-level result. R4 counts V-path executions that started before a flag change.
  
  Whole-function F′/V stays for functions that do not yield.
- **Assembler and writer rules:**
  - append only, never move existing code;
  - no instruction straddles a 0x4000 page, and every page ends with NOP or a control transfer (0 straddles today; 85% of page ends are NOP);
  - 16-bit branch relaxation with islands inside F′ over 32 KiB (46 SP functions are larger than 32 KiB);
  - resolve CALL→J→ENTER chains (250 sites);
  - code stays under 16 MiB (largest SP script 6.57 MB);
  - **native table re-encoding:** entries are stored rotated by `(code size + index) & 63` (`tools/ysc_dis.py:27`), so appending pages changes every entry's encoding. New natives (officer_mode) are appended to the table. `convert/ysc.cpp:142-165` only rewrites global operands in place and never changes the size, so this is new code;
  - code page table and resource layout grow (resource writer in M1);
  - **statics growth (revised in revision 4):** header static count (+0x24) raised by K; shadow initial values (+0x30) inserted before the argument block, which stays last; +0x20 unchanged; operands ≥ s−a renumbered (§3.3 S row); frame growth (ENTER u16, renumbered locals) for L-kind arrays;
  - **named ENTER in copies (new):** every appended F′ and V gets an ENTER with a non-empty name, so patterns that start at a vanilla ENTER with name length 0 (`2d 05 0d 00 00 …`, game.hpp:171) never match a copy;
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

**SDK pattern lookups (new).** RunScriptFunction starts the target script and takes the first FindPattern match over the loaded code (`mods.cpp:990-991`) **(checked)**. ml::game::Pay's pattern starts at the bank function's ENTER (`sdk/include/modloader/game.hpp:171`); examples/stocks/main.cpp:132, examples/property/shop.cpp:682 and examples/epsilon/main.cpp:56/97 use appinternet patterns too **(checked)**. In a rewritten program the original body is a forwarder that no longer matches, so the first match is the appended V (vanilla code: right for 0..2, wrong for the officer) or nothing if V's bytes changed (for example page-end NOP padding). Pay then falls back to AddCash with no bank history (game.hpp:174-175), and the stocks, property and epsilon mods stop working. Fix:
- RunScriptFunction resolves the pattern against the **vanilla code image** (the loader already reads the original script from the archive, `dlcpacks.cpp:482-561`), maps the hit through the manifest (original address → forwarder) and jumps to the original address, so the forwarder reaches F′ and the officer logic stays in the path;
- the named ENTER in copies (writer rules above) keeps copies out of vanilla-start patterns;
- this only matters once appinternet or stock_controller becomes aware (M5 decision), but the audit of examples/* checks that each pattern still resolves on the rewritten pack.

**"3 = MP" sites:** in an officer-aware script, a branch that sends character 3 to MP stats or MP behaviour is rewritten to the officer behaviour. In blind scripts they never see 3 as the *player* character. The phone owner is a separate case (§3.5).

**Player-speaker sites (revision 4).** Aware Q4 scripts register the player as a conversation speaker with a protagonist voice.
- pb_prostitute@15489 `SWITCH 0/1/2` passes PLAYER_PED_ID to the conversation helper @84490 (+0 ped, +1 voice label, +7 active) as "MICHAEL" / "FRANKLIN" / "TREVOR" (@15522 / @15542 / @15562), with no case 3 **(checked: @15489, @15522)**.
- taxilauncher@35703 sets statics 283/284/285 only for c = 0/1/2, and its dispatch @32247–@32321 registers speaker 0 with static 283 and passes 284/285 as line roots (review).
- Voice-name string counts (review): tennis 9, darts 7, stripclub 6, offroad_races 6, stripclub_drinking 3, respawn_controller 3, pb_prostitute 3, hunting_ambient 2, gunclub_shop 2 (towing 33, but towing stays blind, §0.4 Q8).
- Under §2.1 such compare chains default to "leave as is", which would make them DEFAULT by accident.

One recorded policy, applied by S2 to every site:
- never CASE3 to a protagonist voice;
- default: DEFAULT (no player speaker), or the script's variant without player lines, **provided U39 shows** that lines with an unregistered speaker or an empty root label are skipped without blocking the thread;
- otherwise skip only the player lines (GUARD around the player-line calls);
- if U39 shows that a conversation with an unregistered player speaker does not start, the affected activities would stall, and the GUARD variant becomes mandatory.

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
     - a whitelisted shadow (aware thread, block B live, serve byte set; global shadow in block B or shadow static);
     - an in-bounds 4+-element array;
     - a logged thread kill (blind thread, dev build, or block B missing);
     - a D11 scratch sink sized from the operand and initialised from element 0, plus officer_mode off from the next tick (aware thread, release build; above the sink cap a logged kill, §3.3).
   - No other outcome exists, because of the bounds check in every handler.
   - For aware scripts, the analyzer's character-type inference proves that every ARRAY op whose index may be character-typed has a base that resolves to a whitelisted array or to an array with count ≥ 4.
   - Bases that cannot be resolved statically (pointer parameters) are listed for runtime coverage.
   - Block 13 is read only behind `officer_mode()` (§3.2).
3. **Logic completeness:**
   - For each aware script, the analyzer enumerates every character-decision site: compares with 0/1/2/145, `<3 <=2 >2 >=3 >3`, SWITCH on a character, IS_BIT_SET/SET_BIT/CLEAR_BIT with a character, 1..7 masks, loops over 0..2, count-slot loops, literal character arguments, **character values passed to natives**, **constant-index access to every catalogued per-character array (including IOFFSET chains into G114990)**, whole-array copies, three-way groups, reads of +4322/+4323, **and SET_PLAYER_MODEL / CHANGE_PLAYER_PED calls**.
   - **New:** for every Q4 activity script and its launcher, the analyzer lists every per-character array it indexes with a character value, including values cached in statics (range_modern static 90, 81 sites), with the treatment chosen for each. "NONE" is not allowed for an array that an aware script indexes with the character: it needs an E shadow (saved or non-saved). The same check covers PILOT_SCHOOL G116985 and the golf and tennis records.
   - **New:** every modified function is marked yielding / never-returning or not (§3.4), and every raw +4321 load is classified (§3.2 step 3).
   - Each must have a decision in the rule file. The build refuses a script with an undecided site and demotes it to blind with a warning.
4. **Runtime confirmation:**
   - the OOB logger shows zero unserved index-3 hits over the test plan;
   - coverage counters show every CASE3 / EXTEND-LOOP / UNROLLED / shadow rule exercised;
   - differential protagonist runs show no data difference (§5).

### 3.6 Global placement and relocation

- **No vanilla global is relocated or resized.** The save layout, existing saves and all IOFFSET chains stay exactly as they are.
- Officer data lives only in block 13, whose indices are ours and stay stable across game builds (unless the ownership check below moves it to another free block).
- **Block ownership check (new).** At loader start-up, cached by archive and pack CRCs, the loader reads the globals-block field of the script header (+0x28 >> 18) of every script in the game archives, the dlcpacks and every mounted mod pack (including converted .ysc overlays from other mods).
  - If any script other than ml_char4_reg claims block 13, the loader picks the first free block in 13–15 / 21–63 and generates ml_char4_reg and the rule offsets for it (B is in the cache key; ML4_ save names are name-keyed, so saves stay compatible). If no block is free, `officer_mode()` stays false and a warning is shown.
  - Today 13–15 and 21–63 are unowned (technique survey header scan of 1,151 scripts). A future owner is a guess, but the result would be silent data corruption, which the degrade-with-warning policy must prevent.
  - Readiness at runtime is "block B exists **and** its header holds our magic and layout version" (§3.2), written from ml_char4_reg's initial-value page. `Global()` in `game/scripts.cpp:118-123` only tests the block pointer for non-null **(checked)**, so the magic check is new loader code.
- Vanilla indices in the catalog and the rules move between game builds. They are re-derived through:
  - save names (standard_global_reg is emulated in C++ at start-up, the same idea as the saving survey's `reg.py`);
  - learn_ysc-style function pairing (as for `ysc_tables.inc`, 5,223 pairs);
  - the globals signature (header +0x18, 0xA2BE51AC).
- **Block 13 registration:** a generated script `ml_char4_reg.ysc` (joaat 0x4F0DCB34):
  - header +0x28 = (13<<18)|N, plus initial-value pages;
  - main resets block 13 to defaults, then START_SAVE_DATA(&G3407872, Nsaved, 1), REGISTER_* for the saved part, STOP_SAVE_DATA, then writes the magic, the layout version, a new generation number and the ready flag;
  - **main then stays alive for the whole session (new):** a WAIT(0) loop, never marked no-longer-needed by the loader, so the program stays resident and is not placed again until the next load. Reason: every placement re-creates the block from its initial values. Both load paths re-register it (0x91b9f0 @0x91bb98 `cmp [prog+0x28],0` → @0x91bba3 call 0x91bde0; 0x91c380 @0x91d51f–0x91d526), 0x91bde0 allocates a new buffer and copies the initial pages without reading or freeing the old pointer, and 0x91b9f0's only guard (@0x91bb64–0x91bb91) compares globals signatures, which match when the same program is placed again (verified in review). Whether a running thread pins its program, and when an unneeded program is evicted, is U38;
  - **the officer-aware bank-function copy used by the SDK (D10) moves to a second generated script, `ml_char4_bank`**, whose header has +0x28 = 0. It reaches block B through GLOBAL_U24. RunScriptFunction requests, starts and immediately marks its target no longer needed (`mods.cpp:973-984`) **(checked)**; with the copy in ml_char4_reg, a RunFunction after an eviction would re-create block 13 from initial values with no reset and no STOP_SAVE_DATA, for the rest of the session.
- **Startup patch (documented exception to D5's F′/officer_mode rule, revised).** startup's registration list gets case 14 in @143246 and the loop bound at @143036 changes `PUSH_U8 14`→`15` **(checked: list of 14 cases at @143253 ending with sp_dlc_registration (12) and sp_pilotschool_reg (13); loop @143017–143145; yields after cases 8 and 12 @143151; dependencies for 12/13 @143186)**.
  - The edit is unconditional, and S8 does not check it. startup runs only at boot and on loads, before any officer state exists.
  - Reason: an F′ that starts with `officer_mode()` would hang boot. `officer_mode()` needs block B, which only case 14 creates, and no override is served before main_persistent exists (`core.cpp:248-262`). The F′ would always run V, whose SWITCH default returns −1 (@143454); the loop then keeps requesting it: local 3 is cleared (@143128), WAIT(0) (@143141), back to @143022 (@143145) **(checked)**. The game would stay on the loading screen.
  - **Case 14 protects itself:** when `DOES_SCRIPT_WITH_NAME_HASH_EXIST(joaat("ml_char4_reg"))` (registered; natmap handler 0x222f4f0) is false, the patched loop sets bit 14 in local 4 (the done bitset, SET_BIT @143102–143106) and continues. It never feeds −1 into REQUEST/HAS_LOADED.
  - **Frame budget for case 14 (revision 4).** The loop has no counter: a script that exists but never loads keeps startup in it (@143060 REQUEST, @143066 HAS_SCRIPT_WITH_NAME_HASH_LOADED; not loaded → @143128 local 3 = 0 → next case → WAIT(0) @143141 → @143022 **(checked)**). The patch raises ENTER @143017 from locals 6 to 7 (**checked:** locals = 6; locals 2–5 are all used: index, done flag, bitset, hash), a same-size operand edit inside the unconditional patch, and counts frames in local 6. After N frames (generous, for example 600) without HAS_SCRIPT_WITH_NAME_HASH_LOADED for case 14, it sets bit 14 and continues. The loader then sees no ready flag, keeps `officer_mode()` false and warns; its "start ml_char4_reg only when block B is null" fallback can still try later. Alternative: a startup static instead of local 6.
  - **Boot sentinel (revision 4).** A budget does not catch a fatal resource error (wrong encryption → ERR_GEN_ZLIB_1, research/phase0.md line 448), the cache key reproduces a deterministic writer bug on every boot, and the loader has no safe mode; the only escape would be starting without `-nobattleye`, which turns off every mod. So:
    - before mounting the char4 pack the loader writes "char4 boot pending" with the pack's cache key, and clears it when ml_char4_reg's ready flag (or main_persistent) is seen;
    - if the entry is still there at the next start, the loader builds the pack without the startup patch, ml_char4_reg and the aware rewrites (officer off) and shows a warning that names the cache key;
    - only a clean boot after the cache was regenerated clears the fallback.
  - **Header rules (revision 4; S7).** ml_char4_reg: +0x18 (globals signature) = 0, as sp_pilotschool_reg and 62 test scripts have (review count); placement's guard (§2.3) is then skipped in either placement order, and a foreign owner is caught by the magic check. ml_char4_bank (+0x28 = 0): +0x18 = 0 or 0xA2BE51AC, because a block-0 program is compared with table[0] = 0xA2BE51AC and any other value would hit the `int3` at every RunFunction. Rewritten scripts keep their original +0x18. The guard cannot fire on the first placement of ml_char4_reg today (table[13] = 0 in the dump **(checked)**); it fires only if another program registered block B first with a different non-zero signature.
  - startup and ml_char4_reg are generated as **one unit**: if either fails S1 or S7, neither is written to the pack (vanilla startup, officer off, warning).
  - startup also marks each registration script no longer needed (@143098); ml_char4_reg's resident main thread keeps its program loaded (guess; U38).
  - Boot order is favourable: startup@238 runs the registration loop before standard_global_init (@317) and standard_global_reg (@384), and so before main (checked).
  - That the block is created when the program loads, and is re-created correctly after loads and SP↔MP transitions, is unverified (U28). This is why readiness goes through `officer_mode()`.
  - Alternative (if the startup patch is rejected): no startup patch; the loader starts ml_char4_reg when the startup thread appears. `officer_mode()` stays false until the ready flag is set. Saved data is applied at STOP_SAVE_DATA (§2.4), so the timing is not critical.
  - **The loader starts ml_char4_reg itself only when `0x3ED15A8[B]` is null**, never just because no ml_char4_reg thread exists.
  - Development hook on 0x91bde0: log any registration of block B while `0x3ED15A8[B]` is non-null (old and new pointer) (R7).
- **ml_char4_reg is mounted and started whenever the loader runs (new)**, even with the police mod absent or disabled, so officer blocks round-trip through saves made while the mod is off (§6, unclaimed-block row). In that case `officer_mode()` stays false and no aware rewrite is applied. **Revision 4:** ml_char4_reg and the SP3 stats file (§3.7) form one always-mounted unit whenever the loader runs, so SP3_* stats keep their registration (and, if U6 makes them SP-saved, their values) through a mod-off session. The pre-save step flushes the stat mirror and the left-behind officer state only when the officer feature was active for the loaded save, so a mod-off session never overwrites the mirror (otherwise a flush from unregistered or reset SP3 stats would zero it; guess). A session without the loader (started without `-nobattleye`, Q3) is plain vanilla; §3.9's slot index detects the saves it makes (side-file rule 9).
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
- a loader wrapper around CHANGE_PLAYER_PED / SET_PLAYER_MODEL: **a MinHook on each native's own handler (revised)**, CHANGE_PLAYER_PED 0x1d1e830 and SET_PLAYER_MODEL 0x1d1e820 (one native per handler in natmap) **(checked)**, filtered by the calling thread's program hash (TLS +0x7A0, `game/script.cpp:16`, → thread +0x10). Not OverrideScriptNative: 60 + 17 script/native pairs in 72 scripts would not fit its 64 slots (§3.2). The hook also sees mod and loader calls made through natives::Call; the mod's own calls are handled the same way. SP SET_PLAYER_MODEL callers **(checked)**: main@569271 (Editor cleanup), benchmark@13612, mission_repeat_controller@665272, animal_controller@3275/@485624, selector@520435/@520752, maintransition@507065/@1516358; main_install@277 is install-time;
- a MinHook on the SP load handler 0xd1ef80 (or player PostLoad 0x222ba40) that forces removal before the save is applied. **It is also the single place where the session latches are cleared (§3.2) and where SP3_* stats are reset (below).** U36 checks that it fires for a normal load, a MISREP load and SHUTDOWN_AND_LOAD_MOST_RECENT_SAVE;
- the teardown signals of §3.11.
- **(revision 4)** while a save is in flight ("save busy", §3.9) the CHANGE_PLAYER_PED hook never applies the alias, entry 3 or G143050[145].17 = 4.
- MinHook is already used for engine functions (`script.cpp:48`, `fixes.cpp:79`).

**Why k = 0 (Michael):**
- no ×2.5 inertia tweak (F/T only, 0xe0a6e0);
- no Trevor helmet rules (0x165a480).
- Known leftovers under the alias: Michael's lens flare, pilot-helmet prop 27 (the mod re-applies its own helmet), the BUDDY_SEES_MICHAEL_DEATH line, Michael's AI ped not recognised as a protagonist by the engine while the alias is active, the pause Skills page highlighting column 0 while showing Michael's real SP0 numbers, the 0xB5F970 routine, and freemode NPCs treated as Michael by the engine.
- Alternative (M0 decides): alias MP slot 4 instead and write 0x481DC38 := 4 after the switch. No protagonist leftovers and no ability, but possible MP-specific behaviour keyed on index ≥ 3 (unknown).

**SP3 stats:**
- An extra stats definition file of data-file type 0x23 in our generated DLC pack, mounted together with ml_char4_reg whenever the loader runs, police mod on or off (§3.6, revision 4). The mounter vtable 0x27186a8 → 0x1eab1d0 → 0x1ea7d40 registers SP stats at DLC mount.
- It declares explicit `SP3_<name>` (characterStat = false) for every characterStat name in the extracted SP stats list.
- **Save category is decided by U6/U18.** Saved SP3 stats would be written into *every* SGTA5 save, protagonist saves included. That raises three unknowns: the capacity of the stats section (690 triples plus 187 StatId5 names), a vanilla load of unknown stat hashes, and leftovers after the mod is removed.
  - Preferred if any of these is bad: declare SP3_* without SP-save persistence (if the format allows it; guess) and have the broker mirror SP3_* values into block 13 before saves and restore them after load. The stats section then stays vanilla.
- **Reset at every load (new):** the load hook sets every SP3_* stat to its default, then applies the saved values or the block-13 mirror. It runs unconditionally, including for saves without SP3 values or without an officer block and for legacy imports. Otherwise an officer session's SP3 values could carry into a save that has none (guess; U6).

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
- **(revision 4)** the owner-3 elements of G143050 (+16, +23, +28 of all 162 entries) equal their session-start image (not 0: entry 96 has +28 = 1), so officer contacts never reach the save (§4 "Phone contacts");
- SPn stats, G62254[0..2] and PP_INFO[0..2] are untouched by officer activity (guaranteed by the shadow design and asserted);
- if SP3_* stats are SP-saved (U6), they are the only stats-section addition, and their effect on vanilla loads has passed D3.

**Officer save block:** registered by `ml_char4_reg` (§3.6), stored under DLCScriptSaveData inside the game save.
- Contents: the header (nonce, layout version, active flag, position, heading, game clock), all saved shadows (the officer counterparts of the vanilla saved arrays) and the stat mirror.
- Member names are `ML4_<vanilla save path>`. Loading is name-keyed, so the layout can grow without breaking older officer data.
- Bulky mod data (reports, records) goes in side files keyed by the nonce: `data\save\<nonce>.json`.
- **Side-file rules (new):**
  1. **Write-ahead, immutable generations:** the broker's pre-save step writes `<nonce>.json` completely (temporary file, flush, atomic rename / ReplaceFile) **before** the save native is released, and never changes that file afterwards.
  2. **Slot index:** slot file → {current nonce, previous nonce, mtime, size} (mtime and size added in revision 4, rule 9), updated by a per-file mtime watcher over every SGTA5xxxx and MISREP0000 file. story.cpp today tracks only the newest SGTA5* mtime and no MISREP (§2.4), so it is extended.
  3. **Pruning:** only nonces that no index entry references and that are older than N days.
  4. **Load:** a missing nonce file → warning, and the slot's previous generation is used.
  5. ml::save gets the same temporary-file-and-rename write (`mods.cpp:854` truncates in place today).
  6. **Producer-aware rotation (revision 4).** Side files hold the data of producers (the police mod, any ml::save v2 mod). A disabled mod is never loaded (`mods.cpp:1449-1452`) and Dispatch skips a Faulted one (`mods.cpp:749-756`) (review), so nothing would write its section for a new nonce. For each producer that is not loaded or not running at a save, the pre-save step therefore carries its section into the new nonce verbatim (copy or hard link) from the generation the slot was loaded from, or the last one written for it. When no producer is loaded it does not rotate at all: officer data cannot have changed, and a shared immutable generation is harmless because pruning counts references across slots. Without this rule two saves with the mod off left {current, previous} with no files and orphaned the last good generation.
  7. **Parent chain (revision 4):** each nonce records its parent. Load falls back along the chain, and pruning never removes an ancestor of a referenced nonce.
  8. **Block mirror (revision 4):** the write-ahead side file also holds a copy of the saved part of block B (header plus saved shadows, about 3.6k slots) and the SP3 mirror, so a save whose block was dropped can be restored. Before revision 4, §6's "restore from the side files" could not work: they held only reports and records, and the nonce itself lived in the dropped header.
  9. **Saves made outside the loader (revision 4):** the slot index stores {nonce, mtime, size} per slot file at every brokered save. A mismatch at loader start or at load marks the slot "saved outside the loader" and keeps its old nonce as a restore candidate instead of treating the slot as legacy. Typical cause: a session started without `-nobattleye` (Q3) leaves the loader idle (`dllmain.cpp:53-57` **(checked)**), and story mode then loads and saves, autosaves to slot 02 included, as vanilla. Whether such a vanilla re-save drops the block is U4.
  - Whether the game or the cloud ever loads SGTA5xxxx.bak automatically is unverified (U37); the slot index keeps the previous generation for that case.
- Arrays and structs inside a DLC save block, and a block this size, are unproven (U4/U18). If they fail, D7 falls back to a minimal in-save header (nonce plus active flag) with everything else in side files.

**Save broker** (loader service):
- Interception **(revised: a MinHook on each save native's own handler, filtered by the calling thread's program hash, instead of OverrideScriptNative; one native per handler in natmap)** **(checked list, §2.4)**:
  - SET_SAVE_MENU_ACTIVE 0x1d60e70 (savegame_bed, save_anywhere);
  - DO_AUTO_SAVE 0x1d613d0 (autosave_controller);
  - QUEUE_MISSION_REPEAT_SAVE 0x1d615b0 (mission_repeat_controller);
  - **QUEUE_MISSION_REPEAT_SAVE_FOR_BENCHMARK_TEST** 0x1d615d0 (benchmark).
  - **GET_STATUS_OF_MISSION_REPEAT_SAVE** 0x1d615f0 (revision 4; one native per handler in natmap **(checked)**), filtered to mission_repeat_controller and benchmark: both poll it in a WAIT loop right after their queue native (§2.4). Together with the two queue hooks it refuses repeat saves while the officer is the player (§3.11).
  - The hooks also see mod and loader calls made through natives::Call; the broker re-issues its own calls through the trampoline.
- **Two parts (revised):**
  - **(a) pre-save step, for every intercepted save native, whoever the player is:** flush the left-behind officer state into block 13 (only when the officer feature was active for the loaded save, §3.6, revision 4); write a new nonce into the block-13 header before the native runs (or keep the nonce when no producer is loaded, rule 6); write that nonce's side files (write-ahead, above, with the carried-forward sections of producers that are not loaded and the block mirror, rules 6–8); raise the SDK "save about to happen" event; then release the native. Reason: before, a save made as M/F/T (bed, quick, MISREP, autosave) kept the loaded or last-brokered nonce, so two slots could reference one set of side files, and officer data changed since the last officer save pointed to an older generation. A cancelled save menu leaves an orphan generation, which is harmless and pruned. For SET_SAVE_MENU_ACTIVE the broker polls GET_STATUS_OF_MANUAL_SAVE after the release (as in step 6) and, on a cancel, rolls the in-memory header nonce back to the previous one, whoever the player is.
  - **(b) officer swap sequence**, only while the officer is the player (below).
- **DO_AUTO_SAVE during the broker's own swap (revised in revision 4): passed through, not swallowed.**
  - A swallowed call is lost with a misleading message: after DO_AUTO_SAVE @915 and +2 = 1 @919, autosave_controller sees IS_AUTO_SAVE_IN_PROGRESS false and +6 clear and shows SAVE_OFF (@936–@970) **(checked: @915–@951)**; the next pass sets +4 (@980 → @1006–@1063) and the one after that decrements +10 (@145–@233) (review). Revision 3's "deferred" was therefore undefined.
  - autosave_controller reaches DO_AUTO_SAVE only when a protagonist is the player (§2.4), and step 3 restores the hardening and removes the alias before its CHANGE_PLAYER_PED. So the hook lets the call through with pre-save step (a) and the R3 check, and step 7 starts only after IS_AUTO_SAVE_IN_PROGRESS is false.
  - On an R3 failure the hook swallows the call and sets the loader's pending flag (also the alternative if pass-through proves unsafe).
- Quick save (G9506, save_anywhere) is watched too.

**Autosave policy** (revised in revision 3):
- **autosave_controller already drops requests made as the officer** (§2.4): get_cur = 145 → `<3` false → @368 sets G102550+4 = 1 → the next pass decrements +10 and terminates. DO_AUTO_SAVE (@915) is never reached while the officer is the player **(checked)**. Revision 2's "swallow DO_AUTO_SAVE and record pending" therefore never fired. The 218 request sites keep incrementing G102550+10 as usual.
- **Record (loader only, keeps autosave_controller blind, D3):** while the officer is the logical player, the loader polls every frame and records *pending* when G102550+10 > 0, when +4 is set, or when an autosave_controller thread (hash 922362507) runs. A request lives at least 3 frames (main starts the controller, state 1, the state-2 cancel, the decrement on the next pass), so a per-frame poll sees it (confirmed in U22).
- **Re-request:** after the switch to a protagonist has completed (IS_PLAYER_SWITCH_IN_PROGRESS false, the player is a protagonist, alias removed, entry 3 and hardening restored), if pending is set and G102550+10 == 0, the loader sets +10 = 1. This respects the vanilla cap (request sites refuse when +10 > 1, or when +8 is set and +10 > 0). main@794333 then starts autosave_controller, which runs all its own gates (injured @455, switch in progress @526–@543, memory card @479, fade @633–@651, mission checks), its pre-save @2234 and DO_AUTO_SAVE. The broker's DO_AUTO_SAVE hook runs only the pre-save step (a) and passes the call through after the vanilla-clean check. The loader never calls DO_AUTO_SAVE itself.
- Pending is cleared on load, new game, officer disable and teardown.
- The officer's state is flushed at switch-out, so the re-request costs no extra swap or fade.
- **Race (new):** a request with +8 set skips the fade gate, so an autosave can start right after a CHANGE_PLAYER_PED to a protagonist. The broker therefore restores G2349[3], G2349[145] and G143050[145] **before** step 3's CHANGE_PLAYER_PED (moved from step 5), and the DO_AUTO_SAVE hook runs the R3 vanilla-clean check and aborts the save on failure.
- **Alternative, only if the loader route fails:** patch autosave_controller@346–@368 so that it takes the vanilla wait exit @365 while the officer is the player (`officer_mode()` and PLAYER_PED_ID == OFFICER_PED, copying the G44938 == 17 branch). This is a behaviour edit in a blind script and adds the officer_mode native to it, so D3's patch class would have to be widened explicitly. It also keeps G102550+1 = 1 (state 1, @446544) for the whole officer session. +1 is read through a shared getter in about 98 scripts (review count): category 5 of the shared can-launch gate refuses on it (launcher_golf@8430–@8481 → @9274 → 0), and every re_* uses category 5, so random events would be off (golf uses categories 4/6, which do not check it); main@785732 (Director Mode availability 8), pi_menu@458522, cellphone_controller@42854, appcontacts@31368, flow_controller@634081, player_controller@142285 (sets G102550 = 1). These are the test points if it is ever chosen.
- Menu, bed and quick saves as the officer still run the full swap sequence below.
- The officer menu gets a "save now" item (below).
- The player is told once per session: "As the officer, autosaves happen when you switch to a protagonist."
- Trade-off: officer progress since the last switch is lost on a crash. No timed brokered autosave (user decision Q2); "save now" in the officer menu covers it.

**"Save now", the mod's 存檔 action and the officer's bedside save (new):**
- All three set **G9506 = 1**, the vanilla quick-save entry (cellphone_flashhand@4471–4472). main (@793257–793288) then starts save_anywhere, and its can-save routine (@468268–@468537) applies to the officer too, since it has no character gate: injured, control off, not ready for a cutscene, shooting, combat, melee, being jacked, entering a vehicle, getting up, in air, ragdoll, being arrested, G1575097 (@468636), G24447+4, a pending switch request (99964+44 == 1, @468608), player_timetable_scene (@468582), G80623/G65074 (@468540), memory card in use, autosave in progress, the mission flag. Its store helpers (@434113 etc.) skip non-playable characters (partly checked).
- The broker then sees save_anywhere's SET_SAVE_MENU_ACTIVE (@229) and runs the swap sequence.
- save_anywhere refuses silently in free roam (§2.4), so the mod shows its own message when no SET_SAVE_MENU_ACTIVE arrives within a short time.
- **Mod-level gates before G9506 is set:** no active pursuit or custody, not driving or piloting (car, aircraft, boat), no pending switch request (99964+44), wanted level 0. A refusal shows a notice and never starts a swap.
  - **Revision 4:** also the "may switch now" predicate of the wheel (§3.10), because the broker's save is itself a player swap, and no "save busy" state (below).
  - save_anywhere's own routine (@468268–@468537) has no activity term and does not read G114037, which taxiservice alone sets to 1 during a ride (@64930, @65676; cleared @493, @67529) and selector's chain reads (@24700) **(checked)**.
  - The light Q4 scripts never call SET_MISSION_FLAG (taxiservice, stripclub, stripclub_drinking, pb_prostitute, act_cinema, cablecar, rollercoaster, ob_telescope; review counts), and taxiservice calls SET_PLAYER_CONTROL only in its cleanup (@514 **(checked)**), so control is probably on during a ride (unverified) and a passenger is not driving. Before this rule the switch gate refused in a taxi but the save gate did not.
- Today's paths have none of these checks **(checked)**: the root menu action 存檔 (`examples/police/main.cpp:35` → `:117-121` → SaveForOfficer), the bedside save (`life.cpp:193`) and `officer.cpp:371-391`, which checks only that the left-behind protagonist exists (otherwise it refuses with "無法存檔：找不到留下的主角", `officer.cpp:380-384` **(checked)**; this becomes a normal case once Phase A hands him to player_controller, step 2b), then holds (freeze + invincible, `officer.cpp:36-44`) and calls CHANGE_PLAYER_PED.
- save_anywhere runs its pre-save @789 (from @168) before SET_SAVE_MENU_ACTIVE, while the officer is still the player; step 4 re-runs @789 after the swap anyway. That this first run does not kill the thread is checked in U22.

Sequence (explicit saves as the officer):

1. Fade out and let the officer's state settle. **If the officer is in a vehicle, bring it to a stop first** (or refuse above walking pace), and keep the vehicle and the ped frozen until step 7. (That a left-behind vehicle keeps rolling during the swap is a guess.)
2. Flush engine-held officer values into block 13: inventory (into the WEAPONS shadow layout), SP3 stat mirror, position, heading, clock, look reference.

   **Step 2b (revision 4): materialise the protagonist.** After the Phase A hand-over, player_controller may have deleted the left-behind protagonist (§3.10), and it re-creates him only when the player comes near his last-known position. If his ped no longer exists or is dead:
   - preferred, the game's own path: step 3's restore and alias removal, then a switch request (99964+45/+46/+44, as SwitchFrom does at `officer.cpp:313-315` **(checked)**), so player_controller_b creates him and restores him from PP_INFO. The game's switch replaces step 3's CHANGE_PLAYER_PED, and step 7 is the mod's switch back. It costs a second full switch (U22 measures it);
   - alternative: CREATE_PED at PP_INFO's position and heading (+2300/+2310), never at the officer's position and never inside an officer-only interior (such as the officer's MP apartment), then restore VARIATIONS, WEAPONS (+298), health (+290) and armour (+294) before the save native is released. Calling player_controller's own create and restore functions (@255266, @543508, @542050) through CallScriptFunction is unverified: they run against another thread's statics (guess);
   - after the save, he is handed back to player_controller with PP_INFO unchanged.

   Guess to test (U15): setting 99964+7 = c pins the ped (keep reason 19 at @254296 **(checked)**); its meaning is unknown.

3. **Restore G2349[3], G2349[145] and G143050[145] (moved here from step 5; race above).** Remove the alias. CHANGE_PLAYER_PED to the protagonist that was left behind, using the game's take-control bookkeeping (§3.10).
4. Run the game's pre-save routine (save_anywhere@789 or autosave_controller@2234 through CallScriptFunction), so e-mails, ped info and the safehouse override are fresh.
5. Sanitize (invariant check; abort and warn on failure). The pre-save step (a) writes the new nonce, the active flag and the side files.
6. Re-issue the save native and **poll GET_STATUS_OF_MANUAL_SAVE until it is != 1**, as savegame_bed@3361 does, with a timeout. GAME_SAVED, IS_AUTO_SAVE_IN_PROGRESS and the mtime do not fire on a cancel. A changed SGTA5xxxx or MISREP mtime means "saved": the slot index records the nonce. Otherwise ("cancelled or failed") the header nonce is rolled back to the previous one, the written-ahead file is left as an orphan for pruning, and step 7 runs.
   - Sub-test (U22): does SET_SAVE_MENU_ACTIVE show its menu while the screen is faded out? Vanilla avoids that (save_anywhere@193–207 opens the menu only when the screen is not faded or fading out). If it does not, fade in on the protagonist with the HUD and radar hidden before re-issuing.
7. Swap back, only after IS_AUTO_SAVE_IN_PROGRESS is false (revision 4), re-apply the alias, entry 3 and the hardening, unfreeze, fade in.

**"Save busy" (revision 4; loader-owned exclusion between saves and switches):**
- Set while the broker sequence runs (steps 1–7) and while a save is in flight: IS_AUTO_SAVE_IN_PROGRESS, GET_STATUS_OF_MANUAL_SAVE == 1 after a brokered SET_SAVE_MENU_ACTIVE, or GET_STATUS_OF_MISSION_REPEAT_SAVE == 1.
- While it is set:
  - the "may switch now" gate, `OfficerAvailable()`, OwnWheel/GameWheel, SwitchTo, SwitchFrom and Restore refuse;
  - the loader calls DISABLE_CONTROL_ACTION(0,19,1) every frame and turns player control off during steps 1–5 and 7;
  - DISABLE_FRONTEND_THIS_FRAME is used only outside step 6's menu, and only after checking that it does not suppress SET_SAVE_MENU_ACTIVE (that it might is a guess).
- A switch into the officer that starts during an autosave waits before its CHANGE_PLAYER_PED stage until IS_AUTO_SAVE_IN_PROGRESS is false. The CHANGE_PLAYER_PED hook never applies the alias, entry 3 or G143050[145].17 = 4 while a save is in flight (§3.7).
- Reason: during the broker's swap a protagonist is the physical player and nothing blocks the game's wheel (review):
  - selector's main loop (@279 → @1715 → @2077 → @2103–@2131) tests the wheel control (@26217 / @25719: IS_CONTROL_PRESSED and, on some paths, IS_DISABLED_CONTROL_PRESSED / IS_DISABLED_CONTROL_JUST_RELEASED) and then calls @24088, whose chain has no IS_SCREEN_FADED_OUT and no IS_PLAYER_CONTROL_ON. It does test IS_PAUSE_MENU_ACTIVE @24643, GET_PAUSE_MENU_STATE @24860 and IS_SAFE_TO_START_PLAYER_SWITCH @24212, whose behaviour during a fade or a save is unknown;
  - neither selector nor player_controller_b calls IS_AUTO_SAVE_IN_PROGRESS or GET_STATUS_OF_MANUAL_SAVE (rg count 0); autosave_controller blocks the wheel (G24447+6 via @1068) only while its warning is shown (@987–@999);
  - the mod's GameWheel runs whenever the officer is not the player (`wheel.cpp:176-187`; SwitchTo on close at `:100-101` **(checked)**).
- The selector paths that read IS_DISABLED_CONTROL_PRESSED are not blocked by DISABLE_CONTROL_ACTION; player control off is expected to cover them (guess; U22 (a)).
- Whether a switch into the officer right after DO_AUTO_SAVE can leak into slot 02 depends on when the engine captures the save data (at the call or later; unknown, U22 (b)).

**Load:**
- The block is reset to defaults by ml_char4_reg before its STOP applies saved data. Without the reset, a save that lacks the block would inherit the previous session's officer.
- The loader reads the nonce and "enabled for this save" only after ml_char4_reg's ready flag for this placement is set (§3.6).
- The load hook resets SP3_* stats (§3.7) and clears the session latches (§3.2).
- Then the mod reads the nonce:
  - officer active → create the officer and switch to him (today's Restore path), bind OFFICER_PED;
  - no block: if the slot index holds a nonce for this slot file (recorded by a brokered save, rule 9) and the file no longer matches its recorded mtime and size, offer a restore from that nonce's block mirror (rule 8), with a warning. Only a slot never recorded with a nonce takes the legacy import (below). This needs the identity of the loaded slot file at the load hook (the 0xd1ef80 argument or the last SGTA5 file read; unverified, U36); story.cpp cannot tell slots apart today (§2.4) (revision 4);
  - otherwise defaults.

**Legacy import (decided; only for slots never recorded with a nonce, revision 4):**
- Today's `officer` record (active flag and position) is imported only into the save whose game clock matches it within ±30 game minutes. This is the rule persist.cpp already applies.
- `officer_look` and `officer_reports` are shared by all slots today. They are **copied into every legacy save the first time that save is loaded**, so each save keeps what the player sees today. A notice says so.
- After import the legacy JSON is kept read-only as a backup and is never written again.

**ml::save v2 and SDK** (for every mod; D10):
- Existing API unchanged: `ml::game::CurrentCharacter()` / `CharacterIndex()` stay −1..2 (by model), so the existing `[3]` arrays (`examples/bank/main.cpp:158`, `examples/property/main.cpp:122-123`, `examples/property/garage.cpp:229-230`, `examples/social/main.cpp:122`, `examples/social/gen.cpp:59`; 23 call sites) stay in bounds.
- New opt-in API, available only when the loader's MLApi has the new members (`detail::Has` check):
  - `ml::game::Slot()` returns 0..3, where 3 is the officer;
  - `Character::Officer`;
  - ml::save slot 3 for mods that declare 4 slots;
  - per-save scoping by nonce (committed by the pre-save step of every save, write-ahead);
  - events "save about to happen" (raised by the pre-save step, before the native is released) and "save loaded";
  - `CharacterElement(global, character)` returns the vanilla element or the shadow, so our bank, stocks, property and browser pages read officer data uniformly.
- `Cash/AddCash/Pay` for the officer: `Pay` currently runs appinternet's bank function, which is blind and has no case 3 (§0.2 items 5, 6, 12). For `Officer` it runs the aware copy in `ml_char4_bank` (`RunFunction("ml_char4_bank", …)`; no global block, so a fresh placement cannot re-create block 13, §3.6) with **account 3**. That copy updates G62254[3] (shadow), the bank log shadow and SP3_TOTAL_CASH through its CASE3/UNROLLED rules. `Cash(Officer)` reads SP3_TOTAL_CASH.
- All `examples/*` are audited before slot 3 is enabled for them, **including their RunFunction pattern lookups** (game.hpp:171 Pay, examples/stocks/main.cpp:132, examples/property/shop.cpp:682, examples/epsilon/main.cpp:56/97): a test checks that each pattern still resolves, through the vanilla-image redirect, on the rewritten pack (§3.4).

**Optional native officer saves (M7):** the engine saves with the officer as the player. This requires:
- the main@779336 gate, autosave_controller@314 (the drop path of §3.9), startup_positioning;
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
  - **Consequence (revision 4): player_controller then deletes him like any left-behind protagonist.**
    - Its loop @252794 leaves only during a switch or request (99964+44 == 1, IS_PLAYER_SWITCH_IN_PROGRESS), a mission-type state, or G80362. With the officer as the player, +4321 = 145 matches none of the slots 0..2 (@555681), so all three left-behind protagonists are processed (review).
    - The keep test @254246 returns "delete" unless collision is loaded and the distance is under 200 m (@254401–@254442), the ped is visible and under 40 m (@254450–@254480), G133532[c] == 0 (318 in the dump), 99964+7 == c (@254296, keep reason 19), or a bit of 99964+47 is set (0 in the dump) **(checked: @254246, @254296)**.
    - Delete path: @253391 DOES_ENTITY_BELONG_TO_THIS_SCRIPT → +2296 time and the vehicle (@551461) → DELETE_VEHICLE @253624, DELETE_PED @253637 / @253666 **(checked: @253624, @253637)**. A ped owned by no script or by player_controller_b is adopted first (@253699–@253754) and deleted on a later frame.
    - Re-creation (@253186 → @546553 → @254720 → @255266 CREATE_PED, then @543508 / @542050) happens only when the player is near the last-known position (review).
    - Today KeepProtagonist (`officer.cpp:47-50` **(checked)**) keeps him as the mod's mission entity, so this appears only with the hand-over. Every path that needs him (the broker's save, the fault teardown) therefore handles a missing ped (§3.9 step 2b, §3.11).
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
  - **"May switch now" gate (new).** Per-slot availability is not enough: today OwnWheel opens in any state except a running switch (`wheel.cpp:139` reads `IS_DISABLED_CONTROL_PRESSED(0,19) || IS_CONTROL_PRESSED(0,19)`, `:142` checks only `!IS_PLAYER_SWITCH_IN_PROGRESS`, `:172` calls SwitchFrom), and SwitchFrom (`officer.cpp:305-319`) checks only `character >= 0 && IsPlayer()` **(checked)**. player_controller_b consumes the request with only an IS_PED_INJURED check (@340–@372, request type 1), so nothing else stops it. OwnWheel, SwitchFrom and SwitchTo get one global gate that mirrors selector@24088 **without its character term** (calling selector@24088 itself would refuse: its @24397–@24490 branch runs get_cur = 145 → is_playable false):
    - injured (@24093) **(checked)**; being arrested (@24144); ragdoll (@24157); player_timetable_scene running (@24188 → @25573, hash −1424752554); in a vehicle but not seated and more than 1.5 m above ground (@24202 → @25482); `!IS_SAFE_TO_START_PLAYER_SWITCH` (@24212); parachute free fall or parachute state (@24502–@24540, when G24447+12 is clear); switch in progress (@24559);
    - the selector's block flags **G24447+5 / +6** (@24869) **(checked)**, set by activity scripts: range_modern@591029–591038 sets +5, act_cinema@679600–679609 sets +6 **(checked)**; also G24447+11, +12, +17;
    - the rest of the @24570–@24848 chain: cutscene, pause menu, system UI, stunt jump, loading screen, G114037, G77414, G65071, weapon wheel and phone HUD components (16/19), G80617 == 4/5.
    - The wheel reads **only** `IS_CONTROL_PRESSED(0,19)`, so an activity's `DISABLE_CONTROL_ACTION(0,19,1)` blocks it as it blocks the game's wheel (act_cinema@1796–1800, pb_prostitute@18572).
    - **(revised in revision 4)** No blanket "aware thread running" term. Read literally it never passes after M2: shop_controller runs all session (started by main_persistent@2540848–@2540852 **(checked)**; it terminates only at @328/@340), atm_trigger and ob_vend1/2 are object brains (standard_global_init@42172–@42218, @42747+), launcher_golf is a world-point brain (@43746), and selector, respawn_controller and player_controller_b run continuously (review). The mirrored chain above already covers G24447+5/+6/+11/+12/+17, G114037 (taxi ride), control action 19 and cutscenes. Per-script "engaged" states are added only where U34/U9 find an activity that the chain misses (for example a private dance or an open shop menu). Persistent controllers and idle object or world-point brains never block.
    - The same predicate gates "save now", 存檔 and the bedside save (§3.9), because the broker's swap is a player switch.
    - **"Save busy" (revision 4):** every switch path refuses while a save is in flight or the broker runs (§3.9).
    - Greying the protagonist slots while the officer is wanted depends on whether G24447+20..23 are refreshed while a non-protagonist is the player (guess; U34).
  - **Session state:** in the torn-down state (§3.2) the officer slot is greyed and SwitchTo/Restore are refused until the next load (after a mod fault: until restart).
- The left-behind officer is managed by the mod: wander while visible, then record position, heading, time and patrol car and despawn. This replaces the immediate Drop().
- **Left-behind injuries (new):**
  - If the left-behind officer is killed, he is "hospitalised": the next switch to him starts at the nearest hospital, the hospital fee is charged to his account, and the last-known position is reset.
  - Damage to left-behind protagonists by the officer is left to the game, after U30 shows how player_controller reacts (PP_INFO health, AVAILABLE, script deaths).
- Re-apply relationships, uniform props and look after every respawn (respawn_controller@50930 resets the group to PLAYER at 51733).
- Guard arrest while the officer is the player: **a loader service from revision 4** (§3.2 "Arrest guard"), active before M2 and, after M2, whenever respawn_controller cannot serve him as 3 (degraded, torn-down, police-mod fault). Today it is the mod's override (`examples/police/guard.cpp:67-72`).
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

Each path tears the officer session down cleanly, or, for the repeat saves (benchmark, Replay Mission), is refused while the officer is the player (revision 4). Teardown means:
- record the officer state into block 13;
- remove the alias and restore G2349[3], G2349[145] and G143050[145] **once the officer is no longer the player (revision 4)**. While he stays the player (the fault row's prompt, or a refused Online attempt that does not reload, U31) they follow §3.2: 145.17 = 4 under the mission_triggerer_d condition, G143050[145].17 = 4 restored by the broker before every save, the alias kept. At the Online transition signal D9 restores everything at once; if story mode then continues with the officer as the player, the hardening is re-applied (U31);
- keep the loader's arrest guard active while the officer is the player (§3.2);
- set `officer_mode()` false; **keep the detour's serve byte** (§3.3 gate 4, revision 4), so aware threads that hold a cached 3 still reach the catalogued shadows;
- mark OFFICER_PED stale;
- set the session state to **torn-down** (§3.2): `OfficerAvailable()` is false, the wheel slot is greyed, SwitchTo/Restore are refused.

The officer is restored only through a normal load of a brokered save. The load hook is the only place that clears the torn-down state (U36 checks that it fires for every load kind).

| Path | Evidence | Signal and treatment |
|---|---|---|
| Rockstar Editor (pause menu) | main@465762: REPLAY_SYSTEM_HAS_REQUESTED_A_SCRIPT_CLEANUP → G80141 = 1, G104707 = 1 → @568590(1): FORCE_CLEANUP 8/64, CLEAR_AREA, then @569229–@569280 loops SET_PLAYER_MODEL(player_zero) until the model is a protagonist (@465152) **(checked)** | The loader polls the same native, or observes it through an override in main, and tears down first. What happens on return is unverified (guess: SP reloads from a save; U23). |
| Director Mode | main@784857: HAS_DIRECTOR_MODE_BEEN_LAUNCHED_BY_CODE → SET_DIRECTOR_MODE_LAUNCHED_BY_SCRIPT; availability @785577(1,0,119) → SET_DIRECTOR_MODE_AVAILABLE at @784904/@784912, **called only when the value changes** **(checked)**; director_mode restores the player with CHANGE_PLAYER_PED @909159 (also @627589) | A loader hook on SET_DIRECTOR_MODE_AVAILABLE (its own handler 0x1d554e0, filtered to main's program hash; revised from OverrideScriptNative, §3.2; part of the loader's char4 service, not the mod) forces 0 while the officer is active. The loader remembers main's last requested value and re-issues it when the officer is no longer active. If Director Mode is launched anyway, tear down (U24). |
| Benchmark (settings) | benchmark@11952–12025 repeat save (gated by G124084; meaning unknown, 1 in the dump), @13570–13621 forces player_zero, QUEUE_MISSION_REPEAT_LOAD @501391; command-line path RESTART_GAME @1075 / QUIT_GAME @1082 **(checked)** | **Refused as the officer (revision 4).** The QUEUE_MISSION_REPEAT_SAVE_FOR_BENCHMARK_TEST hook does not call the original and latches "refused" for that thread; the GET_STATUS_OF_MISSION_REPEAT_SAVE hook returns 2, so benchmark terminates (@12007); the loader then fades in and gives control back if needed (U25) and shows "switch to a protagonist to run the benchmark". Reason: the caller polls the status in the same frame (@11983–@11996 **(checked)**), so a multi-frame swap has no defined way to hold it, and benchmark@11967 → @458070 and @16279 probably capture the player before the queue native, as mission_repeat_controller does (guess). Alternative (hold): the status hook returns 1 while the broker runs steps 1–5, then the broker calls the original queue native and passes the real status through; there is no step 7 (the protagonist stays the player, so the @12931 model test passes), and the officer becomes left-behind state, restored by the MISREP reload (U36). |
| Pause menu → Replay Mission | mission_repeat_controller QUEUE_MISSION_REPEAT_SAVE @571897, SET_PLAYER_MODEL(player_zero) @665272 when the model is not a protagonist **(checked)**; in pausemenu_sp_repeat only the shared helper copies refer to the character (no gate found); status loop @571902–@571931 **(checked)** | **Refused as the officer (revision 4; Q1 and §1: mission replay is NONE).** The QUEUE_MISSION_REPEAT_SAVE hook does not call the original and latches "refused"; the status hook returns 2, so the vanilla abort @509(1) runs (unpause, fade in, clear the G80611 bits, SET_PLAYER_IS_REPEATING_A_MISSION(0), terminate; review); notice "switch to a protagonist to replay a mission". Reason: before the queue native mission_repeat_controller has already captured the player into the saved repeat block (@571867 → @669275 → @203639, @193247, @571893 → @665533; §2.4), so a swap at queue time would still write the officer's position and snapshot into the MISREP's return point. Supporting replay as the officer would need the swap before those captures. |
| Peyote trip | animal_controller@3275 SET_PLAYER_MODEL(char_model(static 149)) on return **(checked)** | NONE in v1 (§3.10; U33). |
| Going to GTA Online and back | main_persistent@2529528 sets G80362 = 1 (phone owner → 3) during the transition **(checked)**; maintransition SHUTDOWN_AND_LOAD_MOST_RECENT_SAVE ×4 on return | Teardown at the first transition signal (G80362 rising or maintransition starting; their meaning as transition flags is a guess), before freemode exists. The return loads the most recent save, which is clean. Officer progress since the last save is lost, as for vanilla progress (guess; U31). Whether that load reaches the load hook is U36. |
| main's forced-protagonist check | main@14801 (every second, when G153585 = 0) **(checked)** | Not reached while G153585 = 1. Relevant for M7 and for load ordering. |
| Editor or Director Mode exit without a reload | §3.11 rows above | The session stays torn-down: a switch to the officer is refused until the next load (§3.2). |
| **Police mod faulted while the officer is the player (new)** | AbandonCurrentFiber sets State::Faulted (`mods.cpp:77-87`) **(checked)**, reached from native crashes (`:141`) and task crashes (`:643`); RunTasks stops a faulted mod's tasks (`:688-705`); the override lambda calls the mod's callback with no state check (`:534-541`) **(checked)**; toggling a mod only sets g_restartNeeded (`ui/menu.cpp:364-372`) **(checked)**. The mod's SET_DIRECTOR_MODE_AVAILABLE override (`examples/police/guard.cpp:49`) would keep running. | **Fault teardown by the loader (revised in revision 4):** fade out; the loader's arrest guard stays active (§3.2). When a protagonist is available, switch to him through the switch request 99964+45/+46/+44 (as `officer.cpp:313-315`), but only while the "may switch now" conditions hold (not injured, not being arrested, respawn_controller not running); until then, wait. The request path needs no existing ped (player_controller_b creates him from PP_INFO, §3.10), so it is used unconditionally instead of the broker's CHANGE_PLAYER_PED swap, which needs the ped. With no available protagonist: a "load your last save" prompt. The alias, entry 3 and the hardening follow the teardown list above (restored once the officer is no longer the player); session torn-down until restart; block 13 left as it is; warning. The char4-critical overrides (Director Mode block, save broker, arrest guard) are in the loader's char4 service, so they do not depend on the mod. Separately, the loader calls the original native instead of a Faulted mod's override callback. **Corrected:** revision 3 said "the player is not trapped"; that holds only with the arrest guard in the loader, because while the officer stays the player an unguarded arrest softlocks the game (phase0.md §53.16). The loss is progress since the last save and the mod's features. |
| Police mod absent or disabled | — | ml_char4_reg and the SP3 stats file stay mounted, so officer blocks and SP3 stats round-trip (§3.6); `officer_mode()` false; no aware rewrites; the pre-save step carries the police mod's side-file section forward and skips the officer flush (§3.9 rules 6–9, revision 4). |

---

## 4. Per-system treatment table

| System | Where it lives | Officer | Work |
|---|---|---|---|
| Who the player is | helpers in 734–777 scripts; G122220–2 | OWN (3 in aware scripts; 145 in blind) | §3.2 identity rewrite with officer_mode(); G122220–2 untouched |
| Character sheet entry | G2349[3], G118[3] (runtime); G143050[3] (saved) | OWN runtime entry; saved entry untouched | loader service; broker restore and check |
| Cash and HUD cash | G62254 (by account), SPn_TOTAL_CASH, StatId5 0x4eef0d4, HUD 0x54a550 | OWN | shadow 62254[3]; alias k; SP3_TOTAL_CASH; CASE3 on @857075 / @856578; UNROLLED on @856918 |
| Bank account and payments | wrapper gunclub_shop@854349 / carwash1@40944; bank function @854401 (param 0 = account); log G135796; G2349[c].17 | OWN, account 3 | 2349[3].17 = 3; wrapper `PUSH 3`→`PUSH 4` in aware scripts; CASE3 (manual @854444) in the bank function (F′); log shadow |
| ATM and phone bank page | atm_trigger@9716, appinternet@3077230: account from **G143050[c].17**; account-name SWITCH 0/1/2 | OWN | until M2: G143050[145].17 := 4 (refuse, broker restores); M2: atm_trigger aware (read 2349[c].17, CASE3 label); appinternet M5 or our browser |
| Michael-account leak through 145 | G2349[145].17 = 0 | fixed | 145.17 := 4 while the officer is the player and mission_triggerer_d is not running; M1: mission_triggerer_d safety patch, which lifts the condition only when the served copy is ours (§3.2); hardening follows "officer is the logical player", also when degraded (§3.2) |
| Hospital / arrest fees | respawn_controller@20594–@20677, @48484, @48074 | OWN | respawn_controller aware; hash-triple rule |
| Arrest weapon loss | respawn_controller@20681–@20800 | OWN (vanilla rule) | aware: WEAPONS shadow; the loader's arrest guard before M2 and whenever respawn_controller cannot serve the officer as 3 (degraded, torn-down, mod fault; §3.2, revision 4) |
| Stocks | M_/F_/T_ groups G135563…135651; stock_controller@7748; appinternet@24146 | OWN | GROUP4 (33 slots); our browser page through CharacterElement |
| Weapons, ammo, tints, parachute | ped inventory; WEAPONS G118197; WEAPON_TO_SHOW G119629; store player_controller_b@29307 | OWN | shadows; game store and restore (M3); interim mod store in the same layout |
| Ammu-Nation | shop_controller (@96425 single gate for all shop types), gunclub_shop (177 triples, dual-mode) | OWN | both aware; **GUARD by shop type at @96425 (gun shops only until M4)**; SP3 stats; shop blips (same filter in blip_controller); gunclub_shop reads SHOOTING_RANGE[c] (@95342 → @96206), so the SHOOTING_RANGE shadow (§2.2, §0.4 Q5) must exist before the M0 tracer bullet; what the officer's range medals unlock is logged in U7 |
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
| Phone contacts | owner-3 columns (2349[x] +16, +23, +28; entry 96 has +28 = 1); appinternet owner-3 ops on contacts 147/82; **(revision 4)** appcontacts@45289–@45323 calls @46149(G8817, G21666, 1) when the phonebook value is 2 or the contact is 130, and @46149 writes G2349[c].24[owner] and, for c < 162, the saved G143050[c].24[owner] (@46189–@46213; owner 3 accepted) (**checked:** @45289–@45313) | OWN | write the runtime column only, re-applied after load (keeps the save vanilla); **(revision 4)** entries get value 1, never 2, and contact 130 is never listed, so the appcontacts mirror cannot fire; the G143050 owner-3 image is in the vanilla-clean invariant (§3.9) and is restored to the image, not 0; the service contacts (at least taxi 131) come in M2 with Q4, the rest in M5; U5 watches all three owner-3 fields |
| Contacts app crash | appcontacts@41041 G24150[owner] [3] | fix | safety patch (M1); until then: known crash when the officer confirms the first contact entry (prediction, U27) |
| Officer texts | PhonePresence +99[3] | OWN later | M7 |
| Protagonist calls, texts, e-mails | comms_controller Player_Char_Bitset (bits 0–2), masks 1..7, EMI_AR | NONE | blind (145 → tests bit 17): static check that no comm uses bit 17 |
| Story flow, missions, triggers | flow_controller, mission_triggerer_a–d, code_controller | NONE | blind; mission_triggerer_d safety patch for its account read |
| Random events | 38 re_* | NONE as content (rewards no longer reach Michael) | blind; optional suppression |
| Strangers & Freaks | randomchar_controller, 32 launchers | NONE | blind |
| Friends, booty calls | friends_*, bootycallhandler | NONE | blind |
| Strip clubs | stripclub (@4109 SWITCH 0/1/2), stripclub_drinking (payment sites) | SHARED service, OWN money; relationship data NONE | aware ("light"), Q4; U9 maps the sites; player-speaker sites follow §3.4's policy (U39) |
| Prostitutes | pb_prostitute (payment site; PROSTITUTE_SAVED_DATA G125049 / SERVICES G125138 per character) | SHARED service, OWN money; relationship data NONE | aware ("light"), Q4; relationship arrays NONE; the speaker SWITCH @15489 (no case 3) follows §3.4's player-speaker policy (U39) |
| Minigame fees (golf, tennis, darts, range, hunting, races; the vanilla towing job stays NONE, §0.4 Q8) | activities: debit sites in golf, tennis, darts, range_modern, hunting_ambient, towing, offroad_races, launcher_racing (its bank function @2367 is why it is in `w/debit_calls.txt`). **Launchers (new):** the can-launch routine (`PUSH 6`/`PUSH 4`; `CALL`) runs IS_PLAYER_PLAYING, DOES_ENTITY_EXIST, IS_PED_INJURED, then `get_cur; is_playable; INOT; JZ; PUSH 0; LEAVE` → refuses 145: launcher_golf@1150 → @7453–@7509 **(checked)**, launcher_range@9943, launcher_tennis@115509, launcher_darts@25833, launcher_hunting_ambient@7399, launcher_offroadracing@15796. Cash gates launcher_golf@1656–1705 and launcher_range@1376–1429 compare `getter(get_cur)` with the fee ("MG_YOU_IS_BROKE"); the getters (launcher_golf@6349 **(checked)**, launcher_range@8901) are SWITCH 0/1/2 over SP0/1/2_TOTAL_CASH with default 0. controller_towing@43021: with param 2 set, @43148 `get_cur != 1 → return 0` (Franklin only) **(checked)**; it requests "towing" at @1333–1336. | fees OWN; records: **not shared with protagonists, served by E shadows** (non-saved, or saved per §0.4 Q5) | activities and their launchers aware ("light"), Q4: identity rewrite of the can-launch routine, CASE3 SP3_TOTAL_CASH in the cash getters (hash-triple rule), controller_towing and towing stay blind (§0.4 Q8); launcher_tonya and the other RC launchers stay blind; U9 maps each launch path; SHOOTING_RANGE shadow (§2.2); player-speaker sites follow §3.4's policy (U39) |
| Family, timetable, switch scenes | player_timetable_scene, family_scene_* | NONE | blind (the officer's day is life.cpp) |
| Safehouses and beds | building_controller, savegame_bed | NONE (officer home = property 20) | — |
| SP properties and businesses | PROPERTIES G139903 owner enum | NONE | audit: 3 never written |
| Completion %, achievements, minigame records | completionpercentage_controller, achievement_controller, range, golf, pilot school | NONE for completion and achievements; the officer's own minigame records: see the "Minigame fees" row | blind (completion, achievements, pilot school) |
| Collectibles | global flags | SHARED | none (their autosave requests are deferred, §3.9) |
| Public services | taxiservice and **taxilauncher** (revision 4: the phone booking path @32216 → @35956(131) → @35804 accepts c = 0..2 only through @35914(131,c) = contact 131's owner column (G2349[131] fields 13–16 = 1,1,1,0 in both sheets) or c == @2545() (222/161), so 145 never passes, and the officer's iFruit is empty (phase0.md §53.16); review, @35914 **(checked)**), carwash1/2, ob_vend1/2, cablecar, bigwheel, rollercoaster, act_cinema, ob_telescope | SHARED service, OWN money | aware ("light": wrapper + identity), Q4; taxilauncher@35804 gets a case 3 that reads @35914(131,3), @35703 follows the player-speaker policy (§3.4), and the taxi contact comes in M2 ("Phone contacts" row); launcher_carwash (is_playable copy @4876, message CWASH_NOMONEY @9037 **(checked)**) is traced in U9 and made aware if its path gates on the character |
| Peyote animal trips | animal_controller | NONE (v1) | blind; block pickups if U33 shows a deferred trip |
| Director Mode | director_mode; main@784857 availability | NONE | SET_DIRECTOR_MODE_AVAILABLE hook filtered to main, in the loader's char4 service (§3.11) |
| Rockstar Editor, benchmark, mission replay | main@465762, benchmark, mission_repeat_controller | NONE | Editor: teardown; benchmark and Replay Mission: refused as the officer through the repeat-save status hook (§3.11, revision 4) |
| Mission replay / snapshots | G105547 / G108531; replay_controller, mission_repeat_controller | NONE | blind; never active with the officer |
| Character wheel | selector, G24447 [4] slot 3 | OWN slot 3 | Phase A overlay with availability rules and the "may switch now" gate mirroring selector@24088 (§3.10); refused while "save busy" (§3.9) → Phase B aware |
| Switching | player_controller_b; G99964 [4] | OWN | Phase A bookkeeping → Phase B aware (OFFICER_PED on creation) |
| Left-behind world | player_controller@252794 (cycles slots 0–2; vehicle tracking @252985–253153) | protagonists: the game; officer: the mod | hand-over (Phase A); injury policy (§3.10, U30); **(revision 4)** player_controller deletes a handed-over protagonist beyond 200 m / out of sight (@254246) and re-creates him only nearby, so the broker materialises him (§3.9 step 2b) |
| Last-known state, wanted level | LAST_KNOWN_* G120199…G120227 | OWN | shadows |
| Relationship groups, TC modifier, blip name | player_controller_b@26867 / @3982 / @27303 | OWN | the mod (Phase A) |
| Saving | main@779336, autosave_controller, save_anywhere, savegame_bed, mission_repeat_controller, benchmark, MISREP | OWN data, own block | broker (§3.9): "save busy" exclusion; pre-save step (new nonce or carry-forward, write-ahead side files with a block mirror) for every save; materialise a despawned protagonist (step 2b); officer swap sequence; "save now" through G9506 with the "may switch now" predicate; autosave requests recorded by the loader and re-raised once after the next switch; repeat saves refused as the officer |
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
  - F′ begins with the officer_mode() fallback to V;
  - **(new)** every modified function is marked yielding / never-returning or not, through the call graph; yielding ones are entered on `pack_active()` and have every decision site guarded with `officer_mode()` (§3.4);
  - **(new)** appended F′ and V have a named ENTER.
- **S2 Decision completeness** for every aware script (§3.5 item 3), including NATIVE-ARG, UNROLLED and SET_PLAYER_MODEL sites, **and (revision 4) player-speaker sites**: protagonist voice-name strings next to a PLAYER_PED_ID registration through a conversation helper; each gets the §3.4 policy, never CASE3 to a protagonist voice.
- **S3 Index safety:** character-typed ARRAY indexes resolve to whitelisted or 4+-element arrays. **(new)** It lists every static or local array reached with a character index and marks multi-instance programs (object brains: atm_trigger, ob_vend1/2, …; shop_controller instances), so S and L arrays get the per-thread treatment of §3.3. It also lists, per Q4 script and launcher, every per-character array indexed with a character value (§3.5 item 3).
- **S4 Identity containment:**
  - no direct +4321 loads in aware scripts; no retargeted helpers in blind scripts;
  - audit of character-valued global writes;
  - **owner-domain report**: every array indexed by an owner-typed value has count ≥ 4 or a safety patch;
  - no block-13 access outside an officer_mode() region;
  - **(new)** every raw +4321 load is classified "preceded by update_cur" (→ get_cur4) or "raw cache read" (→ load_cur4);
  - **(new)** the static/local array report of S3 with the multi-instance marks.
- **S5 Save block:** unique `ML4_` names, all names `[A-Za-z0-9._:]` and shorter than 64 characters, registered size equals the layout, vanilla registration unchanged.
- **S6 Bit-3 report:** every bitset reachable with 3 in aware scripts, with values from the 100% dump (gread). Bit 3 must be free or the site must be guarded.
- **S7 Writer report (new):**
  - native-table round trip: decode with the new code size, compare hashes;
  - **(revised)** the appended officer_mode and pack_active entries: each is a registered native whose handler no other native shares (natmap), takes 0 arguments, returns 0/false in story mode, and is not otherwise used in the program; their slot indices are in the manifest;
  - maximum call depth (< 128) and stack use per thread entry point against its start stack size, **including shadow statics (statics + worst-case frames must fit each start stack size)**;
  - forwarder fit (12 + 2p bytes), or proof that no function pointer to the function exists;
  - in-place edits limited to the allowed kinds (D5);
  - **(new)** startup and ml_char4_reg pass together, or neither is written to the pack (§3.6);
  - **(revision 4)** static layout of every program with S-kind shadows: argument block last; +0x20 unchanged; every STATIC operand ≥ s−a renumbered by +K; static count + K strictly below every start stack size (0x921acc `cmp r9d,eax; jnl` → error) and, with the worst-case frames, fitting it; the static map in the manifest;
  - **(revision 4)** header +0x18: ml_char4_reg 0; ml_char4_bank 0 or 0xA2BE51AC; rewritten scripts keep their original value (§3.6);
  - **(revision 4)** the manifest records the code size, native count and placeholder handler used by the InitNativeTables identity check (§3.2).
- **S8 Fallback equivalence:** for every modified function, F′ with officer_mode() = false is V, and every in-place edit is on the allowed list. **(new)** Equivalence compares global stores, not only return values (§3.2 step 3); for yielding functions it is checked site by site with `officer_mode()` false. startup's registration patch is the one documented exception and is not checked (§3.6). (Not an Online gate any more, Q3: it guarantees vanilla behaviour whenever the officer is off or block 13 is not ready.) **(revision 4)** For programs with S-kind shadows, S8 also compares the image at thread start (statics after Reset with a sample argument block) between the vanilla layout and the rewritten program, with the static map applied. The per-function check cannot see a header change, so this is the check that covers the argument copy.

### 5.2 Runtime asserts and logs

- **R1 OOB logger** (part of the detour): an unserved index in an aware thread (3, 145 or another value, revision 4) is an error (script, ip, global or static, idx, count). An index 3 or 145 in a blind thread is a warning (finds today's officer crashes, for example appcontacts). Count summary on the debug pipe.
- **R2 Thread-death watchdog:** threads reaching state 2 (thread+0x18), with the op decoded at thread+0x1C, plus kills from the null-block path (0x91f42a). **(new)** Also logs every global block whose pointer (0x3ED15A8) changes within a session.
- **R3 Invariant checks:**
  - the vanilla-clean check before each save (also run inside the DO_AUTO_SAVE hook, which aborts on failure);
  - at every player change, the alias state must match the player model, and OFFICER_PED must equal the player while the officer is the logical player;
  - **(new)** after each program load, `natives[slot]` of every aware program equals the loader's officer_mode / pack_active handler; **(revision 4)** it also logs the identity check's result (code size, native count, placeholder handler; §3.2) and which copy each aware program came from, not just the slot value;
  - **(new)** no two slot files reference the same current nonce (slot index, §3.9), except a generation kept without rotation because no producer was loaded (§3.9 rule 6, revision 4).
- **R4 Coverage counters** per rule and per shadow (block 13 debug area). **(new)** Also counts executions of V-path invocations that started before an officer_mode() flag change (yielding functions, §3.4).
- **R5 Watches** on G2349[3], G2349[145].17, G143050[3], **G143050[145].17**, the owner-3 phone fields (+16, +23, +28 of every entry of G2349 and of G143050; the saved ones against their session-start image, revision 4), G21666, G4525199 and the saved character-valued globals.
- **R6 Readiness log (new):** for each thread, the time of the first get_cur4 call relative to block 13 creation and to ml_char4_reg's start, at boot, after each load and after an SP↔MP round trip. **(revision 3)** Also every detour serve refused because block B was null, and the time each aware program is placed vs the first execution of its officer_mode slot vs the slot write (U2).
- **R7 Block registration log (new, development):** a hook on 0x91bde0 logs any registration of block B while `0x3ED15A8[B]` is non-null (old and new pointer), and any foreign script registering block B.

### 5.3 Differential tests

- **D1 Protagonist regression:**
  - same save copy; vanilla build vs patched build; scripted route through `tools/gtactl.py` (window off-screen, no focus);
  - block 0 dumps with `gdump.py` at fixed points; diff against a noise baseline taken from ≥ 3 vanilla runs;
  - expected: no difference in protagonist data.
- **D2 Officer isolation:** M/F/T cash, SPn stats, bank logs, PP_INFO[0..2] and G143050 identical before and after an officer session that buys, pays, uses the ATM, dies, gets arrested and saves.
- **D3 Vanilla-load check:** each test save is loaded with the loader disabled. The story must load, G122220–2 must be in 0..2, and the models, cash and weapons must match the pre-session values. A vanilla re-save followed by a modded load must show what happens to the officer block and to SP3_* stats (U4, U6). **(revision 4)** Also: a mod-off evening (police mod disabled, two saves to one slot), then a modded reload: reports and records come back; a vanilla session (started without `-nobattleye`) that saves and autosaves, then a modded load: a restore is offered, not the legacy import. Scratch slots and the slot 02 backup rule apply (§5.4).
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
- **(new) start each Q4 activity as the officer (golf, range, tennis, darts, hunting, off-road races, street races; stunt plane races if §0.4 Q6 says yes; strip club, prostitute, car wash), once with the officer's account low (must refuse with the game's "broke" message) and once high (must start and charge his account, not M/F/T)**; **(revision 4)** log whether each player-speaker conversation starts, skips the player lines and lets the thread continue (U39);
- **(new) wheel attempts as the officer in the cinema, in a range lane, during an arrest, while ragdolling, in parachute free fall and during a stunt jump: each refused**;
- **(revision 4) the wheel must open next to an ATM, next to a vending machine and inside the golf launcher area (idle brains never block); "save now" as a taxi passenger (G114037 set) and during a strip-club dance must be refused; log IS_PLAYER_CONTROL_ON during a taxi ride**;
- **ATM and the phone's Internet bank page as the officer**, before and after the M2 fix;
- **phone contacts as the officer: confirm the first entry (appcontacts G24150)**;
- **(revision 4) call a taxi from the officer's phone (it must arrive and charge his account); call each officer contact, save, and diff G143050 against the session-start image**;
- death, arrest;
- Pay 'n' Spray, cash pickup;
- switches officer↔M/F/T in all directions, including short switches; **switching into an unavailable protagonist (must be refused)**;
- **left-behind deaths: kill the left-behind officer as a protagonist; hurt a left-behind protagonist as the officer**;
- menu, bed, quick and autosave as the officer; **deferred autosave written at the next switch** (revised: requests made as the officer produce no DO_AUTO_SAVE; after the switch exactly one autosave follows, through the loader's re-request; G102550 logged throughout; slot 02 rules above);
- **(new) open the save menu as the officer and press Back (no nonce change, no side-file commit, officer restored); quick save while driving at speed (vehicle stopped first or refused); "save now" in combat, ragdolling or wanted (refused with a notice, no swap)**;
- **(revision 4) save as the officer more than 200 m from where the protagonist was left, and inside the officer's apartment, so that player_controller has deleted him (step 2b); then load with the loader disabled and check position, outfit and weapons**;
- **(revision 4) hold the wheel key through a brokered save, including the menu closing (no switch may start); switch into the officer right after the re-requested DO_AUTO_SAVE, then load slot 02 with the loader disabled: player model and G143050[145].17 vanilla (slot 02 rules above)**;
- **(new) protagonist saves to two different slots after an officer session: different nonces; kill the game right after a save completes (write-ahead: the nonce's side file already exists and is complete)**;
- **(revision 4) disable the police mod, save the same slot twice, re-enable and load: reports and records come back; re-save a modded slot in a launch without `-nobattleye` (scratch profile copy), then load it with the loader: a restore is offered, not the legacy import**;
- load of each save type;
- **(new) officer session with non-default SP3 values, then load a save without SP3 values or without an officer block: SP3_TOTAL_CASH is back to its default**;
- loading an old save (legacy import with clock match and without);
- mission start as a protagonist right after officer play;
- **Rockstar Editor round trip, Director Mode attempt, benchmark, pause menu → Replay Mission, peyote pickup then switch, GTA Online and back, all as the officer**;
- **sitting in a Gauntlet as the officer while mission_triggerer_d runs (hardening condition)**, if a suitable save exists;
- **(revised in revision 4) benchmark and Replay Mission as the officer: refused with a notice, MISREP0000 mtime unchanged, screen and control back; the same as a protagonist after an officer session, then check officer_mode(), the alias and OFFICER_PED after the reload; Director Mode or Editor exit without a reload, then try to switch to the officer (must be refused)**;
- **(new) inject a police-mod fault while the officer is the player (fault teardown: back on the left-behind protagonist, alias off, warning, officer refused until restart)**;
- **(revision 4) inject a police-mod fault with no switchable protagonist, then get arrested (the loader's guard must hold: no softlock) and use an ATM (no Michael account); in a development build, an arrest in the degraded state**;
- **(revision 4) as the officer, with a range_modern lane active and a gunclub_shop menu open, inject a D11 (a test site outside the whitelist) and, separately, a police-mod fault: no thread death, no unserved hit on catalogued arrays (OOB logger), the range result in the SHOOTING_RANGE shadow, the session state as specified (degraded or torn-down, never torn-down → degraded)**;
- **(revision 4, development build) remove the whitelist entry of a nested array (a STORED_VEH_DATA inner base) and trigger it as the officer in release mode: no thread death, no write outside the sink (guard pages), one warning on the next tick**;
- **(new) boot: normal; ml_char4_reg missing from the pack; ml_char4_reg forced to fail S7. Each must reach story mode, the last two with a warning (startup's case 14 skips itself)**; **(revision 4) also ml_char4_reg present but corrupt (truncated page, wrong resource flags), recording whether it hangs until the frame budget runs out or raises a fatal error; the game killed during boot and relaunched, which must take the sentinel fallback and reach story mode (U40)**;
- one hour of free roam with the OOB logger.

### 5.5 Game updates

- Everything is derived at loader start-up from the user's own game files: no shipped patched binaries.
- Script rules are keyed by function fingerprints. Vanilla global indices are re-derived from save names and learn_ysc pairing. Engine RVAs come from relocate signatures (`relocate sig/match`), for:
  - the detour site 0x91ee3d, its six continuations and the opcode list;
  - the GLOBAL_U24 null-block path;
  - the model statics, the prefix table, StatId5;
  - CHANGE_PLAYER_PED's dispatch;
  - the load handler and 0x1c3bf90;
  - **(new)** InitNativeTables, the block registration 0x91bde0, the three block-free paths and the counter 0x3ED1BD0, the block tables 0x3ED15A8 / 0x3ED17A8, and the handlers of the hooked natives (CHANGE_PLAYER_PED, SET_PLAYER_MODEL, the four save natives, SET_DIRECTOR_MODE_AVAILABLE, and from revision 4 GET_STATUS_OF_MISSION_REPEAT_SAVE 0x1d615f0 and GET_TIME_SINCE_LAST_ARREST 0x1d20950);
  - **(revision 4)** Reset 0x921930's argument rules (argument count +0x20, thread+0xC8), which the argument-last static layout depends on; the placement guard 0x91bb64 and its table 0x3ED1AD0 (header rules); the element-size operand decoding of the six ARRAY handlers (D11 sink sizing).
- **(new)** The block ownership scan (§3.6) is re-run whenever an archive or pack CRC changes.
- **Degradation rules (degrade-with-warning policy):**

| Failure | Result |
|---|---|
| an aware script's fingerprints do not match | that script becomes blind; warning |
| the detour site or alias data are not found | officer = 145 everywhere (today's behaviour); warning |
| a catalogued array has no save-name match | its shadow is disabled and the scripts touching it become blind |
| a stats name cannot be resolved | that StatId5 entry is set to 0 |
| block 13 not registered at runtime | officer_mode() stays false (officer = 145); the detour's gate 0 refuses every serve; warning. (Reachable now: the startup patch no longer hangs boot when ml_char4_reg is missing.) |
| **(new)** block 13 claimed by another script and no free block in 13–15 / 21–63 | officer_mode() false; warning |
| **(new)** a foreign owner, or a bad magic or layout version in block B at runtime | officer_mode() false; warning |
| **(new)** startup or ml_char4_reg failing S1 or S7 at build time; ml_char4_reg missing at runtime | build time: neither is written (vanilla startup). Runtime: the patched startup's case 14 skips itself and boot completes. Either way officer_mode() false; warning |
| **(revision 4)** ml_char4_reg present but not loading, a fatal error at boot, or the game killed during boot | frame budget: bit 14 set, boot completes, officer_mode() false, warning. Fatal error or kill: the boot sentinel builds the next pack without the startup patch, ml_char4_reg and the aware rewrites (officer off) and names the cache key in a warning |
| **(revision 4)** the placed copy of an aware script fails the InitNativeTables identity check (another mod's copy is served) | no slot write; that program runs blind; warning naming the script |
| **(revision 4)** an aware program with arguments whose argument-last layout fails S7/S8 | no S kind for it: per-site stubs, otherwise the script is demoted to blind; warning |
| native table re-encoding fails S7 | the pack is not generated for that script; it stays vanilla |
| an unserved index in an aware thread in a release build (revised in revision 4) | D11: operand-sized scratch sink initialised from element 0 (above the cap: a logged kill); officer_mode() off from the next tick (session state "degraded" unless torn-down; alias, entry 3, hardening, arrest guard and broker stay on; catalogued shadows still served); warning |
| **(new)** a safety patch not applicable (another mod's copy of mission_triggerer_d or appcontacts, fingerprint mismatch) | keep the runtime workaround: mission_triggerer_d → hardening only while no mission_triggerer_d thread exists; appcontacts → today's crash stays, with a warning that the officer's contacts app may close; warning |
| **(new)** the police mod faults while the officer is the player | fault teardown (§3.11); torn-down until restart; warning |
| **(new)** an SDK pattern no longer resolves on the rewritten pack | the vanilla-image redirect (§3.4) is used; if that fails too, RunFunction reports "not found" and the caller's fallback runs (Pay → AddCash); warning |

- The cache is invalidated by the script CRC and the build hash. The per-update workflow: re-run the analyzer, review unmatched rules (estimate 1–3 days).

---

## 6. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| A save written with officer values in vanilla fields (PP_*_PED = 3, freemode player ped, G143050[3] or [145] changed, officer cash in SPn) | corrupts or confuses saves, including the 100% save | Computed identity (D2); the shadows never touch vanilla storage; the broker plus invariant check aborts a non-clean save; never use slot 03 in tests; slot 02 restore procedure; D3 vanilla-load test |
| A save path not intercepted (benchmark repeat save, Editor, a future path) | engine saves with the officer as the player | handler hooks on all four save natives and on the repeat-save status (§3.9); G9506 / G102550 watches; §3.11 teardowns; R3 assert; DO_AUTO_SAVE also refuses non-protagonists (0x1d613d0) as a second line of defence unless the alias is on, so the alias is removed first |
| Autosave swaps on every purchase or collectible | screen blackouts, streaming stalls | deferred, merged autosave at the next switch (§3.9); U22 measures cost |
| **(new)** The deferred autosave never happens (autosave_controller drops requests made as the officer before DO_AUTO_SAVE) | officer and protagonist progress since the last save not autosaved | loader records requests every frame and raises one vanilla request after the next switch (§3.9); U22; §5.4 scenario |
| **(new)** An autosave starts in the middle of the broker's swap (a +8 request skips the fade gate) | hardening values or officer state in a save | hardening restored before CHANGE_PLAYER_PED; DO_AUTO_SAVE passed through after step 3's restore (revision 4: a swallowed call is lost with a SAVE_OFF message) and step 7 only after IS_AUTO_SAVE_IN_PROGRESS is false; R3 check in the DO_AUTO_SAVE hook |
| **(new)** Two save slots share one nonce, or side files are lost by a crash or a truncated write | the wrong officer data in a slot, or none | pre-save step with a new nonce for every save; write-ahead immutable side files; per-slot index over SGTA5/MISREP; prune only unreferenced nonces; R3 check |
| **(new)** Explicit save as the officer cancelled, refused or made while moving | swap left half-done, nonce changed without a save, rolling vehicle | G9506 entry with save_anywhere's checks and mod-level gates; vehicle stopped and frozen; GET_STATUS_OF_MANUAL_SAVE poll with nonce rollback |
| Alias still active when a save loads | the loaded protagonist is not recognised; stats index wrong | forced removal in a hook on 0xd1ef80 / PostLoad; R3 assert at every player change |
| Alias side effects (0xB5F970 for the officer, freemode NPCs treated as Michael by 21 engine compares, pause header or blip colour) | odd behaviour or wrong UI | U29/U6 checks; the MP-slot alternative if they are bad |
| Stats falling to MP0/MP1 in memory | Online character data touched (flush behaviour unknown) | the alias keeps the index on k; never let it fall to 3/4 unless the MP-slot alternative is chosen deliberately; inert in Online (D9) |
| SP3_* stats in every save | stats-section overflow, vanilla load issues, leftovers after uninstall | U6/U18; non-saved SP3 stats + block-13 mirror if needed |
| Story content broken | lost progress | story scripts stay blind and unchanged; the data detour cannot change non-crashing executions; D1 differential runs |
| The 145 hardening kills mission_triggerer_d | story trigger lost until reload | hardening only while mission_triggerer_d is not running; safety patch in M1, relied on only when the served copy is ours (build record), otherwise the runtime condition stays (§3.2, §5.5); U35 |
| Thread deaths from missed index-3 sites | a controller stops (respawn_controller etc.); possible kill/relaunch loop (main@789315–789383 restarts respawn_controller while the player is injured or arrested) | dev: fail loud; release: D11 sink (operand-sized, initialised from element 0, §3.3) + officer_mode off on the next tick; OOB logger; S3; coverage |
| Reading block 13 before it exists | the reader is killed (null-block path) | officer_mode() gate; boot order (§3.6); R6; U28 |
| **(new)** Boot hang from the startup registration patch | game stays on the loading screen, or a fatal error, on every boot | unconditional, documented patch; case 14 skips itself when ml_char4_reg does not exist; startup and ml_char4_reg generated as one unit; boot tests (§5.4); **(revision 4)** frame budget for case 14; boot sentinel with a fallback pack (also for fatal errors and a corrupt ml_char4_reg); +0x18 header rules (§3.6); U40 |
| **(new)** Block 13 re-created from its initial values by a second placement of ml_char4_reg (SDK RunFunction, loader fallback) | officer data reset mid-session, silently | ml_char4_reg's main thread stays resident; SDK bank copy in ml_char4_bank (no block); fallback start only on a null block; ready flag + generation in officer_mode(); R7; U3/U38 |
| **(new)** Detour serves through a stale or null block address | writes into a freed buffer, or near address 0 (whole game crashes) | gate 0 (live pointer), live classification of bases, shadow from the live pointer, serve byte (§3.3); R2/R6; U28 |
| **(new)** officer_mode slot unpatched, or patched by pointer equality into a shared handler | an aware program runs a stub, or other natives get the loader's handler | InitNativeTables hook writes by slot index, only after the identity check (code size, native count, placeholder handler; revision 4); unique-handler 0-argument native that returns 0 in story mode; R3 check after each load; S7 |
| **(new)** Foreign owner of block 13 (future update, another pack) | silent data corruption | ownership scan at start-up; relocation to a free block; magic and layout check at runtime (§3.6, §5.5) |
| **(new)** Yielding function keeps its entry-time F′/V choice | officer features missing after a mid-session enable, or F′ running after a teardown | site guards in yielding functions; pack_active()/officer_mode() split (§3.4); R4 |
| **(new)** Static or local shadows shared by concurrent threads or recursion | one instance reads another's officer data, or stale data across thread restarts | shadow statics (per thread; argument block last, revision 4); frame growth for locals (§3.3); S3/S4 multi-instance report |
| **(new)** SDK pattern lookups match V or nothing in a rewritten program | Pay falls back to AddCash without bank history; the stocks, property and epsilon mods stop working | vanilla-image resolution + manifest redirect; named ENTER in copies; examples audit (§3.4) |
| Integrity checks around interpreter code (obfuscated .text), or 0x13D24E0 (walks all global blocks with sampling and an imported call; guess: integrity check) | crash or detection | M0 soak tests; fallback to per-site stubs (no .text patch) and a block-11 extension; SP only |
| Detour thunk corrupts interpreter state | random crashes | assembly fast path that preserves every live register; C++ only on the log path with full save and aligned rsp; dispatch by opcode (shared block); M0 soak |
| Entry 3 or owner-3 data really used in SP (MP placeholder, entry 96 field 28, appinternet owner-3 ops, palette G4525199) | officer data visible in odd places | no constant-3 indexing of 2349/118 entries exists (checked); runtime watch U5 on all three owner-3 fields; the saved copy is never written |
| Owner-domain crashes (G24150[3]) | phone app dies for the officer today | safety patch; R1 warnings in blind threads; U27 |
| Bit-3 collisions in per-character bitsets | wrong flags for protagonists | S6 report; guard or GROUP4 where bit 3 is used |
| Shared-global leakage (aware script writes 3; blind or saved readers) | crash or bad save | S4 audit; broker sanitizer for saved character-valued globals |
| Whole-array copies, snapshot restores and unrolled code skipping the shadow | officer data lost or stale | enumerated (6 struct copies, UNROLLED class); the officer is never in missions; Director Mode blocked |
| Another mod replaces an aware script | patch mismatch; **(revision 4)** a foreign copy placed instead of ours would get natives[slot] overwritten, or a write past its native table | apply rules to the active copy; otherwise blind plus warning; **(revision 4)** char4 pack ordered above the other generated packs, other overlays outranked or dropped at build time, runtime identity check before the slot write (§3.2, §3.4); U41 |
| Rewritten dual-mode scripts and startup running in GTA Online | ban / instability | cannot happen in practice: the loader and pack need `-nobattleye`, which keeps the game out of Online (Q3). Defence in depth: officer_mode() false in network games; teardown at the first transition signal (U31) |
| Existing SDK mods see character 3 | out-of-bounds writes in the game process | D10: old API unchanged; opt-in v2 with a version check; audit examples/* |
| Officer identity lost when the game replaces the player ped | officer silently becomes 145, default looks | OFFICER_PED re-bind and look re-apply in the wrapper; per-frame check (R3); teardown for protagonist-model paths |
| Officer used to get around story locks | story breaks | Phase A availability rules (§3.10) |
| Save size limit exceeded by the officer block, or DLC block cannot hold arrays/structs | save failure | U4/U18 probes; keep the in-save core small (≈3.6k slots); fallback to header + side files |
| An unclaimed DLCScriptSaveData block dropped by a vanilla re-save (also by a session started without `-nobattleye`, Q3) | officer data lost for that save | **(revised in revision 4)** the write-ahead side file mirrors the saved block and the SP3 mirror; the slot index records {nonce, mtime, size}, so a block-less load of a recorded slot offers a restore instead of the legacy import (needs the loaded slot's identity, U36); ml_char4_reg and the SP3 stats file stay mounted whenever the loader runs, even with the police mod absent or off (§3.6, §3.9 rules 6–9) |
| Performance of F′ / the forwarder hop / extended loops | frame time | OOB path is rare (index 3 only); measure in shop menus (wardrobe hits 33,981 COMPONENTS sites) |
| Another freemode-model player treated as the officer | identity leak | officer identified by ped handle plus officer_mode(), not by model |
| An entry-145 crash today (arrest as the officer) | respawn_controller dies; black screen with the loading icon for more than 75 s (phase0.md §53.16) | arrest guard, from revision 4 a loader service active whenever respawn_controller cannot serve the officer as 3, also after a mod fault (§3.2); respawn_controller aware in M2 |
| **(new)** Q4 activity refused by its blind launcher, or gated on protagonist cash | activity unusable for the officer (Q4 broken), or "broke" while he has money | launchers aware (§3.1; controller_towing and towing stay blind, §0.4 Q8); U9 maps every launch path; §5.4 scenarios |
| **(new)** Officer switch started in a state the game's wheel refuses (cinema, range lane, arrest, ragdoll, free fall, stunt jump) | broken activity, arrest or parachute state | "may switch now" gate mirroring selector@24088 (§3.10); §5.4 scenarios |
| **(new)** Police mod faults while the officer is the player | officer session armed with nothing to drive it (Director Mode block, wheel, look re-apply) | fault teardown by the loader; char4-critical overrides in the loader; faulted mods' override callbacks skipped (§3.11) |
| **(new)** Officer-off latches never cleared, or the officer re-entered after a teardown | officer in an undefined state after benchmark, replay, Editor or Online return | session states; latches cleared only in the load hook (U36); torn-down refuses switching (§3.2) |
| **(new)** SP3_* values carried into a save that has none | another save shows the officer's SP3 cash or stats | SP3 reset at every load before saved or mirrored values are applied (§3.7); U6 |
| **(revision 4)** Shadow statics overwrite the argument block | every aware program that takes arguments breaks for every character (atm_trigger loses its ATM, gunclub_shop its shop index) | argument block kept last, operands renumbered, static map (§3.3 S row); S7 layout check; S8 thread-start image; v1: S kind only without arguments; U2 |
| **(revision 4)** Left-behind protagonist deleted by player_controller after the Phase A hand-over | an officer save has no ped to swap to; a fault teardown cannot swap | step 2b materialises him (switch request preferred, CREATE_PED from PP_INFO as the alternative); the fault teardown uses the switch request (§3.9, §3.11); U15, U22 |
| **(revision 4)** Saves made with the police mod off, or by a session without the loader | officer side files orphaned; the slot falls to the legacy import | producer-aware rotation, parent chain, block mirror, slot index with mtime and size (§3.9 rules 6–9); U36 |
| **(revision 4)** D11 sink too small, zero-filled or nested | writes past the sink; nested inner arrays fail again | sinks in loader memory, sized from the operand, initialised from element 0, nested with a cap, guard pages; logging deferred to OnTick (§3.3) |
| **(revision 4)** The serve byte cleared under threads that cached 3 | thread kill (dev) or lost purchases and medals (release) after a D11 or teardown | serve byte kept until block free, invalid block, Online signal or load (§3.3 gate 4); torn-down final until the load hook (§3.2) |
| **(revision 4)** Repeat save (Replay Mission, benchmark) as the officer | the caller polls in the same frame, so a swap cannot hold it; a MISREP with the officer's position or model | refusal through the status hook (§3.11); U25 |
| **(revision 4)** A switch during a save, or a save during a switch | a protagonist switch in the middle of the broker's sequence; officer values in slot 02 | "save busy" exclusion (§3.9); U22 |
| **(revision 4)** Player-speaker conversations in aware scripts | a protagonist voice for the officer, or a stalled activity | §3.4 policy; U39 |
| **(revision 4)** Officer contacts mirrored into the saved G143050 by appcontacts | the save is no longer vanilla | value 1 only, never contact 130; owner-3 image in the vanilla-clean invariant (§3.9); R5 |

---

## 7. Unknowns and the experiments that settle them

| ID | Unknown | Experiment | Decides | When |
|---|---|---|---|---|
| U1 | Is a detour at 0x91ee3d (5-byte jmp to a near thunk, opcode dispatch, jumps back into the six continuations) stable, and free of integrity reactions? | Phase 1: log-only detour (including blind-thread warnings); play as the officer and as protagonists for 60 min over 3–4 launches. Phase 2: serve G62254[3] from block 13 for one aware test program; read and write it from that program; confirm a blind thread is still killed. | D4 primary vs the stub fallback | M0 |
| U2 | Can replacement .ysc code grow (appended pages, F′ + V, in-place forwarder, retargeted CALL, appended native with re-encoded table)? | blip_controller (already proven to load from a pack): append a page, move one function to F′/V with a forwarder, add one native entry, add a function that forces a NOP-padded page boundary; confirm the code size through GetScriptCode, native resolution, and that no thread dies. **(new)** Serve the appended entry from the InitNativeTables hook and log the time a program is placed, the first execution of its officer_mode slot and the time the slot is written; at boot, after a load, and for object-brain starts (atm_trigger next to an ATM); ≥ 3 launches. **(revision 4)** Argument-last static layout: a rewritten atm_trigger with a K-slot shadow, started as an object brain next to an ATM, must hold the ATM object in static 649+K (DOES_ENTITY_EXIST true at @230); a rewritten gunclub_shop entered as Michael in two different Ammu-Nations must hold different shop indices in static 1407+K; ≥ 3 launches each. Identity check: a placement of an unmodified copy must get no slot write | rewrite technique; officer_mode serving; S kind for programs with arguments | M0 |
| U3 | Does a script-registered global block 13 work: reset per session, survives switches and mission replay, tolerated by 0x13D24E0? | ml_char4_reg with header (13<<18)\|N, started from the patched startup list; read and write G3407872+k from scripts and ScriptGlobal; 1-hour soak. **(new)** With ml_char4_reg's resident main thread: check `ProgramByHash(joaat("ml_char4_reg"))` every minute; compare `0x3ED15A8[13]` and a marker slot before and after a no-op RunFunction into ml_char4_bank (must not change) (U38) | storage location (fallback: extend block 11 / sp_dlc_registration) | M0 |
| U4 | Own save block: round trip; **arrays and structs (START_SAVE_ARRAY_WITH_SIZE / START_SAVE_STRUCT_WITH_SIZE) inside a DLC block; a never-seen script name (0xd2eba0 special-cases two hashes)**; a vanilla load of such a save; a vanilla re-save (kept or dropped?); quick save and MISREP coverage; old saves keep the defaults | START_SAVE_DATA + REGISTER_INT64 "ML4_TEST_i" + one array + one struct; save to a spare slot; reload; load with the loader disabled; re-save in vanilla; reload with the loader | D7, side-file need | M0 |
| U5 | Is entry 3 or any owner-3 field used by SP code while protagonists are played? | hardware read and write watches on G2349[3] .0/.3/.17, G118[3], G143050[3], G129050[3], owner-3 fields +16/+23/+28 of all entries (entry 96 especially), during play as M/F/T (phone, contacts, texts, the Internet app, a mission) | D1 confirmation (fallback: officer phone in a separate block area) | M0 |
| U6 | Engine alias: does the HUD refresh on a 0→0 index change? Are SP3_* stats from a type-0x23 DLC file registered and saved by the stats section, and what do they do to a vanilla load and re-save? Values of the [0x47AA0D0]/[0x47AA0DC] flags in SP. **Alias leftovers: pause header name and portrait, minimap blip colour, default radio station.** | register SP3_TOTAL_CASH in a test stats file; alias k = 0; switch officer↔Michael and officer↔Trevor; check HUD, pickups, respray; screenshots with the alias on and off; save, reload, STAT_GET_INT; load with the loader disabled, re-save, compare. **(new)** An officer session with non-default SP3 values, then load a save with no SP3 values or no officer block and read SP3_TOTAL_CASH (expected: default, after the load-hook reset) | D6, k choice, SP3 save category, stat mirror need, SP3 reset | M0 |
| U7 | Tracer bullet: what officer-aware shop_controller + gunclub_shop touch | apply the identity rewrite to these two only (with the shop-type GUARD); detour in log mode; enter Ammu-Nation as the officer; list arrays; then serve them. **(new)** Needs the §0.4 Q5 decision first: gunclub_shop@95342 → @96206 reads SHOOTING_RANGE[c] during the tracer, so its shadow (saved or non-saved) must exist; log which Ammu-Nation items depend on the range medal values (unlock use is a guess) | M2 sizing; catalog validation | M0 |
| U8 | Arrest as the officer today | let police arrest the officer; EnumScripts afterwards (prediction: respawn_controller gone) | Phase A guard urgency | M0 |
| U9 | Service payment results with account "none" (refused vs free), for **all** paid free-roam scripts: taxi, car wash, vending, cable car, fairground, cinema, telescope, strip club and drinks, prostitutes, golf, tennis, darts, range, hunting, races, vehicle_gen_controller (not towing: §0.4 Q8). **(extended)** Per activity, the **full launch path** (world point → launcher → can-launch routine → cash gate → activity script) and every character gate on it: launcher_golf/range/tennis/darts/hunting_ambient/offroadracing/racing, launcher_stunts (§0.4 Q6), launcher_carwash (is_playable copy @4876, CWASH_NOMONEY @9037; gate unverified), how stripclub and pb_prostitute are entered, and **(revision 4)** taxilauncher's phone path (@32216 → @35956(131) → @35804). controller_towing and towing are out of scope (§0.4 Q8 answer) | desk work first: trace each path in the disassembly. Then in game: with 2349[145].17 = 4 the officer approaches each activity; log refusals (launcher threads, OOB logger, EnumScripts); read the cash and logs of M/F/T; list every payment site per script | 145 hardening side effects until M2; the full list of scripts made aware in M2 (activities **and their launchers**, user decision Q4) | M0 |
| U10 | Is G2349 (entry 145/3) rebuilt from G143050 at load or init? | write markers, load a save, read them back | re-apply frequency | M0 |
| U11 | Bit 3 free in Player_Char_Bitset, FLOW_HELP, G97513, G93274[].11? | S6 static report plus setting bit 3 on a queued comm or help entry and observing | logic decisions | M1 |
| U12 | Character type of the 254 unproven count-3 arrays; local arrays; pointer-parameter bases; unrolled constant-index sites for all catalogued arrays | C++ analyzer with type inference plus save-name review | final catalog | M1 |
| U13 | Roles of snapshots G105547/G108531; G102432, G100622, G34212 +0/+12 meanings | dumps before a mission, after a fail, after a replay; diffs | catalog decisions | M1 |
| U14 | The selector slot-3 "windfall" path (natives 0x317989BB72A2EBFB, 0xA6E586C3BD772B7B; selector's freemode SET_PLAYER_MODEL @520435/@520752) | OverrideScriptNative logging on selector | Phase B design | M3 |
| U15 | Does player_controller adopt a protagonist handed over by the mod? Does the game keep the officer in 99964[3] when switching away? **(revision 4)** When does it delete the handed-over protagonist (beyond 200 m with collision, out of sight beyond 40 m, @254246), and does 99964+7 = c pin him (keep reason 19, @254296; meaning unknown)? | hand-over test (switching survey); **(revision 4)** save as the officer more than 200 m from the drop-off and inside the officer's apartment (step 2b runs), then load with the loader disabled and check position, outfit and weapons; set 99964+7 and repeat; ≥ 3 launches | Phase A/B design; step 2b | M1b |
| U16 | The officer's TC modifier, short switches, relationship group after respawn | the experiments listed in the switching survey | Phase A details | M1b |
| U17 | Freemode Pedtype and ability type; helmet behaviour under the alias | GET_PED_TYPE; peds.meta extraction; aircraft and quad bike | M7, helmet handling | M2 |
| U18 | Maximum script save size; **stats-section capacity with SP3_* stats** | 1k / 8k / 32k / 64k-member probe blocks; saves with N SP3 stats, file sizes compared | in-save core size; SP3 save category | M0 |
| U19 | Missing scripts and dlcpacks versions | extractall: the three scripts plus every dlcpacks script_rel; re-run the scans (including acct2.py) | completeness | M0 (extraction), M1 (scan) |
| U20 | StatId5 names (118 unresolved) and the 690 triple names | extract the stats XMLs; match joaat("SPk_" + name) | alias and triple rules | M0 |
| U21 | Does the engine credit SPn stats while the officer is played *without* the alias (today's leakage)? | snapshot all SPn stats before and after 10 min of officer play | urgency of the alias | M0 |
| U22 | **(rewritten)** Cost of a broker swap (protagonist near / far). Autosave: requests made as the officer produce no DO_AUTO_SAVE (expected: consumed at autosave_controller@368); does a per-frame poll see every request (≥ 3 frames expected); after the switch, does exactly one autosave follow the loader's re-request? Does SET_SAVE_MENU_ACTIVE show its menu while the screen is faded out? Does save_anywhere's pre-save @789 run as the officer without a thread death (G9506 entry)? **(revision 4)** Cost of step 2b (switch request vs CREATE_PED) when the protagonist was despawned. (a) Hold the wheel key through a brokered save, including the menu closing: no switch may start. (b) Switch into the officer right after the re-requested DO_AUTO_SAVE, then load slot 02 with the loader disabled and check the player model and G143050[145].17: does the engine capture the save data at the call or later? | timed swaps over 3 launches; log G102550 +1/+4/+8/+10 and the autosave_controller thread lifetime after an officer purchase, then the re-request at the next switch, over 3 launches, following the slot 02 backup rule (§5.4); the DO_AUTO_SAVE swallow hook stays only as a guard; open the save menu behind a fade; set G9506 as the officer with the OOB logger on, and check that each save_anywhere refusal state refuses | autosave policy details; fade handling in the swap sequence; "save now" path; step 2b variant; "save busy" rules | M0 |
| U23 | Rockstar Editor round trip as the officer: what state comes back (reload? stale OFFICER_PED? alias?) | enter the Editor as the officer, return, dump state | §3.11 Editor teardown | M0 |
| U24 | Does forcing SET_DIRECTOR_MODE_AVAILABLE(0) in main block Director Mode, and is the re-issue on officer exit correct? | override test; try launching from the Editor | Director Mode block | M0 |
| U25 | **(revised in revision 4)** Repeat saves as the officer. Broker off: does QUEUE_MISSION_REPEAT_SAVE as the officer give status 2 (engine refusal) or write a MISREP with the freemode model? What does GET_STATUS_OF_MISSION_REPEAT_SAVE return when idle? With the refusal hooks: is the screen faded or control off after benchmark's TERMINATE_THIS_THREAD? | in a scratch copy of the profile folder: Replay Mission and benchmark as the officer, broker off, then with the hooks (notice, MISREP0000 mtime unchanged, screen and control back); ≥ 3 launches | §3.11 refusal; whether the loader must fade in after the benchmark refusal; the benchmark hold alternative | M0 (broker off), M2 (hooks) |
| U26 | Does PLAYER_PED_ID change after SET_PLAYER_MODEL (and after the mod's gender change)? | log the handle before and after | OFFICER_PED re-bind | M0 |
| U27 | Today's phone as the officer: does appcontacts die on G24150[3]? | confirm the first contact entry as the officer; EnumScripts | safety-patch urgency | M0 |
| U28 | When is block 13 created (program load vs first run), and is it present after a load and after an SP↔MP round trip before any aware thread runs? **(new)** Are blocks freed and re-created on a save load (does the counter 0x3ED1BD0 reach 0)? | R6 readiness log over boots and loads. **(new)** Compare `0x3ED15A8[0]`, `[13]` and `[17]` before and after loading another save, over 3 launches; R7 log | officer_mode() readiness rules; detour address handling | M0 |
| U29 | CHANGE_PLAYER_PED under the alias: effect of 0xB5F970(…, 0) for the officer; a freemode NPC nearby while the alias is on | switch to the officer with the alias on; compare ped flags, ability state and audio with the alias off | k choice | M0 |
| U30 | Left-behind deaths: the officer killed by a protagonist; a left-behind protagonist killed by the officer | kill each kind; read PP_INFO health, AVAILABLE; watch for script deaths | left-behind policy | M1b |
| U31 | A refused Online attempt as the officer (wheel MP slot if still reachable, pause-menu tab 42, invites): do the transition scripts start, which signal fires first (G80362, maintransition), and does story mode come back sane? | try each entry point as the officer with `-nobattleye`; transition timing log; EnumScripts and invariant check afterwards | D9 teardown trigger | M2 |
| U32 | What character-taking natives do with 3 (SETUP_SHOP_PED_APPAREL_QUERY_TU, GET_NUM_TATTOO_SHOP_DLC_ITEMS, _GET_STAT_HASH_FOR_CHARACTER_STAT, packed stats) | call each with 0..4 from a test script; compare results | NATIVE-ARG decisions | M1 |
| U33 | Peyote picked up as the officer: does a trip start after the next switch? | pick up, switch, observe animal_controller state | peyote policy | M1b |
| U34 | Selector per-slot state (G24447+20..23) meaning; story locks on protagonists; prologue. **(new)** Are +20..23 refreshed while a non-protagonist (the officer) is the player, e.g. greyed while he is wanted? Do the selector's block flags (G24447+5/+6/+11/+12/+17) cover every activity that must block the wheel? | dumps of G24447 and G120247 in locked and unlocked states, and as the officer (wanted and not; in the cinema, a range lane, a stunt jump); **(revision 4)** as the officer, list the activity phases the chain misses (private dance, open shop menu; taxi ride with IS_PLAYER_CONTROL_ON logged) | Phase A availability; "may switch now" gate and its per-script engaged states | M1b |
| U35 | Is mission_triggerer_d running in the 100% save, and when? | EnumScripts over a session | hardening condition | M0 |
| U36 | **(new)** Does the load hook (0xd1ef80, or PostLoad 0x222ba40) fire for a normal load, a MISREP load (benchmark, pause menu → Replay Mission) and SHUTDOWN_AND_LOAD_MOST_RECENT_SAVE (Online return)? | log hook entries in each path, ≥ 3 launches; then run benchmark and Replay Mission as a protagonist after an officer session (as the officer they are refused, §3.11) and check officer_mode(), the alias and OFFICER_PED after the reload. **(revision 4)** Also log which slot file (or MISREP) the handler loads: its argument, or the last SGTA5 file read | where the session latches are cleared and the SP3 reset runs (§3.2, §3.7); the loaded slot's identity for the restore offer (§3.9 rule 9) | M0 |
| U37 | **(new)** Does the game (or the cloud) ever load SGTA5xxxx.bak automatically, e.g. when the main file is corrupt? | in a scratch copy of the profile folder, corrupt a test slot's main file and load; never slot 03 | side-file fallback rules (§3.9) | M2 |
| U38 | **(new)** Does a running thread keep its program resident, and when is a program evicted after SET_SCRIPT_AS_NO_LONGER_NEEDED (startup@143098 and RunScriptFunction, `mods.cpp:984`, both do it)? | `ProgramByHash` every minute for ml_char4_reg with its resident thread and for a test build whose main exits; a no-op RunFunction before and after eviction; compare `0x3ED15A8[13]` and a marker slot | resident-thread rule; ml_char4_bank split (§3.6) | M0 |
| U39 | **(revision 4)** What does a conversation do when the player speaker is not registered, or a line root label is empty (taxilauncher statics 284/285 for c = 3)? | as the officer today (these blind scripts already see 145): prostitute negotiation and a strip-club dance; log whether the conversation starts, whether the player lines are skipped and whether the thread waits; a test conversation with an empty root label; ≥ 3 launches | §3.4 player-speaker policy (DEFAULT vs skipping only the player lines) | M0 |
| U40 | **(revision 4)** What does a present but unloadable ml_char4_reg do at boot: hang in startup's loop, or raise a fatal error (as ERR_GEN_ZLIB_1 does)? Does the boot sentinel catch a kill during boot? | scratch builds with a truncated page and with wrong resource flags; a game kill during boot, then a relaunch; ≥ 3 launches each | frame budget N; sentinel rules (§3.6) | M0 |
| U41 | **(revision 4)** Which overlay wins when two generated packs (both `<order value="999" />`, overlay ml_stream.rpf) replace the same script, and does a higher order on the char4 pack always win? | two test packs replacing blip_controller with marked copies, with equal and with different orders; log which copy the InitNativeTables identity check sees | char4 pack order; build-time overlay handling (§3.4) | M1 |

---

## 8. Phased plan

Effort is in engineer-weeks for one experienced engineer, with ranges. M1b runs in parallel.

**M0: De-risk (4–5 weeks).** Starts with the biggest unknowns.
- Work:
  - extraction (U19, U20);
  - U1 detour (log, then serve; opcode dispatch, thread gating, gate 0 on the live block pointer);
  - U2 code growth (including native re-encoding and the InitNativeTables slot serving);
  - U3 block 13, U28 readiness and block lifetime, U38 program residency; boot tests of the startup patch (normal, ml_char4_reg missing, ml_char4_reg failing S7; ≥ 3 launches each);
  - U36 load-hook coverage;
  - U4 save block (arrays, structs, script name) and vanilla compatibility, U18 size and stats section;
  - U5 entry-3 and owner-3 watches;
  - U6 alias with SP3_TOTAL_CASH and leftovers, U29 alias dispatch;
  - U7 tracer bullet (Ammu-Nation as officer = 3; needs the §0.4 Q5 answer first);
  - U8 arrest, U9 services and launch paths (full list), U10 rebuild, U21 stat leakage;
  - U22 autosave cost, U23 Editor, U24 Director Mode, U26 handle change, U27 contacts crash, U35 mission_triggerer_d;
  - **(revision 4)** U39 player-speaker conversations, U40 boot failures of ml_char4_reg; the U2 argument-layout probe; the U25 broker-off repeat-save probe.
- Immediately shippable safety fixes (3–4 days inside M0; loader and mod only, no toolchain):
  - G2349[145].17 := 4 while the officer is the player **and no mission_triggerer_d thread exists**;
  - G143050[145].17 := 4 while the officer is the player, restored by the mod's current save routine and on exit;
  - arrest guard, **as a loader handler hook** on GET_TIME_SINCE_LAST_ARREST (0x1d20950) filtered to respawn_controller, with the post-arrest fade-in and control release (revision 4; today the mod's override, guard.cpp:67-72);
  - SET_DIRECTOR_MODE_AVAILABLE override in main (moved into the loader's char4 service as a handler hook in M2);
  - Editor teardown (restore the hardening values);
  - **(new)** autosave record and re-request after the next switch (loader only, §3.9): today the officer's autosave requests are simply dropped;
  - **(new)** "may switch now" gate in OwnWheel/SwitchFrom/SwitchTo, and `IS_CONTROL_PRESSED(0,19)` only (mod only, §3.10); **(revision 4)** no blanket "aware thread running" term, the same predicate for "save now", 存檔 and the bedside save, and the loader's "save busy" exclusion (§3.9);
  - **(new)** "save now", 存檔 and the bedside save through G9506 with the mod-level gates; vehicle stopped and frozen before the swap (mod only, §3.9).
- Done when:
  - each experiment has a result over ≥ 3 launches;
  - a written go/no-go exists for D1 (value 3), D4 (detour vs stubs), D6 (alias k = 0 vs MP slot; SP3 save category), D7 (own save block vs side file; autosave policy);
  - the tracer bullet buys a weapon as the officer with no thread deaths, Trevor's phone, calls and blips are unchanged, and the clothes shop does not open for the officer.

**M1: Toolchain and catalog (9–11 weeks; +1 in revision 3 for the slot-serving hook, statics and frame growth, site guards, the ownership scan and the pattern redirect; +1 in revision 4 for the argument-last statics, the placement identity check, the D11 sinks and the boot sentinel; estimate).**
- Work:
  - C++ script library: CFG, stack analysis, assembler with page and branch rules, resource writer for code, statics, globals pages **and native table (re-encoding, append)**;
  - fingerprint library shared with learn_ysc / relocate;
  - analyzer: character-type and owner-type inference, site enumeration (including NATIVE-ARG, UNROLLED, SET_PLAYER_MODEL), S1–S8 checks, residual scan;
  - catalog v1 (all per-character arrays with rule kinds and save names);
  - block 13 layout generator (magic, layout version, generation, ready flag) and the ml_char4_reg / ml_char4_bank generators; startup patch generated as one unit with ml_char4_reg;
  - block ownership scan over game archives, dlcpacks and mounted mod packs; relocation to a free block;
  - officer_mode() / pack_active() entries served by the InitNativeTables hook (manifest of slot indices) and the F′/V/forwarder scheme; site guards for yielding functions; load_cur4;
  - static shadows by statics growth, local arrays by frame growth;
  - production detour (assembly thunk, opcode dispatch, gate 0 on the live block pointer, thread gating, serve byte, live base classification, whitelist, logger, D11 release policy); coverage counters;
  - safety patches: appcontacts G24150, mission_triggerer_d account read; build record that tells whether the served copy is ours;
  - RunScriptFunction pattern resolution against the vanilla image with the manifest redirect;
  - rule-file format and build pipeline into the generated pack, with cache;
  - **(revision 4)** argument-last S-kind layout (static renumbering, static map in the manifest, SDK Static() translation); InitNativeTables identity check, the char4 pack's overlay order and the build-time overlay handling (U41); D11 sinks in loader memory (operand-sized, initialised from element 0, nested, capped, guard pages; OnTick logging); boot sentinel, case-14 frame budget and the +0x18 header rules.
- Done when:
  - the pipeline regenerates and verifies the identity rewrite for every candidate script;
  - D1 differential runs (≥ 3 launches) with all candidate scripts rewritten but the officer inactive show zero protagonist data differences;
  - S8 passes for every rewritten script;
  - the catalog covers 100% of U7's logged arrays;
  - the boot tests pass (normal, ml_char4_reg missing, ml_char4_reg failing S7, **ml_char4_reg corrupt, game killed during boot**; ≥ 3 launches each), and R3 shows every officer_mode slot written after each load, with the identity check passing.

**M1b: Police-mod switching hygiene, Phase A (2–3 weeks, parallel).**
- Work:
  - everything listed in §3.10 Phase A, including the availability rules (U34), the "may switch now" gate, the left-behind injury policy (U30) and peyote (U33);
  - OFFICER_PED re-bind and look re-apply on player-ped replacement;
  - session states and `OfficerAvailable()` (torn-down refuses SwitchTo/Restore and greys the slot);
  - per-save officer records through a nonce side file (an interim persist.cpp);
  - **(revision 4)** step 2b: materialise a despawned protagonist for the officer's save (switch request), and the same request in the fault teardown.
- Done when:
  - switching officer↔each protagonist ×10 per launch, ×3 launches leaves correct PP_INFO for the protagonist that was left;
  - player_controller owns and despawns the protagonist, and an officer save still works after he was despawned (step 2b);
  - no stale weapons or position on switch-back;
  - unavailable protagonists are refused;
  - wheel attempts in the §5.4 refused states (cinema, range lane, arrest, ragdoll, free fall, stunt jump) are refused;
  - the arrest guard (loader) holds, also after an injected police-mod fault.

**M2: Economy slice (8–10 weeks; +1 in revision 3 for the launchers, side-file generations and the fault teardown; +1 in revision 4 for the repeat-save refusal, the side-file carry-forward and block mirror, and the service contacts; estimate).**
- Work:
  - aware: shop_controller (shop-type GUARD), gunclub_shop, respawn_controller, blip_controller (shop blips, same filter), **atm_trigger**, and every paid free-roam script listed by U9 (Q4), **together with its launch path: launcher_golf, launcher_range, launcher_tennis, launcher_darts, launcher_hunting_ambient, launcher_offroadracing, launcher_racing, taxilauncher (and launcher_stunts / launcher_carwash per §0.4 Q6 and U9); not controller_towing and towing (§0.4 Q8)**; player-speaker sites per §3.4 (U39); SHOOTING_RANGE and the other Q4 record shadows (§0.4 Q5);
  - account 3 (wrapper edits, bank function manual CASE3 and UNROLLED sync, log shadow);
  - cash shadow and sync; the hash-triple rule;
  - alias k plus the SP3 stats file (save category from U6/U18);
  - save broker v1 (handler hooks; pre-save step with a new nonce for every save; menu, bed, quick save, deferred autosave re-request; **repeat-save refusal through the status hook (Replay Mission, benchmark)**; DO_AUTO_SAVE passed through after step 3; GET_STATUS_OF_MANUAL_SAVE poll with nonce rollback; "save busy"; step 2b) plus the officer save block v1 (header, cash mirror, weapons, health, armour, last-known);
  - side files: write-ahead immutable generations, the slot-file index over SGTA5xxxx and MISREP (story.cpp extension), prune rule, temporary-file-and-rename for ml::save (U37); **(revision 4)** producer-aware rotation, parent chain, block mirror, {nonce, mtime, size} per slot and the restore offer on a block-less load (U36 slot identity);
  - SP3_* reset in the load hook; session latches cleared there (U36);
  - §3.11 teardowns (Editor, Director Mode, Online transition, police-mod fault; benchmark and replay refused as the officer); the char4-critical overrides (Director Mode block, broker, arrest guard) in the loader's char4 service; faulted mods' override callbacks skipped;
  - refused-Online check and transition teardown (D9, U31);
  - legacy import (decided rule; only for slots never recorded with a nonce);
  - **(revision 4)** the officer's service contacts (at least taxi 131) in the owner-3 runtime column, value 1, never contact 130; the G143050 owner-3 image in the vanilla-clean invariant;
  - SDK v2 (D10): opt-in slot API, ml::save slot 3 and events, officer Pay through ml_char4_bank's aware bank function; audit examples/*, including their RunFunction pattern lookups.
- Done when, over ≥ 3 launches each:
  - an officer purchase at Ammu-Nation is charged to the officer's account, the HUD shows SP3 cash, and the unlocks persist;
  - the ATM shows the officer's account and log;
  - the chosen services charge the officer;
  - each Q4 activity starts as the officer and charges his account; with his account low it refuses with the game's own message;
  - **(revision 4)** a taxi called from the officer's phone arrives and charges him; Replay Mission and benchmark as the officer are refused cleanly;
  - the hospital fee comes from the officer;
  - protagonist saves in two slots carry different nonces, and each slot's side file survives a kill right after the save;
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
  - the remaining officer contacts in the owner-3 column (runtime only, re-applied after load; value 1, never contact 130; the service contacts came in M2);
  - settings, ringtone and palette (G4525199);
  - stock GROUP4 and our browser bank and stock pages through CharacterElement;
  - decide whether appinternet (bank page G143050 read, owner-3 contact ops) / stock_controller become aware or are fully replaced by our browser. Making either aware requires the RunScriptFunction pattern redirect (§3.4), because ml::game::Pay and the stocks, property and epsilon mods look up appinternet functions by pattern.
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
  - an update drill: re-derive all rules, RVAs and globals against a different build or the legacy-converted set, verifying degrade-with-warning paths; **(new)** inject a foreign owner of block 13 (a test pack whose script header claims it: relocation or officer off with a warning), a bad magic at runtime, a non-applicable safety patch (another copy of mission_triggerer_d), and a police-mod fault; **(revision 4)** a test pack that overlays gunclub_shop or shop_controller with a vanilla copy ordered above ours (expected: no slot write, a warning, the script blind, no thread death), and a corrupt ml_char4_reg (sentinel fallback);
  - documentation in research/phase0.md;
  - release.
- Done when:
  - all static checks are green;
  - the D1/D2/D3/D4 suites pass on 4/4 launches;
  - an injected rule mismatch demotes exactly one script, with a visible warning.

**Total:** about 39–51 engineer-weeks for M0–M6 plus M8 (M1b in parallel, M7 optional); revision 3 estimated 37–49, revision 2 35–47.

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
- **Revision 3 checks:**
  - native → handler map (handler sharing, S7): `D:\Projects\gtav-research\char4\exedim\natmap.txt`
  - the seven surveys (including the technique survey's statics/frame-growth proposal) and the first-round critiques: `D:\Projects\gtav-research\char4\wf\surveys.json`, `D:\Projects\gtav-research\char4\wf\critiques.json`
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

### Review round 2: parts rejected or corrected

No round-2 item was rejected outright; all 22 are merged (§0.3). The parts below were rejected or corrected during verification.

| Item (part) | Decision | Reason / evidence |
|---|---|---|
| officer_mode() through OverrideScriptNative could hit the int3 default handler for an unresolved native | **Rejected (already excluded)** | The facts are right (InitNativeTables 0x91bf20 `lea r10, 0x50CB60; cmovz rdx, r10`, bytes CC C3; 0x91c380 @0x91d52e ignores the return value), but S7 required a native that resolves, and ApiOverrideScriptNative refuses a hash FindHandler cannot resolve (`mods.cpp:527-531` **(checked)**, `natives.cpp:134-146`). The other three sub-claims (pool, shared handlers, late patching) were merged (§0.3 item 7). |
| "Records NONE" inside an aware Q4 script "has no rule kind" | **Partly rejected** | E rules could already point at the non-saved shadow area (§3.3), and S2/S3 demote a script loudly at build time; G134146 is a directly resolvable base, so a D11 release-path outcome is unlikely. The real gap was an inconsistent decision ("NONE" for an array aware scripts index with the character), fixed in §0.3 item 5. |
| save_anywhere "shows AM_H_SAVEAONM" on refusal | **Corrected** | It shows it only when G44938 ∉ {15, 5} (@239–@255); G44938 = 15 in the dump, so the refusal is silent in free roam **(checked)**. The mod shows its own message (§3.9). |
| Step 3 of the identity rewrite "rewrites +4322/+4323" at selector@31586 | **Corrected** | In that branch the model is not a protagonist, so update_cur only writes +4321 = 145 (path @46406 → @46509 → @46559). Most raw +4321 loads (player_controller_b @27327, respawn_controller @22219, selector @33400 …) already follow an update_cur call. The real exception is selector@31586/@31610 (load_cur4, §3.2). |
| Forwarders and V copies change existing mods' behaviour | **Corrected** | V is vanilla and F′ equals V for 0..2, so existing mods (characters 0..2 only, D10) behave as today. The real risk is a silent "pattern not found" when V differs from vanilla (§3.4). |
| A police-mod fault "traps" the player | **Overstated** | Loading a save from the pause menu works; the load hook removes the alias, and the broker and hardening are loader services. The loss is progress since the last save plus the mod's features. |
| "Generate the char4 pack only when the police mod is active; otherwise mount nothing" | **Rejected** | It would leave officer DLCScriptSaveData blocks unclaimed in saves made while the mod is off, which §6 lists as a loss path (dropped by a re-save, U4). ml_char4_reg stays registered whenever the loader runs (§3.6). |
| "Skip a faulted mod's overrides" as the whole fix | **Corrected** | Alone, it would lift the Director Mode block while the officer is the player. The char4-critical overrides move into the loader first (§3.11). |
| "Clear the D11 latch at every load" | **Policy choice, not a defect** | Put to the user (§0.4 Q7). |
| Check mission_triggerer_d's "code hash" to tell the patched copy from another | **Corrected** | thread+0x10 holds the name hash, the same for both copies. The build record (or an appended marker native) is used instead (§3.2). |
| Blocks are freed "on every save load" | **Unverified** | Blocks are freed only when the counter 0x3ED1BD0 reaches 0. Whether that happens at a save load is U28. The detour no longer depends on it (§3.3). |
| The save system "keeps the old member addresses" after block 13 is re-created | **Unverified (guess)** | Not shown. The re-creation itself is verified (0x91bde0), and §3.6 now prevents it. |
| The 11 confirmed character-indexed static arrays sit in multi-instance aware programs | **Not shown** | The list was not found in the artefacts. The S-kind fix (appended statics) does not depend on it; S3/S4 now report it. |
| Tow-job offer is a protagonist call or text | **Guess** | Not checked; part of U9 and §0.4 Q8. |

### Review round 3: parts rejected or corrected

No round-3 item was rejected outright; all 15 are merged (§0.5). The parts below were rejected or corrected during verification.

| Item (part) | Decision | Reason / evidence |
|---|---|---|
| Item 6: the "cheap prologue" alternative (copy the parameter statics from address 0 at thread start) | **Rejected** | A start without arguments skips the copy (0x921d07 `test ebp,ebp; jz` **(checked)**), so the prologue would copy shadow initial values into the parameter statics. It could only work with a marker that shows whether arguments arrived (§3.3 S row). |
| Item 7: element sizes of tens of thousands of slots at character indices | **Overstated** | The cited large elements are MP arrays: shop_controller@916420 and carmod_shop use G4718592 (block 18), respawn_controller@25129 and the other 5,574-size sites G2359296 (block 9), indexed by a local or by CALL @25538; index 3 for the officer is unlikely there. An overrun happens only if the implementation ignores the operand, an omission in the spec rather than a proven mechanism. The operand-sized sinks were merged anyway (§3.3). |
| Item 10: the int3 placement guard fires on ml_char4_reg's first placement | **Rejected** | table[13] is 0 in the dump and 0x3ED1AC0 is 0 **(checked)**, so the guard skips; a re-placement of the same program has the same signature. It fires only if another program registered block B first with another non-zero signature. The header rules were merged for ml_char4_bank (compared with table[0]) and as defence (§3.6). |
| Item 10, fix (a) as proposed: "count frames in a spare local" | **Corrected** | ENTER @143017 has locals = 6 **(checked)** and locals 2–5 are all used (index, done flag, bitset, hash). ENTER grows to 7 (a same-size operand edit), or a startup static is used. |
| Item 12, sequence A: an unserved WEAPONS[145] in an aware respawn_controller kills the thread after a fault | **Partly rejected** | In release builds §3.3 already sent an unserved access in an aware thread to the D11 scratch (the thread survives and the vanilla "walk out of the station" runs); only development builds kill it. D11's text said "index 3", so §3.3 now states "any unserved index". Sequence B (an arrest before M2 or after a fault, with the mod's guard gone) stands. |
| Item 13: DISABLE_FRONTEND_THIS_FRAME during the whole broker sequence | **Corrected** | It may suppress the broker's own save menu in step 6 (guess); player_controller_b@1244 uses it only during its own switch (review). It is used only outside step 6's menu, after a check. While the menu is up, selector's pause-menu tests (@24643, @24860) probably refuse (guess). |
| Item 11: a swap at queue time as a way to support repeat saves as the officer | **Corrected** | mission_repeat_controller has already captured the player into the saved repeat block before the queue native (@571867 → @669275 → @203639, @193247, @571893 → @665533; review), so a swap at queue time would still save the officer's position and snapshot. Refusal is the v1 path; benchmark's captures (@458070, @16279) are a guess. |
| Item 2: the SP3 mirror is zeroed by a mod-off session | **Conditional** | Only if U6 makes SP3_* SP-saved, or if the pre-save flush refreshes the mirror from unregistered or reset SP3 stats (guess). Both are prevented by §3.6 (one mounted unit; no flush when the officer was not active for the loaded save). |
| Item 3: the tow job's speaker sites (33 voice strings in towing) | **Moot** | §0.4 Q8 keeps controller_towing and towing blind and outside Q4. The body's leftover towing entries (§0.0 effect column, §1, §3.1, §4, §5.4, §6, U9, M2) now follow that answer. |
| Item 5: the effect of the appcontacts mirror on vanilla | **Probably benign, fixed anyway** | Entry 96 already has owner-3 = 1 in field 28, so one more owner-3 value is likely harmless (guess), but it breaks §4's "keeps the save vanilla". Value 1 / never 130 plus the invariant check were merged. |
| Item 4: a charge made mid-swap going to the protagonist through get_cur4 | **Guess** | Not shown; covered anyway by "save busy" and the "may switch now" predicate on the save gates. |
| Item 1: CREATE_PED at the officer's position (an MP apartment at z = −100) | **Not proposed** | It was a caution about a naive fix; step 2b never creates the protagonist at the officer's position or inside an officer-only interior. |
| Items 9/14: a foreign copy counted as aware degrades the whole session through D11 | **Guess** | Not shown; moot once the identity check keeps foreign copies out of the aware set. |