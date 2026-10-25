# Halo OpenCE Forge v1.1.0 (Standalone Addon)

A standalone **Halo 3-style 3D In-Game Forge Editor & 2D Blueprint Mode** built on top of [Halo: Open Community Edition (OpenCE)](https://github.com/d00tbeep/open-ce).

This repository is structured as a **non-destructive drag-and-drop package**: you can copy its files directly into your existing Halo OpenCE directory without overwriting `halo.exe`, `SDL3.dll`, or any of your base game files.

---

## What's New in v1.1.0 (Multiplayer Collaborative Forge Update)

- **Per-Player Monitor vs. Spartan Synchronization (`!FT:`)**:
  - Each player in a multiplayer Forge session is independently rendered as **343 Guilty Spark Monitor** when in Forge Mode (`F1`) or as their **Spartan** when in Play Mode, regardless of which mode the local viewer is in.
  - Remote Monitors fly at their true 3D altitude and orient smoothly using their 3D aiming vector.
  - Single-tick mode switching (`F1`) synchronizes exact position and facing to the Host without respawn delays or rubberbanding.
- **Zero Active Camo Bleed**:
  - Eliminated host-to-client `_distributed_unit_camouflaged_bit` bleed by scoping Monitor unit light/shadow suppression strictly to the local render pass (`forge_render_begin_monitor_visibility` / `forge_render_end_monitor_visibility`).
- **Full Bidirectional Multiplayer Tag & Object Sync (`!FS:`, `!FH:`, `!FM:`, `!FD:`, `!FQ`)**:
  - Host and Clients can spawn, grab, move, rotate, and delete map-native objects, external Xbox campaign tags (`cached_tags/`), custom `.ocetags` packages, Lighting & Emitter objects, Player Spawns, Teleporters, and Gametype Objectives (`CTF`, `Oddball`, `KOTH`, `Race`).
  - Clients automatically fault in external campaign tags and `.ocetags` definitions on the fly when receiving Host spawn packets (`!FH:`) and remap `network_objects_handle_changes` definition indices seamlessly.
  - Late-joining clients automatically synchronize all active Monitor states and placed/edited Forge instances (`!FQ`).
- **Network Protocol Version**: Updated to `0x4627` (`v1.1.0`).

---

## Features

- **3D In-Game Monitor Mode (`F1`)**
  - Seamless transition between Spartan and 343 Guilty Spark Monitor camera.
  - Monitor model linked Spartan glow attachments automatically hidden in multiplayer/forge.
  - Interact with objects within **20m**; use the **Mouse Scroll Wheel** while holding an item to adjust distance between **1m and 30m**.
  - Hold **`Z`** to live-snap the held object to the terrain/floor below and **Left-Click** to place it on the ground.
- **Visual Map Spawns & Objective Markers**
  - **Player Spawns** render as team-coloured circles on the ground (`Yellow` = FFA, `Red` = Red Team, `Blue` = Blue Team) with a directional arrow showing the exact facing angle the player will spawn with.
  - **Existing Map Spawns & Gametype Objectives** (`Player Spawns`, `CTF Flags`, `Oddball Spawns`, `King of the Hill`, `Race Track Points`, and `Teleporters`) are automatically loaded from the map scenario (`scen->players` & `scen->netgame_flags`), rendered in 3D during Forge Mode, and can be grabbed, moved, rotated, deleted, or configured via **`X` (Object Properties)**.
- **Customizable Forge UI (`OpenCE Forge/ui/`)**
  - Edit `OpenCE Forge/ui/forge_ui.xml` to customize font styles (`fixedsys`, `conduit`, `larabie`, `handel`), text sizes, colours, and panel colours.
  - Edit the PNG images in `OpenCE Forge/ui/` (`panel_spawn_bg.png`, `panel_header_bg.png`, `panel_props_bg.png`, `hud_bar_bg.png`, etc.) in any image editor to customize the Forge menu backgrounds.
- **Spawn Menu (`F2`) & Add Tags to Cache (`F9`)**
  - Accessible exclusively while in Forge Mode (`F1`).
  - Browse Vehicles, Weapons, Scenery, Lighting & Emitters, Gametype Markers, Teleporters (Origin & Destination with working runtime collision matrices), and AI Encounters.
  - Press **`F9` (Add Tags to Cache)** inside the `F2` menu to open the **MAPS** window and extract tags from any map into your local tag cache.
- **First-Launch Local Tag Cache Builder**
  - So that no copyrighted `.map` files or pre-packaged tag caches need to be distributed with the build, `HaloCE Forge.exe` prompts to build your local tag cache (`OpenCE Forge/cached_tags/`) on first launch:
    1. Point to your **Halo Custom Edition `maps`** folder (`bitmaps.map`, `sounds.map`, `loc.map`, and multiplayer `.map` files).
    2. Point to your **Halo Xbox / Retail PC `maps`** folder (`bitmaps.map`, `sounds.map`, and campaign `.map` files).
  - Forge extracts and converts campaign & multiplayer tags, bitmaps, shaders, models, vehicles, weapons, and scenery directly into `OpenCE Forge/cached_tags/` (`forge_catalog.dat` + `.ftag` files) for instant runtime injection into any map.
- **Clean Forge Map Packaging (`forge_maps/`)**
  - Saved Forge maps are neatly packaged into their own subfolder (`forge_maps/<map_name>/<map_name>.map`, `.bmp`, `.txt`) alongside `maps/` and `custom_maps/`, keeping your `custom_maps/` folder clean while still appearing in the map list.

---

## Installation (Drag & Drop)

1. Download or clone this repository.
2. Copy the following into your existing **Halo OpenCE** game folder (where `halo.exe` and `SDL3.dll` are located):
   - `HaloCE Forge.exe`
   - `OpenCE Forge/`
   - `forge_maps/`
3. Launch **`HaloCE Forge.exe`**.
4. On first launch, follow the prompt to select your Custom Edition and Retail/Xbox `maps` folders to populate `OpenCE Forge/cached_tags/`.

---

## Controls in Forge

| Key / Input | Action |
| :--- | :--- |
| **`F1`** | Toggle between Spartan & 3D Monitor Forge Mode |
| **`F2`** | Open Forge Spawn Menu *(Monitor Mode only)* |
| **`F5`** | Save Forge Map (`forge_maps/<name>/`) |
| **`F9`** *(in `F2` menu)* | Open **Add Tags to Cache (MAPS)** window |
| **`Left Click`** | Grab / Place object within 20m |
| **`Mouse Wheel`** | Push / Pull held object (1m – 30m) |
| **`Hold R` + Mouse** | Rotate held object |
| **`Hold Z` + Left Click** | Live-snap held object to floor & click to place |
| **`X`** | Open Object Properties (Team, Spawn Time, Teleporter Channel, Gametype Mode) |
| **`Delete` / `Backspace`** | Delete highlighted or held object / spawn / objective |

---

## Building from Source

To recompile `HaloCE Forge.exe` against the [OpenCE](https://github.com/d00tbeep/open-ce) source tree:

1. Place `FORGE CODE/` and `OpenCE Forge/` inside your OpenCE repository root.
2. Run:
   ```bash
   python "FORGE CODE/build_forge.py"
   ```
3. The standalone executable and assets will be output to `OpenCE Build/`.
