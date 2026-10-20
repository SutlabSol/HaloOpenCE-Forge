#!/usr/bin/env python3
"""
Build & Packaging script for OpenCE Forge ("HaloCE Forge.exe") and Forge Asset Compiler.
Packages the complete standard OpenCE Windows release plus the drop-in OpenCE Forge Add-On
inside:
  C:\\Users\\LeviB\\Projects\\OpenCE-Forge\\OpenCE Build\\
Strictly leaves all upstream files in source/ and port/ untouched.
"""

import ctypes
from ctypes import wintypes
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path
from types import SimpleNamespace

ROOT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT_DIR))

from tools.linux_build import (
    XDK_INCLUDE,
    configuration_defines,
    game_defines_and_includes,
    march_flag,
)
from tools.windows_build import (
    BUILD,
    EXPAT_DIR,
    GAME_FLAGS,
    KCP_DIR,
    LINUX_DIR,
    MONOCYPHER_DIR,
    PLATFORM_FLAGS,
    PORT_DIR,
    SDL_DIR,
    TOML_DIR,
    WINDOWS_ABI_FLAGS,
    ZLIB_DIR,
)

FORGE_DIR = ROOT_DIR / "FORGE CODE"
FORGE_BUILD_DIR = FORGE_DIR / "build"
OPENCE_BUILD_DIR = ROOT_DIR / "OpenCE Build"
FORGE_NET_VERSION = "0x461D"  # ('F' << 8) | 29


def find_llvm_tool(name: str) -> str:
    which = shutil.which(name)
    if which:
        return which
    for base in (r"C:\Program Files\LLVM\bin", r"C:\Program Files (x86)\LLVM\bin"):
        cand = os.path.join(base, f"{name}.exe")
        if os.path.isfile(cand):
            return cand
    return name


def get_cflags_sets() -> tuple[list[str], list[str]]:
    sln = SimpleNamespace(
        port_release=False,
        port_profile=False,
        port_lto="off",
        port_portable=False,
        port_pgo="off",
    )
    linux_config = json.loads((ROOT_DIR / LINUX_DIR / "port.json").read_text(encoding="utf-8"))
    abi = (
        WINDOWS_ABI_FLAGS
        + [march_flag(sln)]
        + configuration_defines(sln)
        + ["-fstack-protector-strong"]
    )
    prefix_header = PORT_DIR / "include" / "halo_windows_prefix.h"
    tags_header = BUILD / "halo_msvc_tags.h"
    crt_include = PORT_DIR / "include" / "crt"
    posix_include = PORT_DIR / "include" / "posix"
    linux_platform = Path(linux_config["platform_sources"])

    extra_game = shlex.split(game_defines_and_includes(linux_config))
    game_cflags = [
        *abi,
        *GAME_FLAGS,
        "-include", str(prefix_header),
        "-include", str(tags_header),
        f"-I{FORGE_DIR / 'include'}",
        f"-I{crt_include}",
        f"-I{PORT_DIR / 'include'}",
        "-iquote", str(LINUX_DIR / "include"),
        "-iquote", str(Path(linux_config["game_sources"])),
        *extra_game,
        f"-I{XDK_INCLUDE}",
        f"-I{SDL_DIR / 'include'}",
    ]

    platform_cflags = [
        *abi,
        *PLATFORM_FLAGS,
        "-include", str(prefix_header),
        f"-I{FORGE_DIR / 'include'}",
        f"-I{posix_include}",
        f"-I{crt_include}",
        f"-I{linux_platform}",
        f"-I{PORT_DIR / 'include'}",
        f"-I{TOML_DIR}",
        f"-I{EXPAT_DIR}",
        f"-I{KCP_DIR}",
        f"-I{MONOCYPHER_DIR}",
        f"-I{ZLIB_DIR}",
        "-iquote", str(LINUX_DIR / "include"),
        "-Isource",
        "-Isource/cseries",
        f"-I{SDL_DIR / 'include'}",
        f"-I{XDK_INCLUDE}",
    ]
    return game_cflags, platform_cflags


def compile_c(clang: str, cflags: list[str], src_path: Path, obj_path: Path, extra_flags: list[str] | None = None) -> None:
    obj_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        clang,
        *cflags,
        *(extra_flags or []),
        "-c",
        str(src_path),
        "-o",
        str(obj_path),
    ]
    res = subprocess.run(cmd, cwd=str(ROOT_DIR))
    if res.returncode != 0:
        sys.exit(res.returncode)


def write_rgba_png(path: Path, width: int, height: int, rgba_bytes: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw_rows = bytearray()
    stride = width * 4
    for y in range(height):
        raw_rows.append(0)  # Filter type 0 (None)
        raw_rows.extend(rgba_bytes[y * stride : (y + 1) * stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw_rows), 9)
    png_data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png_data)


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


def load_opence_svg_glyphs() -> dict[str, tuple[str, float]]:
    """
    Loads the exact vector glyph paths (<path id="gA" d="..."/>) from
    port/assets/menus/svg/ so Forge's menu buttons use the 100% identical
    letterforms, proportions, and cap height (21.5) as OpenCE's main menu buttons.
    """
    svg_root = ROOT_DIR / "port/assets/menus/svg"
    glyphs: dict[str, tuple[str, float]] = {}
    for svg_file in sorted(svg_root.rglob("*.svg")):
        text = svg_file.read_text(encoding="utf-8", errors="ignore")
        for letter, d_str in re.findall(r'<path id="g([A-Z])" d="([^"]+)"', text):
            if letter not in glyphs:
                xs = [float(m) for m in re.findall(r"([-0-9.]+),[-0-9.]+", d_str)]
                glyphs[letter] = (d_str, max(xs) if xs else 18.0)
    # Add matching 'J' glyph (21.5 cap height, 4.65 stroke width) for "INJECT CUSTOM TAGS"
    if "J" not in glyphs:
        d_j = (
            "M10.22,0L14.88,0L14.88,14.86C14.88,18.53 11.9,21.5 8.24,21.5"
            "L5.64,21.5C1.97,21.5 0,18.53 0,14.86L0,13.2L4.65,13.2L4.65,14.31"
            "C4.65,16.44 5.68,17.66 7.4,17.66L8.1,17.66C9.8,17.66 10.22,16.44 10.22,14.31Z"
        )
        glyphs["J"] = (d_j, 14.88)
    return glyphs


def draw_svg_glyph_to_gdi_path(gdi32, hdc, d_str: str, ox: float, oy: float, scale: float, ss: int) -> None:
    for cmd, args_str in re.findall(r"([MLCZ])([^MLCZ]*)", d_str):
        nums = [float(v) for v in re.findall(r"[-0-9.]+", args_str)]
        if cmd == "M" and len(nums) >= 2:
            px = round((ox + nums[0] * scale) * ss)
            py = round((oy + nums[1] * scale) * ss)
            gdi32.MoveToEx(hdc, px, py, None)
        elif cmd == "L" and len(nums) >= 2:
            px = round((ox + nums[0] * scale) * ss)
            py = round((oy + nums[1] * scale) * ss)
            gdi32.LineTo(hdc, px, py)
        elif cmd == "C" and len(nums) >= 6:
            pts = (POINT * 3)(
                POINT(round((ox + nums[0] * scale) * ss), round((oy + nums[1] * scale) * ss)),
                POINT(round((ox + nums[2] * scale) * ss), round((oy + nums[3] * scale) * ss)),
                POINT(round((ox + nums[4] * scale) * ss), round((oy + nums[5] * scale) * ss)),
            )
            gdi32.PolyBezierTo(hdc, pts, 3)
        elif cmd == "Z":
            gdi32.CloseFigure(hdc)


def render_forge_menu_button_pngs(menus_root: Path) -> None:
    """
    Renders the 2-frame glowing OpenCE main menu button PNGs from the exact
    OpenCE vector glyph paths (port/assets/menus/svg) with 4x4 supersampling,
    matching the exact MENU_ITEM_TOP (6.57), MENU_ITEM_SCALE (0.9037),
    MENU_ITEM_MIDDLE (130.0), dim blue (#2895ff, alpha 0.467) and highlighted
    white (#ffffff + #2895ff glow) of OpenCE's native main menu buttons.
    """
    gdi32 = ctypes.windll.gdi32
    glyphs = load_opence_svg_glyphs()

    w, h = 256, 64
    ss = 4
    sw, sh = w * ss, h * ss

    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = sw
    bmi.biHeight = -sh  # Top-down DIB
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0  # BI_RGB

    hdc = gdi32.CreateCompatibleDC(0)
    bits_ptr = ctypes.c_void_p()
    hbmp = gdi32.CreateDIBSection(hdc, ctypes.byref(bmi), 0, ctypes.byref(bits_ptr), 0, 0)
    old_bmp = gdi32.SelectObject(hdc, hbmp)
    white_brush = gdi32.GetStockObject(0)  # WHITE_BRUSH
    null_pen = gdi32.GetStockObject(8)     # NULL_PEN
    old_brush = gdi32.SelectObject(hdc, white_brush)
    old_pen = gdi32.SelectObject(hdc, null_pen)
    gdi32.SetPolyFillMode(hdc, 2)          # WINDING (matches SVG fill-rule="nonzero" for R, O, A, D, P)

    # Pair spacings matching tools/port_settings.py and port/assets/menus/svg/shell/main_menu/*.svg
    pair_spaces = {
        "FO": 1.6, "OR": 2.4, "RG": 2.2, "GE": 2.4,
        "SI": 2.4, "IN": 2.6, "NG": 2.4, "GL": 2.4, "LE": 2.1,
        "PL": 2.2, "LA": 1.2, "AY": 0.4, "YE": 1.2, "ER": 2.2,
        "MU": 2.2, "UL": 2.4, "LT": 0.8, "TI": 1.8, "IP": 2.6,
        "NJ": 2.2, "JE": 2.2, "EC": 2.2, "CT": 1.6,
        "CU": 2.2, "US": 2.2, "ST": 1.8, "TO": 1.6, "OM": 2.4,
        "TA": 0.8, "AG": 1.8, "GS": 2.2,
    }

    buttons = [
        ("ce/port/main_menu/menu_forge", "FORGE"),
        ("ce/port/main_menu/forge_select/item_single_player", "SINGLE PLAYER"),
        ("ce/port/main_menu/forge_select/item_multiplayer", "MULTIPLAYER"),
        ("ce/port/main_menu/forge_select/item_inject_tags", "ADD TAGS TO CACHE"),
    ]

    for rel_base, text in buttons:
        # Compute total unscaled width in glyph units (cap height = 21.5)
        total_unscaled = 0.0
        advances: list[float] = []
        for idx, ch in enumerate(text):
            if ch == " ":
                w_ch = 8.5
                sp = 0.0
            else:
                _, w_ch = glyphs.get(ch, glyphs["E"])
                nxt = text[idx + 1] if idx + 1 < len(text) else ""
                sp = 0.0 if (not nxt or nxt == " ") else pair_spaces.get(ch + nxt, 2.2)
            adv = w_ch + sp
            advances.append(adv)
            total_unscaled += adv

        # Use OpenCE's exact MENU_ITEM_SCALE = 0.9037 (or fit within 232px for longer 3-word submenu labels)
        scale = 0.9037
        if total_unscaled * scale > 232.0:
            scale = 232.0 / total_unscaled

        # Center horizontally around MENU_ITEM_MIDDLE = 130.0 and vertically around MENU_ITEM_TOP = 6.57
        cur_x = 130.0 - (total_unscaled * scale) / 2.0
        top_y = 6.57 + (21.5 * (0.9037 - scale)) / 2.0

        ctypes.memset(bits_ptr, 0, sw * sh * 4)
        gdi32.BeginPath(hdc)
        for idx, ch in enumerate(text):
            if ch != " " and ch in glyphs:
                d_str, _ = glyphs[ch]
                draw_svg_glyph_to_gdi_path(gdi32, hdc, d_str, cur_x, top_y, scale, ss)
            cur_x += advances[idx] * scale
        gdi32.EndPath(hdc)
        gdi32.FillPath(hdc)

        raw_bgra = ctypes.string_at(bits_ptr, sw * sh * 4)

        # Downsample 4x4 supersampled bitmap into 256x64 anti-aliased alpha mask (0..255)
        mask = [0] * (w * h)
        for y in range(h):
            for x in range(w):
                acc = 0
                base_y = y * ss
                base_x = x * ss
                for dy in range(ss):
                    row_off = (base_y + dy) * sw + base_x
                    for dx in range(ss):
                        acc += raw_bgra[(row_off + dx) * 4 + 1]
                mask[y * w + x] = acc // (ss * ss)

        # Frame 0: Normal dim blue (#2895ff at 0.467 opacity -> alpha 119)
        rgba0 = bytearray(w * h * 4)
        for i, m in enumerate(mask):
            if m > 0:
                a = (m * 119) // 255
                rgba0[i * 4 + 0] = 0x28
                rgba0[i * 4 + 1] = 0x95
                rgba0[i * 4 + 2] = 0xFF
                rgba0[i * 4 + 3] = a
        write_rgba_png(menus_root / f"{rel_base}__0.png", w, h, bytes(rgba0))

        # Frame 1: Highlighted white (#ffffff) with OpenCE's blue outer glow (#2895ff, stroke-width 6.3, stdDeviation 2.19)
        # Step A: Dilate mask by ~2.8px radius to match stroke-width="6.3"
        dilated = [0] * (w * h)
        for y in range(3, h - 3):
            for x in range(3, w - 3):
                mx = 0
                for dy in range(-3, 4):
                    r_off = (y + dy) * w + x
                    for dx in range(-3, 4):
                        if dy * dy + dx * dx <= 8:
                            v = mask[r_off + dx]
                            if v > mx:
                                mx = v
                dilated[y * w + x] = mx

        # Step B: Two-pass separable Gaussian-like blur (radius 3) matching feGaussianBlur stdDeviation="2.19"
        weights = (1, 4, 8, 10, 8, 4, 1)  # sum = 36
        tmp_blur = [0] * (w * h)
        glow = [0] * (w * h)
        for y in range(3, h - 3):
            r_off = y * w
            for x in range(3, w - 3):
                s = 0
                for k, wt in enumerate(weights):
                    s += dilated[r_off + x + (k - 3)] * wt
                tmp_blur[r_off + x] = s // 36
        for y in range(3, h - 3):
            r_off = y * w
            for x in range(3, w - 3):
                s = 0
                for k, wt in enumerate(weights):
                    s += tmp_blur[(y + (k - 3)) * w + x] * wt
                glow[r_off + x] = s // 36

        rgba1 = bytearray(w * h * 4)
        for i in range(w * h):
            m = mask[i]
            g = glow[i]
            if m == 0 and g == 0:
                continue
            # Glow opacity="0.650" (166/255) under white text fill="#fff"
            ga = (g * 166) // 255
            out_a = m + (ga * (255 - m)) // 255
            if out_a > 0:
                r = (m * 255 + ((ga * (255 - m)) // 255) * 0x28) // out_a
                gr = (m * 255 + ((ga * (255 - m)) // 255) * 0x95) // out_a
                b = 255
                rgba1[i * 4 + 0] = min(255, r)
                rgba1[i * 4 + 1] = min(255, gr)
                rgba1[i * 4 + 2] = b
                rgba1[i * 4 + 3] = min(255, out_a)
        write_rgba_png(menus_root / f"{rel_base}__1.png", w, h, bytes(rgba1))

    # Point 10: Also render 'MAPS' header PNG (512x64) for the Add Tags to Cache screen (replacing 'MODS')
    ctypes.memset(bits_ptr, 0, sw * sh * 4)
    gdi32.BeginPath(hdc)
    hdr_text = "MAPS"
    hdr_scale = 1.48
    cur_x = 6.0
    top_y = 14.0
    for idx, ch in enumerate(hdr_text):
        d_str, w_ch = glyphs.get(ch, glyphs["E"])
        draw_svg_glyph_to_gdi_path(gdi32, hdc, d_str, cur_x, top_y, hdr_scale, ss)
        cur_x += (w_ch + 2.4) * hdr_scale
    gdi32.EndPath(hdc)
    gdi32.FillPath(hdc)
    raw_hdr = ctypes.string_at(bits_ptr, sw * sh * 4)
    hw = 512
    rgba_hdr = bytearray(hw * h * 4)
    for y in range(h):
        for x in range(w):
            acc = 0
            base_y = y * ss
            base_x = x * ss
            for dy in range(ss):
                row_off = (base_y + dy) * sw + base_x
                for dx in range(ss):
                    acc += raw_hdr[(row_off + dx) * 4 + 1]
            m = acc // (ss * ss)
            if m > 0:
                di = (y * hw + x) * 4
                rgba_hdr[di + 0] = 0x28
                rgba_hdr[di + 1] = 0x95
                rgba_hdr[di + 2] = 0xFF
                rgba_hdr[di + 3] = (m * 220) // 255
    write_rgba_png(menus_root / "ce/port/main_menu/forge_select/header_maps.png", hw, h, bytes(rgba_hdr))

    gdi32.SelectObject(hdc, old_brush)
    gdi32.SelectObject(hdc, old_pen)
    gdi32.SelectObject(hdc, old_bmp)
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc)


def generate_forge_menus(forge_addon_dir: Path) -> None:
    """
    Generates `OpenCE Forge/menus/ce/main_menu.xml` and its glowing button PNGs so
    the native OpenCE menu system displays:
      - `FORGE` on the main menu (with Single Player and Multiplayer removed per Point 9)
      - `MULTIPLAYER` and `ADD TAGS TO CACHE` inside the Forge menu (with header `MAPS` per Point 10)
      - `EDIT MAPS` (with `USER CREATED` and `DOWNLOADED` tabs) inside Forge Multiplayer
    """
    menus_root = forge_addon_dir / "menus"
    render_forge_menu_button_pngs(menus_root)

    mod_rows_xml = "\n".join(
        f'\t\t<child widget="main_menu/mods/op_mod_{i}" x="20" y="{73 + 30 * (i - 1)}"/>'
        for i in range(1, 12)
    )
    edit_map_rows_xml = "\n".join(
        f'\t\t<child widget="main_menu/new_select/list_item_{i}" x="20" y="{103 + 30 * (i - 1)}"/>'
        for i in range(1, 11)
    )

    xml_content = f"""<?xml version="1.0" encoding="UTF-8"?>
<!-- OpenCE Forge native menu integration (replaces built-in ce/main_menu.xml) -->
<menus root="main_menu/main_menu">
	<bitmap name="main_menu/menu_forge">
		<frame png="ce/port/main_menu/menu_forge__0.png" width="256" height="64"/>
		<frame png="ce/port/main_menu/menu_forge__1.png" width="256" height="64"/>
	</bitmap>
	<bitmap name="main_menu/forge_select/item_single_player">
		<frame png="ce/port/main_menu/forge_select/item_single_player__0.png" width="256" height="64"/>
		<frame png="ce/port/main_menu/forge_select/item_single_player__1.png" width="256" height="64"/>
	</bitmap>
	<bitmap name="main_menu/forge_select/item_multiplayer">
		<frame png="ce/port/main_menu/forge_select/item_multiplayer__0.png" width="256" height="64"/>
		<frame png="ce/port/main_menu/forge_select/item_multiplayer__1.png" width="256" height="64"/>
	</bitmap>
	<bitmap name="main_menu/forge_select/item_inject_tags">
		<frame png="ce/port/main_menu/forge_select/item_inject_tags__0.png" width="256" height="64"/>
		<frame png="ce/port/main_menu/forge_select/item_inject_tags__1.png" width="256" height="64"/>
	</bitmap>
	<bitmap name="main_menu/forge_select/header_maps">
		<frame png="ce/port/main_menu/forge_select/header_maps.png" width="512" height="64"/>
	</bitmap>
	<widget name="main_menu/forge_select/header_maps" controller="1" left="35" top="11" width="363" height="59" bitmap="main_menu/forge_select/header_maps">
	</widget>
	<widget name="main_menu/blueline" controller="1" width="192" height="2" bitmap="bitmaps/blue">
	</widget>
	<widget name="main_menu/build_number" type="text" controller="1" width="150" height="20" font="ui\\small_ui" color="#FFFF8000">
		<data input="build number textbox only"/>
	</widget>
	<widget name="main_menu/current_profile_name" type="text" controller="1" left="10" top="446" width="340" height="34" font="ui\\large_ui" color="#FFCCCCCC">
	</widget>
	<widget name="main_menu/halo_logo" controller="1" width="640" height="256" bitmap="ui\\shell\\main_menu\\halo_logo">
	</widget>
	<widget name="main_menu/main_menu" width="640" height="480" flags="pass_unhandled_to_focused_child">
		<on event="created" run="main menu intialize" label="ring_loop"/>
		<on event="back" open="main_menu/quit_select/quit_screen"/>
		<on event="b" open="main_menu/quit_select/quit_screen"/>
		<child widget="main_menu/halo_logo" y="28"/>
		<child widget="main_menu/main_menu_select_list"/>
		<child widget="main_menu/build_number" x="450" y="400"/>
	</widget>
	<widget name="main_menu/main_menu_item_load_camp" type="text" width="256" height="33" bitmap="main_menu/menu_load_campaign" string_list="main_menu/main_menu_options" string_index="1" font="ui\\small_ui" color="#000080FF">
		<on event="a" run="new game if no plyr profiles" label="load_campaign" open="main_menu/campaign_select/campaign_menu" branch="true"/>
		<on event="start" run="new game if no plyr profiles" label="load_campaign" open="main_menu/campaign_select/campaign_menu" branch="true"/>
		<on event="left_mouse" run="mouse emit accept event"/>
		<conditional widget="main_menu/new_campaign_creating_profile" if_failed="true"/>
	</widget>
	<widget name="main_menu/main_menu_item_forge" type="text" width="256" height="33" bitmap="main_menu/menu_forge" string_list="main_menu/main_menu_options" string_index="2" font="ui\\small_ui" color="#000080FF">
		<on event="a" run="new game if no plyr profiles" open="main_menu/forge_select/forge_menu" branch="true"/>
		<on event="start" run="new game if no plyr profiles" open="main_menu/forge_select/forge_menu" branch="true"/>
		<on event="left_mouse" run="mouse emit accept event"/>
		<conditional widget="main_menu/profile_manager/multiplayer_creating_profile" if_failed="true"/>
	</widget>
	<widget name="main_menu/main_menu_item_multiplayer" type="text" width="256" height="33" bitmap="main_menu/menu_multiplayer" string_list="main_menu/main_menu_options" string_index="2" font="ui\\small_ui" color="#000080FF">
		<on event="a" run="new game if no plyr profiles" open="main_menu/multiplayer_type_select/multiplayer_type_select_screen" branch="true"/>
		<on event="start" run="new game if no plyr profiles" open="main_menu/multiplayer_type_select/multiplayer_type_select_screen" branch="true"/>
		<on event="left_mouse" run="mouse emit accept event"/>
		<conditional widget="main_menu/profile_manager/multiplayer_creating_profile" if_failed="true"/>
	</widget>
	<widget name="main_menu/main_menu_item_profile" type="text" width="256" height="33" bitmap="main_menu/menu_profile" string_list="main_menu/main_menu_options" string_index="2" font="ui\\small_ui" color="#000080FF">
		<on event="a" run="new game if no plyr profiles" open="main_menu/profile_manager/player_profile_manager" branch="true"/>
		<on event="start" run="new game if no plyr profiles" open="main_menu/multiplayer_type_select/multiplayer_type_select_screen" branch="true"/>
		<on event="left_mouse" run="mouse emit accept event"/>
		<conditional widget="main_menu/profile_manager/pm_entry_creating_profile" if_failed="true"/>
	</widget>
	<widget name="main_menu/main_menu_item_quit_game" type="text" width="256" height="33" bitmap="main_menu/menu_quit">
		<on event="start" open="main_menu/quit_select/quit_screen"/>
		<on event="a" open="main_menu/quit_select/quit_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/main_menu_item_settings" type="text" width="256" height="33" bitmap="main_menu/menu_settings" string_list="main_menu/main_menu_options" string_index="3" font="ui\\small_ui" color="#000080FF">
		<on event="a" run="profile set edit begin" label="settings" open="main_menu/settings_select/player_setup/player_profile_edit/player_profile_edit_screen" branch="true"/>
		<on event="start" run="profile set edit begin" label="settings" open="main_menu/settings_select/player_setup/player_profile_edit/player_profile_edit_screen" branch="true"/>
		<on event="left_mouse" run="mouse emit accept event"/>
		<conditional widget="main_menu/profile_manager/settings_creating_profile" if_failed="true"/>
	</widget>
	<widget name="main_menu/main_menu_list_ext_desc" controller="1" left="-1" top="-1" width="1" height="1" bitmap="bitmaps/white" color="#FF0080FF">
	</widget>
	<!-- Point 9: Remove Single Player and Multiplayer options from the main menu screen -->
	<widget name="main_menu/main_menu_select_list" type="column_list" width="640" height="480" flags="pass_unhandled_to_focused_child up_down_tabs_items" description="main_menu/main_menu_list_ext_desc">
		<data input="main menu fake animate"/>
		<on event="down"/>
		<child widget="main_menu/main_menu_item_forge" x="192" y="264"/>
		<child widget="main_menu/main_menu_item_profile" x="192" y="300"/>
		<child widget="main_menu/main_menu_item_settings" x="192" y="336"/>
		<child widget="main_menu/main_menu_item_mods" x="192" y="372"/>
		<child widget="main_menu/main_menu_item_quit_game" x="192" y="408"/>
	</widget>
	<widget name="main_menu/new_campaign_creating_profile" width="640" height="480" flags="pass_unhandled_to_focused_child no_history" auto_close="1">
		<on event="deleted" run="new campaign decision" open="main_menu/new_campaign_saving_profile"/>
	</widget>
	<widget name="main_menu/new_campaign_saving_profile" width="640" height="480" flags="pass_unhandled_to_focused_child no_history" auto_close="3000" auto_close_fade="1000">
		<on event="b" run="NULL"/>
		<on event="back" run="NULL"/>
		<on event="deleted" run="NULL" open="main_menu/difficulty_select/difficulty_select_list_screen"/>
		<child widget="error/warning_creating_player_profile_screen"/>
	</widget>

	<!-- Forge Submenu: Single Player / Multiplayer / Add Tags to Cache -->
	<widget name="main_menu/forge_select/forge_menu" width="640" height="480" flags="pass_unhandled_to_focused_child">
		<child widget="main_menu/halo_logo" y="28"/>
		<child widget="main_menu/forge_select/forge_menu_select_list"/>
	</widget>
	<widget name="main_menu/forge_select/forge_menu_select_list" type="column_list" width="640" height="480" flags="pass_unhandled_to_focused_child up_down_tabs_items">
		<on event="created" run="campaign menu init"/>
		<child widget="main_menu/forge_select/forge_item_single_player" x="192" y="265"/>
		<child widget="main_menu/forge_select/forge_item_multiplayer" x="192" y="305"/>
		<child widget="main_menu/forge_select/forge_item_inject_tags" x="192" y="345"/>
		<child widget="main_menu/campaign_select/menu_item_back" x="192" y="385"/>
	</widget>
	<widget name="main_menu/forge_select/forge_item_single_player" type="text" width="256" height="33" bitmap="main_menu/forge_select/item_single_player" string_list="main_menu/main_menu_options" string_index="1" font="ui\\small_ui" color="#000080FF">
		<on event="a" open="main_menu/solo_level_select/solo_level_select_screen"/>
		<on event="start" open="main_menu/solo_level_select/solo_level_select_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/forge_select/forge_item_multiplayer" type="text" width="256" height="33" bitmap="main_menu/forge_select/item_multiplayer" string_list="main_menu/main_menu_options" string_index="2" font="ui\\small_ui" color="#000080FF">
		<on event="a" open="main_menu/forge_select/forge_mp_type_select_screen"/>
		<on event="start" open="main_menu/forge_select/forge_mp_type_select_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/forge_select/forge_item_inject_tags" type="text" width="256" height="33" bitmap="main_menu/forge_select/item_inject_tags" string_list="main_menu/main_menu_options" string_index="3" font="ui\\small_ui" color="#000080FF">
		<on event="a" open="main_menu/forge_select/forge_inject_screen"/>
		<on event="start" open="main_menu/forge_select/forge_inject_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>

	<!-- Forge Multiplayer Menu (with EDIT MAPS replacing EDIT GAMETYPES) -->
	<widget name="main_menu/forge_select/forge_type_edit_maps_item" type="text" left="51" width="232" height="32" bitmap="bitmaps/list_item_bkd" text="EDIT MAPS" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="5">
		<on event="a" open="main_menu/forge_select/forge_edit_maps_screen"/>
		<on event="start" open="main_menu/forge_select/forge_edit_maps_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/forge_select/forge_mp_type_select_list" type="column_list" width="640" height="480" flags="pass_unhandled_to_focused_child up_down_tabs_items" description="main_menu/multiplayer_type_select/multiplayer_type_list_ext_desc">
		<data input="multiplayer type menu update desc"/>
		<child widget="main_menu/multiplayer_type_select/multiplayer_type_join_item" x="-32" y="78"/>
		<child widget="main_menu/multiplayer_type_select/multiplayer_type_join_internet_item" y="111"/>
		<child widget="main_menu/multiplayer_type_select/multiplayer_type_join_lan_item" y="144"/>
		<child widget="main_menu/multiplayer_type_select/multiplayer_type_join_direct_item" y="177"/>
		<child widget="main_menu/multiplayer_type_select/multiplayer_type_create_item" x="-32" y="210"/>
		<child widget="main_menu/multiplayer_type_select/multiplayer_type_create_internet_item" y="243"/>
		<child widget="main_menu/multiplayer_type_select/multiplayer_type_create_lan_item" y="276"/>
		<child widget="main_menu/forge_select/forge_type_edit_maps_item" y="340"/>
		<child widget="main_menu/multiplayer_type_select/multiplayer_type_button_bar" y="414"/>
	</widget>
	<widget name="main_menu/forge_select/forge_mp_type_select_screen" width="640" height="480" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/gradient">
		<on event="created" run="mp type menu initialize"/>
		<on event="created" run="campaign menu init"/>
		<child widget="main_menu/multiplayer_type_select/header_multiplayer"/>
		<child widget="main_menu/forge_select/forge_mp_type_select_list"/>
		<child widget="main_menu/blueline" x="64" y="325"/>
	</widget>

	<!-- Forge Edit Maps Screen (User Created / Downloaded tabs) -->
	<widget name="main_menu/forge_select/edit_maps_kind_row" width="256" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds_sm" font="ui\\large_ui" color="#FF2896FF" align="center" text_y="3">
		<child widget="main_menu/forge_select/edit_maps_kind_spinner" x="9" y="2"/>
	</widget>
	<widget name="main_menu/forge_select/edit_maps_kind_spinner" type="spinner" width="238" height="22" flags="left_right_tabs_items" strings="USER CREATED|DOWNLOADED" font="ui\\large_ui" color="#FF2896FF" align="center" text_y="1" header_bitmap="bitmaps/arrow_sm_left" footer_bitmap="bitmaps/arrow_sm_right" header_bounds="7 -6 19 0" footer_bounds="7 238 19 244">
		<on event="left_mouse" run="mouse spinner 1wide click"/>
	</widget>
	<widget name="main_menu/forge_select/forge_edit_maps_list" type="column_list" width="640" height="480" flags="pass_unhandled_to_focused_child up_down_tabs_children" description="main_menu/solo_level_select/solo_level_extended_desc">
		<data input="load game list update"/>
		<on event="created" run="load game menu init"/>
		<on event="custom_activation" run="load game menu activated"/>
		<child widget="main_menu/forge_select/edit_maps_kind_row" x="82" y="73"/>
{edit_map_rows_xml}
		<child widget="main_menu/solo_level_select/solo_level_select_button_bar" y="414"/>
	</widget>
	<widget name="main_menu/forge_select/forge_edit_maps_screen" width="640" height="480" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/gradient">
		<child widget="main_menu/new_select/sel_list_desc_bkd"/>
		<child widget="main_menu/forge_select/forge_edit_maps_list"/>
		<child widget="main_menu/solo_level_select/header_replay_level"/>
	</widget>

	<!-- Point 10: Forge Add Tags to Cache Screen (with header 'MAPS') -->
	<widget name="main_menu/forge_select/forge_inject_button_ok" type="text" width="128" height="24" bitmap="bitmaps/text_button_background" text="INJECT" font="ui\\small_ui" color="#FFFFFFFF" align="center" text_y="2">
		<on event="a" run="port mods save"/>
		<on event="start" run="port mods save"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/forge_select/forge_inject_button_bar" type="column_list" width="640" height="28" flags="pass_unhandled_to_focused_child left_right_tabs_items">
		<child widget="main_menu/forge_select/forge_inject_button_ok" x="380" y="1"/>
		<child widget="common_button_cancel" x="510" y="1"/>
	</widget>
	<widget name="main_menu/forge_select/forge_inject_list" type="column_list" width="640" height="480" flags="pass_unhandled_to_focused_child up_down_tabs_children" description="main_menu/mods/mod_details">
		<data input="port mods update"/>
		<on event="created" run="port mods init"/>
{mod_rows_xml}
		<child widget="main_menu/forge_select/forge_inject_button_bar" y="414"/>
	</widget>
	<widget name="main_menu/forge_select/forge_inject_screen" width="640" height="480" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/gradient">
		<on event="b" back="true"/>
		<on event="back" back="true"/>
		<child widget="main_menu/new_select/sel_list_desc_bkd"/>
		<child widget="main_menu/forge_select/forge_inject_list"/>
		<child widget="main_menu/forge_select/header_maps"/>
	</widget>
</menus>
"""
    out_xml = menus_root / "ce" / "main_menu.xml"
    out_xml.parent.mkdir(parents=True, exist_ok=True)
    out_xml.write_text(xml_content, encoding="utf-8")
    print(f"[build_forge] Generated native Forge menus: {out_xml}")


def _build_panel_png_bytes(
    width: int,
    height: int,
    top_rgb: tuple[int, int, int],
    bot_rgb: tuple[int, int, int],
    border_rgb: tuple[int, int, int],
    header_h: int = 0,
    header_rgb: tuple[int, int, int] = (20, 52, 96),
    alpha: int = 224,
) -> bytes:
    buf = bytearray(width * height * 4)
    for y in range(height):
        t = y / max(1, height - 1)
        r = round(top_rgb[0] * (1.0 - t) + bot_rgb[0] * t)
        g = round(top_rgb[1] * (1.0 - t) + bot_rgb[1] * t)
        b = round(top_rgb[2] * (1.0 - t) + bot_rgb[2] * t)
        a = alpha
        if header_h > 0 and y < header_h:
            ht = y / max(1, header_h - 1)
            r = round(header_rgb[0] * (1.15 - 0.3 * ht))
            g = round(header_rgb[1] * (1.15 - 0.3 * ht))
            b = round(header_rgb[2] * (1.15 - 0.3 * ht))
            a = min(255, alpha + 20)
        # Subtle horizontal scanline texture every 3rd row
        if y % 3 == 0:
            r = min(255, r + 5)
            g = min(255, g + 8)
            b = min(255, b + 12)
        for x in range(width):
            idx = (y * width + x) * 4
            # Outer 2px glowing border + header separator line
            if x == 0 or x == width - 1 or y == 0 or y == height - 1:
                buf[idx : idx + 4] = bytes((border_rgb[0], border_rgb[1], border_rgb[2], 245))
            elif x == 1 or x == width - 2 or y == 1 or y == height - 2 or (header_h > 0 and y == header_h):
                buf[idx : idx + 4] = bytes((
                    min(255, (border_rgb[0] + r) // 2),
                    min(255, (border_rgb[1] + g) // 2),
                    min(255, (border_rgb[2] + b) // 2),
                    235,
                ))
            else:
                buf[idx : idx + 4] = bytes((max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)), a))
    return bytes(buf)


def generate_customizable_forge_ui_assets(forge_addon_dir: Path) -> None:
    """
    Generates `OpenCE Forge/ui/forge_ui.xml` and default editable PNG background images
    (`panel_spawn_menu.png`, `panel_preview.png`, `panel_top_bar.png`, `panel_bottom_bar.png`,
    `panel_modal.png`) if they do not already exist, so the user can freely customize
    fonts, font styles, font sizes, colors, and menu background images!
    """
    ui_dir = forge_addon_dir / "ui"
    ui_dir.mkdir(parents=True, exist_ok=True)

    panels = [
        ("panel_spawn_menu.png", 260, 220, (12, 26, 48), (6, 14, 28), (68, 152, 242), 24, (22, 58, 104), 228),
        ("panel_preview.png", 260, 52, (10, 30, 42), (6, 18, 28), (68, 255, 152), 0, (16, 48, 64), 224),
        ("panel_top_bar.png", 234, 28, (14, 28, 52), (8, 16, 32), (60, 140, 230), 0, (14, 28, 52), 212),
        ("panel_bottom_bar.png", 510, 18, (12, 24, 44), (6, 14, 28), (53, 120, 196), 0, (12, 24, 44), 196),
        ("panel_modal.png", 420, 260, (14, 28, 52), (8, 16, 32), (68, 152, 242), 24, (22, 58, 104), 236),
    ]
    for filename, w, h, top_c, bot_c, brd_c, hdr_h, hdr_c, alpha in panels:
        png_path = ui_dir / filename
        if not png_path.is_file():
            rgba = _build_panel_png_bytes(w, h, top_c, bot_c, brd_c, hdr_h, hdr_c, alpha)
            write_rgba_png(png_path, w, h, rgba)

    xml_path = ui_dir / "forge_ui.xml"
    if not xml_path.is_file():
        xml_text = """<?xml version="1.0" encoding="utf-8"?>
<!--
  ============================================================================
  OpenCE Forge - Customizable In-Game UI Theme & Background Configuration
  File: OpenCE Forge/ui/forge_ui.xml
  Press [F8] in-game at any time to live-reload this file and all PNG images!
  ============================================================================

  FONT ATTRIBUTES (available on every UI section below):
    font="terminal"      Font family: "terminal", "system", "small", "large", "ticker",
                         or any loaded font tag path (e.g. "ui\\small_ui", "ui\\large_ui").
    style="plain"        Font style: "plain", "bold", "italic", "condense", "underline".
    size="1.0"           Font size multiplier (e.g. "0.85", "1.0", "1.2", "1.5").
    title_color="#..."   Header / title text color (#RRGGBB or #AARRGGBB hex).
    text_color="#..."    Normal item / body text color (#RRGGBB or #AARRGGBB hex).
    selected_color="#..." Highlighted / selected item text color (#RRGGBB or #AARRGGBB hex).

  BACKGROUND & PANEL ATTRIBUTES:
    background="panel_spawn_menu.png"
                         PNG image file inside `OpenCE Forge/ui/` drawn behind the menu.
                         Edit these PNGs in Photoshop/GIMP/Paint.NET and press [F8] in-game!
    use_image="true"     Set "true" to draw the PNG image, or "false" to use solid bg_color.
    image_tint="#FFFFFFFF"
                         Optional ARGB tint multiplied with the PNG background (#FFFFFFFF = pure PNG).
    bg_color="#E00A1526" Solid fallback background color when use_image="false".
    header_color="#00000000"
                         Optional header overlay color (#00000000 = transparent so PNG header shows).
    highlight_color="#D8225C9C"
                         Selected row highlight bar color.
    border_color="#00000000"
                         Optional extra border color (#00000000 = use the border in the PNG image).
-->
<forge_ui>
    <!-- [F2] Forge Object Spawn Palette Menu (Left-Aligned Category & Item List) -->
    <spawn_menu
        font="terminal"
        style="plain"
        size="1.0"
        title_color="#FFA6EBFF"
        text_color="#FFC7DBF5"
        selected_color="#FFFFF5A6"
        background="panel_spawn_menu.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E00A1526"
        header_color="#6014325A"
        highlight_color="#D8225C9C"
        border_color="#804498F2"
    />

    <!-- Live 3D Item Preview Card (Below the [F2] Item List) -->
    <preview_panel
        font="terminal"
        style="plain"
        size="1.0"
        title_color="#FF80FFB8"
        text_color="#FFC7E0FA"
        selected_color="#FFFFEB8C"
        background="panel_preview.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#D80A1526"
        header_color="#00000000"
        highlight_color="#D8225C9C"
        border_color="#8044FF98"
    />

    <!-- [X] Object & Teleporter Properties Panel -->
    <properties_menu
        font="terminal"
        style="plain"
        size="1.0"
        title_color="#FFA6EBFF"
        text_color="#FFD1E0F5"
        selected_color="#FFFFF5A6"
        background="panel_modal.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E00A1526"
        header_color="#6014325A"
        highlight_color="#D8225C9C"
        border_color="#804498F2"
    />

    <!-- Top-Right Forge Mode Status & Object Budget Pill -->
    <top_bar
        font="terminal"
        style="plain"
        size="1.0"
        title_color="#FF8CE0FF"
        text_color="#FFD1E0F2"
        selected_color="#FFFFF5A6"
        background="panel_top_bar.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#CC0A1426"
        header_color="#00000000"
        border_color="#803C8CE6"
    />

    <!-- Bottom Control Strip ([F1] Forge Mode / [F2] Menu / [F5] Save) -->
    <bottom_bar
        font="terminal"
        style="plain"
        size="1.0"
        title_color="#FFA6EBFF"
        text_color="#FFC7E5FF"
        selected_color="#FFFFF5A6"
        background="panel_bottom_bar.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#B8081222"
        header_color="#00000000"
        border_color="#603578C4"
    />

    <!-- [F5] Initial Save Map Name Modal -->
    <save_modal
        font="terminal"
        style="plain"
        size="1.0"
        title_color="#FFA6EBFF"
        text_color="#FFB8E0FA"
        selected_color="#FFFFF2A6"
        background="panel_modal.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#EC0A1526"
        header_color="#6014325A"
        highlight_color="#D8225C9C"
        border_color="#804498F2"
    />

    <!-- [F6] MAPS: Add Tags to Cache Modal -->
    <maps_modal
        font="terminal"
        style="plain"
        size="1.0"
        title_color="#FFA6EBFF"
        text_color="#FFD1DBEB"
        selected_color="#FFFFF299"
        background="panel_modal.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E60A1526"
        header_color="#6014325A"
        highlight_color="#D8225C9C"
        border_color="#804498F2"
    />

    <!-- Status Notification Toast Bar -->
    <toast_bar
        font="terminal"
        style="plain"
        size="1.0"
        title_color="#FFA6F2FF"
        text_color="#FFA6F2FF"
        selected_color="#FFFFFFFF"
        background="panel_bottom_bar.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#D80A182C"
        header_color="#00000000"
        border_color="#804498F2"
    />

    <!-- 3D Teleporter In/Out, Objective Markers & Reticle Hover Labels -->
    <markers
        font="terminal"
        style="plain"
        size="1.0"
        title_color="#F259FF8C"
        text_color="#F2FFE673"
        selected_color="#F273FFA6"
        background="none"
        use_image="false"
    />
</forge_ui>
"""
        xml_path.write_text(xml_text, encoding="utf-8")
    print(f"[build_forge] Ensured customizable Forge UI XML & PNG backgrounds in: {ui_dir}")



def build_asset_compiler() -> Path:
    clang = find_llvm_tool("clang")
    game_cflags, _ = get_cflags_sets()
    cb_obj = FORGE_BUILD_DIR / "forge_cache_builder.o"
    ac_obj = FORGE_BUILD_DIR / "forge_asset_compiler.o"
    exe_out = FORGE_BUILD_DIR / "forge_asset_compiler.exe"

    print("[build_forge] Compiling forge_cache_builder.c...")
    compile_c(clang, game_cflags, FORGE_DIR / "src" / "forge_cache_builder.c", cb_obj)
    print("[build_forge] Compiling forge_asset_compiler.c...")
    compile_c(clang, game_cflags, FORGE_DIR / "tools" / "forge_asset_compiler.c", ac_obj)

    obj_root = ROOT_DIR / "build" / "windows" / "obj"
    link_cmd = [
        clang,
        "--target=i686-pc-windows-msvc",
        "-fuse-ld=lld",
        "-g",
        "-Wl,/LARGEADDRESSAWARE",
        "-Wl,/STACK:0x1000000",
        str(ac_obj),
        str(cb_obj),
        str(obj_root / "port" / "linux" / "game" / "cache_file_formats.o"),
        str(obj_root / "port" / "linux" / "game" / "tag_validate.o"),
        *[str(p) for p in sorted((obj_root / "port" / "linux" / "game").glob("tag_schema_*.o"))],
        "-o",
        str(exe_out),
    ]
    print("[build_forge] Linking forge_asset_compiler.exe...")
    res = subprocess.run(link_cmd, cwd=str(ROOT_DIR))
    if res.returncode != 0:
        sys.exit(res.returncode)
    print(f"[build_forge] Built {exe_out}")
    return exe_out


def package_opence_build_folder(haloce_forge_exe: Path) -> None:
    """
    Populates `C:\\Users\\LeviB\\Projects\\OpenCE-Forge\\OpenCE Build\\` with:
      1. All standard OpenCE release files (as tools/ci_build.py produces)
      2. Game maps (`maps/` and `custom_maps/`)
      3. The drag-and-drop OpenCE Forge Add-On (`HaloCE Forge.exe` + `OpenCE Forge/`)
    """
    print(f"[build_forge] Packaging standard OpenCE release + Forge Add-On in: {OPENCE_BUILD_DIR}")
    OPENCE_BUILD_DIR.mkdir(parents=True, exist_ok=True)

    # 1. Standard OpenCE Windows release files (matching tools/ci_build.py)
    standard_files = [
        (ROOT_DIR / "build/windows/halo.exe", OPENCE_BUILD_DIR / "halo.exe"),
        (ROOT_DIR / "build/windows/SDL3.dll", OPENCE_BUILD_DIR / "SDL3.dll"),
        (ROOT_DIR / "port/assets/network/brokers.txt", OPENCE_BUILD_DIR / "brokers.txt"),
        (ROOT_DIR / "port/third_party/extract-xiso/LICENSE.TXT", OPENCE_BUILD_DIR / "extract-xiso-LICENSE.txt"),
        (ROOT_DIR / "port/third_party/mbedtls/LICENSE", OPENCE_BUILD_DIR / "mbedtls-LICENSE.txt"),
        (ROOT_DIR / "port/third_party/miniupnpc/LICENSE", OPENCE_BUILD_DIR / "miniupnpc-LICENSE.txt"),
        (ROOT_DIR / "port/assets/fonts/Overpass-OFL.txt", OPENCE_BUILD_DIR / "Overpass-OFL.txt"),
        (ROOT_DIR / "port/third_party/expat/COPYING", OPENCE_BUILD_DIR / "expat-COPYING.txt"),
        (ROOT_DIR / "port/third_party/opus/COPYING", OPENCE_BUILD_DIR / "opus-COPYING.txt"),
        (ROOT_DIR / "port/assets/icons/lucide/LICENSE", OPENCE_BUILD_DIR / "lucide-LICENSE.txt"),
    ]
    for src, dst in standard_files:
        if src.is_file():
            shutil.copy2(src, dst)

    # 2. Populate standard `maps/` folder inside `OpenCE Build/maps/` if Xbox maps exist
    xbox_maps_src = FORGE_DIR / "GAME MAPS" / "XBOX MAPS"
    maps_dst = OPENCE_BUILD_DIR / "maps"
    maps_dst.mkdir(parents=True, exist_ok=True)
    if xbox_maps_src.is_dir():
        for mp in sorted(xbox_maps_src.glob("*.map")):
            target = maps_dst / mp.name
            if not target.is_file():
                shutil.copy2(mp, target)

    # 3. Populate `custom_maps/` and `forge_maps/` inside `OpenCE Build/`
    custom_maps_dst = OPENCE_BUILD_DIR / "custom_maps"
    custom_maps_dst.mkdir(parents=True, exist_ok=True)
    saved_forge_maps_dst = OPENCE_BUILD_DIR / "forge_maps"
    saved_forge_maps_dst.mkdir(parents=True, exist_ok=True)

    ce_default_src = FORGE_DIR / "GAME MAPS" / "CUSTOM EDITION DEFAULT MAPS"
    if ce_default_src.is_dir():
        for res_name in ("bitmaps.map", "sounds.map", "loc.map"):
            s = ce_default_src / res_name
            d = custom_maps_dst / res_name
            if s.is_file() and not d.is_file():
                shutil.copy2(s, d)

    # Point 11: Migrate any saved Forge map/bmp/txt files in custom_maps/ into forge_maps/<name>/ to keep custom_maps/ clean
    for map_file in list(custom_maps_dst.glob("*.map")):
        if map_file.name.lower() in ("bitmaps.map", "sounds.map", "loc.map"):
            continue
        stem = map_file.stem
        txt_file = custom_maps_dst / f"{stem}.txt"
        is_forge = False
        try:
            if txt_file.is_file() and "[OpenCE Forge Map]" in txt_file.read_text(encoding="utf-8", errors="ignore"):
                is_forge = True
            else:
                with map_file.open("rb") as mf:
                    hdr = mf.read(512)
                    if len(hdr) >= 512 and hdr[0x1FC:0x200] == b"1GLF":
                        is_forge = True
        except Exception:
            pass
        if is_forge:
            sub = saved_forge_maps_dst / stem
            sub.mkdir(parents=True, exist_ok=True)
            for ext in (".map", ".bmp", ".txt"):
                src_f = custom_maps_dst / f"{stem}{ext}"
                if src_f.is_file():
                    shutil.move(str(src_f), str(sub / f"{stem}{ext}"))

    # 4. Install the Drag-and-Drop Forge Add-On (`HaloCE Forge.exe` + `OpenCE Forge/`) into `OpenCE Build/`
    forge_addon_dir = OPENCE_BUILD_DIR / "OpenCE Forge"
    forge_maps_dir = forge_addon_dir / "forge_maps"
    forge_tools_dir = forge_addon_dir / "tools"
    forge_maps_dir.mkdir(parents=True, exist_ok=True)
    forge_tools_dir.mkdir(parents=True, exist_ok=True)

    root_forge_dir = ROOT_DIR / "OpenCE Forge"
    if root_forge_dir.is_dir():
        root_fm = root_forge_dir / "forge_maps"
        if root_fm.is_dir():
            for f in root_fm.iterdir():
                if f.is_file():
                    dst_f = forge_maps_dir / f.name
                    if not dst_f.is_file() or f.name in ("forgeasset.map", "customforgeasset.map"):
                        shutil.copy2(f, dst_f)
        if (root_forge_dir / "credits.md").is_file():
            shutil.copy2(root_forge_dir / "credits.md", forge_addon_dir / "credits.md")

    # Ensure bloodgulch.map is also in OpenCE Forge/forge_maps/ as a self-contained base scenario fallback
    bg_src = ce_default_src / "bloodgulch.map"
    bg_dst = forge_maps_dir / "bloodgulch.map"
    if bg_src.is_file() and not bg_dst.is_file():
        shutil.copy2(bg_src, bg_dst)

    generate_forge_menus(forge_addon_dir)
    generate_customizable_forge_ui_assets(forge_addon_dir)
    generate_cached_tags_folder(forge_addon_dir)

    shutil.copy2(FORGE_DIR / "README.md", forge_addon_dir / "README.md")
    shutil.copy2(FORGE_DIR / "projectrules.md", forge_addon_dir / "projectrules.md")

    ac_exe = FORGE_BUILD_DIR / "forge_asset_compiler.exe"
    if ac_exe.is_file():
        shutil.copy2(ac_exe, forge_tools_dir / "forge_asset_compiler.exe")

    # Clean up old root-level build artifacts so everything is cleanly inside `OpenCE Build/`
    for stray_file in ("HaloCE Forge.exe", "HaloCE Forge.pdb", "halo.exe", "SDL3.dll", "brokers.txt"):
        p = ROOT_DIR / stray_file
        if p.is_file():
            p.unlink()
    for stray_dir in ("OpenCE Forge", "custom_maps"):
        d = ROOT_DIR / stray_dir
        if d.is_dir():
            shutil.rmtree(d)


def _extract_shader_diffuse_bitmap_name(
    tag_buf: bytes,
    inst_off: int,
    tag_count: int,
    shdr_grp: str,
    shdr_data_addr: int,
) -> str:
    tag_sz = len(tag_buf)
    if shdr_data_addr < 0x40440000 or shdr_data_addr >= 0x40440000 + tag_sz:
        return ""
    s_off = shdr_data_addr - 0x40440000
    # Primary base_map offsets first: 0xA4 for 'soso' (shader_model), 0x88 for 'senv' (shader_environment)
    cand_offsets: list[int] = []
    if shdr_grp == "soso":
        cand_offsets.append(0xA4)
    elif shdr_grp == "senv":
        cand_offsets.append(0x88)
    for disp in range(0x28, 0x160, 4):
        if disp not in cand_offsets:
            cand_offsets.append(disp)

    for disp in cand_offsets:
        if s_off + disp + 16 > tag_sz:
            continue
        r_grp, r_name_addr, _, r_idx = struct.unpack_from("<IIIi", tag_buf, s_off + disp)
        if r_grp == 0x6269746D and r_idx != -1:
            bname = ""
            if 0x40440000 <= r_name_addr < 0x40440000 + tag_sz:
                end_n = tag_buf.find(b"\x00", r_name_addr - 0x40440000)
                if end_n != -1:
                    bname = tag_buf[r_name_addr - 0x40440000 : end_n].decode("latin1", errors="ignore")
            if not bname and 0 <= (r_idx & 0xFFFF) < tag_count:
                tio = inst_off + (r_idx & 0xFFFF) * 32
                if tio + 32 <= tag_sz:
                    (_, _, _, _, b_name_addr, _) = struct.unpack_from("<IIIIII", tag_buf, tio)
                    if 0x40440000 <= b_name_addr < 0x40440000 + tag_sz:
                        end_n = tag_buf.find(b"\x00", b_name_addr - 0x40440000)
                        if end_n != -1:
                            bname = tag_buf[b_name_addr - 0x40440000 : end_n].decode("latin1", errors="ignore")
            if bname and "cubemap" not in bname.lower() and "multipurpose" not in bname.lower() and "vector" not in bname.lower():
                return bname
    return ""


def _extract_ftag_from_cache(
    f_data: bytes,
    tag_buf: bytes,
    inst_off: int,
    tag_count: int,
    vertex_data_off: int,
    index_data_rel_off: int,
    grp_str: str,
    tag_path: str,
    mod_data_addr: int,
    is_gbxmodel: int,
    radius: float,
    out_ftag: Path,
    shader_bitmaps: dict[str, str] | None = None,
) -> bool:
    tag_sz = len(tag_buf)
    if mod_data_addr < 0x40440000 or (mod_data_addr - 0x40440000) + 232 > tag_sz:
        return False
    mod_off = mod_data_addr - 0x40440000
    reg_count, reg_addr = struct.unpack_from("<iI", tag_buf, mod_off + 196)
    geom_count, geom_addr = struct.unpack_from("<iI", tag_buf, mod_off + 208)
    shdr_count, shdr_addr = struct.unpack_from("<iI", tag_buf, mod_off + 220)
    if geom_count <= 0 or geom_addr < 0x40440000 or (geom_addr - 0x40440000) + geom_count * 48 > tag_sz:
        return False

    geom_list: list[int] = []
    if 0 < reg_count <= 32 and reg_addr >= 0x40440000 and (reg_addr - 0x40440000) + reg_count * 76 <= tag_sz:
        for r in range(reg_count):
            r_off = (reg_addr - 0x40440000) + r * 76
            perm_count, perm_addr = struct.unpack_from("<iI", tag_buf, r_off + 64)
            if perm_count > 0 and perm_addr >= 0x40440000 and (perm_addr - 0x40440000) + 88 <= tag_sz:
                perm0_off = perm_addr - 0x40440000
                for lod in range(4, -1, -1):
                    (gi,) = struct.unpack_from("<h", tag_buf, perm0_off + 64 + lod * 2)
                    if 0 <= gi < geom_count:
                        geom_list.append(gi)
                        break
    if not geom_list:
        geom_list = [0]

    part_stride = 132 if is_gbxmodel else 104
    parts_out: list[tuple[int, int, bytes, bytes, bytes]] = []
    for g_idx in geom_list:
        g_off = (geom_addr - 0x40440000) + g_idx * 48
        part_count, part_addr = struct.unpack_from("<iI", tag_buf, g_off + 36)
        if part_count <= 0 or part_addr < 0x40440000 or (part_addr - 0x40440000) + part_count * part_stride > tag_sz:
            continue
        for p in range(part_count):
            if len(parts_out) >= 24:
                break
            p_off = (part_addr - 0x40440000) + p * part_stride
            (shdr_idx,) = struct.unpack_from("<h", tag_buf, p_off + 4)
            tri_count, tri_file_off = struct.unpack_from("<iI", tag_buf, p_off + 72)
            (vtx_count,) = struct.unpack_from("<i", tag_buf, p_off + 88)
            (vtx_file_off,) = struct.unpack_from("<I", tag_buf, p_off + 100)
            if tri_count <= 0 or vtx_count <= 0 or tri_count > 32768 or vtx_count > 32768:
                continue

            shader_hint = ""
            if 0 <= shdr_idx < shdr_count and shdr_addr >= 0x40440000 and (shdr_addr - 0x40440000) + (shdr_idx + 1) * 32 <= tag_sz:
                s_off = (shdr_addr - 0x40440000) + shdr_idx * 32
                (s_name_addr,) = struct.unpack_from("<I", tag_buf, s_off + 4)
                if 0x40440000 <= s_name_addr < 0x40440000 + tag_sz:
                    end_nul = tag_buf.find(b"\x00", s_name_addr - 0x40440000)
                    if end_nul != -1:
                        shader_hint = tag_buf[s_name_addr - 0x40440000 : end_nul].decode("latin1", errors="ignore")
                        if shader_bitmaps and shader_hint.lower() in shader_bitmaps:
                            bmp_name = shader_bitmaps[shader_hint.lower()]
                            if bmp_name:
                                shader_hint = f"{shader_hint}|{bmp_name}"

            uncomp_sz = vtx_count * 68
            idx_sz = (tri_count + 2) * 2
            if vtx_file_off >= 0x40440000 and (vtx_file_off - 0x40440000) + uncomp_sz <= tag_sz:
                v_bytes = tag_buf[vtx_file_off - 0x40440000 : vtx_file_off - 0x40440000 + uncomp_sz]
            elif vertex_data_off + vtx_file_off + uncomp_sz <= len(f_data):
                v_bytes = f_data[vertex_data_off + vtx_file_off : vertex_data_off + vtx_file_off + uncomp_sz]
            else:
                continue

            idx_base = vertex_data_off + index_data_rel_off
            if tri_file_off >= 0x40440000 and (tri_file_off - 0x40440000) + idx_sz <= tag_sz:
                i_bytes = tag_buf[tri_file_off - 0x40440000 : tri_file_off - 0x40440000 + idx_sz]
            elif idx_base + tri_file_off + idx_sz <= len(f_data):
                i_bytes = f_data[idx_base + tri_file_off : idx_base + tri_file_off + idx_sz]
            else:
                continue

            sh_bytes = shader_hint.encode("ascii", errors="ignore")[:127].ljust(128, b"\x00")
            parts_out.append((vtx_count, tri_count, sh_bytes, v_bytes, i_bytes))

    if not parts_out:
        return False

    out_ftag.parent.mkdir(parents=True, exist_ok=True)
    grp_int = (ord(grp_str[0]) << 24) | (ord(grp_str[1]) << 16) | (ord(grp_str[2]) << 8) | ord(grp_str[3])
    tname_bytes = tag_path.encode("ascii", errors="ignore")[:255].ljust(256, b"\x00")
    with out_ftag.open("wb") as wf:
        wf.write(b"FTAG")
        wf.write(struct.pack("<IIf", 1, grp_int, radius if 0.05 < radius < 100.0 else 1.5))
        wf.write(tname_bytes)
        wf.write(struct.pack("<I", len(parts_out)))
        for vtx_c, tri_c, sh_b, v_b, i_b in parts_out:
            wf.write(struct.pack("<ii", vtx_c, tri_c))
            wf.write(sh_b)
            wf.write(v_b)
            wf.write(i_b)
    return True


def generate_cached_tags_folder(forge_addon_dir: Path) -> None:
    """
    Populates `OpenCE Forge/cached_tags/` by pulling every Forge-placeable tag
    (weapons, vehicles, scenery, devices) and its 3D model + diffuse texture hint
    from:
      1. All 10 Campaign maps (`FORGE CODE/GAME MAPS/PC MAPS/a10.map` .. `d40.map`)
      2. All 19 Default Multiplayer maps (`CUSTOM EDITION DEFAULT MAPS/` & `PC MAPS/`)
      3. `OpenCE Forge/forge_maps/forgeasset.map`
      4. `OpenCE Forge/forge_maps/customforgeasset.map`
    """
    cached_root = forge_addon_dir / "cached_tags"
    for sub in ("scenery", "vehicles", "weapons", "bipeds", "devices", "ai"):
        (cached_root / sub).mkdir(parents=True, exist_ok=True)

    # Point 3: Exclude bipeds ('bipd') and AI ('actv') from the spawn catalog
    cat_map = {
        "weap": (0, "weapons"),
        "vehi": (1, "vehicles"),
        "scen": (3, "scenery"),
        "mach": (4, "devices"),
        "ctrl": (4, "devices"),
        "lifi": (4, "devices"),
    }
    shader_grps = {"soso", "senv", "sgla", "schi", "scex", "swat", "smet", "spla", "sotr"}

    catalog_lines: list[str] = [
        "# OpenCE Forge Cached Tag Folder System Manifest",
        "# Format: <group_tag>|<category_id>|<source_cache_id>|<tag_path>",
    ]
    seen_catalog_keys: set[tuple[str, str]] = set()
    extracted_ftags: set[Path] = set()
    shader_bitmaps: dict[str, str] = {}

    # Collect all source maps: Campaign + Default MP maps FIRST (they contain complete shader & bitmap tables),
    # followed by forgeasset.map and customforgeasset.map!
    source_maps: list[tuple[int, Path]] = []

    pc_maps_dir = FORGE_DIR / "GAME MAPS" / "PC MAPS"
    if pc_maps_dir.is_dir():
        for mp in sorted(pc_maps_dir.glob("*.map")):
            if mp.name.lower() not in ("bitmaps.map", "sounds.map", "loc.map", "ui.map"):
                source_maps.append((0, mp))

    ce_maps_dir = FORGE_DIR / "GAME MAPS" / "CUSTOM EDITION DEFAULT MAPS"
    if ce_maps_dir.is_dir():
        for mp in sorted(ce_maps_dir.glob("*.map")):
            if mp.name.lower() not in ("bitmaps.map", "sounds.map", "loc.map", "ui.map"):
                source_maps.append((0, mp))

    fa_path = forge_addon_dir / "forge_maps" / "forgeasset.map"
    if fa_path.is_file():
        source_maps.append((0, fa_path))

    cfa_path = forge_addon_dir / "forge_maps" / "customforgeasset.map"
    if cfa_path.is_file():
        source_maps.append((1, cfa_path))

    # PASS 1: Collect all shader -> diffuse bitmap mappings across every Campaign and MP map first!
    for _, map_path in source_maps:
        if not map_path.is_file():
            continue
        f_data = map_path.read_bytes()
        if len(f_data) < 2048:
            continue
        tag_off, tag_sz = struct.unpack_from("<II", f_data, 0x10)
        if tag_off < 2048 or tag_off + tag_sz > len(f_data):
            continue
        tag_buf = f_data[tag_off : tag_off + tag_sz]
        inst_addr, _, _, tag_count = struct.unpack_from("<IIII", tag_buf, 0)
        if inst_addr < 0x40440000 or tag_count <= 0 or tag_count > 16384:
            continue
        inst_off = inst_addr - 0x40440000
        for i in range(tag_count):
            io = inst_off + i * 32
            if io + 32 > tag_sz:
                break
            grp_u32, _, _, _, name_addr, data_addr = struct.unpack_from("<IIIIII", tag_buf, io)
            grp_s = struct.pack(">I", grp_u32).decode("latin1", errors="ignore")
            if grp_s in shader_grps and 0x40440000 <= name_addr < 0x40440000 + tag_sz:
                end_n = tag_buf.find(b"\x00", name_addr - 0x40440000)
                if end_n != -1:
                    tname = tag_buf[name_addr - 0x40440000 : end_n].decode("latin1", errors="ignore").lower()
                    if tname and tname not in shader_bitmaps:
                        bmp_name = _extract_shader_diffuse_bitmap_name(tag_buf, inst_off, tag_count, grp_s, data_addr)
                        if bmp_name:
                            shader_bitmaps[tname] = bmp_name

    monitor_extracted = False

    # PASS 2: Extract .ftag files with complete shader|bitmap hints
    for src_id, map_path in source_maps:
        if not map_path.is_file():
            continue
        f_data = map_path.read_bytes()
        if len(f_data) < 2048:
            continue
        tag_off, tag_sz = struct.unpack_from("<II", f_data, 0x10)
        if tag_off < 2048 or tag_off + tag_sz > len(f_data):
            continue
        tag_buf = f_data[tag_off : tag_off + tag_sz]
        inst_addr, _, _, tag_count, _, vertex_data_off, _, index_data_rel_off = struct.unpack_from("<IIIIIIII", tag_buf, 0)
        if inst_addr < 0x40440000 or tag_count <= 0 or tag_count > 16384:
            continue
        inst_off = inst_addr - 0x40440000

        models_by_idx: dict[int, tuple[int, int]] = {}
        models_by_name: dict[str, tuple[int, int]] = {}
        for i in range(tag_count):
            io = inst_off + i * 32
            if io + 32 > tag_sz:
                break
            grp_u32, _, _, _, name_addr, data_addr = struct.unpack_from("<IIIIII", tag_buf, io)
            grp_s = struct.pack(">I", grp_u32).decode("latin1", errors="ignore")
            if 0x40440000 <= name_addr < 0x40440000 + tag_sz:
                end_n = tag_buf.find(b"\x00", name_addr - 0x40440000)
                tname = tag_buf[name_addr - 0x40440000 : end_n].decode("latin1", errors="ignore") if end_n != -1 else ""
            else:
                tname = ""
            if grp_s in ("mod2", "mode"):
                is_gbx = 1 if grp_s == "mod2" else 0
                models_by_idx[i] = (data_addr, is_gbx)
                if tname:
                    models_by_name[tname.lower()] = (data_addr, is_gbx)

        # Extract 343 Guilty Spark ('characters\monitor\monitor') into bipeds/characters_monitor_monitor.ftag for Monitor mode
        if not monitor_extracted and "characters\\monitor\\monitor" in models_by_name:
            m_addr, m_gbx = models_by_name["characters\\monitor\\monitor"]
            if _extract_ftag_from_cache(
                f_data, tag_buf, inst_off, tag_count, vertex_data_off, index_data_rel_off,
                "bipd", "characters\\monitor\\monitor", m_addr, m_gbx, 1.0,
                cached_root / "bipeds" / "characters_monitor_monitor.ftag",
                shader_bitmaps,
            ):
                monitor_extracted = True

        for i in range(tag_count):
            io = inst_off + i * 32
            if io + 32 > tag_sz:
                break
            grp_u32, _, _, _, name_addr, data_addr = struct.unpack_from("<IIIIII", tag_buf, io)
            grp_s = struct.pack(">I", grp_u32).decode("latin1", errors="ignore")
            if grp_s not in cat_map:
                continue
            if not (0x40440000 <= name_addr < 0x40440000 + tag_sz):
                continue
            end_n = tag_buf.find(b"\x00", name_addr - 0x40440000)
            if end_n == -1:
                continue
            tname = tag_buf[name_addr - 0x40440000 : end_n].decode("latin1", errors="ignore")
            if not tname:
                continue
            tname_low = tname.lower()
            if grp_s == "weap" and (
                "flag" in tname_low
                or "ball" in tname_low
                or "target_laser" in tname_low
                or "double_speed" in tname_low
            ):
                continue

            cat_id, sub_folder = cat_map[grp_s]
            slug = "".join("_" if c in "\\/ :" else c for c in tname)
            out_ftag = cached_root / sub_folder / f"{slug}.ftag"

            if out_ftag not in extracted_ftags and data_addr >= 0x40440000 and (data_addr - 0x40440000) + 0x38 <= tag_sz:
                obj_off = data_addr - 0x40440000
                (rad,) = struct.unpack_from("<f", tag_buf, obj_off + 0x04)
                (mod_name_addr,) = struct.unpack_from("<I", tag_buf, obj_off + 0x2C)
                (mod_idx,) = struct.unpack_from("<i", tag_buf, obj_off + 0x34)
                mod_info = None
                if mod_idx != -1 and (mod_idx & 0xFFFF) in models_by_idx:
                    mod_info = models_by_idx[mod_idx & 0xFFFF]
                elif 0x40440000 <= mod_name_addr < 0x40440000 + tag_sz:
                    en2 = tag_buf.find(b"\x00", mod_name_addr - 0x40440000)
                    if en2 != -1:
                        mname = tag_buf[mod_name_addr - 0x40440000 : en2].decode("latin1", errors="ignore").lower()
                        mod_info = models_by_name.get(mname)
                if mod_info:
                    if _extract_ftag_from_cache(
                        f_data, tag_buf, inst_off, tag_count, vertex_data_off, index_data_rel_off,
                        grp_s, tname, mod_info[0], mod_info[1], rad, out_ftag, shader_bitmaps,
                    ):
                        extracted_ftags.add(out_ftag)

            key = (grp_s, tname_low)
            if key not in seen_catalog_keys and out_ftag.is_file():
                seen_catalog_keys.add(key)
                catalog_lines.append(f"{grp_s}|{cat_id}|{src_id}|{tname}")

    (cached_root / "catalog.txt").write_text("\n".join(catalog_lines) + "\n", encoding="utf-8")
    compile_objective_marker_objs_to_ftags(forge_addon_dir)
    print(
        f"[build_forge] Populated Cached Tag Folder System ({cached_root}) from {len(source_maps)} maps "
        f"with {len(extracted_ftags)} 3D .ftag bundles, {len(shader_bitmaps)} shader bitmap hints & {len(seen_catalog_keys)} catalog entries"
    )


def _pack_ftag_parts_from_triangles(
    verts_pos_norm: list[tuple[float, float, float, float, float, float]],
    triangles: list[tuple[int, int, int]],
    shader_hint: str = "forge\\marker_translucent",
    max_tris_per_part: int = 4800,
) -> list[tuple[int, int, bytes, bytes, bytes]]:
    parts_out: list[tuple[int, int, bytes, bytes, bytes]] = []
    sh_bytes = shader_hint.encode("ascii", errors="ignore")[:127].ljust(128, b"\x00")

    for start in range(0, len(triangles), max_tris_per_part):
        batch = triangles[start : start + max_tris_per_part]
        if not batch:
            continue

        local_remap: dict[int, int] = {}
        local_verts = bytearray()
        local_tris: list[tuple[int, int, int]] = []

        for g0, g1, g2 in batch:
            for g in (g0, g1, g2):
                if g not in local_remap:
                    local_remap[g] = len(local_remap)
                    x, y, z, nx, ny, nz = verts_pos_norm[g]
                    if abs(nz) < 0.9:
                        bx, by, bz = -ny, nx, 0.0
                    else:
                        bx, by, bz = 0.0, -nz, ny
                    bl = (bx * bx + by * by + bz * bz) ** 0.5
                    if bl > 1e-6:
                        bx /= bl
                        by /= bl
                        bz /= bl
                    else:
                        bx, by, bz = 1.0, 0.0, 0.0
                    tx = ny * bz - nz * by
                    ty = nz * bx - nx * bz
                    tz = nx * by - ny * bx
                    local_verts.extend(
                        struct.pack(
                            "<ffffffffffffffhhff",
                            x, y, z,
                            nx, ny, nz,
                            bx, by, bz,
                            tx, ty, tz,
                            0.5, 0.5,
                            0, 0,
                            1.0, 0.0,
                        )
                    )
            local_tris.append((local_remap[g0], local_remap[g2], local_remap[g1]))

        if not local_tris:
            continue

        strip: list[int] = []
        for idx_t, (i0, i1, i2) in enumerate(local_tris):
            if idx_t == 0:
                strip.extend((i0, i1, i2))
            elif (idx_t & 1) == 1:
                # Odd strip index (5, 15, 25...): D3D reverses triangle strip winding on odd indices,
                # so swap (i2, i1) to preserve identical front-facing winding for every triangle!
                prev_last = strip[-1]
                strip.extend((prev_last, i0, i0, i2, i1))
            else:
                prev_last = strip[-1]
                strip.extend((prev_last, i0, i0, i1, i2))

        if len(strip) < 3 or len(strip) > 32766:
            continue

        tri_count = len(strip) - 2
        idx_bytes = struct.pack(f"<{len(strip)}H", *strip)
        parts_out.append((len(local_remap), tri_count, sh_bytes, bytes(local_verts), idx_bytes))

    return parts_out


def _write_ftag_file(
    out_ftag: Path,
    grp_str: str,
    tag_path: str,
    radius: float,
    parts_out: list[tuple[int, int, bytes, bytes, bytes]],
) -> bool:
    if not parts_out:
        return False
    out_ftag.parent.mkdir(parents=True, exist_ok=True)
    grp_int = (ord(grp_str[0]) << 24) | (ord(grp_str[1]) << 16) | (ord(grp_str[2]) << 8) | ord(grp_str[3])
    tname_bytes = tag_path.encode("ascii", errors="ignore")[:255].ljust(256, b"\x00")
    with out_ftag.open("wb") as wf:
        wf.write(b"FTAG")
        wf.write(struct.pack("<IIf", 1, grp_int, radius))
        wf.write(tname_bytes)
        wf.write(struct.pack("<I", len(parts_out)))
        for vtx_c, tri_c, sh_b, v_b, i_b in parts_out:
            wf.write(struct.pack("<ii", vtx_c, tri_c))
            wf.write(sh_b)
            wf.write(v_b)
            wf.write(i_b)
    return True


def _load_and_convert_marker_obj(
    obj_path: Path,
    base_span_units: float,
    decimate_above_y: float | None = None,
    grid_res: float = 64.0,
) -> tuple[list[tuple[float, float, float, float, float, float]], list[tuple[int, int, int]]]:
    vs: list[tuple[float, float, float]] = []
    vns: list[tuple[float, float, float]] = []
    fs: list[tuple[int, int, int]] = []
    f_vns: list[tuple[int, int, int]] = []
    with obj_path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if line.startswith("v "):
                toks = line.split()
                if len(toks) >= 4:
                    vs.append((float(toks[1]), float(toks[2]), float(toks[3])))
            elif line.startswith("vn "):
                toks = line.split()
                if len(toks) >= 4:
                    vns.append((float(toks[1]), float(toks[2]), float(toks[3])))
            elif line.startswith("f "):
                toks = line.split()[1:]
                if len(toks) >= 3:
                    v_idxs: list[int] = []
                    vn_idxs: list[int] = []
                    for t in toks:
                        parts = t.split("/")
                        v_idxs.append(int(parts[0]) - 1)
                        if len(parts) >= 3 and parts[2]:
                            vn_idxs.append(int(parts[2]) - 1)
                        else:
                            vn_idxs.append(-1)
                    for i in range(1, len(v_idxs) - 1):
                        fs.append((v_idxs[0], v_idxs[i], v_idxs[i + 1]))
                        f_vns.append((vn_idxs[0], vn_idxs[i], vn_idxs[i + 1]))

    if not vs or not fs:
        return [], []

    min_y = min(v[1] for v in vs)
    halo_1m = 1.0 / 3.048  # 0.328084 Halo world units = 1.0 meter
    scale = halo_1m / base_span_units

    if decimate_above_y is None and vns:
        # Low-poly marker (Player-Spawn, Teleporter-Origin, Teleporter-Destination):
        # Preserve exact (v_idx, vn_idx) pairs so flat surfaces and sharp edges render cleanly!
        corner_map: dict[tuple[int, int], int] = {}
        verts_out_exact: list[tuple[float, float, float, float, float, float]] = []
        tris_out_exact: list[tuple[int, int, int]] = []
        for (i0, i1, i2), (n0, n1, n2) in zip(fs, f_vns):
            p0 = vs[i0]
            p1 = vs[i1]
            p2 = vs[i2]
            xh0, yh0, zh0 = p0[2] * scale, p0[0] * scale, (p0[1] - min_y) * scale + 0.012
            xh1, yh1, zh1 = p1[2] * scale, p1[0] * scale, (p1[1] - min_y) * scale + 0.012
            xh2, yh2, zh2 = p2[2] * scale, p2[0] * scale, (p2[1] - min_y) * scale + 0.012
            ex1, ey1, ez1 = xh1 - xh0, yh1 - yh0, zh1 - zh0
            ex2, ey2, ez2 = xh2 - xh0, yh2 - yh0, zh2 - zh0
            fnx = ey1 * ez2 - ez1 * ey2
            fny = ez1 * ex2 - ex1 * ez2
            fnz = ex1 * ey2 - ey1 * ex2
            fnl = (fnx * fnx + fny * fny + fnz * fnz) ** 0.5
            if fnl < 1e-9:
                continue
            fnx /= fnl
            fny /= fnl
            fnz /= fnl
            tri_c: list[int] = []
            for vi, ni, (xh, yh, zh) in (
                (i0, n0, (xh0, yh0, zh0)),
                (i1, n1, (xh1, yh1, zh1)),
                (i2, n2, (xh2, yh2, zh2)),
            ):
                ckey = (vi, ni)
                ci = corner_map.get(ckey)
                if ci is None:
                    ci = len(verts_out_exact)
                    corner_map[ckey] = ci
                    if 0 <= ni < len(vns):
                        nx_o, ny_o, nz_o = vns[ni]
                        nxh, nyh, nzh = nz_o, nx_o, ny_o
                        nl = (nxh * nxh + nyh * nyh + nzh * nzh) ** 0.5
                        if nl > 1e-6:
                            nxh, nyh, nzh = nxh / nl, nyh / nl, nzh / nl
                        else:
                            nxh, nyh, nzh = fnx, fny, fnz
                    else:
                        nxh, nyh, nzh = fnx, fny, fnz
                    verts_out_exact.append((xh, yh, zh, nxh, nyh, nzh))
                tri_c.append(ci)
            if tri_c[0] != tri_c[1] and tri_c[1] != tri_c[2] and tri_c[2] != tri_c[0]:
                tris_out_exact.append((tri_c[0], tri_c[1], tri_c[2]))
        return verts_out_exact, tris_out_exact

    if decimate_above_y is not None:
        # Identify connected components so base ring and floating icon never tear at any height boundary
        parent = list(range(len(vs)))

        def _find(a: int) -> int:
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        for i0, i1, i2 in fs:
            r0, r1, r2 = _find(i0), _find(i1), _find(i2)
            parent[r1] = r0
            parent[r2] = r0

        comp_max_y: dict[int, float] = {}
        for idx, (_x, y, _z) in enumerate(vs):
            r = _find(idx)
            prev_my = comp_max_y.get(r, -1e9)
            if y > prev_my:
                comp_max_y[r] = y

        cell_map: dict[tuple[int, int, int, int], int] = {}
        clustered_vs: list[list[float]] = []
        v_remap = [0] * len(vs)
        for idx, (x, y, z) in enumerate(vs):
            r = _find(idx)
            if comp_max_y.get(r, 1.0) < 0.25:
                # Base ring / arrow component: preserve exact vertices without decimation
                key = (r, round(x * 10000.0), round(y * 10000.0), round(z * 10000.0))
            else:
                # High-poly floating icon component: uniform grid clustering within component
                key = (r, round(x * grid_res), round(y * grid_res), round(z * grid_res))
            c = cell_map.get(key)
            if c is None:
                c = len(clustered_vs)
                cell_map[key] = c
                clustered_vs.append([x, y, z, 1.0])
            else:
                cv = clustered_vs[c]
                cv[0] += x
                cv[1] += y
                cv[2] += z
                cv[3] += 1.0
            v_remap[idx] = c

        vs = [(cv[0] / cv[3], cv[1] / cv[3], cv[2] / cv[3]) for cv in clustered_vs]
        new_fs: list[tuple[int, int, int]] = []
        seen_tris: set[tuple[int, int, int]] = set()
        for i0, i1, i2 in fs:
            c0, c1, c2 = v_remap[i0], v_remap[i1], v_remap[i2]
            if c0 == c1 or c1 == c2 or c2 == c0:
                continue
            tkey = (c0, c1, c2)
            if tkey in seen_tris:
                continue
            seen_tris.add(tkey)
            new_fs.append((c0, c1, c2))
        fs = new_fs

    halo_pos: list[tuple[float, float, float]] = []
    for x_obj, y_obj, z_obj in vs:
        xh = z_obj * scale
        yh = x_obj * scale
        zh = (y_obj - min_y) * scale + 0.012
        halo_pos.append((xh, yh, zh))

    norms = [[0.0, 0.0, 0.0] for _ in range(len(halo_pos))]
    valid_tris: list[tuple[int, int, int]] = []
    for i0, i1, i2 in fs:
        x0, y0, z0 = halo_pos[i0]
        x1, y1, z1 = halo_pos[i1]
        x2, y2, z2 = halo_pos[i2]
        ex1, ey1, ez1 = x1 - x0, y1 - y0, z1 - z0
        ex2, ey2, ez2 = x2 - x0, y2 - y0, z2 - z0
        nx = ey1 * ez2 - ez1 * ey2
        ny = ez1 * ex2 - ex1 * ez2
        nz = ex1 * ey2 - ey1 * ex2
        if nx * nx + ny * ny + nz * nz < 1e-12:
            continue
        valid_tris.append((i0, i1, i2))
        for vi in (i0, i1, i2):
            norms[vi][0] += nx
            norms[vi][1] += ny
            norms[vi][2] += nz

    verts_out: list[tuple[float, float, float, float, float, float]] = []
    for idx, (xh, yh, zh) in enumerate(halo_pos):
        nx, ny, nz = norms[idx]
        nl = (nx * nx + ny * ny + nz * nz) ** 0.5
        if nl > 1e-6:
            nx /= nl
            ny /= nl
            nz /= nl
        else:
            nx, ny, nz = 0.0, 0.0, 1.0
        verts_out.append((xh, yh, zh, nx, ny, nz))

    return verts_out, valid_tris


def compile_objective_marker_objs_to_ftags(forge_addon_dir: Path) -> None:
    import math
    obj_dir = ROOT_DIR / "Forge Objective Markers"
    markers_out_dir = forge_addon_dir / "cached_tags" / "markers"
    markers_out_dir.mkdir(parents=True, exist_ok=True)
    if not obj_dir.is_dir():
        return

    specs = [
        ("Player-Spawn.obj", "player_spawn.ftag", "spawn\\player_spawn", 1052.129, None, 64.0),
        ("Teleporter-Origin.obj", "teleporter_origin.ftag", "teleporter\\origin", 1052.129, None, 64.0),
        ("Teleporter-Destination.obj", "teleporter_destination.ftag", "teleporter\\destination", 1052.129, None, 64.0),
        ("Race-Marker.obj", "race_marker.ftag", "objective\\race_track", 1.0, 0.16, 48.0),
        ("Oddball-Marker.obj", "oddball_marker.ftag", "objective\\oddball_spawn", 1.0, 0.16, 44.0),
    ]

    spawn_verts: list[tuple[float, float, float, float, float, float]] = []
    spawn_tris: list[tuple[int, int, int]] = []

    for obj_name, ftag_name, tag_path, span_u, dec_y, g_res in specs:
        obj_file = obj_dir / obj_name
        if not obj_file.is_file():
            continue
        v_list, t_list = _load_and_convert_marker_obj(obj_file, span_u, dec_y, g_res)
        if obj_name == "Player-Spawn.obj":
            spawn_verts = list(v_list)
            spawn_tris = list(t_list)
        parts = _pack_ftag_parts_from_triangles(v_list, t_list)
        if _write_ftag_file(markers_out_dir / ftag_name, "scen", tag_path, 0.45, parts):
            print(f"[build_forge] Compiled 3D Marker {obj_name} -> markers/{ftag_name} ({len(v_list)} verts, {len(t_list)} tris, {len(parts)} parts)")

    if spawn_verts and spawn_tris:
        ctf_verts = list(spawn_verts)
        ctf_tris = list(spawn_tris)

        pole_segs = 12
        pole_r = 0.0085
        z_bot, z_top = 0.025, 0.56
        base_idx = len(ctf_verts)
        for s in range(pole_segs):
            ang = s * (2.0 * math.pi / pole_segs)
            ca, sa = math.cos(ang), math.sin(ang)
            ctf_verts.append((pole_r * ca, pole_r * sa, z_bot, ca, sa, 0.0))
            ctf_verts.append((pole_r * ca, pole_r * sa, z_top, ca, sa, 0.0))
        for s in range(pole_segs):
            ns = (s + 1) % pole_segs
            b0, t0 = base_idx + s * 2, base_idx + s * 2 + 1
            b1, t1 = base_idx + ns * 2, base_idx + ns * 2 + 1
            ctf_tris.append((b0, b1, t1))
            ctf_tris.append((b0, t1, t0))

        cols, rows = 10, 6
        f_x0, f_x1 = 0.0, -0.22
        f_z0, f_z1 = 0.36, 0.54
        for side in (1.0, -1.0):
            grid_base = len(ctf_verts)
            for r in range(rows + 1):
                v_frac = r / rows
                z = f_z0 + v_frac * (f_z1 - f_z0)
                for c in range(cols + 1):
                    u_frac = c / cols
                    x = f_x0 + u_frac * (f_x1 - f_x0)
                    wave = 0.018 * math.sin(u_frac * math.pi * 1.6) * u_frac
                    y = wave + side * 0.0025
                    ctf_verts.append((x, y, z, 0.0, side, 0.0))
            for r in range(rows):
                for c in range(cols):
                    i00 = grid_base + r * (cols + 1) + c
                    i10 = i00 + 1
                    i01 = i00 + (cols + 1)
                    i11 = i01 + 1
                    if side > 0:
                        ctf_tris.append((i00, i10, i11))
                        ctf_tris.append((i00, i11, i01))
                    else:
                        ctf_tris.append((i00, i11, i10))
                        ctf_tris.append((i00, i01, i11))

        ctf_parts = _pack_ftag_parts_from_triangles(ctf_verts, ctf_tris)
        if _write_ftag_file(markers_out_dir / "ctf_flag_marker.ftag", "scen", "objective\\ctf_flag", 0.60, ctf_parts):
            print(f"[build_forge] Compiled 3D CTF Flag Marker -> markers/ctf_flag_marker.ftag ({len(ctf_verts)} verts, {len(ctf_tris)} tris)")


def build_haloce_forge_exe() -> Path:
    clang = find_llvm_tool("clang")
    objcopy = find_llvm_tool("llvm-objcopy")
    game_cflags, platform_cflags = get_cflags_sets()

    forge_sources = [
        "forge_cache_builder.c",
        "forge_runtime_tags.c",
        "forge_ai.c",
        "forge_blueprint_2d.c",
        "forge_3d_mode.c",
        "forge_network.c",
        "forge_hooks.c",
    ]
    forge_objs: list[Path] = []
    for src_name in forge_sources:
        obj_path = FORGE_BUILD_DIR / src_name.replace(".c", ".o")
        print(f"[build_forge] Compiling {src_name}...")
        compile_c(clang, game_cflags, FORGE_DIR / "src" / src_name, obj_path)
        forge_objs.append(obj_path)

    # Compile isolated network protocol objects with HALO_PORT_NETWORK_VERSION -> forge_get_network_version()
    net_override_flags = [
        "-include",
        str(FORGE_DIR / "include" / "forge_net_override.h"),
    ]
    net_dir = FORGE_BUILD_DIR / "net_obj"
    net_units = [
        (ROOT_DIR / "source/networking/network_server_message_handler.c", game_cflags, "source/networking/network_server_message_handler.o"),
        (ROOT_DIR / "source/networking/network_client_manager.c", game_cflags, "source/networking/network_client_manager.o"),
        (ROOT_DIR / "port/linux/game/menu_functions.c", game_cflags, "port/linux/game/menu_functions.o"),
        (ROOT_DIR / "port/linux/src/p2p_lobby.c", platform_cflags, "port/linux/src/p2p_lobby.o"),
    ]
    net_replacements: dict[str, Path] = {}
    for src_file, cflags_set, rel_obj_key in net_units:
        out_o = net_dir / Path(rel_obj_key).name
        print(f"[build_forge] Compiling Forge network-aware {src_file.name}...")
        compile_c(clang, cflags_set, src_file, out_o, net_override_flags)
        net_replacements[rel_obj_key.replace("\\", "/")] = out_o

    # Compile cseries.c and cseries_windows.c with -DHALO_RELEASE so modded map tag assertions log and recover cleanly
    for rel_src in ("source/cseries/cseries.c", "source/cseries/cseries_windows.c"):
        src_file = ROOT_DIR / rel_src
        rel_obj_key = rel_src.replace(".c", ".o")
        out_o = net_dir / Path(rel_obj_key).name
        print(f"[build_forge] Compiling release-resilient {src_file.name}...")
        compile_c(clang, game_cflags, src_file, out_o, ["-DHALO_RELEASE"])
        net_replacements[rel_obj_key] = out_o

    # Wrap menu_functions.o symbols so forge_hooks.c intercepts menu events and unlocks all campaign maps in Forge mode
    subprocess.run([
        objcopy,
        "--redefine-sym", "_pc_menu_event_function_invoke=_orig_pc_menu_event_function_invoke",
        "--redefine-sym", "_pc_menu_game_data_function_invoke=_orig_pc_menu_game_data_function_invoke",
        "--redefine-sym", "_player_profile_get=_forge_player_profile_get",
        str(net_replacements["port/linux/game/menu_functions.o"]),
    ], check=True)

    # Wrap base engine symbols via llvm-objcopy --redefine-sym
    obj_root = ROOT_DIR / "build" / "windows" / "obj"
    wrap_dir = FORGE_BUILD_DIR / "wrapped_obj"
    wrap_dir.mkdir(parents=True, exist_ok=True)

    menu_files_wrapped = wrap_dir / "menu_files.o"
    shutil.copyfile(obj_root / "port/linux/src/menu_files.o", menu_files_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_config_folder=_forge_menu_config_folder",
        "--redefine-sym", "_SDL_GlobDirectory=_forge_menu_SDL_GlobDirectory",
        "--redefine-sym", "__imp__SDL_GlobDirectory=___imp__forge_menu_SDL_GlobDirectory",
        str(menu_files_wrapped),
    ], check=True)
    net_replacements["port/linux/src/menu_files.o"] = menu_files_wrapped

    chat_wrapped = wrap_dir / "network_chat.o"
    shutil.copyfile(obj_root / "port/linux/game/network_chat.o", chat_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_network_chat_update=_orig_network_chat_update",
        "--redefine-sym", "_network_chat_draw=_orig_network_chat_draw",
        "--redefine-sym", "_network_chat_typing=_orig_network_chat_typing",
        "--redefine-sym", "_network_chat_handle_up=_orig_network_chat_handle_up",
        str(chat_wrapped),
    ], check=True)
    net_replacements["port/linux/game/network_chat.o"] = chat_wrapped

    director_wrapped = wrap_dir / "director.o"
    shutil.copyfile(obj_root / "source/camera/director.o", director_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_director_update=_orig_director_update",
        "--redefine-sym", "_director_get_perspective=_orig_director_get_perspective",
        str(director_wrapped),
    ], check=True)
    net_replacements["source/camera/director.o"] = director_wrapped

    fp_weapons_wrapped = wrap_dir / "first_person_weapons.o"
    shutil.copyfile(obj_root / "source/interface/first_person_weapons.o", fp_weapons_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_first_person_weapon_draw=_orig_first_person_weapon_draw",
        str(fp_weapons_wrapped),
    ], check=True)
    net_replacements["source/interface/first_person_weapons.o"] = fp_weapons_wrapped

    hud_weapon_wrapped = wrap_dir / "hud_weapon.o"
    shutil.copyfile(obj_root / "source/interface/hud_weapon.o", hud_weapon_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_hud_render_weapon_interface=_orig_hud_render_weapon_interface",
        str(hud_weapon_wrapped),
    ], check=True)
    net_replacements["source/interface/hud_weapon.o"] = hud_weapon_wrapped

    models_wrapped = wrap_dir / "models.o"
    shutil.copyfile(obj_root / "source/models/models.o", models_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_render_model=_orig_render_model",
        str(models_wrapped),
    ], check=True)
    net_replacements["source/models/models.o"] = models_wrapped

    hs_wrapped = wrap_dir / "hs.o"
    shutil.copyfile(obj_root / "source/hs/hs.o", hs_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_hs_compile_and_evaluate=_orig_hs_compile_and_evaluate",
        str(hs_wrapped),
    ], check=True)
    net_replacements["source/hs/hs.o"] = hs_wrapped

    hs_runtime_wrapped = wrap_dir / "hs_runtime.o"
    shutil.copyfile(obj_root / "source/hs/hs_runtime.o", hs_runtime_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_hs_runtime_update=_orig_hs_runtime_update",
        str(hs_runtime_wrapped),
    ], check=True)
    net_replacements["source/hs/hs_runtime.o"] = hs_runtime_wrapped

    cinematics_wrapped = wrap_dir / "cinematics.o"
    shutil.copyfile(obj_root / "source/cutscene/cinematics.o", cinematics_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_cinematic_start=_orig_cinematic_start",
        str(cinematics_wrapped),
    ], check=True)
    net_replacements["source/cutscene/cinematics.o"] = cinematics_wrapped

    scenario_wrapped = wrap_dir / "scenario.o"
    shutil.copyfile(obj_root / "source/scenario/scenario.o", scenario_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_scenario_trigger_volume_test_object=_orig_scenario_trigger_volume_test_object",
        str(scenario_wrapped),
    ], check=True)
    net_replacements["source/scenario/scenario.o"] = scenario_wrapped

    game_engine_wrapped = wrap_dir / "game_engine.o"
    shutil.copyfile(obj_root / "source/game/game_engine.o", game_engine_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_game_engine_remap_object_definition=_orig_game_engine_remap_object_definition",
        "--redefine-sym", "_game_engine_post_rasterize_objects=_orig_game_engine_post_rasterize_objects",
        str(game_engine_wrapped),
    ], check=True)
    net_replacements["source/game/game_engine.o"] = game_engine_wrapped

    menu_tags_wrapped = wrap_dir / "menu_tags.o"
    shutil.copyfile(obj_root / "port/linux/game/menu_tags.o", menu_tags_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_menu_tags_loaded=_orig_menu_tags_loaded",
        "--redefine-sym", "_menu_tags_unloaded=_orig_menu_tags_unloaded",
        str(menu_tags_wrapped),
    ], check=True)
    net_replacements["port/linux/game/menu_tags.o"] = menu_tags_wrapped

    # Point 11: Redirect custom_maps\<name>.(map|bmp|txt) to forge_maps\<name>\<name>.(map|bmp|txt)
    for ce_unit in ("port/linux/game/custom_edition_cache.o", "port/linux/game/custom_edition_maps.o", "port/linux/game/map_share_files.o"):
        ce_src = obj_root / ce_unit
        if ce_src.is_file():
            ce_wrapped = wrap_dir / Path(ce_unit).name
            shutil.copyfile(ce_src, ce_wrapped)
            subprocess.run([
                objcopy,
                "--redefine-sym", "_file_reference_create_from_path=_forge_ce_file_reference_create_from_path",
                "--redefine-sym", "_halo_windows_fopen=_forge_ce_fopen",
                "--redefine-sym", "_halo_xbox_fopen=_forge_ce_fopen",
                "--redefine-sym", "_fopen=_forge_ce_fopen",
                "--redefine-sym", "_halo_xbox_FindFirstFileA@8=_forge_ce_FindFirstFileA@8",
                "--redefine-sym", "_halo_xbox_FindNextFileA@8=_forge_ce_FindNextFileA@8",
                "--redefine-sym", "_halo_xbox_CloseHandle@4=_forge_ce_CloseHandle@4",
                str(ce_wrapped),
            ], check=True)
            net_replacements[ce_unit] = ce_wrapped

    # Collect all 840 base object files, substituting wrapped & network-isolated copies
    all_objs: list[str] = []
    for o_path in sorted(obj_root.rglob("*.o")):
        rel_key = o_path.relative_to(obj_root).as_posix()
        if rel_key in net_replacements:
            all_objs.append(str(net_replacements[rel_key]))
        else:
            all_objs.append(str(o_path))

    for fo in forge_objs:
        all_objs.append(str(fo))

    res_file = ROOT_DIR / "build" / "windows" / "halo.res"
    if res_file.is_file():
        all_objs.append(str(res_file))

    rsp_file = FORGE_BUILD_DIR / "haloce_forge.rsp"
    rsp_file.write_text("\n".join(f'"{p}"' for p in all_objs) + "\n", encoding="utf-8")

    port_cfg = json.loads((ROOT_DIR / PORT_DIR / "port.json").read_text(encoding="utf-8"))
    libs = [
        str(ROOT_DIR / SDL_DIR / "lib" / "x86" / "SDL3.lib"),
        *[f"-l{lib}" for lib in port_cfg.get("libraries", [])],
    ]

    OPENCE_BUILD_DIR.mkdir(parents=True, exist_ok=True)
    exe_out = OPENCE_BUILD_DIR / "HaloCE Forge.exe"
    link_cmd = [
        clang,
        "--target=i686-pc-windows-msvc",
        "-fuse-ld=lld",
        "-g",
        "-Wl,/LARGEADDRESSAWARE",
        "-Wl,/STACK:0x1000000",
        "-Wl,/SUBSYSTEM:CONSOLE",
        "-o",
        str(exe_out),
        "--rsp-quoting=windows",
        f"@{rsp_file}",
        *libs,
    ]
    print(f"[build_forge] Linking {exe_out}...")
    res = subprocess.run(link_cmd, cwd=str(ROOT_DIR))
    if res.returncode != 0:
        sys.exit(res.returncode)

    package_opence_build_folder(exe_out)
    print(f"[build_forge] SUCCESS: Packaged {OPENCE_BUILD_DIR} with {exe_out.name} ({exe_out.stat().st_size:,} bytes)")
    return exe_out


if __name__ == "__main__":
    ac_exe = build_asset_compiler()
    if "--run-step4" in sys.argv:
        res = subprocess.run([str(ac_exe), "--step4"], cwd=str(ROOT_DIR))
        if res.returncode != 0:
            sys.exit(res.returncode)
    build_haloce_forge_exe()
