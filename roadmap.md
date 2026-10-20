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
  - Hidden Spartan-linked model attachments and glow widgets (such as the 2 green lights) when switching to Monitor Mode in multiplayer.
  - Fixed engine light assertion crash (`light->type == 0`) when toggling `F1` into Monitor Mode.
- [x] **Object Manipulation & Physics Controls**
  - **20m Interaction Range**: Raycast targeting and grabbing restricted to objects within 20 metres (`6.67` world units) for clean selection.
  - **Scroll-Wheel Distance Control**: Push and pull held objects between **1m and 30m** (`0.33` to `10.0` world units) using the mouse scroll wheel.
  - **3-Axis Object Rotation (`Hold R`)**: Rotate held objects smoothly with the mouse.
  - **Live Snap-to-Floor (`Hold Z` + Left Click)**: Holding `Z` continuously raycasts downward to snap the held object flush to the terrain/floor below, and clicking places it directly at the snapped position.
- [x] **2D Top-Down Blueprint Mode**
  - Orthographic top-down map inspection, grid overlay, pan/zoom controls, and rapid multi-object layout.

---

### Phase 2: Local Tag Cache System & Cross-Map Asset Injection
- [x] **First-Launch Local Tag Cache Builder (`OpenCE Forge/cached_tags/`)**
  - Eliminated the need to distribute copyrighted `.map` files or pre-packaged tag caches with the Forge release.
  - Interactive first-launch folder selector prompts the user for:
    1. **Halo Custom Edition `maps` folder** (`bitmaps.map`, `sounds.map`, `loc.map`, and multiplayer `.map` files).
    2. **Halo Retail PC / Xbox `maps` folder** (`bitmaps.map`, `sounds.map`, and campaign `.map` files).
  - Automatically extracts and converts campaign and multiplayer vehicles, weapons, scenery, bipeds, bitmaps, shaders, and collision models into standalone `.ftag` files and `forge_catalog.dat` inside `OpenCE Forge/cached_tags/`.
- [x] **Resource Map Bitmap & Sound Internalization**
  - Resolves external `bitmaps.map` and `sounds.map` offsets across both Retail/Campaign and Custom Edition resource maps so imported campaign tags render with full textures and shaders inside multiplayer maps.
- [x] **In-Game "Add Tags to Cache" (`F9` MAPS Window)**
  - Renamed `Inject Custom Tags` to **`Add Tags to Cache`** (`MAPS` header) in the `F2` menu.
  - Allows selecting any `.map` file in-game and extracting its unique tags directly into `OpenCE Forge/cached_tags/`.
- [x] **3D Spawn Menu Model Previews**
  - Live rotating 3D wireframe/textured preview of highlighted tags in the `F2` Spawn Menu with automatic bounding-box normalization and camera centring.

---

### Phase 3: Gametype Objectives, Map Spawns & Teleporters
- [x] **Team-Coloured Player Spawn Markers**
  - Player Spawns render in 3D during Forge Mode as ground circles with a directional arrow pointing in the exact direction the player will spawn facing.
  - Automatically colour-coded by team assignment:
    - **FFA / Both**: Yellow
    - **Red Team**: Red
    - **Blue Team**: Blue
- [x] **Visibility & Editing of Existing Map Spawns & Objectives**
  - Automatically loads all built-in map scenario Player Spawns (`scen->players`) and Netgame Flags (`scen->netgame_flags`) when entering a map.
  - Renders and allows full interaction (grab, move, rotate facing angle, snap to floor, delete, and edit `X` Properties) for:
    - **Player Spawns** (Team & Gametype filter: All / CTF / Slayer / Oddball / KOTH / Race)
    - **CTF Flag Points** (Red & Blue Team flags)
    - **Oddball Spawns**
    - **King of the Hill (KOTH) Hills** (`Reference ID` hill index)
    - **Race Track Checkpoints / Nodes** (`Reference ID` checkpoint order)
    - **Teleporter Origins & Destinations** (`Reference ID` channel pairing `1–26`)
- [x] **Working Runtime Teleporters**
  - Fixed crash when spawning Teleporter Origin (`[>`) and Destination (`>1`) markers by synthesizing valid runtime collision node matrices and updating `scen->netgame_flags` dynamically.
  - Real-time cooldown and player orientation transfer when walking through paired teleporter channels in Spartan mode.

---

### Phase 4: Map Saving, Main Menu & Customizable UI
- [x] **Clean `forge_maps/` Map Packaging (`F5` Save)**
  - Saves forged maps into dedicated per-map folders (`forge_maps/<map_name>/<map_name>.map`, `.bmp`, `.txt`) alongside `maps/` and `custom_maps/`, keeping `custom_maps/` clean while automatically registering saved Forge maps in the game's map list.
  - Persists both newly spawned Forge objects/markers and edits/deletions made to existing map Player Spawns and Netgame Objective flags directly into the saved `.map` file.
- [x] **Standalone Forge Main Menu (`OpenCE Forge/menus/`)**
  - Removed standard Campaign / Multiplayer options from the main menu to focus on the standalone **FORGE** workflow while retaining **FORGE -> SINGLE PLAYER**, **HOST MULTIPLAYER**, and **JOIN MULTIPLAYER**.
- [x] **User-Editable Forge UI (`OpenCE Forge/ui/`)**
  - Added [`forge_ui.xml`](file:///C:/Users/levib/Desktop/OPEN%20CE/OpenCE-Forge/OpenCE%20Forge/ui/forge_ui.xml) allowing users to customize fonts (`fixedsys`, `conduit`, `larabie`, `handel`), text scales, text colours, and panel colours without recompiling.
  - Added editable `.png` background textures (`panel_spawn_menu.png`, `panel_top_bar.png`, `panel_bottom_bar.png`, `panel_preview.png`, `panel_modal.png`) in `OpenCE Forge/ui/`.
  - Lowered the bottom HUD bar below the radar height for unobstructed gameplay visibility.
- [x] **OpenCE Upstream Sync & Standalone Drag-and-Drop Build**
  - Synced shared engine sources with the latest upstream [OpenCE](https://github.com/d00tbeep/open-ce) repository.
  - Packaged as a non-destructive drop-in release (`HaloCE Forge.exe`, `OpenCE Forge/`, `forge_maps/`) that coexists cleanly inside an existing Halo OpenCE directory.

---

## Upcoming / Planned Ideas

- [ ] **Object Budget & Quota Meter**: Visual budget bar in the top HUD showing remaining scenario palette slots and memory headroom.
- [ ] **Multi-Select & Group Prefabs**: Select multiple Forge objects at once to move, rotate, duplicate, or save as reusable prefab templates.
- [ ] **Undo / Redo History Stack**: Quick hotkey (`Ctrl+Z` / `Ctrl+Y`) to revert recent placements, deletions, or property edits.
- [ ] **Visual Hill & Boundary Volumes**: Render translucent 3D boundary cylinders/boxes for KOTH hill zones and soft kill/return boundaries.
- [ ] **Multiplayer Live Co-Op Forging Enhancements**: Synchronize held-object highlights and live drag previews across all connected Monitor players in real time.
