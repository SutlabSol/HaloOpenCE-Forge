# OpenCE Forge — Project Rules & Guardrails (`projectrules.md`)

This document is the authoritative rulebook for **OpenCE Forge (`HaloCE Forge.exe`)**. Every code module, build script, and asset pipeline tool must strictly adhere to these rules.

---

## 1. Repository & Source Code Isolation Rules
1. **ZERO Upstream Source Modifications**:
   - **DO NOT** modify any file in `source/`, `port/`, or any existing upstream OpenCE file.
   - All Forge code must be 100% additive and link alongside the compiled OpenCE object files (`build/windows/obj/...`, `build/linux/obj/...`, `build/android/obj/...`).
2. **Dedicated Code Directory**:
   - All Forge C source files, headers, standalone tools, and build scripts must live strictly inside:
     `C:\Users\LeviB\Projects\OpenCE-Forge\FORGE CODE\`
3. **Git & Version Control**:
   - **DO NOT** commit or push to GitHub until explicitly instructed by the user after local verification.
   - `GAME MAPS/` and `FORGE CODE/GAME MAPS/` must remain in `.gitignore`.
4. **Target Platforms**:
   - **Windows** (`HaloCE Forge.exe`), **Linux**, and **Android**.
   - The **Web** build is explicitly excluded for Forge.
5. **Output Binary & Runtime Locations**:
   - Executable: `C:\Users\LeviB\Projects\OpenCE-Forge\HaloCE Forge.exe` (in the root folder alongside `halo.exe`).
   - Forge runtime directory & credits: `C:\Users\LeviB\Projects\OpenCE-Forge\OpenCE Forge\` (including `OpenCE Forge/credits.md`).
   - Forge master caches:
     - `C:\Users\LeviB\Projects\OpenCE-Forge\OpenCE Forge\forge_maps\forgeasset.map`
     - `C:\Users\LeviB\Projects\OpenCE-Forge\OpenCE Forge\forge_maps\customforgeasset.map`
   - Saved/Exported user maps:
     - `C:\Users\LeviB\Projects\OpenCE-Forge\custom_maps\<map_name>.map`
     - `C:\Users\LeviB\Projects\OpenCE-Forge\custom_maps\<map_name>.bmp` (140x114 24-bit BMP thumbnail)
     - `C:\Users\LeviB\Projects\OpenCE-Forge\custom_maps\<map_name>.txt` (plaintext description)

---

## 2. Asset Pipeline & Tag Scope Rules
1. **Allowed Root Tag Classes Only (`forgeasset.map` & `customforgeasset.map`)**:
   - Strictly object and entity palettes:
     - **Bipeds (`bipd`)**: Player models and AI character bodies, along with their dependent **Actor Variants (`actv`)**, **Actors/Characters (`actr`)**, and **Animations (`antr`)**.
     - **Vehicles (`vehi`)**: All drivable, flyable, and turret vehicles.
     - **Weapons (`weap`)**: All firearms, plasma weapons, heavy weapons, and grenades (plus `eqip` / `itmc` for grenade/weapon collections).
     - **Scenery (`scen`)**: Props, barriers, crates, furniture, and structure accents (plus `mach`, `ctrl`, `lifi` interactive devices).
     - **Essential Object Dependencies**: Models (`mod2`), Collision Models (`coll`), Physics (`phys`), Damage Effects (`jpt!`), Projectiles (`proj`), Effects (`effe`), Shaders (`shdr`, `soso`, `schi`, `scex`, `sgla`, `senv`, `spla`, `swat`, `smet`), Bitmaps (`bitm`), and Sounds (`snd!`).
2. **NO Structure BSPs (`sbsp`) Extracted**:
   - **NEVER** extract or duplicate `sbsp` tags, level geometry, terrain, lightmaps, or BSP models from donor maps.
   - `forgeasset.map` and `customforgeasset.map` are purely object/entity repositories.
3. **Resource Map Linking & PC Asset Internalization**:
   - Format: Halo Custom Edition cache format (`version == 609`, tag cache at `0x40440000`, max `0x01700000` bytes).
   - Tags present in standard Custom Edition `bitmaps.map`, `sounds.map`, or `loc.map` preserve their external resource map links (`in_resource_map = 1`).
   - Tags imported from `PC MAPS/` (`version == 7`) whose raw bitmap pixels or sound samples are not in Custom Edition's `bitmaps.map` / `sounds.map` have their raw pixel/sample data internalized directly into the `.map` file and their external flag cleared (`bitmap_data.flags &= ~0x100`, `samples.flags &= ~1`).
4. **Community Attribution (`OpenCE Forge/credits.md`)**:
   - Every root tag extracted into `forgeasset.map` or injected into `customforgeasset.map` must be logged in `OpenCE Forge/credits.md` with:
     - Tag class and path (e.g., `characters\elite\elite major.actv`, `scenery\covenant_crate\covenant_crate.scen`).
     - Source map filename (e.g., `a10.map`, `yoyorast_island.map`).
     - Attribution format: `*<map file name>* and its original creator` (parsed from accompanying `.txt` / readme metadata when available).
   - Whenever **"Inject Custom Tags"** is used in-game or via CLI, `OpenCE Forge/credits.md` is automatically updated with the newly injected tags and their source map attribution.

---

## 3. Definition of "AI" in OpenCE Forge
1. **In-Game Halo Engine NPC AI Only**:
   - "AI" refers **exclusively** to native Halo: Combat Evolved NPC actors and bipeds (Marines, Crewmen, Elites, Grunts, Jackals, Hunters, Flood, Sentinels) driven by the game's internal combat, perception, and pathfinding engine (`source/ai/`: `actors.c`, `encounters.c`, `game_allegiance.c`).
   - It does **not** mean external AI agents, chatbots, or LLMs.
2. **Team Assignment**:
   - Placed AI units are assigned to either:
     - **Friendly** (`_game_team_player` / `_game_team_human`, allied with players / Blue team).
     - **Enemy** (`_game_team_covenant` / `_game_team_flood` / `_game_team_sentinel`, hostile to players and Friendly AI).
3. **Live & Saved Behavior**:
   - In 3D In-Engine Mode, spawned AI units immediately initialize via `actor_create_for_unit` + `actor_customize_unit` with `force_active = TRUE` and engage opposing teams/players in real combat.
   - When saved via **"Save Copy As..."**, placed AI units are written into `scenario->ai_actor_palette` and `scenario->ai_encounters` (`squads`, `starting_locations`, and BSP-cluster-linked `firing_positions`) so stock `halo.exe` spawns and runs them natively on both Multiplayer and Co-op Campaign maps.

---

## 4. Multiplayer Asset Guard & Map Sharing Lifecycle
1. **Live Multiplayer / Online Co-op Forge Sessions**:
   - **Restricted to `forgeasset.map` Only**: During live online Forge sessions with multiple players, the in-game spawn palette **only** offers items from `forgeasset.map`.
   - `customforgeasset.map` is **disabled** during live multiplayer/co-op Forge editing so every connected Forge client is guaranteed to have 100% identical tags with zero missing-tag crashes or mid-session asset transfers.
2. **Single-Player Forge Mode (3D In-Engine)**:
   - **Unrestricted**: The creator has full access to both `forgeasset.map` and `customforgeasset.map` (including all user-injected custom tags).
3. **"Save Copy As..." Standalone Map Baking**:
   - When saving a map to `custom_maps/<map_name>.map`, all placed tags (whether from `forgeasset.map` or `customforgeasset.map`) and their dependencies are baked directly into `custom_maps/<map_name>.map`.
   - When that saved map is later hosted in stock `halo.exe` or `HaloCE Forge.exe`, OpenCE's native P2P map sharing (`map_share.c`) distributes the standalone `.map` file automatically.

---

## 5. Session Isolation & Network Handshake
1. **Forge Lobby Isolation**:
   - Forge sessions advertise a distinct Forge session marker in the lobby payload (`p2p_lobby.c` / network game data).
   - Only `HaloCE Forge.exe` clients list and join active Forge editing sessions.
   - Stock `halo.exe` clients ignore or are cleanly rejected from Forge editing lobbies (`"Requires HaloCE Forge to join"`).
   - Saved maps (`custom_maps/<map_name>.map`) hosted in normal gameplay mode are 100% compatible with stock `halo.exe`.
