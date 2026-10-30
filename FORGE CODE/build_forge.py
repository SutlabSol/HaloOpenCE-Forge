#!/usr/bin/env python3
"""
Build & Packaging script for OpenCE Forge ("halo.exe") and Forge Asset Compiler.
Packages the complete OpenCE Forge Windows release inside:
  C:\\Users\\LeviB\\Projects\\OpenCE-Forge\\OpenCE Build\\
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
FORGE_NET_VERSION = "0x4628"  # ('F' << 8) | 40


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

    w1, h1 = 256, 64
    out_w, out_h = 1024, 256  # 4x hi-res matching port/assets/menus/ce/port/main_menu/menu_mods__0.png
    ss_hi = 2                 # 2x2 supersampling at 4x resolution -> 2048x512 GDI canvas (8x SVG scale)
    sw, sh = out_w * ss_hi, out_h * ss_hi
    svg_to_gdi = 4 * ss_hi    # 8x SVG units

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
        "RE": 2.2, "EH": 2.4, "HA": 1.6, "AS": 1.8, "SH": 2.4,
        "CU": 2.2, "US": 2.2, "ST": 1.8, "TO": 1.6, "OM": 2.4,
        "TA": 0.8, "AG": 1.8, "GS": 2.2,
    }

    buttons = [
        ("ce/port/main_menu/menu_forge", "FORGE"),
        ("ce/port/main_menu/forge_select/item_single_player", "SINGLE PLAYER"),
        ("ce/port/main_menu/forge_select/item_multiplayer", "MULTIPLAYER"),
        ("ce/port/main_menu/forge_select/item_inject_tags", "REHASH CUSTOM TAGS"),
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
                draw_svg_glyph_to_gdi_path(gdi32, hdc, d_str, cur_x, top_y, scale, svg_to_gdi)
            cur_x += advances[idx] * scale
        gdi32.EndPath(hdc)
        gdi32.FillPath(hdc)

        raw_bgra = ctypes.string_at(bits_ptr, sw * sh * 4)

        # Downsample 2048x512 into 1024x256 hi-res anti-aliased alpha mask (only active rows y=0..152)
        active_h4 = 152
        mask_hi = bytearray(out_w * out_h)
        mask_1x = [0] * (w1 * h1)
        for y in range(active_h4):
            r0 = (y * 2) * sw
            r1 = (y * 2 + 1) * sw
            out_row = y * out_w
            for x in range(out_w):
                x2 = x * 2
                acc = (
                    raw_bgra[(r0 + x2) * 4 + 1]
                    + raw_bgra[(r0 + x2 + 1) * 4 + 1]
                    + raw_bgra[(r1 + x2) * 4 + 1]
                    + raw_bgra[(r1 + x2 + 1) * 4 + 1]
                )
                mask_hi[out_row + x] = acc >> 2

        # Also downsample to 256x64 for fast, smooth outer glow computation
        for y1 in range(38):
            for x1 in range(w1):
                s1 = 0
                by = y1 * 4
                bx = x1 * 4
                for dy in range(4):
                    ro = (by + dy) * out_w + bx
                    s1 += mask_hi[ro] + mask_hi[ro + 1] + mask_hi[ro + 2] + mask_hi[ro + 3]
                mask_1x[y1 * w1 + x1] = s1 >> 4

        # Frame 0: Normal dim blue (#2895ff at 0.467 opacity -> alpha 119) at full 1024x256 hi-res
        rgba0 = bytearray(out_w * out_h * 4)
        for i in range(out_w * active_h4):
            m = mask_hi[i]
            if m > 0:
                a = (m * 119) // 255
                rgba0[i * 4 + 0] = 0x28
                rgba0[i * 4 + 1] = 0x95
                rgba0[i * 4 + 2] = 0xFF
                rgba0[i * 4 + 3] = a
        write_rgba_png(menus_root / f"{rel_base}__0.png", out_w, out_h, bytes(rgba0))

        # Frame 1: Highlighted white (#ffffff) at 1024x256 with OpenCE's blue outer glow (#2895ff)
        dilated = [0] * (w1 * h1)
        for y in range(3, 38):
            for x in range(3, w1 - 3):
                mx = 0
                for dy in range(-3, 4):
                    r_off = (y + dy) * w1 + x
                    for dx in range(-3, 4):
                        if dy * dy + dx * dx <= 8:
                            v = mask_1x[r_off + dx]
                            if v > mx:
                                mx = v
                dilated[y * w1 + x] = mx

        weights = (1, 4, 8, 10, 8, 4, 1)  # sum = 36
        tmp_blur = [0] * (w1 * h1)
        glow_1x = [0] * (w1 * h1)
        for y in range(3, 40):
            r_off = y * w1
            for x in range(3, w1 - 3):
                s = 0
                for k, wt in enumerate(weights):
                    s += dilated[r_off + x + (k - 3)] * wt
                tmp_blur[r_off + x] = s // 36
        for y in range(3, 40):
            r_off = y * w1
            for x in range(3, w1 - 3):
                s = 0
                for k, wt in enumerate(weights):
                    s += tmp_blur[(y + (k - 3)) * w1 + x] * wt
                glow_1x[r_off + x] = s // 36

        rgba1 = bytearray(out_w * out_h * 4)
        for y in range(active_h4):
            fy = (y + 0.5) * 0.25 - 0.5
            y0 = max(0, min(h1 - 2, int(fy)))
            wy = int((fy - y0) * 256.0)
            wy0 = 256 - wy
            r_g0 = y0 * w1
            r_g1 = (y0 + 1) * w1
            out_row = y * out_w
            for x in range(out_w):
                m = mask_hi[out_row + x]
                fx = (x + 0.5) * 0.25 - 0.5
                x0 = max(0, min(w1 - 2, int(fx)))
                wx = int((fx - x0) * 256.0)
                wx0 = 256 - wx
                g_top = (glow_1x[r_g0 + x0] * wx0 + glow_1x[r_g0 + x0 + 1] * wx) >> 8
                g_bot = (glow_1x[r_g1 + x0] * wx0 + glow_1x[r_g1 + x0 + 1] * wx) >> 8
                g = (g_top * wy0 + g_bot * wy) >> 8
                if m == 0 and g == 0:
                    continue
                ga = (g * 166) // 255
                out_a = m + (ga * (255 - m)) // 255
                if out_a > 0:
                    i4 = (out_row + x) * 4
                    r = (m * 255 + ((ga * (255 - m)) // 255) * 0x28) // out_a
                    gr = (m * 255 + ((ga * (255 - m)) // 255) * 0x95) // out_a
                    rgba1[i4 + 0] = min(255, r)
                    rgba1[i4 + 1] = min(255, gr)
                    rgba1[i4 + 2] = 255
                    rgba1[i4 + 3] = min(255, out_a)
        write_rgba_png(menus_root / f"{rel_base}__1.png", out_w, out_h, bytes(rgba1))

    # Point 10: Also render 'MAPS' header PNG at 2048x256 (4x hi-res matching base game header_mods.png)
    ctypes.memset(bits_ptr, 0, sw * sh * 4)
    gdi32.BeginPath(hdc)
    hdr_text = "MAPS"
    hdr_scale = 1.0
    cur_x = 30.68
    top_y = 33.5
    for idx, ch in enumerate(hdr_text):
        d_str, w_ch = glyphs.get(ch, glyphs["E"])
        draw_svg_glyph_to_gdi_path(gdi32, hdc, d_str, cur_x, top_y, hdr_scale, svg_to_gdi)
        cur_x += (w_ch + 2.4) * hdr_scale
    text_end_x = cur_x - 2.4 * hdr_scale
    gdi32.EndPath(hdc)
    gdi32.FillPath(hdc)
    raw_hdr = ctypes.string_at(bits_ptr, sw * sh * 4)
    hw4 = 2048
    rgba_hdr = bytearray(hw4 * out_h * 4)
    bkd_x0, bkd_y0 = 25.92 * 4.0, 29.2 * 4.0
    bkd_x1, bkd_y1 = (text_end_x + 4.72) * 4.0, (29.2 + 28.3) * 4.0
    blur_r = 3.6 * 4.0
    max_x4 = min(out_w, int(bkd_x1 + blur_r + 8))
    for y in range(out_h):
        r0 = (y * 2) * sw
        r1 = (y * 2 + 1) * sw
        dy_b = max(bkd_y0 - (y + 0.5), 0.0, (y + 0.5) - bkd_y1)
        if dy_b >= blur_r:
            continue
        for x in range(max_x4):
            dx_b = max(bkd_x0 - (x + 0.5), 0.0, (x + 0.5) - bkd_x1)
            dist_b = (dx_b * dx_b + dy_b * dy_b) ** 0.5
            bkd_a = 0.0
            if dist_b < blur_r:
                t_b = 1.0 - (dist_b / blur_r)
                bkd_a = 0.893 * (t_b * t_b * (3.0 - 2.0 * t_b))
            x2 = x * 2
            acc = (
                raw_hdr[(r0 + x2) * 4 + 1]
                + raw_hdr[(r0 + x2 + 1) * 4 + 1]
                + raw_hdr[(r1 + x2) * 4 + 1]
                + raw_hdr[(r1 + x2 + 1) * 4 + 1]
            )
            fg_a = (acc * 0.25) / 255.0
            out_a = fg_a + bkd_a * (1.0 - fg_a)
            if out_a > 1e-4:
                di = (y * hw4 + x) * 4
                r_c = (0x29 * fg_a + 0x02 * bkd_a * (1.0 - fg_a)) / out_a
                g_c = (0x95 * fg_a + 0x19 * bkd_a * (1.0 - fg_a)) / out_a
                b_c = (0xFD * fg_a + 0x31 * bkd_a * (1.0 - fg_a)) / out_a
                rgba_hdr[di + 0] = min(255, max(0, int(round(r_c))))
                rgba_hdr[di + 1] = min(255, max(0, int(round(g_c))))
                rgba_hdr[di + 2] = min(255, max(0, int(round(b_c))))
                rgba_hdr[di + 3] = min(255, max(0, int(round(out_a * 255.0))))
    write_rgba_png(menus_root / "ce/port/main_menu/forge_select/header_maps.png", hw4, out_h, bytes(rgba_hdr))

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
	<!-- Point 9: Remove Single Player, Multiplayer, and Mods options from the main menu screen -->
	<widget name="main_menu/main_menu_select_list" type="column_list" width="640" height="480" flags="pass_unhandled_to_focused_child up_down_tabs_items" description="main_menu/main_menu_list_ext_desc">
		<data input="main menu fake animate"/>
		<on event="down"/>
		<child widget="main_menu/main_menu_item_forge" x="192" y="265"/>
		<child widget="main_menu/main_menu_item_profile" x="192" y="305"/>
		<child widget="main_menu/main_menu_item_settings" x="192" y="345"/>
		<child widget="main_menu/main_menu_item_quit_game" x="192" y="385"/>
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
		<on event="a" run="port mods save"/>
		<on event="start" run="port mods save"/>
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

    new_select_xml = """<?xml version="1.0" encoding="UTF-8"?>
<!-- OpenCE Forge map chooser options: adds FORGE MAPS as the 5th spinner option -->
<menus>
	<widget name="main_menu/new_select/list_item_0_map_kind" width="256" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds_sm" font="ui\\large_ui" color="#FF2896FF" align="center" text_y="3">
		<child widget="main_menu/new_select/list_item_0_map_kind_spinner" x="9" y="2"/>
	</widget>
	<widget name="main_menu/new_select/list_item_0_map_kind_spinner" type="spinner" width="238" height="22" flags="left_right_tabs_items" string_list="main_menu/new_select/var_map_kinds" font="ui\\large_ui" color="#FF2896FF" align="center" text_y="1" list_flags="items_from_strings" header_bitmap="bitmaps/arrow_sm_left" footer_bitmap="bitmaps/arrow_sm_right" header_bounds="7 -6 19 0" footer_bounds="7 238 19 244">
		<on event="left_mouse" run="mouse spinner 1wide click"/>
	</widget>
	<strings name="main_menu/new_select/var_map_kinds">
		<string text="SINGLEPLAYER"/>
		<string text="MULTIPLAYER"/>
		<string text="CUSTOM SINGLEPLAYER"/>
		<string text="CUSTOM MULTIPLAYER"/>
		<string text="FORGE MAPS"/>
	</strings>
</menus>
"""
    out_new_select = menus_root / "ce" / "main_menu.new_select.port.xml"
    out_new_select.write_text(new_select_xml, encoding="utf-8")

    settings_profile_edit_xml = """<?xml version="1.0" encoding="UTF-8"?>
<!-- OpenCE Forge Settings Menu: adds CLEAR CUSTOM CACHE and RESET TAG CACHE -->
<menus>
	<strings name="main_menu/settings_select/player_setup/player_profile_edit/forge_profile_edit_descriptions">
		<string text="Rename this profile.\\n\\n\\nProfile:"/>
		<string text="Choose the keys and mouse\\nbuttons for each action.\\n\\nProfile:"/>
		<string text="Set this profile's controller\\nsensitivity and layouts.\\n\\nProfile:"/>
		<string text="Adjust the mouse's sensitivity\\nand aiming.\\n\\nProfile:"/>
		<string text="Adjust the volume of the music\\nand of everything else.\\n\\nProfile:"/>
		<string text="Choose a window or the full\\nscreen, the frame rate and more.\\n\\nProfile:"/>
		<string text="Internet play, updates and the\\nmultiplayer HUD.\\n\\nProfile:"/>
		<string text="Change the current profile's\\nfree-for-all multiplayer color.\\n\\nProfile:"/>
		<string text="Halo: Combat Evolved, the Xbox\\ngame, on this computer.\\n\\nProfile:"/>
		<string text="Deletes all custom .ocetags files\\nin custom_tags/ and clears the\\ncustom tag cache.\\nProfile:"/>
		<string text="Clears the Xbox &amp; custom tag cache,\\nthen restarts HaloCE Forge.exe and\\nasks to build the cache again."/>
	</strings>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/name_profile_item" type="text" left="51" top="75" width="232" height="30" bitmap="bitmaps/list_item_bkd_top" string_list="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_options" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" run="player profile change name" open="main_menu/settings_select/player_setup/player_profile_edit/name_edit/profile_name_screen"/>
		<on event="start" run="player profile change name" open="main_menu/settings_select/player_setup/player_profile_edit/name_edit/profile_name_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controller_setup_profile_item" type="text" left="51" top="105" width="232" height="30" bitmap="bitmaps/list_item_bkd" string_list="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_options" string_index="1" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" open="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/controls_settings_screen"/>
		<on event="start" open="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/controls_settings_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/gamepads_profile_item" type="text" left="51" top="135" width="232" height="30" bitmap="bitmaps/list_item_bkd" string_list="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_options" string_index="2" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" open="main_menu/settings_select/player_setup/player_profile_edit/gamepad_setup/gamepad_setup_screen"/>
		<on event="start" open="main_menu/settings_select/player_setup/player_profile_edit/gamepad_setup/gamepad_setup_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/mouse_settings_profile_item" type="text" left="51" top="165" width="232" height="30" bitmap="bitmaps/list_item_bkd" string_list="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_options" string_index="3" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" open="main_menu/settings_select/player_setup/player_profile_edit/mouse_settings/mouse_settings_screen"/>
		<on event="start" open="main_menu/settings_select/player_setup/player_profile_edit/mouse_settings/mouse_settings_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/audio_settings_profile_item" type="text" left="51" top="195" width="232" height="30" bitmap="bitmaps/list_item_bkd" string_list="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_options" string_index="4" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" open="main_menu/settings_select/player_setup/player_profile_edit/audio_settings/audio_settings_screen"/>
		<on event="start" open="main_menu/settings_select/player_setup/player_profile_edit/audio_settings/audio_settings_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/video_settings_profile_item" type="text" left="51" top="225" width="232" height="30" bitmap="bitmaps/list_item_bkd" string_list="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_options" string_index="5" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" open="main_menu/settings_select/player_setup/player_profile_edit/video_settings/video_settings_screen"/>
		<on event="start" open="main_menu/settings_select/player_setup/player_profile_edit/video_settings/video_settings_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/network_settings_profile_item" type="text" left="51" top="255" width="232" height="30" bitmap="bitmaps/list_item_bkd" string_list="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_options" string_index="8" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" open="main_menu/settings_select/player_setup/player_profile_edit/network_setup/network_settings_screen"/>
		<on event="start" open="main_menu/settings_select/player_setup/player_profile_edit/network_setup/network_settings_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/color_profile_item" type="text" left="51" top="285" width="232" height="30" bitmap="bitmaps/list_item_bkd" string_list="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_options" string_index="6" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" open="main_menu/settings_select/player_setup/player_profile_edit/color_edit/color_select_screen"/>
		<on event="start" open="main_menu/settings_select/player_setup/player_profile_edit/color_edit/color_select_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/about_item" type="text" left="51" top="315" width="232" height="30" bitmap="bitmaps/list_item_bkd" string_list="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_options" string_index="9" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="left_mouse" run="NULL"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/clear_custom_cache_profile_item" type="text" left="51" top="345" width="232" height="30" bitmap="bitmaps/list_item_bkd" text="CLEAR CUSTOM CACHE" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" run="port mods close"/>
		<on event="start" run="port mods close"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/reset_tag_cache_profile_item" type="text" left="51" top="375" width="232" height="30" bitmap="bitmaps/list_item_bkd" text="RESET TAG CACHE" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
		<on event="a" run="port mods close"/>
		<on event="start" run="port mods close"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/header_profile_settings" controller="1" left="35" top="11" width="605" height="59" bitmap="main_menu/settings_select/player_setup/player_profile_edit/header_edit_profile_settings">
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/player_profile_edit_screen" width="640" height="480" flags="pass_unhandled_to_focused_child pause_game" bitmap="bitmaps/gradient">
		<on event="created" run="close if not editing profile"/>
		<on event="b" run="player profile end editing" back="true"/>
		<on event="back" run="player profile end editing" back="true"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/header_profile_settings"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_select_list"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/profile_label" x="293" y="375"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_button_bar" type="column_list" width="640" height="28" flags="pass_unhandled_to_focused_child left_right_tabs_items">
		<data input="common button bar update"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_button_ok" x="380" y="1"/>
		<child widget="common_button_cancel" x="510" y="1"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_button_ok" type="text" width="128" height="24" bitmap="bitmaps/text_button_background" string_list="strings/common_button_captions" string_index="1" font="ui\\small_ui" color="#FFFFFFFF" align="center" text_y="2">
		<on event="a" run="player profile save changes" open="error/warning_saving_file_screen"/>
		<on event="start" run="player profile save changes" open="error/warning_saving_file_screen"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_extended_desc_pic" controller="1" left="288" top="85" width="279" height="202" bitmap="main_menu/settings_select/player_setup/player_profile_edit/profile_options">
		<data input="port gamepad layout preview"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_extended_desc_text" type="text" controller="1" left="294" top="290" width="296" height="110" string_list="main_menu/settings_select/player_setup/player_profile_edit/forge_profile_edit_descriptions" font="ui\\large_ui" color="#FF2896FF">
		<replace search="__BUILD__" function="build_number"/>
		<replace search="__PID__" function="pid"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_extended_description" controller="1" left="275" top="78" width="320" height="362" bitmap="bitmaps/list_field">
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_extended_desc_text"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_extended_desc_pic" x="4"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_select_list" type="column_list" width="640" height="480" flags="pass_unhandled_to_focused_child up_down_tabs_items" description="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_extended_description">
		<data input="plyr prof edit select menu upd8"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/name_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controller_setup_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/gamepads_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/mouse_settings_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/audio_settings_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/video_settings_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/network_settings_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/color_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/about_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/clear_custom_cache_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/reset_tag_cache_profile_item"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/profile_edit_button_bar" y="414"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/profile_label" type="text" controller="1" width="400" height="22" font="ui\\large_ui" color="#FFFFFFFF">
		<data input="get edit plyr profile name"/>
	</widget>
</menus>
"""
    out_settings_edit = menus_root / "ce" / "main_menu.settings_select.player_setup.player_profile_edit.xml"
    out_settings_edit.write_text(settings_profile_edit_xml, encoding="utf-8")

    controls_setup_xml = """<?xml version="1.0" encoding="UTF-8"?>
<menus>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/controls_settings_screen" width="640" height="480" flags="pass_unhandled_to_focused_child pause_game" bitmap="bitmaps/gradient">
		<on event="b" back="true"/>
		<on event="back" back="true"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/header_profile_controls_settings"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/options_menu"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/header_profile_controls_settings" controller="1" left="35" top="11" width="605" height="59" bitmap="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/header_profile_controls_settings">
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/help" type="text" controller="1" left="68" top="364" width="482" height="50" string_list="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/help_strings" font="ui\\large_ui" color="#FFFFFFFF">
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/options_menu" type="column_list" width="640" height="480" flags="pass_unhandled_to_focused_child up_down_tabs_children" description="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/help">
		<data input="controls update menu"/>
		<on event="created" run="controls screen init"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_group" x="54" y="71"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_1" x="54" y="100"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_2" x="54" y="126"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_3" x="54" y="152"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_4" x="54" y="178"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_5" x="54" y="204"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_6" x="54" y="230"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_7" x="54" y="256"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_8" x="54" y="282"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_9" x="54" y="308"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_10" x="54" y="334"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/button_bar" y="414"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/button_bar" type="column_list" width="640" height="28" flags="pass_unhandled_to_focused_child left_right_tabs_items">
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/button_defaults" y="1"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/button_ok" x="380" y="1"/>
		<child widget="common_button_cancel" x="510" y="1"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_group" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/group_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/group_spinner" x="200" y="1"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/group_label" type="text" controller="1" width="200" height="22" text="SHOW:" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/group_spinner" type="spinner" left="3" top="2" width="297" height="20" flags="pass_unhandled_to_focused_child left_right_tabs_items" strings="MOVEMENT|WEAPONS|ACTIONS|COMMUNICATION|FORGE|FORGE PAD" font="ui\\large_ui" color="#FF2896FF" align="center" text_y="1" header_bitmap="bitmaps/arrow_sm_left" footer_bitmap="bitmaps/arrow_sm_right" header_bounds="7 -6 19 0" footer_bounds="7 300 19 306">
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_1" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_2" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_3" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_4" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_5" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_6" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_7" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_8" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_9" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/op_command_10" width="512" height="28" flags="pass_unhandled_to_focused_child" bitmap="bitmaps/option_bkds" color="#FF2896FF">
		<on event="a" run="controls begin binding"/>
		<on event="start" run="controls begin binding"/>
		<on event="left right" run="controls binding slot"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="200"/>
		<child widget="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" x="356"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_label" type="text" controller="1" width="200" height="22" font="ui\\large_ui" color="#FF2896FF" text_x="13" text_y="4">
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/command_binding" type="text" controller="1" width="152" height="22" font="ui\\small_ui" color="#FF2896FF" align="center" text_y="5" text_flags="no_focus_test">
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/button_defaults" type="text" width="128" height="24" bitmap="bitmaps/text_button_background" string_list="strings/common_button_captions" string_index="3" font="ui\\small_ui" color="#FFFFFFFF" align="center" text_y="2">
		<on event="a" run="controls screen defaults"/>
		<on event="start" run="controls screen defaults"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<widget name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/button_ok" type="text" width="128" height="24" bitmap="bitmaps/text_button_background" string_list="strings/common_button_captions" string_index="1" font="ui\\small_ui" color="#FFFFFFFF" align="center" text_y="2">
		<on event="a" run="controls screen change set" back="true"/>
		<on event="start" run="controls screen change set" back="true"/>
		<on event="left_mouse" run="mouse emit accept event"/>
	</widget>
	<strings name="main_menu/settings_select/player_setup/player_profile_edit/controls_setup/help_strings">
		<string text="Choose what to show, with left and right."/>
		<string text="Enter sets the key or gamepad button; left and right choose\nKey/Mouse (left) or Controller (right)."/>
		<string text="Press a keyboard key, mouse button, or controller button...\nEscape cancels, Delete clears."/>
	</strings>
</menus>
"""
    out_controls_setup = menus_root / "ce" / "main_menu.settings_select.player_setup.player_profile_edit.controls_setup.xml"
    out_controls_setup.write_text(controls_setup_xml, encoding="utf-8")


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


def _read_rgba_png(png_path: Path) -> tuple[int, int, bytes]:
    data = png_path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    w = h = 0
    idat = bytearray()
    while pos < len(data):
        length, ctype = struct.unpack(">I4s", data[pos : pos + 8])
        chunk = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            w, h, bd, ct, _, _, _ = struct.unpack(">IIBBBBB", chunk)
            assert bd == 8 and ct == 6
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    stride = w * 4
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        if f == 1:
            for i in range(4, stride):
                row[i] = (row[i] + row[i - 4]) & 0xFF
        elif f == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = row[i - 4] if i >= 4 else 0
                b = prev[i]
                row[i] = (row[i] + ((a + b) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = row[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
        out[y * stride : (y + 1) * stride] = row
        prev = row
    return w, h, bytes(out)


def extract_escape_menu_background_png(forge_addon_dir: Path) -> None:
    """
    Pulls and stitches the Escape / Pause menu background assets
    (`pausebox_left__0.png`, `pausebox_center__0.png`, `pausebox_right__0.png`)
    into a standalone `escape_menu_background.png` file for use in the Forge menu.
    """
    pause_dir = ROOT_DIR / "port" / "assets" / "menus" / "ce" / "port" / "pause"
    left_p = pause_dir / "pausebox_left__0.png"
    center_p = pause_dir / "pausebox_center__0.png"
    right_p = pause_dir / "pausebox_right__0.png"
    if not (left_p.is_file() and center_p.is_file() and right_p.is_file()):
        return

    lw, lh, l_rgba = _read_rgba_png(left_p)
    cw, ch, c_rgba = _read_rgba_png(center_p)
    rw, rh, r_rgba = _read_rgba_png(right_p)

    # Composite the 3-slice pause box:
    # left cap (16px wide), tiled center strip (4px wide), right cap (16px wide), cropped to active height (194px)
    full_w, full_h = 376, 194
    canvas = bytearray(full_w * full_h * 4)

    def blit(src_w: int, src_h: int, src: bytes, dst_x: int, dst_y: int) -> None:
        for y in range(min(src_h, full_h - dst_y)):
            dy = dst_y + y
            if dy < 0 or dy >= full_h:
                continue
            for x in range(src_w):
                dx = dst_x + x
                if dx < 0 or dx >= full_w:
                    continue
                si = (y * src_w + x) * 4
                sa = src[si + 3]
                if sa == 0:
                    continue
                di = (dy * full_w + dx) * 4
                canvas[di : di + 4] = src[si : si + 4]

    blit(lw, lh, l_rgba, 0, 0)
    cx = lw
    while cx < full_w - rw:
        blit(cw, ch, c_rgba, cx, 0)
        cx += max(1, cw)
    blit(rw, rh, r_rgba, full_w - rw, 0)

    out_paths = [
        forge_addon_dir / "ui" / "escape_menu_background.png",
        OPENCE_BUILD_DIR / "escape_menu_background.png",
        ROOT_DIR / "Forge Objective Markers" / "escape_menu_background.png",
    ]
    for out_p in out_paths:
        out_p.parent.mkdir(parents=True, exist_ok=True)
        write_rgba_png(out_p, full_w, full_h, bytes(canvas))
    print(f"[build_forge] Extracted Escape Menu background PNG -> {out_paths[0]} & {out_paths[2]} ({full_w}x{full_h})")


def generate_customizable_forge_ui_assets(forge_addon_dir: Path) -> None:
    """
    Generates `OpenCE Forge/ui/forge_ui.xml` and default editable PNG background images
    (`panel_spawn_menu.png`, `panel_preview.png`, `panel_top_bar.png`, `panel_bottom_bar.png`,
    `panel_modal.png`, `escape_menu_background.png`) matching the base Halo CE menu UI styling and fonts.
    """
    ui_dir = forge_addon_dir / "ui"
    ui_dir.mkdir(parents=True, exist_ok=True)
    extract_escape_menu_background_png(forge_addon_dir)

    panels = [
        ("panel_spawn_menu.png", 260, 220, (5, 19, 44), (3, 12, 28), (40, 150, 255), 24, (12, 38, 82), 232),
        ("panel_preview.png", 260, 52, (5, 19, 44), (3, 12, 28), (40, 150, 255), 0, (10, 32, 68), 228),
        ("panel_top_bar.png", 234, 28, (5, 19, 44), (3, 12, 28), (40, 150, 255), 0, (10, 32, 68), 224),
        ("panel_bottom_bar.png", 510, 18, (5, 19, 44), (3, 12, 28), (40, 150, 255), 0, (10, 32, 68), 220),
        ("panel_modal.png", 420, 260, (5, 19, 44), (3, 12, 28), (40, 150, 255), 24, (12, 38, 82), 236),
    ]
    for filename, w, h, top_c, bot_c, brd_c, hdr_h, hdr_c, alpha in panels:
        png_path = ui_dir / filename
        rgba = _build_panel_png_bytes(w, h, top_c, bot_c, brd_c, hdr_h, hdr_c, alpha)
        write_rgba_png(png_path, w, h, rgba)

    xml_path = ui_dir / "forge_ui.xml"
    xml_text = """<?xml version="1.0" encoding="utf-8"?>
<!--
  ============================================================================
  OpenCE Forge - Base Game Halo CE Menu UI Theme & Font Configuration
  File: OpenCE Forge/ui/forge_ui.xml
  Press [F8] in-game at any time to live-reload this file and all PNG images!
  ============================================================================

  FONT ATTRIBUTES (available on every UI section below):
    font="large"         Font family: "large" (ui\\large_ui), "small" (ui\\small_ui),
                         "terminal" (ui\\interstate), "ticker", or any loaded font tag path.
    style="plain"        Font style: "plain", "bold", "italic", "condense", "underline".
    size="1.0"           Font size multiplier (e.g. "0.85", "1.0", "1.2", "1.5").
    title_color="#..."   Header / title text color (#RRGGBB or #AARRGGBB hex).
    text_color="#..."    Normal item / body text color (#RRGGBB or #AARRGGBB hex).
    selected_color="#..." Highlighted / selected item text color (#RRGGBB or #AARRGGBB hex).
-->
<forge_ui>
    <!-- [F2] Forge Object Spawn Palette Menu (Base Halo CE Pause/Objectives Menu Style) -->
    <spawn_menu
        font="large"
        style="plain"
        size="1.0"
        title_color="#FFFFFFFF"
        text_color="#FF2896FF"
        selected_color="#FFFFFFFF"
        background="escape_menu_background.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E805132C"
        header_color="#90194480"
        highlight_color="#E81E5AA0"
        border_color="#FF2896FF"
    />

    <!-- Live 3D Item Preview Card (Below the [F2] Item List) -->
    <preview_panel
        font="small"
        style="plain"
        size="1.0"
        title_color="#FFFFFFFF"
        text_color="#FF2896FF"
        selected_color="#FFFFFFFF"
        background="escape_menu_background.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E805132C"
        header_color="#90194480"
        highlight_color="#E81E5AA0"
        border_color="#FF2896FF"
    />

    <!-- [X] Object & Teleporter Properties Panel -->
    <properties_menu
        font="large"
        style="plain"
        size="1.0"
        title_color="#FFFFFFFF"
        text_color="#FF2896FF"
        selected_color="#FFFFFFFF"
        background="escape_menu_background.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E805132C"
        header_color="#90194480"
        highlight_color="#E81E5AA0"
        border_color="#FF2896FF"
    />

    <!-- Top-Right Forge Mode Status & Object Budget Box -->
    <top_bar
        font="small"
        style="plain"
        size="1.0"
        title_color="#FFFFFFFF"
        text_color="#FF2896FF"
        selected_color="#FFFFFFFF"
        background="escape_menu_background.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E805132C"
        header_color="#90194480"
        border_color="#FF2896FF"
    />

    <!-- Bottom Control Strip ([F1] Forge Mode / [F2] Menu / [F5] Quick Save) -->
    <bottom_bar
        font="small"
        style="plain"
        size="1.0"
        title_color="#FFFFFFFF"
        text_color="#FF2896FF"
        selected_color="#FFFFFFFF"
        background="panel_bottom_bar.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E805132C"
        header_color="#90194480"
        border_color="#FF2896FF"
    />

    <!-- [Esc -> SAVE MAP..] Initial Save Map Name Modal (Halo CE Escape Menu Style) -->
    <save_modal
        font="large"
        style="plain"
        size="1.0"
        title_color="#FFFFFFFF"
        text_color="#FF2896FF"
        selected_color="#FFFFFFFF"
        background="escape_menu_background.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E805132C"
        header_color="#90194480"
        highlight_color="#E81E5AA0"
        border_color="#FF2896FF"
    />

    <!-- MAPS: Add Tags to Cache Modal -->
    <maps_modal
        font="large"
        style="plain"
        size="1.0"
        title_color="#FFFFFFFF"
        text_color="#FF2896FF"
        selected_color="#FFFFFFFF"
        background="escape_menu_background.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E805132C"
        header_color="#90194480"
        highlight_color="#E81E5AA0"
        border_color="#FF2896FF"
    />

    <!-- Status Notification Toast Bar -->
    <toast_bar
        font="large"
        style="plain"
        size="1.0"
        title_color="#FFFFFFFF"
        text_color="#FF2896FF"
        selected_color="#FFFFFFFF"
        background="panel_bottom_bar.png"
        use_image="true"
        image_tint="#FFFFFFFF"
        bg_color="#E805132C"
        header_color="#90194480"
        border_color="#FF2896FF"
    />

    <!-- 3D Teleporter In/Out, Objective Markers & Reticle Hover Labels -->
    <markers
        font="small"
        style="plain"
        size="1.0"
        title_color="#FFFFFFFF"
        text_color="#FF2896FF"
        selected_color="#FFFFFFFF"
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
    zlib_objs = [str(p) for p in sorted((obj_root / "port" / "third_party" / "zlib").glob("*.o"))]
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
        *zlib_objs,
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
        (ROOT_DIR / "LICENSE", OPENCE_BUILD_DIR / "LICENSE.txt"),
        (ROOT_DIR / "port/assets/network/brokers.txt", OPENCE_BUILD_DIR / "brokers.txt"),
        (ROOT_DIR / "port/assets/network/masters.txt", OPENCE_BUILD_DIR / "masters.txt"),
        (ROOT_DIR / "deploy/init.txt.example", OPENCE_BUILD_DIR / "init.txt.example"),
        (ROOT_DIR / "port/third_party/jsmn/LICENSE", OPENCE_BUILD_DIR / "jsmn-LICENSE.txt"),
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
    if (ROOT_DIR / "deploy/gametypes").is_dir():
        shutil.copytree(ROOT_DIR / "deploy/gametypes", OPENCE_BUILD_DIR / "gametypes", dirs_exist_ok=True)

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

    # Keep Forge .map files in custom_maps/<name>.map (so standard OpenCE can launch them directly)
    # and keep Forge sidecar files (.bmp, .txt) in forge_maps/<name>/
    for sub in list(saved_forge_maps_dst.iterdir()):
        if not sub.is_dir():
            continue
        stem = sub.name
        legacy_map = sub / f"{stem}.map"
        if legacy_map.is_file():
            dst_map = custom_maps_dst / f"{stem}.map"
            shutil.move(str(legacy_map), str(dst_map))
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
            for ext in (".bmp", ".txt"):
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
                if f.is_file() and f.name.lower() not in ("forgeasset.map", "customforgeasset.map"):
                    dst_f = forge_maps_dir / f.name
                    if not dst_f.is_file():
                        shutil.copy2(f, dst_f)

    # Remove legacy bundles and redundant resource maps (bitmaps.map/sounds.map/loc.map live in custom_maps/)
    for legacy_bundle in ("forgeasset.map", "customforgeasset.map", "bitmaps.map", "sounds.map", "loc.map"):
        lb = forge_maps_dir / legacy_bundle
        if lb.is_file():
            lb.unlink()

    # Ensure all clean Custom Edition default MP maps are in OpenCE Forge/forge_maps/ as zlib-compressed v609 base scenario fallbacks
    if ce_default_src.is_dir():
        for mp in sorted(ce_default_src.glob("*.map")):
            if mp.name.lower() in ("bitmaps.map", "sounds.map", "loc.map", "ui.map"):
                continue
            dst_mp = forge_maps_dir / mp.name
            raw_mp = mp.read_bytes()
            if len(raw_mp) > 0x800:
                need_write = True
                if dst_mp.is_file() and dst_mp.stat().st_size < len(raw_mp):
                    hdr_check = dst_mp.read_bytes()[:0x800]
                    if hdr_check == raw_mp[:0x800]:
                        need_write = False
                if need_write:
                    comp_payload = zlib.compress(raw_mp[0x800:], 6)
                    dst_mp.write_bytes(raw_mp[:0x800] + comp_payload)

    if (forge_addon_dir / "menus").is_dir():
        shutil.rmtree(forge_addon_dir / "menus")
    generate_customizable_forge_ui_assets(forge_addon_dir)

    # Ensure OpenCE Forge/cached_tags/markers and OpenCE Forge/cached_tags/xbox_cache exist
    cached_root = forge_addon_dir / "cached_tags"
    (cached_root / "markers").mkdir(parents=True, exist_ok=True)
    (cached_root / "xbox_cache").mkdir(parents=True, exist_ok=True)
    for cat_name in ("catalog.txt", "xbox_catalog.txt", "custom_catalog.txt"):
        cat_f = cached_root / cat_name
        if cat_f.is_file():
            cat_f.unlink()
    for sub in ("scenery", "vehicles", "weapons", "bipeds", "devices", "bitmaps", "misc"):
        sub_dir = cached_root / sub
        if sub_dir.is_dir():
            shutil.rmtree(sub_dir)
    compile_objective_marker_objs_to_ftags(forge_addon_dir)

    custom_tags_dir = forge_addon_dir / "custom_tags"
    custom_tags_dir.mkdir(parents=True, exist_ok=True)
    root_custom_tags_dir = OPENCE_BUILD_DIR / "custom_tags"
    root_custom_tags_dir.mkdir(parents=True, exist_ok=True)
    ocetags_dir = forge_addon_dir / "ocetags"
    ocetags_dir.mkdir(parents=True, exist_ok=True)
    # Ensure custom_tags/ starts clean by default (do not pre-populate sample .ocetags into the default list)
    for d_clean in (custom_tags_dir, root_custom_tags_dir, ocetags_dir):
        for stale_oce in d_clean.glob("*.ocetags"):
            stale_oce.unlink()

    shutil.copy2(FORGE_DIR / "README.md", forge_addon_dir / "README.md")
    shutil.copy2(FORGE_DIR / "projectrules.md", forge_addon_dir / "projectrules.md")

    ac_exe = FORGE_BUILD_DIR / "forge_asset_compiler.exe"
    if ac_exe.is_file():
        shutil.copy2(ac_exe, forge_tools_dir / "forge_asset_compiler.exe")

    # Clean up old root-level and legacy build artifacts so everything is cleanly inside `OpenCE Build/` as `halo.exe`
    for legacy_f in ("HaloCE Forge.exe", "HaloCE Forge.pdb"):
        lf = OPENCE_BUILD_DIR / legacy_f
        if lf.is_file():
            lf.unlink()
    for stray_file in ("HaloCE Forge.exe", "HaloCE Forge.pdb", "halo.exe", "SDL3.dll", "brokers.txt"):
        p = ROOT_DIR / stray_file
        if p.is_file():
            p.unlink()
    for stray_dir in ("OpenCE Forge", "custom_maps"):
        d = ROOT_DIR / stray_dir
        if d.is_dir():
            shutil.rmtree(d)


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
        ("KOTH-Marker.obj", "koth_marker.ftag", "objective\\koth_hill", 1052.129, None, 64.0),
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

    # Generate unit 3D KOTH boundary beam mesh (markers/koth_beam.ftag)
    # Stretched in 3D between adjacent KOTH hill nodes when a hill node is clicked/grabbed.
    beam_y = 0.014
    beam_z0, beam_z1 = 0.025, 0.053
    beam_faces = [
        # Top (+Z)
        ((0.0, -beam_y, beam_z1), (1.0, -beam_y, beam_z1), (1.0, beam_y, beam_z1), (0.0, beam_y, beam_z1), (0.0, 0.0, 1.0)),
        # Bottom (-Z)
        ((0.0, beam_y, beam_z0), (1.0, beam_y, beam_z0), (1.0, -beam_y, beam_z0), (0.0, -beam_y, beam_z0), (0.0, 0.0, -1.0)),
        # Left (+Y)
        ((0.0, beam_y, beam_z0), (0.0, beam_y, beam_z1), (1.0, beam_y, beam_z1), (1.0, beam_y, beam_z0), (0.0, 1.0, 0.0)),
        # Right (-Y)
        ((0.0, -beam_y, beam_z1), (0.0, -beam_y, beam_z0), (1.0, -beam_y, beam_z0), (1.0, -beam_y, beam_z1), (0.0, -1.0, 0.0)),
    ]
    beam_verts: list[tuple[float, float, float, float, float, float]] = []
    beam_tris: list[tuple[int, int, int]] = []
    for p0, p1, p2, p3, (nx, ny, nz) in beam_faces:
        bi = len(beam_verts)
        for px, py, pz in (p0, p1, p2, p3):
            beam_verts.append((px, py, pz, nx, ny, nz))
        beam_tris.append((bi + 0, bi + 1, bi + 2))
        beam_tris.append((bi + 0, bi + 2, bi + 3))
    beam_parts = _pack_ftag_parts_from_triangles(beam_verts, beam_tris)
    if _write_ftag_file(markers_out_dir / "koth_beam.ftag", "scen", "objective\\koth_beam", 1.0, beam_parts):
        print(f"[build_forge] Compiled 3D KOTH Beam Marker -> markers/koth_beam.ftag ({len(beam_verts)} verts, {len(beam_tris)} tris)")


def build_haloce_forge_exe() -> Path:
    clang = find_llvm_tool("clang")
    objcopy = find_llvm_tool("llvm-objcopy")
    game_cflags, platform_cflags = get_cflags_sets()

    forge_sources = [
        "forge_cache_builder.c",
        "forge_runtime_tags.c",
        "forge_ai.c",
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

    # Wrap menu_functions.o symbols so forge_hooks.c intercepts menu events, unlocks all campaign maps in Forge mode,
    # and separates FORGE MAPS from CUSTOM MULTIPLAYER maps
    subprocess.run([
        objcopy,
        "--redefine-sym", "_pc_menu_event_function_invoke=_orig_pc_menu_event_function_invoke",
        "--redefine-sym", "_pc_menu_game_data_function_invoke=_orig_pc_menu_game_data_function_invoke",
        "--redefine-sym", "_player_profile_get=_forge_player_profile_get",
        "--redefine-sym", "_custom_edition_maps_count=_forge_menu_custom_edition_maps_count",
        "--redefine-sym", "_custom_edition_maps_display_index_of=_forge_menu_custom_edition_maps_display_index_of",
        "--redefine-sym", "_custom_edition_maps_look_again=_forge_menu_custom_edition_maps_look_again",
        "--redefine-sym", "_ui_widget_port_multiplayer_maps=_forge_menu_multiplayer_maps",
        "--redefine-sym", "_ui_widget_port_multiplayer_map_choose=_forge_menu_multiplayer_map_choose",
        str(net_replacements["port/linux/game/menu_functions.o"]),
    ], check=True)

    # Wrap base engine symbols via llvm-objcopy --redefine-sym
    obj_root = ROOT_DIR / "build" / "windows" / "obj"
    wrap_dir = FORGE_BUILD_DIR / "wrapped_obj"
    wrap_dir.mkdir(parents=True, exist_ok=True)

    # Regenerate and compile embedded menu assets (hud_hires_assets.c) and menu_tags.c
    hud_assets_c = ROOT_DIR / "build" / "windows" / "generated" / "hud_hires_assets.c"
    print("[build_forge] Regenerating embedded menu & HUD assets (hud_hires_assets.c)...")
    subprocess.run([sys.executable, str(ROOT_DIR / "tools/embed_assets.py"), str(hud_assets_c)], cwd=str(ROOT_DIR), check=True)
    hud_assets_o = obj_root / "build/windows/generated/hud_hires_assets.o"
    print("[build_forge] Compiling hud_hires_assets.c...")
    compile_c(clang, platform_cflags, hud_assets_c, hud_assets_o)
    menu_tags_o = obj_root / "port/linux/game/menu_tags.o"
    print("[build_forge] Compiling menu_tags.c...")
    compile_c(clang, game_cflags, ROOT_DIR / "port/linux/game/menu_tags.c", menu_tags_o)

    chat_wrapped = wrap_dir / "network_chat.o"
    shutil.copyfile(obj_root / "port/linux/game/network_chat.o", chat_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_network_chat_update=_orig_network_chat_update",
        "--redefine-sym", "_network_chat_draw=_orig_network_chat_draw",
        "--redefine-sym", "_network_chat_typing=_orig_network_chat_typing",
        "--redefine-sym", "_network_chat_handle_up=_orig_network_chat_handle_up",
        "--redefine-sym", "_network_chat_handle_down=_orig_network_chat_handle_down",
        str(chat_wrapped),
    ], check=True)
    net_replacements["port/linux/game/network_chat.o"] = chat_wrapped

    net_objects_wrapped = wrap_dir / "network_objects.o"
    shutil.copyfile(obj_root / "port/linux/game/network_objects.o", net_objects_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_network_objects_handle_changes=_orig_network_objects_handle_changes",
        str(net_objects_wrapped),
    ], check=True)
    net_replacements["port/linux/game/network_objects.o"] = net_objects_wrapped

    director_wrapped = wrap_dir / "director.o"
    shutil.copyfile(obj_root / "source/camera/director.o", director_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_director_update=_orig_director_update",
        "--redefine-sym", "_director_get_perspective=_orig_director_get_perspective",
        str(director_wrapped),
    ], check=True)
    net_replacements["source/camera/director.o"] = director_wrapped

    render_interp_wrapped = wrap_dir / "render_interpolation.o"
    shutil.copyfile(obj_root / "port/linux/game/render_interpolation.o", render_interp_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_render_interpolation_camera=_orig_render_interpolation_camera",
        str(render_interp_wrapped),
    ], check=True)
    net_replacements["port/linux/game/render_interpolation.o"] = render_interp_wrapped

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

    mod_sounds_wrapped = wrap_dir / "mod_sounds.o"
    shutil.copyfile(obj_root / "port/linux/game/mod_sounds.o", mod_sounds_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_mod_sounds_read=_orig_mod_sounds_read",
        str(mod_sounds_wrapped),
    ], check=True)
    net_replacements["port/linux/game/mod_sounds.o"] = mod_sounds_wrapped

    sdl_platform_wrapped = wrap_dir / "sdl_platform.o"
    shutil.copyfile(obj_root / "port/linux/src/sdl_platform.o", sdl_platform_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_platform_ui_pointer_read=_orig_platform_ui_pointer_read",
        "--redefine-sym", "_platform_input_read=_orig_platform_input_read",
        "--redefine-sym", "_platform_next_keystroke=_orig_platform_next_keystroke",
        str(sdl_platform_wrapped),
    ], check=True)
    net_replacements["port/linux/src/sdl_platform.o"] = sdl_platform_wrapped

    xinput_sdl_wrapped = wrap_dir / "xinput_sdl.o"
    shutil.copyfile(obj_root / "port/linux/src/xinput_sdl.o", xinput_sdl_wrapped)
    subprocess.run([
        objcopy,
        "--redefine-sym", "_XInputGetState@8=_orig_XInputGetState@8",
        str(xinput_sdl_wrapped),
    ], check=True)
    net_replacements["port/linux/src/xinput_sdl.o"] = xinput_sdl_wrapped

    # Point 11: Redirect custom_maps\<name>.(map|bmp|txt) and serve embedded .bmp/.txt inside .map
    for ce_unit in ("port/linux/game/custom_edition_cache.o", "port/linux/game/custom_edition_maps.o", "port/linux/game/map_share_files.o"):
        ce_src = obj_root / ce_unit
        if ce_src.is_file():
            ce_wrapped = wrap_dir / Path(ce_unit).name
            shutil.copyfile(ce_src, ce_wrapped)
            redef_args = [
                objcopy,
                "--redefine-sym", "_file_reference_create_from_path=_forge_ce_file_reference_create_from_path",
                "--redefine-sym", "_halo_windows_fopen=_forge_ce_fopen",
                "--redefine-sym", "_halo_xbox_fopen=_forge_ce_fopen",
                "--redefine-sym", "_fopen=_forge_ce_fopen",
                "--redefine-sym", "_halo_xbox_FindFirstFileA@8=_forge_ce_FindFirstFileA@8",
                "--redefine-sym", "_halo_xbox_FindNextFileA@8=_forge_ce_FindNextFileA@8",
                "--redefine-sym", "_halo_xbox_CloseHandle@4=_forge_ce_CloseHandle@4",
            ]
            if ce_unit == "port/linux/game/custom_edition_maps.o":
                redef_args.extend([
                    "--redefine-sym", "_fread=_forge_ce_fread",
                    "--redefine-sym", "_fseek=_forge_ce_fseek",
                    "--redefine-sym", "_ftell=_forge_ce_ftell",
                    "--redefine-sym", "_fclose=_forge_ce_fclose",
                ])
            redef_args.append(str(ce_wrapped))
            subprocess.run(redef_args, check=True)
            net_replacements[ce_unit] = ce_wrapped

    # Collect all 840 base object files, substituting wrapped & network-isolated copies
    all_objs: list[str] = []
    for o_path in sorted(obj_root.rglob("*.o")):
        rel_key = o_path.relative_to(obj_root).as_posix()
        if rel_key.startswith("FORGE CODE/"):
            continue
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
    build_exe_out = ROOT_DIR / "build" / "windows" / "halo.exe"
    exe_out = OPENCE_BUILD_DIR / "halo.exe"
    link_cmd = [
        clang,
        "--target=i686-pc-windows-msvc",
        "-fuse-ld=lld",
        "-g",
        "-Wl,/LARGEADDRESSAWARE",
        "-Wl,/STACK:0x1000000",
        "-Wl,/SUBSYSTEM:CONSOLE",
        "-o",
        str(build_exe_out),
        "--rsp-quoting=windows",
        f"@{rsp_file}",
        *libs,
    ]
    print(f"[build_forge] Linking {build_exe_out}...")
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
