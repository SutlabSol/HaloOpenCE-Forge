# Halo OpenCE Forge (`HaloCE Forge.exe`)

Standalone 3D Forge World Editor & Multiplayer Co-Op Sandbox for **Halo: Combat Evolved (OpenCE)**.

## Drag-and-Drop Installation (Does Not Replace Any Standard OpenCE Files)

This repository is structured as a **non-destructive drag-and-drop overlay** that drops directly into your existing Halo OpenCE directory without replacing or corrupting `halo.exe`, `SDL3.dll`, `maps/`, or `custom_maps/`:

```text
<Your Halo OpenCE Folder>/
|-- HaloCE Forge.exe          <-- Standalone Forge executable (leaves halo.exe & SDL3.dll untouched)
|-- OpenCE Forge/             <-- Standalone Forge menus, tools, and local tag cache
|   |-- menus/                <-- Custom Forge Main Menu (Edit Maps & Join Forges only)
|   |-- tools/                <-- forge_asset_compiler.exe
|   |-- forge_maps/           <-- Local master tag cache (built automatically on first launch)
|   +-- cached_tags/          <-- Local cached 3D tag data
|-- forge_maps/               <-- Clean folder where saved Forge maps are packaged (<map>/<map>.map, .bmp, .txt)
+-- FORGE CODE/               <-- Complete standalone C source code, headers, and build script
```

### How to Install
1. Download or clone this repository.
2. Drag and drop **`HaloCE Forge.exe`**, **`OpenCE Forge/`**, and **`forge_maps/`** directly into your existing Halo OpenCE directory (alongside `halo.exe`, `SDL3.dll`, `maps/`, and `custom_maps/`).
3. Run **`HaloCE Forge.exe`**.

---

## First-Launch Local Tag Cache Builder

So that no copyrighted `.map` files or pre-packaged tag caches need to be distributed with the build, **`HaloCE Forge.exe`** asks to build your local tag cache on first launch:
1. Point to your **Halo Custom Edition `custom_maps` folder** (`bitmaps.map`, `sounds.map`, `loc.map`, and default multiplayer `.map` files).
2. Point to your **Halo Xbox / Retail PC `maps` folder** (`bitmaps.map`, `sounds.map`, and campaign `.map` files).
3. Forge compiles `OpenCE Forge/forge_maps/forgeasset.map` locally with embedded campaign & multiplayer bitmaps, shaders, models, vehicles, weapons, and scenery.

You can also add tags from any custom map in Forge Mode via **`[F2] Spawn Menu -> [F9] Add Tags to Cache`** (`MAPS` window).

---

## Controls & Features

- **`F1`**: Toggle between **Spartan Play Mode** and **Forge Mode** (invulnerable Monitor with seamless position/facing continuity & hidden Spartan attachments).
- **`F2`**: Open **Spawn Menu** *(only available while in Forge Mode)* with live 3D textured preview and **`[F9] Add Tags to Cache`** (`MAPS`).
- **`Left Click`**: Grab / Release object within **20m**.
- **`Mouse Wheel`**: Move held object toward / away from camera (**1m to 30m**).
- **`Hold Z + Left Click`**: Hold `Z` to preview ground snap, then **Left Click** while holding `Z` to place on ground.
- **`Q` / `E`**, **`R`**, **`F`**: Rotate held object Yaw / Pitch / Roll (`G` cycles snap angle).
- **`[` / `]`**: Cycle **Teleporter Channel (`CH 0` - `CH 15`)** on hovered or held Teleporter Origin/Destination node.
- **`M`**: Cycle **Gametype Spawn Filter** (`ALL MP`, `CTF`, `SLAYER`, `ODDBALL`, `KOTH`, `RACE`) on hovered or held item.
- **`Delete / Backspace`**: Delete hovered or held object.
- **`F5`**: **Save Map** — packages `<name>.map`, `<name>.bmp`, and `<name>.txt` into `forge_maps/<name>/` alongside `maps/` and `custom_maps/`.
- **`F6`**: Toggle **2D Top-Down Blueprint Editor**.

---

## Building From Source

Place `FORGE CODE/` inside an OpenCE workspace and run:
```sh
python "FORGE CODE/build_forge.py"
```
