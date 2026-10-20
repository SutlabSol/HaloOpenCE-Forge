# Halo OpenCE Forge — Project Roadmap & Changelog

This document tracks the completed milestones, features, architectural improvements, and upcoming plans for **Halo OpenCE Forge**.

---

## Completed Milestones & Features

### Phase 1: Core 3D Forge Mode & 2D Blueprint Editor
- [x] **3D Monitor Mode (`F1`)**
  - Seamless in-game toggle between the player Spartan and the 343 Guilty Spark Monitor camera.
  - Free-flying 3D camera movement with boost (`Shift`) and precision placement controls.
  - Prevented damage/falling physics when opening the Spawn Menu (`F2`) while floating as the Monitor.
  - Restricted `F2` Spawn Menu access exclusively to Monitor Mode (disabled while playing as a Spartan).
  - Hidden Spartan-linked model attachments and glow widgets when switching to Monitor Mode in multiplayer.
  - Fixed engine light assertion crash (`light->type == 0`) when toggling `F1` into Monitor Mode.
- [x] **Object Manipulation & Physics Controls**
  - **20m Interaction Range**: Raycast targeting and grabbing restricted to objects within 20 metres for clean selection.
  - **Scroll-Wheel Distance Control**: Push and pull held objects between **1m and 30m** using the mouse scroll wheel.
  - **3-Axis Object Rotation (`Hold R`)**: Rotate held objects smoothly with the mouse (`G` cycles rotation snap angle).
  - **Live Snap-to-Floor (`Hold Z` + Left Click)**: Holding `Z` continuously raycasts downward to snap the held object flush to the terrain/floor below, and clicking places it directly at the snapped position.
- [x] **2D Top-Down Blueprint Mode (`F6`)**
  - Orthographic top-down map inspection, grid overlay, pan/zoom controls, and rapid multi-object layout.

---

### Phase 2: Local Tag Cache System & Cross-Map Asset Injection
- [x] **First-Launch Local Tag Cache Builder (`OpenCE Forge/cached_tags/`)**
  - Prompts the user on first launch for Custom Edition (`custom_maps`) and Retail/Xbox (`maps`) directories so no copyrighted `.map` files are redistributed.
  - Compiles `.ftag` 3D hardware mesh/shader bundles, `catalog.txt`, and `bitmaps_cache.bin` into `OpenCE Forge/cached_tags/`.
- [x] **In-Game Custom Map Tag Extraction (`[F2] -> [F9] Add Tags to Cache`)**
  - Extracts unique custom tags from `.map` files into `custom_catalog.txt` and `custom_bitmaps_cache.bin` while recording creator attribution in `OpenCE Forge/credits.md`.
- [x] **Live 3D Spawn Menu Model Previews**
  - Rotating 3D textured model and translucent objective marker previews in the `F2` Spawn Menu.

---

### Phase 3: 3D Translucent Objective Markers, Map Spawns, 10-Point KOTH & Teleporters
- [x] **Custom 3D Translucent World Marker Models (`OpenCE Forge/cached_tags/markers/`)**
  - Imported and triangulated custom 3D `.obj` marker models (`Player-Spawn.obj` at `1.25m x 1.0m` with forward nose orientation, `Teleporter-Origin.obj`, `Teleporter-Destination.obj`, `Race-Marker.obj`, `Oddball-Marker.obj`, and 3D CTF Flagpoles) rendered with translucent team/objective shaders (`55%` normal alpha, `72%` highlighted alpha).
  - Floating 2D world labels stay hidden in Forge Mode until the pickup reticle hovers over or grabs the marker.
- [x] **Full Default Map Spawn & Objective Visibility & Editing**
  - Automatically loads and renders all built-in scenario Player Spawns (`scen->players`: Yellow FFA, Red Team, Blue Team), CTF Flags, Oddball Skull Spawns, KOTH Hill Points, Race Checkpoints, and Teleporter Origins/Destinations (`scen->netgame_flags`).
  - Place new Player Spawns, CTF Flags, Oddball Skull Spawns, Race Checkpoints, KOTH Hills, and Teleporters from the `F2` Spawn Menu.
- [x] **10-Point Customisable KOTH Hill Placement**
  - Spawning `[KOTH] Hill Marker` places a **4m diameter (`2.0m` radius) circle of 10 linked KOTH Hill points**.
  - **Hold `C` or `Shift` + Scroll Wheel** (or `+`/`-`, `[`/`]`) while placing to live-scale the circle radius (`0.5m` to `25.0m`).
  - Once placed, the 10 nodes become individually grabbable so users can drag individual points to customize the KOTH zone shape, with a connected gold perimeter ring rendered between nodes in Forge Mode.
- [x] **Live Teleporter Pair Linking**
  - Placed and moved Teleporter Origin (`In`) and Destination (`Out`) nodes sync live to `scenario->netgame_flags` and teleport players in Play Mode when sharing the same Reference/Channel ID (`#0..#15`).

---

### Phase 4: Map Saving (`forge_maps/`), Custom Multiplayer Menu Integration & UI
- [x] **Clean Per-Map Packaging (`forge_maps/<name>/`) & Custom Multiplayer Listing**
  - Saving a map (`F5` or Pause Menu -> `SAVE FORGE MAP`) packages `<name>.map`, `<name>.bmp`, and `<name>.txt` into `forge_maps/<name>/` while keeping `custom_maps/` completely clean.
  - Hooked `_halo_windows_fopen`, `_file_reference_create_from_path`, and `_halo_xbox_FindFirstFileA` / `_halo_xbox_FindNextFileA` so saved Forge maps in `forge_maps/<name>/<name>.map` appear automatically under **`Custom Multiplayer`** in both Singleplayer and Multiplayer menus and load without any dirty disc error.
- [x] **Customizable Forge UI (`OpenCE Forge/ui/`)**
  - User-editable `OpenCE Forge/ui/forge_ui.xml` and `.png` panel textures.

---

## Upcoming / Planned Ideas

- [ ] **Xbox Map Decompression in First-Launch Cache Builder**: Duplicate/decompress Xbox retail `.map` files during first-launch cache building so campaign bitmaps, shaders, and sounds extract with 100% fidelity.
- [ ] **Modular Object-to-Object Snapping & Custom 3D Building Blocks**: Snap custom 3D building block models together (edge/grid/socket snapping) for rapid custom map construction.
- [ ] **Object Budget & Quota Meter**: Visual budget bar in the top HUD showing remaining scenario palette slots and memory headroom.
- [ ] **Multi-Select & Group Prefabs**: Select multiple Forge objects at once to move, rotate, duplicate, or save as reusable prefab templates.
- [ ] **Undo / Redo History Stack**: Quick hotkey (`Ctrl+Z` / `Ctrl+Y`) to revert recent placements, deletions, or property edits.
