# OpenCE Forge (`HaloCE Forge.exe`) — Architecture & Implementation Plan (`README.md`)

OpenCE Forge is a modular, additive map creation, AI encounter placement, and real-time 3D/2D editing suite for **OpenCE** (Halo: Combat Evolved decompilation port). All source code resides strictly inside `FORGE CODE/` without modifying any upstream file in `source/` or `port/`.

---

## Directory Layout

```text
OpenCE-Forge/
├── HaloCE Forge.exe                      # Root Forge executable (built alongside halo.exe)
├── custom_maps/                          # Exported standalone v609 maps (<name>.map, .bmp, .txt)
├── OpenCE Forge/                         # Runtime Forge directory
│   ├── credits.md                        # Auto-generated tag & map creator attribution log
│   └── forge_maps/
│       ├── forgeasset.map                # Universal master asset cache (CE MP + PC Campaign tags)
│       ├── customforgeasset.map          # User-injected community custom tags cache
│       ├── bitmaps.map                   # CE resource map
│       ├── sounds.map                    # CE resource map
│       └── loc.map                       # CE resource map
└── FORGE CODE/                           # All Forge source code, headers, tools, and build scripts
    ├── README.md                         # Architecture & implementation plan (this file)
    ├── projectrules.md                   # Authoritative rules & guardrails
    ├── build_forge.py                    # Cross-platform build script (Windows, Linux, Android)
    ├── include/
    │   ├── forge_cache_builder.h         # Tag cloner, asset compiler, custom tag injector, map exporter
    │   └── forge_engine.h                # 2D Blueprint, 3D Monitor/Grab/Palette, AI, and Network sync APIs
    ├── src/
    │   ├── forge_cache_builder.c         # Schema-driven tag cloner, PC->CE internalizer, credits writer, exporter
    │   ├── forge_runtime_tags.c          # Runtime tag cache merger for live 3D spawning
    │   ├── forge_blueprint_2d.c          # Mode 1: 2D Overhead Blueprint Editor (Single-Player)
    │   ├── forge_3d_mode.c               # Mode 2: 3D In-Engine Monitor Cam, Physics Beam, Spawn Palette
    │   ├── forge_ai.c                    # Live Engine NPC AI Spawning (Friendly vs Enemy) & Allegiances
    │   ├── forge_network.c               # Forge Lobby Isolation & Multiplayer 3D Entity/AI Replication
    │   └── forge_editor.c                # Master Forge state machine, HUD/UI modals, and entry hooks
    └── tools/
        └── forge_asset_compiler.c        # Standalone CLI compiler & validator for forgeasset.map / customforgeasset.map
```

---

## Execution & Implementation Plan

### Step 1: Asset Pipeline, Credits Generator & Map Exporter (`forge_cache_builder.c` + `forge_asset_compiler.c`)
1. **Schema-Driven Tag Cloner**:
   - Links directly against OpenCE's compiled `tag_schema_*.o`, `tag_validate.o`, and `cache_file_formats.o`.
   - Clones allowed tag classes (`bipd`, `actv`, `actr`, `antr`, `vehi`, `weap`, `eqip`, `itmc`, `scen`, `mach`, `ctrl`, `lifi`, and their model/collision/physics/damage/shader/bitmap/sound dependencies) while strictly excluding `sbsp` (Structure BSPs).
   - Preserves Custom Edition resource map links (`bitmaps.map`, `sounds.map`, `loc.map`) whenever a bitmap/sound exists in CE resource maps, and internalizes PC-only bitmap pixels and sound samples from `PC MAPS/bitmaps.map` and `PC MAPS/sounds.map`.
2. **Automated Attribution (`OpenCE Forge/credits.md`)**:
   - Tracks every harvested or injected tag's class, path, source `.map` filename, and `*<map file name>* and its original creator` in `OpenCE Forge/credits.md`.
3. **Compile & Validate Master Caches**:
   - Build `FORGE CODE/build/forge_asset_compiler.exe`.
   - Generate `OpenCE Forge/forge_maps/forgeasset.map`, `OpenCE Forge/forge_maps/customforgeasset.map`, and `OpenCE Forge/credits.md`.
   - Validate generated maps with `custom_edition_cache_load` + `tag_validate_custom_edition_tags` (0 errors, 0 corrections).
4. **"Save Copy As..." Exporter**:
   - Bakes placed objects (from `forgeasset.map` and, in single-player, `customforgeasset.map`), spawns, netgame flags, netgame equipment, and NPC AI encounters (`ai_actor_palette`, `ai_encounters`, `squads`, `starting_locations`, and BSP-cluster `firing_positions`) into `custom_maps/<name>.map` along with a rendered 140x114 `.bmp` thumbnail and `.txt` description.

### Step 2: 2D Blueprint Mode & 3D In-Engine Mode (`forge_blueprint_2d.c`, `forge_3d_mode.c`, `forge_ai.c`, `forge_runtime_tags.c`)
1. **Mode 1 — 2D Blueprint Editor (Single-Player)**:
   - Extracts 2D collision floorplan wireframe from the active map's `structure_bsp`.
   - Interactive top-down canvas with pan/zoom, $X/Y$ placement, $Z$-elevation slider, yaw rotation, team toggles, budget meter, and one-click export to `custom_maps/`.
   - Full access to both `forgeasset.map` and `customforgeasset.map`.
2. **Mode 2 — 3D In-Engine Mode (Single-Player, Multiplayer PVP & Online Co-op Campaign)**:
   - **Monitor / Free-Cam**: Smooth 6DOF flight movement toggleable at any time.
   - **Physics Beam / Object Grab**: Raycast grab, translation, rotation snapping (15°, 45°, 90°), ground snapping, and deletion.
   - **Categorized Spawn Palette**: *Weapons, Vehicles, Spawns, Objectives, Scenery, Barriers, Teleporters, AI Squads*.
   - **Multiplayer Asset Guard**: Automatically restricts the spawn palette to `forgeasset.map` during live multiplayer/co-op sessions, while enabling both `forgeasset.map` and `customforgeasset.map` in single-player sessions.
   - **Live Engine NPC AI Placement**: Spawns native Halo CE AI units (`actv`/`bipd`) with **Friendly** (Human/Blue) vs **Enemy** (Covenant/Red/Flood) team toggle that immediately engage in combat.

### Step 3: Networking, Lobby Isolation & Packaging (`forge_network.c`, `forge_editor.c`, `build_forge.py`)
1. **Session Handshake & Isolation**:
   - Marks Forge lobbies so only `HaloCE Forge.exe` clients can discover/join Forge editing sessions, while stock `halo.exe` clients are cleanly rejected (`"Requires HaloCE Forge to join"`).
2. **Multiplayer 3D Synchronization**:
   - Host authoritatively simulates dynamic objects, limits, and AI states, replicating Forge placement/grab/move/delete/team actions to connected Forge clients.
3. **Build & Verify `HaloCE Forge.exe`**:
   - Compile all Forge modules and link `C:\Users\LeviB\Projects\OpenCE-Forge\HaloCE Forge.exe` (plus Linux/Android targets in `FORGE CODE/build_forge.py`).
   - Run automated verification tests on `forgeasset.map`, `customforgeasset.map`, `credits.md`, exported maps in `custom_maps/`, and `HaloCE Forge.exe`.
