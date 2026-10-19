# Halo OpenCE Forge (`HaloCE Forge.exe`) — Architecture & Documentation

OpenCE Forge is a standalone, non-destructive 3D Forge World Editor and Multiplayer Co-Op Sandbox for **OpenCE** (Halo: Combat Evolved decompilation port). All Forge source code resides inside `FORGE CODE/` without modifying any upstream file in `source/` or `port/`.

---

## Directory Layout (Drag-and-Drop Overlay)

```text
<Your Halo OpenCE Folder>/
├── HaloCE Forge.exe                      # Standalone Forge executable (leaves halo.exe & SDL3.dll untouched)
├── forge_maps/                           # Clean folder where saved Forge maps are packaged (<name>/<name>.map, .bmp, .txt)
├── OpenCE Forge/                         # Runtime Forge directory
│   ├── credits.md                        # Auto-generated tag & map creator attribution log
│   ├── menus/                            # Custom Forge Main Menu XML & handmade UI art
│   ├── tools/                            # Standalone forge_asset_compiler.exe CLI tool
│   └── cached_tags/                      # Local Tag Cache Folder System (built automatically on first launch)
│       ├── catalog.txt                   # Base tag cache manifest (CE multiplayer + Xbox/PC campaign tags)
│       ├── custom_catalog.txt            # User-added custom map tag manifest ([F9] Add Tags to Cache)
│       ├── bitmaps_cache.bin             # Embedded diffuse bitmap & tag data cache
│       ├── vehicles/                     # Extracted 3D vehicle .ftag bundles
│       ├── weapons/                      # Extracted 3D weapon .ftag bundles
│       ├── scenery/                      # Extracted 3D scenery .ftag bundles
│       ├── devices/                      # Extracted 3D device/machine .ftag bundles
│       └── bipeds/                       # Extracted 3D Monitor & biped .ftag bundles
└── FORGE CODE/                           # Standalone Forge C source code, headers, tools, and build script
    ├── README.md                         # Architecture & documentation (this file)
    ├── projectrules.md                   # Authoritative rules & guardrails
    ├── build_forge.py                    # Build & packaging script
    ├── include/
    │   ├── forge_cache_builder.h         # Tag cloner, cache builder, custom tag extractor, map exporter
    │   ├── forge_engine.h                # 2D Blueprint, 3D Monitor/Grab/Palette, and Network sync APIs
    │   └── forge_net_override.h          # Forge network version isolation header
    ├── src/
    │   ├── forge_cache_builder.c         # Schema-driven tag cloner, bitmap/sound internalizer, credits writer, exporter
    │   ├── forge_runtime_tags.c          # First-launch local tag cache builder & runtime .ftag / bitmap shader loader
    │   ├── forge_blueprint_2d.c          # 2D Top-Down Blueprint Editor
    │   ├── forge_3d_mode.c               # 3D In-Engine Forge Mode, Physics Beam, 3D Preview, and Spawn Menu
    │   ├── forge_ai.c                    # Engine AI support
    │   ├── forge_network.c               # Forge Lobby Isolation & Multiplayer 3D Entity Replication
    │   └── forge_hooks.c                 # Engine & SDL3 hooks and forge_maps/ path redirection
    └── tools/
        └── forge_asset_compiler.c        # Standalone CLI tag cache compiler & validator
```

---

## First-Launch Local Tag Cache Builder (`OpenCE Forge/cached_tags/`)

So that no copyrighted `.map` files or pre-packaged tag caches need to be distributed with the build, **`HaloCE Forge.exe`** asks to build your local tag cache (`OpenCE Forge/cached_tags/`) on first launch:
1. Point to your **Halo Custom Edition `custom_maps` folder** (`bitmaps.map`, `sounds.map`, `loc.map`, and default multiplayer `.map` files).
2. Point to your **Halo Xbox / Retail PC `maps` folder** (`bitmaps.map`, `sounds.map`, and campaign `.map` files).
3. Forge extracts and builds your local tag cache in **`OpenCE Forge/cached_tags/`** (`.ftag` 3D mesh/shader bundles, `catalog.txt`, and embedded campaign/multiplayer bitmaps in `bitmaps_cache.bin`), and records creator attribution in `OpenCE Forge/credits.md`.

You can also add tags from any custom `.map` file at any time in Forge Mode via **`[F2] Spawn Menu -> [F9] Add Tags to Cache`** (`MAPS` window).

---

## Saved Forge Maps (`forge_maps/<map_name>/`)

Pressing **`F5`** (*Save Map*) packages each saved Forge map into its own clean subfolder inside **`forge_maps/<map_name>/`** alongside `maps/` and `custom_maps/`:
- `forge_maps/<map_name>/<map_name>.map`
- `forge_maps/<map_name>/<map_name>.bmp`
- `forge_maps/<map_name>/<map_name>.txt`
