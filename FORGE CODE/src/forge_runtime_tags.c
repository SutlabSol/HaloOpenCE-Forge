/*
 * OpenCE Forge ("HaloCE Forge.exe")
 * Runtime Tag Catalog, Cached Tag Folder System (OpenCE Forge/cached_tags/),
 * Dynamic Runtime Tag Table Expansion, Multiplayer Asset Guard, Custom Tag Extraction,
 * Standalone Map Baking, Dynamic Network Version & 343 Guilty Spark Hardware Model Renderer.
 * Strictly modular inside FORGE CODE/ with zero modifications to source/ or port/.
 */

#include "cseries.h"
#include "game/game.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "units/units.h"
#include "models/models.h"
#include "math/real_math.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "game/players.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "render/render.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "cache/cache_files.h"
#include "custom_edition_cache.h"
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"
#include "rasterizer/xbox/rasterizer_xbox_pixel_shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "forge_engine.h"

#undef fopen
#undef fclose
#undef fread
#undef fwrite
#undef fseek
#undef ftell
#undef remove
#undef malloc
#undef calloc
#undef free
#undef realloc

extern unsigned long system_milliseconds(void);
extern void *cache_files_tag_instances(long *count);
extern void cache_files_set_tag_instances(void *instances, long count);

struct forge_cache_file_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

struct forge_runtime_state g_forge;
int g_forge_session_enabled = 1;

int forge_get_network_version(void)
{
	return g_forge_session_enabled ? 0x461D : 29;
}

void forge_set_status(char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_forge.status_message, sizeof(g_forge.status_message), fmt, ap);
	va_end(ap);
	g_forge.status_message_until_ms = (uint32_t)system_milliseconds() + 6000u;
}

void forge_draw_screen_rect(short x0, short y0, short x1, short y1, uint32_t argb)
{
	if (!global_d3d_device || x1 <= x0 || y1 <= y0)
		return;

	_rasterizer_debug_immediate_begin_screenspace();
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHABLENDENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
	pixel_shader.final_combiner_inputs_efg = 0x1400;
	rasterizer_set_pixel_shader(&pixel_shader);

	IDirect3DDevice8_Begin(global_d3d_device, D3DPT_TRIANGLEFAN);
	IDirect3DDevice8_SetVertexDataColor(global_d3d_device, 9, argb);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x0, y0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x1, y0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x1, y1);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x0, y1);
	IDirect3DDevice8_End(global_d3d_device);
	_rasterizer_debug_immediate_end_screenspace();
}

int forge_is_multiplayer_session(void)
{
	short conn = game_connection();
	return (conn == _game_connection_network_client || conn == _game_connection_network_server) ? 1 : 0;
}

int forge_catalog_entry_allowed(struct forge_catalog_entry const *entry)
{
	if (!entry)
		return 0;
	/* Point 3: Remove option to spawn bipeds and AI characters for now */
	if (entry->category == _forge_cat_bipeds ||
		entry->category == _forge_cat_ai_squads ||
		entry->group_tag == 'bipd' ||
		entry->group_tag == 'actv')
	{
		return 0;
	}
	/*
	 * MULTIPLAYER ASSET GUARD:
	 * Live Multiplayer / Online Co-op Forge sessions are strictly restricted to
	 * universal stock Forge cached tags (source_cache_id == 0).
	 * Custom extracted tags (source_cache_id == 1) are only enabled in Single-Player Forge.
	 */
	if (forge_is_multiplayer_session() && entry->source_cache_id != 0)
		return 0;
	return 1;
}

static void make_short_display_name(char const *tag_path, char const *prefix, char *out, size_t out_sz)
{
	char const *leaf = tag_path;
	char const *p;
	for (p = tag_path; *p; p++)
	{
		if (*p == '\\' || *p == '/')
			leaf = p + 1;
	}
	if (prefix && prefix[0])
		snprintf(out, out_sz, "%s %s", prefix, leaf);
	else
		snprintf(out, out_sz, "%s", leaf);
}

static void add_catalog_entry(
	uint32_t group_tag,
	uint8_t category,
	uint8_t source_cache_id,
	char const *tag_path,
	char const *display_prefix)
{
	int i;
	struct forge_catalog_entry *e;

	if (!tag_path || !tag_path[0] || g_forge.catalog_count >= FORGE_MAX_CATALOG_ENTRIES)
		return;

	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (g_forge.catalog[i].group_tag == group_tag &&
			g_forge.catalog[i].category == category &&
			_stricmp(g_forge.catalog[i].tag_path, tag_path) == 0)
		{
			return;
		}
	}

	e = &g_forge.catalog[g_forge.catalog_count++];
	e->group_tag = group_tag;
	e->category = category;
	e->source_cache_id = source_cache_id;
	e->pad = 0;
	snprintf(e->tag_path, sizeof(e->tag_path), "%s", tag_path);
	make_short_display_name(tag_path, display_prefix, e->display_name, sizeof(e->display_name));
}

static void scan_cached_tags_manifest(char const *manifest_path)
{
	FILE *f = fopen(manifest_path, "r");
	char line[512];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
	{
		char grp_str[16];
		int cat_id = 0;
		int src_id = 0;
		char tag_path[256];
		uint32_t grp = 0;
		char *nl;

		nl = strchr(line, '\r');
		if (nl) *nl = '\0';
		nl = strchr(line, '\n');
		if (nl) *nl = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;

		if (sscanf(line, "%4s|%d|%d|%255[^\n]", grp_str, &cat_id, &src_id, tag_path) == 4)
		{
			if (strlen(grp_str) == 4)
			{
				grp = ((uint32_t)(uint8_t)grp_str[0] << 24) |
					  ((uint32_t)(uint8_t)grp_str[1] << 16) |
					  ((uint32_t)(uint8_t)grp_str[2] << 8) |
					  ((uint32_t)(uint8_t)grp_str[3]);
				add_catalog_entry(
					grp,
					(uint8_t)cat_id,
					(uint8_t)src_id,
					tag_path,
					src_id ? (grp == 'actv' ? "[CUSTOM AI]" : "[CUSTOM]") : (grp == 'actv' ? "[AI]" : ""));
			}
		}
	}
	fclose(f);
}

static void scan_cache_file_into_catalog(char const *map_path, uint8_t source_cache_id)
{
	FILE *f;
	uint8_t hdr[2048];
	uint32_t tag_off, tag_sz;
	uint8_t *tag_buf;
	uint32_t inst_addr, inst_off;
	int32_t tag_count, i;

	f = fopen(map_path, "rb");
	if (!f)
		return;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return;
	}
	tag_off = *(uint32_t const *)(hdr + 0x10);
	tag_sz = *(uint32_t const *)(hdr + 0x14);
	if (tag_off < 2048 || tag_sz < 40 || tag_sz > 64 * 1024 * 1024)
	{
		fclose(f);
		return;
	}
	tag_buf = (uint8_t *)malloc(tag_sz);
	if (!tag_buf)
	{
		fclose(f);
		return;
	}
	fseek(f, (long)tag_off, SEEK_SET);
	if (fread(tag_buf, 1, tag_sz, f) != tag_sz)
	{
		free(tag_buf);
		fclose(f);
		return;
	}
	fclose(f);

	inst_addr = *(uint32_t const *)(tag_buf + 0x00);
	tag_count = *(int32_t const *)(tag_buf + 0x0C);
	if (inst_addr < 0x40440000u || tag_count <= 0 || tag_count > 16384)
	{
		free(tag_buf);
		return;
	}
	inst_off = inst_addr - 0x40440000u;

	for (i = 0; i < tag_count; i++)
	{
		uint32_t io = inst_off + (uint32_t)i * 32u;
		uint32_t grp, name_addr;
		char const *tname;
		char const *prefix = source_cache_id ? "[CUSTOM]" : "";

		if (io + 32u > tag_sz)
			break;
		grp = *(uint32_t const *)(tag_buf + io + 0x00);
		name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
		if (name_addr < 0x40440000u || (name_addr - 0x40440000u) >= tag_sz)
			continue;
		tname = (char const *)(tag_buf + (name_addr - 0x40440000u));

		if (grp == 'weap')
		{
			if (strstr(tname, "flag") == NULL && strstr(tname, "ball") == NULL)
				add_catalog_entry('weap', _forge_cat_weapons, source_cache_id, tname, prefix);
		}
		else if (grp == 'vehi')
		{
			add_catalog_entry('vehi', _forge_cat_vehicles, source_cache_id, tname, prefix);
		}
		else if (grp == 'bipd')
		{
			add_catalog_entry('bipd', _forge_cat_bipeds, source_cache_id, tname, prefix);
		}
		else if (grp == 'scen')
		{
			add_catalog_entry('scen', _forge_cat_scenery, source_cache_id, tname, prefix);
		}
		else if (grp == 'mach' || grp == 'ctrl' || grp == 'lifi')
		{
			add_catalog_entry(grp, _forge_cat_barriers_devices, source_cache_id, tname, prefix);
		}
		else if (grp == 'actv')
		{
			add_catalog_entry('actv', _forge_cat_ai_squads, source_cache_id, tname, source_cache_id ? "[CUSTOM AI]" : "[AI]");
		}
	}

	free(tag_buf);
}

static void scan_donor_maps_directory(char const *dir_path)
{
	char pattern[512];
	WIN32_FIND_DATAA fd;
	HANDLE h;

	snprintf(pattern, sizeof(pattern), "%s\\*.map", dir_path);
	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (_stricmp(fd.cFileName, "bitmaps.map") == 0 ||
			_stricmp(fd.cFileName, "sounds.map") == 0 ||
			_stricmp(fd.cFileName, "loc.map") == 0 ||
			_stricmp(fd.cFileName, "forgeasset.map") == 0 ||
			_stricmp(fd.cFileName, "customforgeasset.map") == 0)
		{
			continue;
		}
		if (g_forge.donor_map_count < FORGE_MAX_DONOR_MAPS_LIST)
		{
			snprintf(
				g_forge.donor_map_files[g_forge.donor_map_count++],
				sizeof(g_forge.donor_map_files[0]),
				"%s/%s",
				dir_path,
				fd.cFileName);
		}
	} while (FindNextFileA(h, &fd));
	CloseHandle(h);
}

void forge_runtime_reload_catalogs(void)
{
	g_forge.catalog_count = 0;

	/* Built-in Spawns & Teleporters */
	add_catalog_entry('spwn', _forge_cat_spawns_teleporters, 0, "spawn\\player_red", "[SPAWN] Red Team");
	add_catalog_entry('spwn', _forge_cat_spawns_teleporters, 0, "spawn\\player_blue", "[SPAWN] Blue Team");
	add_catalog_entry('spwn', _forge_cat_spawns_teleporters, 0, "spawn\\player_ffa", "[SPAWN] FFA / Neutral");
	add_catalog_entry('tele', _forge_cat_spawns_teleporters, 0, "teleporter\\origin", "[TELEPORTER] Origin Node");
	add_catalog_entry('tele', _forge_cat_spawns_teleporters, 0, "teleporter\\destination", "[TELEPORTER] Destination Node");

	/* Built-in Netgame Objectives */
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\ctf_flag_red", "[CTF] Red Flag Base");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\ctf_flag_blue", "[CTF] Blue Flag Base");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\oddball_spawn", "[ODDBALL] Ball Spawn");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\koth_hill", "[KOTH] Hill Marker");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\race_track", "[RACE] Checkpoint");

	/* 1. Scan Cached Tag Folder System manifest first (OpenCE Forge/cached_tags/catalog.txt) */
	scan_cached_tags_manifest("OpenCE Forge/cached_tags/catalog.txt");
	scan_cached_tags_manifest("OpenCE Forge/cached_tags/custom_catalog.txt");

	/* 2. Also scan cached tag binary bundles if present */
	scan_cache_file_into_catalog("OpenCE Forge/cached_tags/bitmaps_cache.bin", 0);
	scan_cache_file_into_catalog("OpenCE Forge/cached_tags/custom_bitmaps_cache.bin", 1);
	scan_cache_file_into_catalog("OpenCE Forge/forge_maps/forgeasset.map", 0);
	scan_cache_file_into_catalog("OpenCE Forge/forge_maps/customforgeasset.map", 1);

	/* Refresh donor maps list for "Add Tags to Cache" (MAPS) UI modal */
	g_forge.donor_map_count = 0;
	scan_donor_maps_directory("custom_maps");
	scan_donor_maps_directory("maps");
	scan_donor_maps_directory("OpenCE Forge/custom_maps");
	scan_donor_maps_directory("FORGE CODE/GAME MAPS/CUSTOM EDITION MAPS");
	scan_donor_maps_directory("FORGE CODE/GAME MAPS/PC MAPS");
}

#ifdef _WIN32
__declspec(dllimport) int __stdcall CreateDirectoryA(char const *lpPathName, void *lpSecurityAttributes);
__declspec(dllimport) int __stdcall MessageBoxA(void *hWnd, char const *lpText, char const *lpCaption, unsigned int uType);
__declspec(dllimport) void *__stdcall LoadLibraryA(char const *lpLibFileName);
__declspec(dllimport) void *__stdcall GetProcAddress(void *hModule, char const *lpProcName);

struct forge_browseinfo_a
{
	void *hwndOwner;
	void *pidlRoot;
	char *pszDisplayName;
	char const *lpszTitle;
	unsigned int ulFlags;
	void *lpfn;
	long lParam;
	int iImage;
};

static int forge_browse_for_folder(char const *title, char const *default_dir, char *out_dir, size_t out_sz)
{
	typedef void *(__stdcall *sh_browse_fn)(struct forge_browseinfo_a *);
	typedef int (__stdcall *sh_get_path_fn)(void *, char *);
	void *shell32 = LoadLibraryA("shell32.dll");
	if (shell32)
	{
		sh_browse_fn pBrowse = (sh_browse_fn)GetProcAddress(shell32, "SHBrowseForFolderA");
		sh_get_path_fn pGetPath = (sh_get_path_fn)GetProcAddress(shell32, "SHGetPathFromIDListA");
		if (pBrowse && pGetPath)
		{
			char disp[260];
			char chosen[260];
			struct forge_browseinfo_a bi;
			void *pidl;
			memset(disp, 0, sizeof(disp));
			memset(chosen, 0, sizeof(chosen));
			memset(&bi, 0, sizeof(bi));
			bi.pszDisplayName = disp;
			bi.lpszTitle = title;
			bi.ulFlags = 0x00000001u | 0x00000040u; /* BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE */
			pidl = pBrowse(&bi);
			if (pidl && pGetPath(pidl, chosen) && chosen[0])
			{
				snprintf(out_dir, out_sz, "%s", chosen);
				return 1;
			}
		}
	}
	snprintf(out_dir, out_sz, "%s", default_dir);
	return 0;
}

static void forge_copy_res_file_if_missing(char const *src_path, char const *dst_path)
{
	FILE *check = fopen(dst_path, "rb");
	FILE *in, *out;
	char buf[65536];
	size_t n;
	if (check)
	{
		fclose(check);
		return;
	}
	in = fopen(src_path, "rb");
	if (!in)
		return;
	out = fopen(dst_path, "wb");
	if (!out)
	{
		fclose(in);
		return;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(out);
	fclose(in);
}
#endif

static void forge_extract_cache_to_cached_tags_folder(
	char const *cache_bin_path,
	uint8_t source_cache_id,
	char const *manifest_path);

static void forge_ensure_local_tag_cache_built(void)
{
	FILE *f = fopen("OpenCE Forge/cached_tags/catalog.txt", "rb");
	if (f)
	{
		fclose(f);
		return;
	}

	/* Point 6: If the local tag cache (OpenCE Forge/cached_tags/) is not yet built on first launch,
	 * ask to build it locally by pointing to Custom Edition and Xbox / Retail PC map folders */
#ifdef _WIN32
	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/forge_maps", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/vehicles", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/weapons", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/scenery", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/devices", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/bipeds", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/misc", NULL);
	CreateDirectoryA("forge_maps", NULL);

	{
		int resp = MessageBoxA(
			NULL,
			"OpenCE Forge - First Launch Local Tag Cache Builder\n\n"
			"No local Forge tag cache was found in 'OpenCE Forge/cached_tags/'.\n\n"
			"Would you like to build the local tag cache now?\n"
			"You will be asked to point to:\n"
			"  1. Your Custom Edition maps folder (bitmaps.map, sounds.map, loc.map & multiplayer maps)\n"
			"  2. Your Xbox / Retail PC maps folder (bitmaps.map, sounds.map & campaign maps)\n\n"
			"(Tip: Press Cancel in either folder dialog to use your local 'custom_maps/' and 'maps/' folders automatically.)",
			"OpenCE Forge - First Launch Tag Cache Builder",
			0x00000004u | 0x00000040u /* MB_YESNO | MB_ICONINFORMATION */);
		if (resp == 6 /* IDYES */)
		{
			char ce_dir[260];
			char pc_dir[260];
			char res_src[320];

			forge_browse_for_folder(
				"Step 1/2: Select your Halo Custom Edition folder (containing bitmaps.map, sounds.map, loc.map & CE maps):",
				"custom_maps",
				ce_dir,
				sizeof(ce_dir));

			forge_browse_for_folder(
				"Step 2/2: Select your Halo Xbox / Retail PC 'maps' folder (containing bitmaps.map, sounds.map & campaign maps):",
				"maps",
				pc_dir,
				sizeof(pc_dir));

			snprintf(res_src, sizeof(res_src), "%s/bitmaps.map", ce_dir);
			forge_copy_res_file_if_missing(res_src, "OpenCE Forge/forge_maps/bitmaps.map");
			snprintf(res_src, sizeof(res_src), "%s/sounds.map", ce_dir);
			forge_copy_res_file_if_missing(res_src, "OpenCE Forge/forge_maps/sounds.map");
			snprintf(res_src, sizeof(res_src), "%s/loc.map", ce_dir);
			forge_copy_res_file_if_missing(res_src, "OpenCE Forge/forge_maps/loc.map");

			if (forge_build_master_asset_cache(
					ce_dir,
					pc_dir,
					"OpenCE Forge/cached_tags/bitmaps_cache.bin",
					"OpenCE Forge/credits.md"))
			{
				forge_extract_cache_to_cached_tags_folder(
					"OpenCE Forge/cached_tags/bitmaps_cache.bin",
					0,
					"OpenCE Forge/cached_tags/catalog.txt");
			}
		}
	}
#endif
}

void forge_runtime_init(void)
{
	if (g_forge.initialized)
		return;
	memset(&g_forge, 0, sizeof(g_forge));
	g_forge.initialized = 1;
	g_forge.mode = _forge_mode_play;
	g_forge.active_modal = _forge_modal_none;
	g_forge.current_category = _forge_cat_vehicles;
	g_forge.snap_mode = _forge_snap_15_deg;
	g_forge.ai_team_mode = _forge_ai_team_enemy_covenant;
	g_forge.cam_speed = 12.0f;
	g_forge.grabbed_object_index = NONE;
	g_forge.grabbed_live_index = -1;
	g_forge.hovered_object_index = NONE;
	g_forge.hovered_live_index = -1;
	g_forge.grab_distance = 5.0f;
	g_forge.bp_zoom = 1.0f;
	g_forge.bp_cam_z = 12.0f;
	snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "forge_custom_arena");
	g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);

#ifdef _WIN32
	CreateDirectoryA("forge_maps", NULL);
#endif
	forge_ensure_local_tag_cache_built();
	forge_runtime_reload_catalogs();
}

/* ============================================================================
 * CACHED TAG FOLDER SYSTEM ("OpenCE Forge/cached_tags/") &
 * DYNAMIC IN-MEMORY TAG TABLE EXPANSION + HARDWARE MESH LOADER
 * Every Scenery, Vehicle, Weapon, Biped, or Device tag not natively present
 * in the current map gets its own unique runtime tag definition in
 * global_tag_instances AND renders its genuine 3D mesh from cached_tags/!
 * ============================================================================ */

#define FORGE_MAX_HW_PARTS      24
#define FORGE_MAX_DYNAMIC_TAGS  128

struct forge_hw_part
{
	struct vertex_buffer vertex_buffer;
	struct triangle_buffer triangle_buffer;
	char shader_hint[128];
};

struct forge_dynamic_tag_entry
{
	uint32_t group_tag;
	char tag_path[256];
	long definition_index;
	float bounding_radius;
	int part_count;
	struct forge_hw_part parts[FORGE_MAX_HW_PARTS];
};

static struct
{
	void *saved_base_instances;
	long saved_base_count;
	void *allocated_instances;
	int count;
	struct forge_dynamic_tag_entry entries[FORGE_MAX_DYNAMIC_TAGS];
} s_dyn_tags;

void forge_runtime_on_map_unload(void)
{
	if (s_dyn_tags.saved_base_instances)
	{
		cache_files_set_tag_instances(s_dyn_tags.saved_base_instances, s_dyn_tags.saved_base_count);
		s_dyn_tags.saved_base_instances = NULL;
		s_dyn_tags.saved_base_count = 0;
	}
	if (s_dyn_tags.allocated_instances)
	{
		free(s_dyn_tags.allocated_instances);
		s_dyn_tags.allocated_instances = NULL;
	}
	s_dyn_tags.count = 0;
}

static void make_cached_ftag_path(uint32_t group_tag, char const *tag_path, char *out_path, size_t out_sz)
{
	char slug[200];
	char const *sub = "misc";
	size_t i, j = 0;

	if (group_tag == 'scen') sub = "scenery";
	else if (group_tag == 'vehi') sub = "vehicles";
	else if (group_tag == 'weap') sub = "weapons";
	else if (group_tag == 'bipd') sub = "bipeds";
	else if (group_tag == 'mach' || group_tag == 'ctrl' || group_tag == 'lifi') sub = "devices";

	for (i = 0; tag_path[i] && j + 1 < sizeof(slug); i++)
	{
		char c = tag_path[i];
		if (c == '\\' || c == '/' || c == ' ' || c == ':')
			slug[j++] = '_';
		else
			slug[j++] = c;
	}
	slug[j] = '\0';
	snprintf(out_path, out_sz, "OpenCE Forge/cached_tags/%s/%s.ftag", sub, slug);
}

static void build_hw_part_from_arrays(
	struct forge_hw_part *dst_part,
	uint8_t *uncomp_verts,
	int32_t vtx_count,
	uint8_t const *idx_bytes,
	int32_t tri_count,
	char const *shader_hint,
	float scale_factor,
	float z_offset)
{
	uint32_t uncomp_sz = (uint32_t)vtx_count * 68u;
	uint32_t comp_sz = (uint32_t)vtx_count * 32u;
	uint32_t idx_sz = ((uint32_t)tri_count + 2u) * 2u;
	void *hw_verts;
	void *hw_tris;
	int v, c;

	for (v = 0; v < vtx_count; v++)
	{
		uint8_t *vp = uncomp_verts + (uint32_t)v * 68u;
		if (scale_factor != 1.0f || z_offset != 0.0f)
		{
			*(float *)(vp + 0) *= scale_factor;
			*(float *)(vp + 4) *= scale_factor;
			*(float *)(vp + 8) = (*(float *)(vp + 8) * scale_factor) + z_offset;
		}
		for (c = 0; c < 9; c++)
		{
			float val = *(float *)(vp + 12 + c * 4);
			if (!(val >= -1.0f && val <= 1.0f))
				*(float *)(vp + 12 + c * 4) = (c % 3 == 0) ? 1.0f : 0.0f;
		}
		*(int16_t *)(vp + 56) = 0;
		*(int16_t *)(vp + 58) = 0;
		*(float *)(vp + 60) = 1.0f;
		*(float *)(vp + 64) = 0.0f;
	}

	hw_verts = XPhysicalAlloc(comp_sz, (unsigned long)-1, 0, PAGE_READWRITE);
	if (!hw_verts)
		hw_verts = malloc(comp_sz);
	hw_tris = XPhysicalAlloc(idx_sz, (unsigned long)-1, 0, PAGE_READWRITE);
	if (!hw_tris)
		hw_tris = malloc(idx_sz);
	if (!hw_verts || !hw_tris)
		return;

	memset(dst_part, 0, sizeof(*dst_part));
	if (shader_hint && shader_hint[0])
		snprintf(dst_part->shader_hint, sizeof(dst_part->shader_hint), "%s", shader_hint);

	rasterizer_geometry_compress_vertices(
		_rasterizer_vertex_type_model_uncompressed,
		vtx_count,
		hw_verts,
		(long)comp_sz,
		uncomp_verts,
		(long)uncomp_sz);
	memcpy(hw_tris, idx_bytes, idx_sz);

	rasterizer_vertex_buffer_new(
		&dst_part->vertex_buffer,
		_rasterizer_vertex_type_model_compressed,
		vtx_count,
		hw_verts,
		(long)comp_sz);

	rasterizer_triangle_buffer_new(
		&dst_part->triangle_buffer,
		_triangle_buffer_type_precompiled_strip,
		tri_count,
		hw_tris);
}

static int load_hw_parts_from_ftag_file(
	char const *ftag_path,
	struct forge_hw_part *out_parts,
	int max_parts,
	int *out_part_count,
	float *out_radius,
	float scale_factor,
	float z_offset)
{
	FILE *f = fopen(ftag_path, "rb");
	char magic[4];
	uint32_t version = 0, grp = 0, part_count = 0, p;
	float radius = 1.5f;
	char tag_name[256];

	if (!f)
		return 0;
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "FTAG", 4) != 0)
	{
		fclose(f);
		return 0;
	}
	if (fread(&version, 4, 1, f) != 1 || version != 1 ||
		fread(&grp, 4, 1, f) != 1 ||
		fread(&radius, 4, 1, f) != 1 ||
		fread(tag_name, 1, 256, f) != 256 ||
		fread(&part_count, 4, 1, f) != 1)
	{
		fclose(f);
		return 0;
	}

	*out_part_count = 0;
	if (out_radius)
		*out_radius = (radius > 0.1f && radius < 100.0f) ? radius : 1.5f;

	for (p = 0; p < part_count && *out_part_count < max_parts; p++)
	{
		int32_t vtx_count = 0, tri_count = 0;
		char shader_hint[128];
		uint32_t uncomp_sz, idx_sz;
		uint8_t *uncomp_verts;
		uint8_t *idx_bytes;

		if (fread(&vtx_count, 4, 1, f) != 1 ||
			fread(&tri_count, 4, 1, f) != 1 ||
			fread(shader_hint, 1, 128, f) != 128)
		{
			break;
		}
		if (vtx_count <= 0 || tri_count <= 0 || vtx_count > 32768 || tri_count > 32768)
			break;

		uncomp_sz = (uint32_t)vtx_count * 68u;
		idx_sz = ((uint32_t)tri_count + 2u) * 2u;
		uncomp_verts = (uint8_t *)malloc(uncomp_sz);
		idx_bytes = (uint8_t *)malloc(idx_sz);
		if (!uncomp_verts || !idx_bytes)
		{
			free(uncomp_verts);
			free(idx_bytes);
			break;
		}
		if (fread(uncomp_verts, 1, uncomp_sz, f) != uncomp_sz ||
			fread(idx_bytes, 1, idx_sz, f) != idx_sz)
		{
			free(uncomp_verts);
			free(idx_bytes);
			break;
		}

		build_hw_part_from_arrays(
			&out_parts[(*out_part_count)++],
			uncomp_verts,
			vtx_count,
			idx_bytes,
			tri_count,
			shader_hint,
			scale_factor,
			z_offset);

		free(uncomp_verts);
		free(idx_bytes);
	}

	fclose(f);
	return (*out_part_count > 0) ? 1 : 0;
}

static int load_hw_parts_from_cache_file(
	char const *map_path,
	uint32_t object_group_tag,
	char const *object_or_model_path,
	struct forge_hw_part *out_parts,
	int max_parts,
	int *out_part_count,
	float *out_radius,
	float scale_factor,
	float z_offset)
{
	FILE *f;
	uint8_t hdr[2048];
	uint32_t tag_off, tag_sz;
	uint8_t *tag_buf;
	uint32_t inst_addr, inst_off;
	uint32_t vertex_data_off, index_data_rel_off;
	int32_t tag_count, i;
	uint32_t mod_data_addr = 0;
	int is_gbxmodel = 0;
	char model_tag_name[256];

	model_tag_name[0] = '\0';
	*out_part_count = 0;
	if (out_radius)
		*out_radius = 1.5f;

	f = fopen(map_path, "rb");
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return 0;
	}
	tag_off = *(uint32_t const *)(hdr + 0x10);
	tag_sz = *(uint32_t const *)(hdr + 0x14);
	if (tag_off < 2048 || tag_sz < 64 || tag_sz > 64 * 1024 * 1024)
	{
		fclose(f);
		return 0;
	}
	tag_buf = (uint8_t *)malloc(tag_sz);
	if (!tag_buf)
	{
		fclose(f);
		return 0;
	}
	fseek(f, (long)tag_off, SEEK_SET);
	if (fread(tag_buf, 1, tag_sz, f) != tag_sz)
	{
		free(tag_buf);
		fclose(f);
		return 0;
	}

	inst_addr = *(uint32_t const *)(tag_buf + 0x00);
	tag_count = *(int32_t const *)(tag_buf + 0x0C);
	vertex_data_off = *(uint32_t const *)(tag_buf + 0x14);
	index_data_rel_off = *(uint32_t const *)(tag_buf + 0x1C);
	if (inst_addr < 0x40440000u || tag_count <= 0 || tag_count > 16384)
	{
		free(tag_buf);
		fclose(f);
		return 0;
	}
	inst_off = inst_addr - 0x40440000u;

	/* Step 1: If object_group_tag is an object tag ('scen','vehi','weap','bipd','mach','ctrl','lifi'),
	 * find the object tag first to get its bounding_radius (+0x04) and model tag reference (+0x28). */
	if (object_group_tag != 'mod2' && object_group_tag != 'mode')
	{
		for (i = 0; i < tag_count; i++)
		{
			uint32_t io = inst_off + (uint32_t)i * 32u;
			uint32_t grp, name_addr, data_addr;
			char const *tname;

			if (io + 32u > tag_sz)
				break;
			grp = *(uint32_t const *)(tag_buf + io + 0x00);
			name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
			data_addr = *(uint32_t const *)(tag_buf + io + 0x14);
			if (name_addr < 0x40440000u || (name_addr - 0x40440000u) >= tag_sz)
				continue;
			tname = (char const *)(tag_buf + (name_addr - 0x40440000u));
			if (grp == object_group_tag && _stricmp(tname, object_or_model_path) == 0)
			{
				if (data_addr >= 0x40440000u && (data_addr - 0x40440000u) + 0x38u <= tag_sz)
				{
					uint32_t obj_off = data_addr - 0x40440000u;
					float rad = *(float const *)(tag_buf + obj_off + 0x04);
					uint32_t mod_name_addr = *(uint32_t const *)(tag_buf + obj_off + 0x2C);
					int32_t mod_idx = *(int32_t const *)(tag_buf + obj_off + 0x34);
					if (out_radius && rad > 0.05f && rad < 100.0f)
						*out_radius = rad;
					if (mod_idx != -1 && (mod_idx & 0xFFFF) < tag_count)
					{
						uint32_t mio = inst_off + (uint32_t)(mod_idx & 0xFFFF) * 32u;
						if (mio + 32u <= tag_sz)
						{
							uint32_t mgrp = *(uint32_t const *)(tag_buf + mio + 0x00);
							mod_data_addr = *(uint32_t const *)(tag_buf + mio + 0x14);
							is_gbxmodel = (mgrp == 'mod2') ? 1 : 0;
						}
					}
					else if (mod_name_addr >= 0x40440000u && (mod_name_addr - 0x40440000u) < tag_sz)
					{
						snprintf(model_tag_name, sizeof(model_tag_name), "%s",
							(char const *)(tag_buf + (mod_name_addr - 0x40440000u)));
					}
				}
				break;
			}
		}
	}
	else
	{
		snprintf(model_tag_name, sizeof(model_tag_name), "%s", object_or_model_path);
	}

	/* Step 2: Locate the model ('mod2' or 'mode') if not already resolved by tag index */
	if (mod_data_addr == 0)
	{
		char const *search_name = model_tag_name[0] ? model_tag_name : object_or_model_path;
		for (i = 0; i < tag_count; i++)
		{
			uint32_t io = inst_off + (uint32_t)i * 32u;
			uint32_t grp, name_addr, data_addr;
			char const *tname;

			if (io + 32u > tag_sz)
				break;
			grp = *(uint32_t const *)(tag_buf + io + 0x00);
			name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
			data_addr = *(uint32_t const *)(tag_buf + io + 0x14);
			if (name_addr < 0x40440000u || (name_addr - 0x40440000u) >= tag_sz)
				continue;
			tname = (char const *)(tag_buf + (name_addr - 0x40440000u));
			if ((grp == 'mod2' || grp == 'mode') && _stricmp(tname, search_name) == 0)
			{
				mod_data_addr = data_addr;
				is_gbxmodel = (grp == 'mod2') ? 1 : 0;
				break;
			}
		}
	}

	if (mod_data_addr < 0x40440000u || (mod_data_addr - 0x40440000u) + 232u > tag_sz)
	{
		free(tag_buf);
		fclose(f);
		return 0;
	}

	{
		uint32_t mod_off = mod_data_addr - 0x40440000u;
		int32_t reg_count = *(int32_t const *)(tag_buf + mod_off + 196);
		uint32_t reg_addr = *(uint32_t const *)(tag_buf + mod_off + 200);
		int32_t geom_count = *(int32_t const *)(tag_buf + mod_off + 208);
		uint32_t geom_addr = *(uint32_t const *)(tag_buf + mod_off + 212);
		int32_t shdr_count = *(int32_t const *)(tag_buf + mod_off + 220);
		uint32_t shdr_addr = *(uint32_t const *)(tag_buf + mod_off + 224);
		int geom_list[32];
		int geom_list_count = 0;
		int g_idx;

		if (geom_count <= 0 || geom_addr < 0x40440000u || (geom_addr - 0x40440000u) + (uint32_t)geom_count * 48u > tag_sz)
		{
			free(tag_buf);
			fclose(f);
			return 0;
		}

		/* Collect permutation 0's highest-detail geometry index from every region of the model */
		if (reg_count > 0 && reg_count <= 32 && reg_addr >= 0x40440000u &&
			(reg_addr - 0x40440000u) + (uint32_t)reg_count * 76u <= tag_sz)
		{
			int r;
			for (r = 0; r < reg_count && geom_list_count < 32; r++)
			{
				uint32_t r_off = (reg_addr - 0x40440000u) + (uint32_t)r * 76u;
				int32_t perm_count = *(int32_t const *)(tag_buf + r_off + 64);
				uint32_t perm_addr = *(uint32_t const *)(tag_buf + r_off + 68);
				if (perm_count > 0 && perm_addr >= 0x40440000u && (perm_addr - 0x40440000u) + 88u <= tag_sz)
				{
					uint32_t perm0_off = perm_addr - 0x40440000u;
					int lod;
					int16_t chosen_geom = -1;
					/* Pick super_high (4) down to super_low (0) */
					for (lod = 4; lod >= 0; lod--)
					{
						int16_t gi = *(int16_t const *)(tag_buf + perm0_off + 64 + lod * 2);
						if (gi >= 0 && gi < geom_count)
						{
							chosen_geom = gi;
							break;
						}
					}
					if (chosen_geom >= 0)
						geom_list[geom_list_count++] = (int)chosen_geom;
				}
			}
		}
		if (geom_list_count == 0)
		{
			geom_list[0] = 0;
			geom_list_count = 1;
		}

		for (g_idx = 0; g_idx < geom_list_count && *out_part_count < max_parts; g_idx++)
		{
			uint32_t g_off = (geom_addr - 0x40440000u) + (uint32_t)geom_list[g_idx] * 48u;
			int32_t part_count = *(int32_t const *)(tag_buf + g_off + 36);
			uint32_t part_addr = *(uint32_t const *)(tag_buf + g_off + 40);
			uint32_t part_stride = is_gbxmodel ? 132u : 104u;
			int p;

			if (part_count <= 0 || part_addr < 0x40440000u ||
				(part_addr - 0x40440000u) + (uint32_t)part_count * part_stride > tag_sz)
			{
				continue;
			}

			for (p = 0; p < part_count && *out_part_count < max_parts; p++)
			{
				uint32_t p_off = (part_addr - 0x40440000u) + (uint32_t)p * part_stride;
				int16_t shdr_idx = *(int16_t const *)(tag_buf + p_off + 4);
				int32_t tri_count = *(int32_t const *)(tag_buf + p_off + 72);
				uint32_t tri_file_off = *(uint32_t const *)(tag_buf + p_off + 76);
				int32_t vtx_count = *(int32_t const *)(tag_buf + p_off + 88);
				uint32_t vtx_file_off = *(uint32_t const *)(tag_buf + p_off + 100);
				char shader_hint_buf[128];
				char const *shader_hint = "";
				shader_hint_buf[0] = '\0';

				if (tri_count <= 0 || vtx_count <= 0 || tri_count > 32768 || vtx_count > 32768)
					continue;

				if (shdr_idx >= 0 && shdr_idx < shdr_count && shdr_addr >= 0x40440000u &&
					(shdr_addr - 0x40440000u) + (uint32_t)(shdr_idx + 1) * 32u <= tag_sz)
				{
					uint32_t s_off = (shdr_addr - 0x40440000u) + (uint32_t)shdr_idx * 32u;
					uint32_t s_name_addr = *(uint32_t const *)(tag_buf + s_off + 4);
					int32_t s_tag_idx = *(int32_t const *)(tag_buf + s_off + 12);
					if (s_name_addr >= 0x40440000u && (s_name_addr - 0x40440000u) < tag_sz)
					{
						char const *shdr_name = (char const *)(tag_buf + (s_name_addr - 0x40440000u));
						char const *bitm_name = NULL;
						if (s_tag_idx != -1 && (s_tag_idx & 0xFFFF) < tag_count)
						{
							uint32_t sio = inst_off + (uint32_t)(s_tag_idx & 0xFFFF) * 32u;
							if (sio + 32u <= tag_sz)
							{
								uint32_t sgrp = *(uint32_t const *)(tag_buf + sio + 0x00);
								uint32_t sdata_addr = *(uint32_t const *)(tag_buf + sio + 0x14);
								if (sdata_addr >= 0x40440000u)
								{
									uint32_t sdoff = sdata_addr - 0x40440000u;
									uint32_t bname_addr = 0;
									if (sgrp == 'soso' && sdoff + 0xC0u <= tag_sz)
										bname_addr = *(uint32_t const *)(tag_buf + sdoff + 0xB4);
									else if (sgrp == 'senv' && sdoff + 0x98u <= tag_sz)
										bname_addr = *(uint32_t const *)(tag_buf + sdoff + 0x8C);
									else if (sgrp == 'sgla' && sdoff + 0xACu <= tag_sz)
										bname_addr = *(uint32_t const *)(tag_buf + sdoff + 0xA0);
									else if ((sgrp == 'schi' || sgrp == 'scex') && sdoff + 0x84u <= tag_sz)
									{
										int32_t mcnt = *(int32_t const *)(tag_buf + sdoff + 0x6C);
										uint32_t maddr = *(uint32_t const *)(tag_buf + sdoff + 0x70);
										if (sgrp == 'scex' && (mcnt <= 0 || maddr < 0x40440000u))
										{
											mcnt = *(int32_t const *)(tag_buf + sdoff + 0x78);
											maddr = *(uint32_t const *)(tag_buf + sdoff + 0x7C);
										}
										if (mcnt > 0 && maddr >= 0x40440000u && (maddr - 0x40440000u) + 0xECu <= tag_sz)
											bname_addr = *(uint32_t const *)(tag_buf + (maddr - 0x40440000u) + 0x70);
									}
									if (bname_addr >= 0x40440000u && (bname_addr - 0x40440000u) < tag_sz)
										bitm_name = (char const *)(tag_buf + (bname_addr - 0x40440000u));
								}
							}
						}
						if (bitm_name && bitm_name[0])
							snprintf(shader_hint_buf, sizeof(shader_hint_buf), "%s|%s", shdr_name, bitm_name);
						else
							snprintf(shader_hint_buf, sizeof(shader_hint_buf), "%s", shdr_name);
						shader_hint = shader_hint_buf;
					}
				}

				{
					uint32_t uncomp_sz = (uint32_t)vtx_count * 68u;
					uint32_t strip_idx_count = (uint32_t)tri_count + 2u;
					uint32_t idx_sz = strip_idx_count * 2u;
					uint8_t *uncomp_verts = (uint8_t *)malloc(uncomp_sz);
					uint8_t *idx_bytes = (uint8_t *)malloc(idx_sz);
					uint32_t k;
					int indices_ok = 1;

					if (!uncomp_verts || !idx_bytes)
					{
						free(uncomp_verts);
						free(idx_bytes);
						continue;
					}

					if (vtx_file_off >= 0x40440000u && (vtx_file_off - 0x40440000u) + uncomp_sz <= tag_sz)
					{
						memcpy(uncomp_verts, tag_buf + (vtx_file_off - 0x40440000u), uncomp_sz);
					}
					else
					{
						fseek(f, (long)(vertex_data_off + vtx_file_off), SEEK_SET);
						if (fread(uncomp_verts, 1, uncomp_sz, f) != uncomp_sz)
						{
							free(uncomp_verts);
							free(idx_bytes);
							continue;
						}
					}

					if (tri_file_off >= 0x40440000u && (tri_file_off - 0x40440000u) + idx_sz <= tag_sz)
					{
						memcpy(idx_bytes, tag_buf + (tri_file_off - 0x40440000u), idx_sz);
					}
					else
					{
						uint32_t idx_base = vertex_data_off + index_data_rel_off;
						fseek(f, (long)(idx_base + tri_file_off), SEEK_SET);
						if (fread(idx_bytes, 1, idx_sz, f) != idx_sz)
						{
							free(uncomp_verts);
							free(idx_bytes);
							continue;
						}
					}

					for (k = 0; k < strip_idx_count; k++)
					{
						uint16_t idx_val = ((uint16_t const *)idx_bytes)[k];
						if (idx_val >= (uint16_t)vtx_count && idx_val != 0xFFFFu)
						{
							indices_ok = 0;
							break;
						}
					}
					if (indices_ok)
					{
						build_hw_part_from_arrays(
							&out_parts[(*out_part_count)++],
							uncomp_verts,
							vtx_count,
							idx_bytes,
							tri_count,
							shader_hint,
							scale_factor,
							z_offset);
					}
					free(uncomp_verts);
					free(idx_bytes);
				}
			}
		}
	}

	free(tag_buf);
	fclose(f);
	return (*out_part_count > 0) ? 1 : 0;
}

static void forge_extract_cache_to_cached_tags_folder(
	char const *cache_bin_path,
	uint8_t source_cache_id,
	char const *manifest_path)
{
	FILE *f, *mf;
	uint8_t hdr[2048];
	uint32_t tag_off, tag_sz;
	uint8_t *tag_buf;
	uint32_t inst_addr, inst_off;
	uint32_t vertex_data_off, index_data_rel_off;
	int32_t tag_count, i;

	f = fopen(cache_bin_path, "rb");
	if (!f)
		return;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return;
	}
	tag_off = *(uint32_t const *)(hdr + 0x10);
	tag_sz = *(uint32_t const *)(hdr + 0x14);
	if (tag_off < 2048 || tag_sz < 64 || tag_sz > 64 * 1024 * 1024)
	{
		fclose(f);
		return;
	}
	tag_buf = (uint8_t *)malloc(tag_sz);
	if (!tag_buf)
	{
		fclose(f);
		return;
	}
	fseek(f, (long)tag_off, SEEK_SET);
	if (fread(tag_buf, 1, tag_sz, f) != tag_sz)
	{
		free(tag_buf);
		fclose(f);
		return;
	}

	inst_addr = *(uint32_t const *)(tag_buf + 0x00);
	tag_count = *(int32_t const *)(tag_buf + 0x0C);
	vertex_data_off = *(uint32_t const *)(tag_buf + 0x14);
	index_data_rel_off = *(uint32_t const *)(tag_buf + 0x1C);
	if (inst_addr < 0x40440000u || tag_count <= 0 || tag_count > 16384)
	{
		free(tag_buf);
		fclose(f);
		return;
	}
	inst_off = inst_addr - 0x40440000u;

	mf = manifest_path ? fopen(manifest_path, "wb") : NULL;

	for (i = 0; i < tag_count; i++)
	{
		uint32_t io = inst_off + (uint32_t)i * 32u;
		uint32_t grp, name_addr, data_addr;
		char const *tname;
		int cat_id = -1;
		char grp_str[5];
		uint32_t mod_data_addr = 0;
		int is_gbxmodel = 0;
		float rad = 1.5f;
		char ftag_path[512];
		FILE *ff;

		if (io + 32u > tag_sz)
			break;
		grp = *(uint32_t const *)(tag_buf + io + 0x00);
		name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
		data_addr = *(uint32_t const *)(tag_buf + io + 0x14);
		if (name_addr < 0x40440000u || (name_addr - 0x40440000u) >= tag_sz)
			continue;
		tname = (char const *)(tag_buf + (name_addr - 0x40440000u));

		if (grp == 'weap')
		{
			if (strstr(tname, "flag") == NULL && strstr(tname, "ball") == NULL)
				cat_id = _forge_cat_weapons;
		}
		else if (grp == 'vehi') cat_id = _forge_cat_vehicles;
		else if (grp == 'scen') cat_id = _forge_cat_scenery;
		else if (grp == 'mach' || grp == 'ctrl' || grp == 'lifi') cat_id = _forge_cat_barriers_devices;
		else if (grp == 'bipd') cat_id = _forge_cat_bipeds;

		if (cat_id < 0 || data_addr < 0x40440000u || (data_addr - 0x40440000u) + 0x38u > tag_sz)
			continue;

		grp_str[0] = (char)((grp >> 24) & 0xFF);
		grp_str[1] = (char)((grp >> 16) & 0xFF);
		grp_str[2] = (char)((grp >> 8) & 0xFF);
		grp_str[3] = (char)(grp & 0xFF);
		grp_str[4] = '\0';

		if (mf && cat_id != _forge_cat_bipeds)
			fprintf(mf, "%s|%d|%d|%s\n", grp_str, cat_id, (int)source_cache_id, tname);

		{
			uint32_t obj_off = data_addr - 0x40440000u;
			float r = *(float const *)(tag_buf + obj_off + 0x04);
			int32_t mod_idx = *(int32_t const *)(tag_buf + obj_off + 0x34);
			if (r > 0.05f && r < 100.0f)
				rad = r;
			if (mod_idx != -1 && (mod_idx & 0xFFFF) < tag_count)
			{
				uint32_t mio = inst_off + (uint32_t)(mod_idx & 0xFFFF) * 32u;
				if (mio + 32u <= tag_sz)
				{
					uint32_t mgrp = *(uint32_t const *)(tag_buf + mio + 0x00);
					mod_data_addr = *(uint32_t const *)(tag_buf + mio + 0x14);
					is_gbxmodel = (mgrp == 'mod2') ? 1 : 0;
				}
			}
		}

		if (mod_data_addr < 0x40440000u || (mod_data_addr - 0x40440000u) + 232u > tag_sz)
			continue;

		make_cached_ftag_path(grp, tname, ftag_path, sizeof(ftag_path));
		ff = fopen(ftag_path, "wb");
		if (ff)
		{
			uint32_t mod_off = mod_data_addr - 0x40440000u;
			int32_t reg_count = *(int32_t const *)(tag_buf + mod_off + 196);
			uint32_t reg_addr = *(uint32_t const *)(tag_buf + mod_off + 200);
			int32_t geom_count = *(int32_t const *)(tag_buf + mod_off + 208);
			uint32_t geom_addr = *(uint32_t const *)(tag_buf + mod_off + 212);
			int32_t shdr_count = *(int32_t const *)(tag_buf + mod_off + 220);
			uint32_t shdr_addr = *(uint32_t const *)(tag_buf + mod_off + 224);
			int geom_list[32];
			int geom_list_count = 0;
			int g_idx;
			uint32_t ver = 1, written_parts = 0;
			long part_count_pos;
			char name_fixed[256];

			memset(name_fixed, 0, sizeof(name_fixed));
			snprintf(name_fixed, sizeof(name_fixed), "%s", tname);

			fwrite("FTAG", 1, 4, ff);
			fwrite(&ver, 4, 1, ff);
			fwrite(&grp, 4, 1, ff);
			fwrite(&rad, 4, 1, ff);
			fwrite(name_fixed, 1, 256, ff);
			part_count_pos = ftell(ff);
			fwrite(&written_parts, 4, 1, ff);

			if (reg_count > 0 && reg_count <= 32 && reg_addr >= 0x40440000u &&
				(reg_addr - 0x40440000u) + (uint32_t)reg_count * 76u <= tag_sz)
			{
				int r;
				for (r = 0; r < reg_count && geom_list_count < 32; r++)
				{
					uint32_t r_off = (reg_addr - 0x40440000u) + (uint32_t)r * 76u;
					int32_t perm_count = *(int32_t const *)(tag_buf + r_off + 64);
					uint32_t perm_addr = *(uint32_t const *)(tag_buf + r_off + 68);
					if (perm_count > 0 && perm_addr >= 0x40440000u && (perm_addr - 0x40440000u) + 88u <= tag_sz)
					{
						uint32_t perm0_off = perm_addr - 0x40440000u;
						int lod;
						for (lod = 4; lod >= 0; lod--)
						{
							int16_t gi = *(int16_t const *)(tag_buf + perm0_off + 64 + lod * 2);
							if (gi >= 0 && gi < geom_count)
							{
								geom_list[geom_list_count++] = (int)gi;
								break;
							}
						}
					}
				}
			}
			if (geom_list_count == 0 && geom_count > 0)
			{
				geom_list[0] = 0;
				geom_list_count = 1;
			}

			for (g_idx = 0; g_idx < geom_list_count && written_parts < FORGE_MAX_HW_PARTS; g_idx++)
			{
				uint32_t g_off = (geom_addr - 0x40440000u) + (uint32_t)geom_list[g_idx] * 48u;
				int32_t part_count = *(int32_t const *)(tag_buf + g_off + 36);
				uint32_t part_addr = *(uint32_t const *)(tag_buf + g_off + 40);
				uint32_t part_stride = is_gbxmodel ? 132u : 104u;
				int p;
				if (part_count <= 0 || part_addr < 0x40440000u ||
					(part_addr - 0x40440000u) + (uint32_t)part_count * part_stride > tag_sz)
					continue;
				for (p = 0; p < part_count && written_parts < FORGE_MAX_HW_PARTS; p++)
				{
					uint32_t p_off = (part_addr - 0x40440000u) + (uint32_t)p * part_stride;
					int16_t shdr_idx = *(int16_t const *)(tag_buf + p_off + 4);
					int32_t tri_count = *(int32_t const *)(tag_buf + p_off + 72);
					uint32_t tri_file_off = *(uint32_t const *)(tag_buf + p_off + 76);
					int32_t vtx_count = *(int32_t const *)(tag_buf + p_off + 88);
					uint32_t vtx_file_off = *(uint32_t const *)(tag_buf + p_off + 100);
					char shader_hint_buf[128];
					uint32_t uncomp_sz, idx_sz;
					uint8_t *uncomp_verts, *idx_bytes;

					if (tri_count <= 0 || vtx_count <= 0 || tri_count > 32768 || vtx_count > 32768)
						continue;
					memset(shader_hint_buf, 0, sizeof(shader_hint_buf));
					if (shdr_idx >= 0 && shdr_idx < shdr_count && shdr_addr >= 0x40440000u &&
						(shdr_addr - 0x40440000u) + (uint32_t)(shdr_idx + 1) * 32u <= tag_sz)
					{
						uint32_t s_off = (shdr_addr - 0x40440000u) + (uint32_t)shdr_idx * 32u;
						uint32_t s_name_addr = *(uint32_t const *)(tag_buf + s_off + 4);
						int32_t s_tag_idx = *(int32_t const *)(tag_buf + s_off + 12);
						if (s_name_addr >= 0x40440000u && (s_name_addr - 0x40440000u) < tag_sz)
						{
							char const *shdr_name = (char const *)(tag_buf + (s_name_addr - 0x40440000u));
							char const *bitm_name = NULL;
							if (s_tag_idx != -1 && (s_tag_idx & 0xFFFF) < tag_count)
							{
								uint32_t sio = inst_off + (uint32_t)(s_tag_idx & 0xFFFF) * 32u;
								if (sio + 32u <= tag_sz)
								{
									uint32_t sgrp = *(uint32_t const *)(tag_buf + sio + 0x00);
									uint32_t sdata_addr = *(uint32_t const *)(tag_buf + sio + 0x14);
									if (sdata_addr >= 0x40440000u)
									{
										uint32_t sdoff = sdata_addr - 0x40440000u;
										uint32_t bname_addr = 0;
										if (sgrp == 'soso' && sdoff + 0xC0u <= tag_sz)
											bname_addr = *(uint32_t const *)(tag_buf + sdoff + 0xB4);
										else if (sgrp == 'senv' && sdoff + 0x98u <= tag_sz)
											bname_addr = *(uint32_t const *)(tag_buf + sdoff + 0x8C);
										else if (sgrp == 'sgla' && sdoff + 0xACu <= tag_sz)
											bname_addr = *(uint32_t const *)(tag_buf + sdoff + 0xA0);
										if (bname_addr >= 0x40440000u && (bname_addr - 0x40440000u) < tag_sz)
											bitm_name = (char const *)(tag_buf + (bname_addr - 0x40440000u));
									}
								}
							}
							if (bitm_name && bitm_name[0])
								snprintf(shader_hint_buf, sizeof(shader_hint_buf), "%s|%s", shdr_name, bitm_name);
							else
								snprintf(shader_hint_buf, sizeof(shader_hint_buf), "%s", shdr_name);
						}
					}

					uncomp_sz = (uint32_t)vtx_count * 68u;
					idx_sz = ((uint32_t)tri_count + 2u) * 2u;
					uncomp_verts = (uint8_t *)malloc(uncomp_sz);
					idx_bytes = (uint8_t *)malloc(idx_sz);
					if (uncomp_verts && idx_bytes)
					{
						if (fseek(f, (long)(vertex_data_off + vtx_file_off), SEEK_SET) == 0 &&
							fread(uncomp_verts, 1, uncomp_sz, f) == uncomp_sz &&
							fseek(f, (long)(vertex_data_off + index_data_rel_off + tri_file_off), SEEK_SET) == 0 &&
							fread(idx_bytes, 1, idx_sz, f) == idx_sz)
						{
							fwrite(&vtx_count, 4, 1, ff);
							fwrite(&tri_count, 4, 1, ff);
							fwrite(shader_hint_buf, 1, 128, ff);
							fwrite(uncomp_verts, 1, uncomp_sz, ff);
							fwrite(idx_bytes, 1, idx_sz, ff);
							written_parts++;
						}
					}
					free(uncomp_verts);
					free(idx_bytes);
				}
			}
			fseek(ff, part_count_pos, SEEK_SET);
			fwrite(&written_parts, 4, 1, ff);
			fclose(ff);
		}
	}

	if (mf)
		fclose(mf);
	free(tag_buf);
	fclose(f);
}

static long append_runtime_tag_instance(
	long group_tag,
	long parent0,
	long parent1,
	char const *tag_path,
	void *base_address)
{
	long existing_count = 0;
	struct forge_cache_file_tag_instance *instances;
	struct forge_cache_file_tag_instance *grown;
	long max_salt = 0xE174;
	long i, new_abs_idx, new_tag_index;
	char *name_copy;

	instances = (struct forge_cache_file_tag_instance *)cache_files_tag_instances(&existing_count);
	if (!instances || existing_count <= 0 || existing_count >= 65000)
		return NONE;

	if (!s_dyn_tags.saved_base_instances)
	{
		s_dyn_tags.saved_base_instances = instances;
		s_dyn_tags.saved_base_count = existing_count;
	}

	grown = (struct forge_cache_file_tag_instance *)malloc((size_t)(existing_count + 1) * sizeof(*grown));
	name_copy = (char *)malloc(strlen(tag_path) + 1);
	if (!grown || !name_copy)
	{
		free(grown);
		free(name_copy);
		return NONE;
	}
	memcpy(grown, instances, (size_t)existing_count * sizeof(*grown));
	strcpy(name_copy, tag_path);

	for (i = 0; i < existing_count; i++)
	{
		long s = (long)((unsigned long)instances[i].tag_index >> 16);
		if (s > max_salt)
			max_salt = s;
	}

	new_abs_idx = existing_count;
	new_tag_index = ((max_salt + 1) << 16) | (new_abs_idx & 0xFFFF);

	memset(&grown[new_abs_idx], 0, sizeof(grown[new_abs_idx]));
	grown[new_abs_idx].group_tag = group_tag;
	grown[new_abs_idx].parent_group_tags[0] = parent0;
	grown[new_abs_idx].parent_group_tags[1] = parent1;
	grown[new_abs_idx].tag_index = new_tag_index;
	grown[new_abs_idx].name = name_copy;
	grown[new_abs_idx].base_address = base_address;

	cache_files_set_tag_instances(grown, existing_count + 1);
	if (s_dyn_tags.allocated_instances)
		free(s_dyn_tags.allocated_instances);
	s_dyn_tags.allocated_instances = grown;

	return new_tag_index;
}

static long find_native_or_mp_variant_tag(uint32_t group_tag, char const *tag_path)
{
	long idx;
	long i;

	idx = tag_loaded((long)group_tag, tag_path);
	if (idx != NONE)
		return idx;

	/* Map between SP and MP vehicle/weapon names natively present in MP maps */
	if (group_tag == 'vehi')
	{
		static struct { char const *a; char const *b; } const vehi_pairs[] = {
			{ "vehicles\\warthog\\warthog", "vehicles\\warthog\\mp_warthog" },
			{ "vehicles\\ghost\\ghost", "vehicles\\ghost\\ghost_mp" },
			{ "vehicles\\scorpion\\scorpion", "vehicles\\scorpion\\scorpion_mp" },
			{ "vehicles\\banshee\\banshee", "vehicles\\banshee\\banshee_mp" },
			{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c_gun_turret\\c_gun_turret_mp" },
			{ "vehicles\\c gun turret\\c gun turret", "vehicles\\c gun turret\\c gun turret_mp" },
			{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c gun turret\\c gun turret_mp" },
			{ "vehicles\\c gun turret\\c gun turret", "vehicles\\c_gun_turret\\c_gun_turret_mp" }
		};
		for (i = 0; i < (long)(sizeof(vehi_pairs) / sizeof(vehi_pairs[0])); i++)
		{
			if (_stricmp(tag_path, vehi_pairs[i].a) == 0)
			{
				idx = tag_loaded('vehi', vehi_pairs[i].b);
				if (idx != NONE) return idx;
			}
			else if (_stricmp(tag_path, vehi_pairs[i].b) == 0)
			{
				idx = tag_loaded('vehi', vehi_pairs[i].a);
				if (idx != NONE) return idx;
			}
		}
	}
	return NONE;
}

static long register_dynamic_cached_tag(uint32_t group_tag, char const *tag_path, uint32_t *out_actual_group)
{
	int i;
	long existing_count = 0;
	struct forge_cache_file_tag_instance *instances;
	long template_idx = NONE;
	uint32_t template_grp = group_tag;
	struct tag_iterator iter;
	struct forge_dynamic_tag_entry *entry;
	char ftag_path[512];
	long new_tag_index;
	void *cloned_def;
	struct forge_cache_file_tag_instance tmpl_inst;

	/* Check if already registered for the current map */
	for (i = 0; i < s_dyn_tags.count; i++)
	{
		if (s_dyn_tags.entries[i].group_tag == group_tag &&
			_stricmp(s_dyn_tags.entries[i].tag_path, tag_path) == 0)
		{
			if (out_actual_group)
				*out_actual_group = (uint32_t)tag_get_group_tag(s_dyn_tags.entries[i].definition_index);
			return s_dyn_tags.entries[i].definition_index;
		}
	}

	if (s_dyn_tags.count >= FORGE_MAX_DYNAMIC_TAGS)
		return NONE;

	instances = (struct forge_cache_file_tag_instance *)cache_files_tag_instances(&existing_count);
	if (!instances || existing_count <= 0 || existing_count >= 65000)
		return NONE;

	/* For flying/hovering/turret vehicles, prefer Banshee or Ghost (which has 2 powered antigrav mass points and no wheels) */
	if (group_tag == 'vehi')
	{
		if (strstr(tag_path, "banshee") || strstr(tag_path, "pelican") ||
			strstr(tag_path, "dropship") || strstr(tag_path, "lifepod") ||
			strstr(tag_path, "fighter") || strstr(tag_path, "turret") ||
			strstr(tag_path, "wraith") || strstr(tag_path, "ghost"))
		{
			template_idx = tag_loaded('vehi', "vehicles\\banshee\\banshee_mp");
			if (template_idx == NONE)
				template_idx = tag_loaded('vehi', "vehicles\\banshee\\banshee");
			if (template_idx == NONE)
				template_idx = tag_loaded('vehi', "vehicles\\ghost\\ghost_mp");
			if (template_idx == NONE)
				template_idx = tag_loaded('vehi', "vehicles\\ghost\\ghost");
		}
		else if (strstr(tag_path, "scorpion") || strstr(tag_path, "tank"))
		{
			template_idx = tag_loaded('vehi', "vehicles\\scorpion\\scorpion_mp");
			if (template_idx == NONE)
				template_idx = tag_loaded('vehi', "vehicles\\scorpion\\scorpion");
		}
	}

	/* Find a valid postprocessed template tag in the current map for physics/collision safety */
	if (template_idx == NONE)
	{
		tag_iterator_new(&iter, (long)group_tag);
		template_idx = tag_iterator_next(&iter);
	}
	if (template_idx == NONE)
	{
		uint32_t fallbacks[4] = { 'scen', 'vehi', 'weap', 'bipd' };
		int k;
		for (k = 0; k < 4; k++)
		{
			tag_iterator_new(&iter, (long)fallbacks[k]);
			template_idx = tag_iterator_next(&iter);
			if (template_idx != NONE)
			{
				template_grp = fallbacks[k];
				break;
			}
		}
	}
	if (template_idx == NONE)
		return NONE;

	tmpl_inst = instances[template_idx & 0xFFFF];

	entry = &s_dyn_tags.entries[s_dyn_tags.count];
	memset(entry, 0, sizeof(*entry));
	entry->group_tag = group_tag;
	snprintf(entry->tag_path, sizeof(entry->tag_path), "%s", tag_path);
	entry->bounding_radius = 1.5f;

	/* 1. Load 3D hardware mesh from Cached Tag Folder System (OpenCE Forge/cached_tags/) */
	make_cached_ftag_path(group_tag, tag_path, ftag_path, sizeof(ftag_path));
	if (!load_hw_parts_from_ftag_file(
			ftag_path,
			entry->parts,
			FORGE_MAX_HW_PARTS,
			&entry->part_count,
			&entry->bounding_radius,
			1.0f,
			0.0f))
	{
		/* 2. Fallback to cached tag binary bundles */
		if (!load_hw_parts_from_cache_file(
				"OpenCE Forge/cached_tags/bitmaps_cache.bin",
				group_tag,
				tag_path,
				entry->parts,
				FORGE_MAX_HW_PARTS,
				&entry->part_count,
				&entry->bounding_radius,
				1.0f,
				0.0f))
		{
			if (!load_hw_parts_from_cache_file(
					"OpenCE Forge/cached_tags/custom_bitmaps_cache.bin",
					group_tag,
					tag_path,
					entry->parts,
					FORGE_MAX_HW_PARTS,
					&entry->part_count,
					&entry->bounding_radius,
					1.0f,
					0.0f))
			{
				if (!load_hw_parts_from_cache_file(
						"OpenCE Forge/forge_maps/forgeasset.map",
						group_tag,
						tag_path,
						entry->parts,
						FORGE_MAX_HW_PARTS,
						&entry->part_count,
						&entry->bounding_radius,
						1.0f,
						0.0f))
				{
					load_hw_parts_from_cache_file(
						"OpenCE Forge/forge_maps/customforgeasset.map",
						group_tag,
						tag_path,
						entry->parts,
						FORGE_MAX_HW_PARTS,
						&entry->part_count,
						&entry->bounding_radius,
						1.0f,
						0.0f);
				}
			}
		}
	}

	cloned_def = malloc(4096);
	if (!cloned_def)
		return NONE;
	memset(cloned_def, 0, 4096);
	memcpy(cloned_def, tmpl_inst.base_address, 1280);

	/* Update bounding radius on the cloned object definition (+0x04) so culling & grabbing match */
	if (entry->bounding_radius > 0.1f)
		*(float *)((uint8_t *)cloned_def + 0x04) = entry->bounding_radius;

	/* Configure authentic vehicle physics, seats, and weapons on cloned vehicle definitions */
	if (template_grp == 'vehi')
	{
		uint8_t *vd = (uint8_t *)cloned_def;
		int32_t seat_count = *(int32_t const *)(vd + 0x2D0);
		uint8_t const *seat_src = *(uint8_t const *const *)(vd + 0x2D4);

		/* Remove template vehicle's attached child weapons (e.g. Warthog chaingun) unless this is a Warthog */
		if (!strstr(tag_path, "warthog"))
		{
			*(int32_t *)(vd + 0x2B8) = 0;
			*(void **)(vd + 0x2BC) = NULL;
		}

		/* Ensure powered_seats (+0x2C4) has a driver powered seat so Alien Fighter/Scout/Plane engines have power */
		if (*(int32_t const *)(vd + 0x2C4) <= 0)
		{
			float *pseat = (float *)calloc(3, sizeof(float));
			if (pseat)
			{
				pseat[0] = 0.0f;
				pseat[1] = 0.5f; /* driver_powerup_time */
				pseat[2] = 0.5f; /* driver_powerdown_time */
				*(int32_t *)(vd + 0x2C4) = 1;
				*(void **)(vd + 0x2C8) = pseat;
			}
		}

		/* Clone seats (+0x2D0, 284 bytes each) so seat 0 is a powered driver seat without Warthog horn */
		if (seat_count > 0 && seat_count <= 16 && seat_src)
		{
			int32_t new_seat_count = (!strstr(tag_path, "warthog") && !strstr(tag_path, "scorpion")) ? 1 : seat_count;
			uint8_t *seat_dst = (uint8_t *)malloc((size_t)new_seat_count * 284u);
			if (seat_dst)
			{
				memcpy(seat_dst, seat_src, (size_t)new_seat_count * 284u);
				/* Set _unit_seat_driver_bit (bit 0) and _unit_seat_valid_for_player_bit (bit 5) */
				*(uint32_t *)(seat_dst + 0x00) |= (1u << 0) | (1u << 5);
				if (!strstr(tag_path, "warthog"))
				{
					/* Clear seat weapons (e.g. Warthog horn) at +0x6C */
					*(int32_t *)(seat_dst + 0x6C) = 0;
					*(void **)(seat_dst + 0x70) = NULL;
				}
				*(int32_t *)(vd + 0x2D0) = new_seat_count;
				*(void **)(vd + 0x2D4) = seat_dst;
			}
		}

		if (strstr(tag_path, "banshee") || strstr(tag_path, "pelican") ||
			strstr(tag_path, "dropship") || strstr(tag_path, "lifepod") ||
			strstr(tag_path, "fighter"))
		{
			int is_heavy_flyer = (strstr(tag_path, "pelican") || strstr(tag_path, "dropship") || strstr(tag_path, "lifepod")) ? 1 : 0;
			int32_t phys_idx = *(int32_t const *)(vd + 0x6C);

			/* _vehicle_type_alien_fighter (5) - Full 3D flight physics */
			*(int16_t *)(vd + 0x2F4) = 5;
			*(float *)(vd + 0x2F8) = is_heavy_flyer ? 0.28f : 0.25f;  /* max forward speed */
			*(float *)(vd + 0x2FC) = is_heavy_flyer ? -0.10f : -0.05f; /* max reverse speed */
			*(float *)(vd + 0x300) = 0.009f; /* acceleration */
			*(float *)(vd + 0x304) = 0.009f; /* deceleration */
			*(float *)(vd + 0x308) = 45.0f;  /* max left turn */
			*(float *)(vd + 0x30C) = -45.0f; /* max right turn */
			*(float *)(vd + 0x314) = is_heavy_flyer ? 65.0f : 90.0f;  /* turn rate */
			*(float *)(vd + 0x330) = 0.14f;  /* slide left */
			*(float *)(vd + 0x334) = 0.14f;  /* slide right */
			*(float *)(vd + 0x364) = -0.1745f;
			*(int32_t *)(vd + 0x3BC) = NONE; /* clear Warthog engine sound */
			*(int32_t *)(vd + 0x3DC) = NONE; /* clear Warthog suspension sound */
			*(int32_t *)(vd + 0x3EC) = NONE; /* clear Warthog tire effect */

			/* Clone 'phys' tag (+0x60..+0x6C) with low gravity scale and antigrav mass points for smooth flight */
			if (phys_idx != NONE)
			{
				uint8_t const *phys_src = (uint8_t const *)tag_get('phys', phys_idx);
				if (phys_src)
				{
					uint8_t *phys_dst = (uint8_t *)malloc(128);
					if (phys_dst)
					{
						long new_phys_idx;
						memcpy(phys_dst, phys_src, 128);
						*(float *)(phys_dst + 0x1C) = 0.12f; /* gravity_scale: float in air instead of dropping like a car */
						*(float *)(phys_dst + 0x20) = 0.002f; /* ground_friction */
						new_phys_idx = append_runtime_tag_instance('phys', NONE, NONE, tag_path, phys_dst);
						if (new_phys_idx != NONE)
							*(int32_t *)(vd + 0x6C) = new_phys_idx;
					}
				}
			}
		}
		else if (strstr(tag_path, "wraith") || strstr(tag_path, "ghost"))
		{
			/* _vehicle_type_alien_scout (4) - Hovercraft physics */
			*(int16_t *)(vd + 0x2F4) = 4;
			*(float *)(vd + 0x2F8) = strstr(tag_path, "wraith") ? 0.16f : 0.24f;
			*(float *)(vd + 0x2FC) = -0.12f;
			*(float *)(vd + 0x300) = 0.008f;
			*(float *)(vd + 0x304) = 0.008f;
			*(float *)(vd + 0x330) = 0.14f;
			*(float *)(vd + 0x334) = 0.14f;
			*(int32_t *)(vd + 0x3DC) = NONE;
			*(int32_t *)(vd + 0x3EC) = NONE;
		}
		else if (strstr(tag_path, "turret"))
		{
			/* _vehicle_type_turret (6) - Stationary turret physics */
			*(int16_t *)(vd + 0x2F4) = 6;
			*(float *)(vd + 0x2F8) = 0.0f;
			*(float *)(vd + 0x2FC) = 0.0f;
			*(int32_t *)(vd + 0x3DC) = NONE;
			*(int32_t *)(vd + 0x3EC) = NONE;
		}
	}

	new_tag_index = append_runtime_tag_instance(
		tmpl_inst.group_tag,
		tmpl_inst.parent_group_tags[0],
		tmpl_inst.parent_group_tags[1],
		tag_path,
		cloned_def);
	if (new_tag_index == NONE)
	{
		free(cloned_def);
		return NONE;
	}

	entry->definition_index = new_tag_index;
	s_dyn_tags.count++;

	if (out_actual_group)
		*out_actual_group = template_grp;
	return new_tag_index;
}

long forge_resolve_engine_tag(uint32_t group_tag, char const *tag_path, uint32_t *out_actual_group)
{
	long idx;

	if (out_actual_group)
		*out_actual_group = group_tag;
	if (!tag_path || !tag_path[0])
		return NONE;

	/* 1. Check if the exact tag (or its native _mp / SP counterpart) is already in the map */
	idx = find_native_or_mp_variant_tag(group_tag, tag_path);
	if (idx != NONE)
		return idx;

	/* 2. Dynamically load from Cached Tag Folder System (OpenCE Forge/cached_tags/) into global_tag_instances! */
	idx = register_dynamic_cached_tag(group_tag, tag_path, out_actual_group);
	if (idx != NONE)
		return idx;

	return NONE;
}

int forge_ui_inject_custom_map(char const *donor_map_path)
{
	int injected = 0;
	int ok;
	char const *base_cache = "OpenCE Forge/cached_tags/bitmaps_cache.bin";
	char const *custom_cache = "OpenCE Forge/cached_tags/custom_bitmaps_cache.bin";
	FILE *test_f;

	if (!donor_map_path || !donor_map_path[0])
		return 0;
	if (forge_is_multiplayer_session())
	{
		forge_set_status("Multiplayer Asset Guard: Custom tag extraction is Single-Player only!");
		return 0;
	}

	test_f = fopen(base_cache, "rb");
	if (test_f)
		fclose(test_f);
	else
		base_cache = "OpenCE Forge/forge_maps/forgeasset.map";

	ok = forge_inject_custom_map_into_cache(
		donor_map_path,
		"OpenCE Forge/forge_maps",
		base_cache,
		custom_cache,
		"OpenCE Forge/credits.md",
		&injected);

	if (ok)
	{
		forge_extract_cache_to_cached_tags_folder(
			custom_cache,
			1,
			"OpenCE Forge/cached_tags/custom_catalog.txt");
		forge_runtime_reload_catalogs();
		forge_set_status("Added %d unique tags from '%s' -> cached_tags & credits.md!", injected, donor_map_path);
		return 1;
	}
	forge_set_status("Failed to extract tags from '%s'", donor_map_path);
	return 0;
}

static int is_ce_v609_map_file(char const *path)
{
	FILE *f = fopen(path, "rb");
	uint32_t hdr[2];
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return 0;
	}
	fclose(f);
	return (hdr[0] == 0x68656164u && hdr[1] == 609u) ? 1 : 0;
}

static void resolve_current_base_map_path(char *out_path, size_t out_sz)
{
	char const *ce_loaded = custom_edition_cache_loaded_path();
	char const *map_leaf;
	FILE *f;

	if (ce_loaded && ce_loaded[0] && is_ce_v609_map_file(ce_loaded))
	{
		snprintf(out_path, out_sz, "%s", ce_loaded);
		return;
	}

	map_leaf = main_get_map_name();
	if (map_leaf && map_leaf[0])
	{
		char const *slash = strrchr(map_leaf, '\\');
		char candidate[512];
		if (!slash)
			slash = strrchr(map_leaf, '/');
		if (slash)
			map_leaf = slash + 1;

		snprintf(candidate, sizeof(candidate), "forge_maps/%s/%s.map", map_leaf, map_leaf);
		if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "custom_maps/%s.map", map_leaf);
		if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "OpenCE Forge/forge_maps/%s.map", map_leaf);
		if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "../FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/%s.map", map_leaf);
		if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/%s.map", map_leaf);
		if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "../FORGE CODE/GAME MAPS/PC MAPS/%s.map", map_leaf);
		if ((f = fopen(candidate, "rb")) != NULL) { fclose(f); snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "FORGE CODE/GAME MAPS/PC MAPS/%s.map", map_leaf);
		if ((f = fopen(candidate, "rb")) != NULL) { fclose(f); snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "maps/%s.map", map_leaf);
		if ((f = fopen(candidate, "rb")) != NULL) { fclose(f); snprintf(out_path, out_sz, "%s", candidate); return; }
	}

	if ((f = fopen("OpenCE Forge/forge_maps/bloodgulch.map", "rb")) != NULL)
	{
		fclose(f);
		snprintf(out_path, out_sz, "OpenCE Forge/forge_maps/bloodgulch.map");
		return;
	}
	if ((f = fopen("../FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bloodgulch.map", "rb")) != NULL)
	{
		fclose(f);
		snprintf(out_path, out_sz, "../FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bloodgulch.map");
		return;
	}
	snprintf(out_path, out_sz, "FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bloodgulch.map");
}

int forge_ui_save_copy_as(char const *new_map_name)
{
	char base_map_path[512];
	char clean_name[64];
	char out_dir[256];
	char const *donors[2];
	int was_already_saved = g_forge.has_saved_map_once;
	int ok;

	if (!new_map_name || !new_map_name[0])
	{
		char const *cur = main_get_map_name();
		char const *slash = cur ? strrchr(cur, '\\') : NULL;
		snprintf(clean_name, sizeof(clean_name), "%s_forge", slash ? (slash + 1) : "bloodgulch");
	}
	else
	{
		snprintf(clean_name, sizeof(clean_name), "%s", new_map_name);
	}

	resolve_current_base_map_path(base_map_path, sizeof(base_map_path));
	donors[0] = "OpenCE Forge/forge_maps/forgeasset.map";
	donors[1] = "OpenCE Forge/forge_maps/customforgeasset.map";

	/* Point 11: Package each saved Forge .map / .bmp / .txt file into its own folder
	 * inside forge_maps/<clean_name>/ alongside maps/ and custom_maps/ to keep custom_maps/ clean! */
	snprintf(out_dir, sizeof(out_dir), "forge_maps/%s", clean_name);
#ifdef _WIN32
	CreateDirectoryA("forge_maps", NULL);
	CreateDirectoryA(out_dir, NULL);
#endif

	ok = forge_export_scenario_map(
		base_map_path,
		"OpenCE Forge/forge_maps",
		"OpenCE Forge/forge_maps",
		donors,
		2,
		&g_forge.edit_state,
		clean_name,
		out_dir);

	if (ok)
	{
		snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s", clean_name);
		g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
		g_forge.has_saved_map_once = 1;

		if (was_already_saved)
		{
			forge_set_status("Updated saved map 'forge_maps/%s/%s.map' (%d placed, %d default edits)!",
				clean_name,
				clean_name,
				(int)g_forge.edit_state.placed_object_count,
				(int)g_forge.edit_state.default_edit_count);
		}
		else
		{
			forge_set_status("Initial save complete: 'forge_maps/%s/%s.map'! Press F5 anytime to quick-update.",
				clean_name,
				clean_name);
		}
		return 1;
	}
	forge_set_status("ERROR: Failed to export forge_maps/%s/%s.map", clean_name, clean_name);
	return 0;
}

void forge_trigger_save_map_action(void)
{
	/* If an initial save has already been done in this session (or loaded from Edit Maps), quick-update immediately! */
	if (g_forge.has_saved_map_once && g_forge.save_map_name[0])
	{
		g_forge.active_modal = _forge_modal_none;
		forge_ui_save_copy_as(g_forge.save_map_name);
		return;
	}

	/* Otherwise, open the initial save prompt so the user can name their custom map */
	if (!g_forge.save_map_name[0])
	{
		char const *cur = main_get_map_name();
		char const *slash = cur ? strrchr(cur, '\\') : NULL;
		snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s_forge", slash ? (slash + 1) : "custom");
		g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
	}
	g_forge.active_modal = _forge_modal_save_copy_as;
}

void forge_load_saved_map_state_if_present(char const *map_name)
{
	char const *slash;
	char leaf[64];
	char txt_path[512];
	FILE *f;
	char line[512];
	int in_forge_state = 0;

	if (!map_name || !map_name[0])
		return;
	/* Check saved custom_maps\<name> and forge_maps\<name>\<name> maps */
	if (_strnicmp(map_name, "custom_maps\\", 12) != 0 &&
		_strnicmp(map_name, "custom_maps/", 12) != 0 &&
		_strnicmp(map_name, "forge_maps\\", 11) != 0 &&
		_strnicmp(map_name, "forge_maps/", 11) != 0)
		return;

	slash = strrchr(map_name, '\\');
	if (!slash)
		slash = strrchr(map_name, '/');
	snprintf(leaf, sizeof(leaf), "%s", slash ? (slash + 1) : map_name);

	snprintf(txt_path, sizeof(txt_path), "forge_maps/%s/%s.txt", leaf, leaf);
	f = fopen(txt_path, "rb");
	if (!f)
	{
		snprintf(txt_path, sizeof(txt_path), "custom_maps/%s.txt", leaf);
		f = fopen(txt_path, "rb");
	}
	if (!f)
		return;

	while (fgets(line, sizeof(line), f))
	{
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
			line[--len] = 0;

		if (strcmp(line, "[OpenCE Forge Map]") == 0)
		{
			/* This map is already a saved Forge map: enable quick-update on F5! */
			snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s", leaf);
			g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
			g_forge.has_saved_map_once = 1;
		}
		else if (strcmp(line, "[ForgeState]") == 0)
		{
			in_forge_state = 1;
		}
		else if (in_forge_state && strncmp(line, "OBJECT|", 7) == 0)
		{
			unsigned int grp = 0;
			int src_cache = 0, baked = 0, team = 0;
			float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0;
			char tpath[256];
			tpath[0] = 0;
			if (sscanf(line + 7, "%u|%d|%d|%f|%f|%f|%f|%f|%f|%d|%255[^\r\n]",
					&grp, &src_cache, &baked, &x, &y, &z, &yaw, &pitch, &roll, &team, tpath) == 11 && tpath[0])
			{
				if (!baked)
				{
					/* Cached tag (.ftag) object not baked into base_map: spawn live now! */
					forge_3d_spawn_object_at((uint32_t)grp, tpath, (uint8_t)src_cache, x, y, z, yaw, pitch, roll, (int16_t)team, 0);
				}
				else if (g_forge.edit_state.placed_object_count < FORGE_MAX_PLACED_OBJECTS)
				{
					/* Record baked object in edit_state so subsequent F5 updates preserve it */
					struct forge_placed_object_entry *po = &g_forge.edit_state.placed_objects[g_forge.edit_state.placed_object_count++];
					memset(po, 0, sizeof(*po));
					po->active = 1;
					po->group_tag = (uint32_t)grp;
					po->source_cache_id = (uint8_t)src_cache;
					po->category = (grp == 'vehi') ? _forge_object_vehicle :
						(grp == 'weap') ? _forge_object_weapon :
						(grp == 'bipd') ? _forge_object_biped :
						(grp == 'eqip') ? _forge_object_equipment : _forge_object_scenery;
					snprintf(po->tag_path, sizeof(po->tag_path), "%s", tpath);
					po->x = x; po->y = y; po->z = z;
					po->yaw = yaw; po->pitch = pitch; po->roll = roll;
					po->team_index = (int16_t)team;
				}
			}
		}
		else if (in_forge_state && strncmp(line, "AI|", 3) == 0)
		{
			float x = 0, y = 0, z = 0, yaw = 0;
			int team = 0;
			char tpath[256];
			tpath[0] = 0;
			if (sscanf(line + 3, "%f|%f|%f|%f|%d|%255[^\r\n]", &x, &y, &z, &yaw, &team, tpath) == 6 && tpath[0])
			{
				forge_3d_spawn_object_at('actv', tpath, 0, x, y, z, yaw, 0.0f, 0.0f, (int16_t)team, 0);
			}
		}
		else if (in_forge_state && strncmp(line, "DEFAULT_EDIT|", 13) == 0)
		{
			unsigned int grp = 0;
			int deleted = 0;
			float ox = 0, oy = 0, oz = 0, nx = 0, ny = 0, nz = 0, nyaw = 0, npitch = 0, nroll = 0;
			char tpath[256];
			tpath[0] = 0;
			if (sscanf(line + 13, "%u|%d|%f|%f|%f|%f|%f|%f|%f|%f|%f|%255[^\r\n]",
					&grp, &deleted, &ox, &oy, &oz, &nx, &ny, &nz, &nyaw, &npitch, &nroll, tpath) == 12 &&
				g_forge.edit_state.default_edit_count < FORGE_MAX_DEFAULT_OBJECT_EDITS)
			{
				struct forge_default_object_edit *de = &g_forge.edit_state.default_edits[g_forge.edit_state.default_edit_count++];
				memset(de, 0, sizeof(*de));
				de->active = 1;
				de->group_tag = (uint32_t)grp;
				de->deleted = (uint8_t)deleted;
				de->orig_x = ox; de->orig_y = oy; de->orig_z = oz;
				de->new_x = nx; de->new_y = ny; de->new_z = nz;
				de->new_yaw = nyaw; de->new_pitch = npitch; de->new_roll = nroll;
				snprintf(de->tag_path, sizeof(de->tag_path), "%s", tpath);
			}
		}
	}
	fclose(f);
}

/* ============================================================================
 * MONITOR ("characters\monitor\monitor") & DYNAMIC CACHED TAG RENDERER
 * - Renders any dynamic cached tag object with its genuine 3D mesh from cached_tags/
 * - Renders ONLY player bipeds in Forge Mode as the Monitor!
 * ============================================================================ */

static struct
{
	int initialized;
	int valid;
	int part_count;
	float radius;
	struct forge_hw_part parts[FORGE_MAX_HW_PARTS];
} s_monitor_hw;

static struct
{
	int initialized;
	char path[256];
	uint32_t item_count;
	char *names_buf;
	uint8_t *index_buf;
	uint32_t names_bytes;
} s_bitmaps_res;

static void ensure_bitmaps_resource_index_loaded(void)
{
	static char const *const candidates[] = {
		"custom_maps/bitmaps.map",
		"maps/bitmaps.map",
		"OpenCE Forge/forge_maps/bitmaps.map",
		"../FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bitmaps.map",
		"FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bitmaps.map",
		"../FORGE CODE/GAME MAPS/PC MAPS/bitmaps.map",
		"FORGE CODE/GAME MAPS/PC MAPS/bitmaps.map"
	};
	int c;
	FILE *f = NULL;
	uint32_t hdr[4];
	uint32_t names_off, index_off, item_count, names_bytes;

	if (s_bitmaps_res.initialized)
		return;
	s_bitmaps_res.initialized = 1;

	for (c = 0; c < 7; c++)
	{
		f = fopen(candidates[c], "rb");
		if (f)
		{
			snprintf(s_bitmaps_res.path, sizeof(s_bitmaps_res.path), "%s", candidates[c]);
			break;
		}
	}
	if (!f)
		return;

	if (fread(hdr, 4, 4, f) != 4 || hdr[0] != 1u)
	{
		fclose(f);
		return;
	}
	names_off = hdr[1];
	index_off = hdr[2];
	item_count = hdr[3];
	if (index_off <= names_off || item_count == 0 || item_count > 32768u)
	{
		fclose(f);
		return;
	}
	names_bytes = index_off - names_off;
	s_bitmaps_res.names_buf = (char *)malloc(names_bytes + 1);
	s_bitmaps_res.index_buf = (uint8_t *)malloc(item_count * 12u);
	if (!s_bitmaps_res.names_buf || !s_bitmaps_res.index_buf)
	{
		free(s_bitmaps_res.names_buf);
		free(s_bitmaps_res.index_buf);
		s_bitmaps_res.names_buf = NULL;
		s_bitmaps_res.index_buf = NULL;
		fclose(f);
		return;
	}
	if (fseek(f, (long)names_off, SEEK_SET) != 0 ||
		fread(s_bitmaps_res.names_buf, 1, names_bytes, f) != names_bytes ||
		fseek(f, (long)index_off, SEEK_SET) != 0 ||
		fread(s_bitmaps_res.index_buf, 1, item_count * 12u, f) != item_count * 12u)
	{
		free(s_bitmaps_res.names_buf);
		free(s_bitmaps_res.index_buf);
		s_bitmaps_res.names_buf = NULL;
		s_bitmaps_res.index_buf = NULL;
		fclose(f);
		return;
	}
	s_bitmaps_res.names_buf[names_bytes] = '\0';
	s_bitmaps_res.names_bytes = names_bytes;
	s_bitmaps_res.item_count = item_count;
	fclose(f);
}

/* Point 6: Load embedded Campaign / Xbox / PC bitmaps directly from forgeasset.map or customforgeasset.map!
 * Campaign items (Pelican, Wraith, Covenant Dropship, Lifepod, Monitor, Campaign trees/rocks/crates)
 * have ext == 0 in forgeasset.map with their s_bitmap_data metadata and raw DXT/RGB pixels embedded in the map! */
static long load_dynamic_bitmap_from_forgeasset_cache(char const *cache_path, char const *bitm_name)
{
	FILE *f;
	uint8_t hdr[2048];
	uint32_t tag_off, tag_sz;
	uint8_t *tag_buf;
	uint32_t inst_addr, inst_off;
	int32_t tag_count, i;
	long result_idx = NONE;

	if (!cache_path || !cache_path[0] || !bitm_name || !bitm_name[0])
		return NONE;

	f = fopen(cache_path, "rb");
	if (!f)
		return NONE;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return NONE;
	}
	tag_off = *(uint32_t const *)(hdr + 0x10);
	tag_sz = *(uint32_t const *)(hdr + 0x14);
	if (tag_off < 2048 || tag_sz < 40 || tag_sz > 64 * 1024 * 1024)
	{
		fclose(f);
		return NONE;
	}
	tag_buf = (uint8_t *)malloc(tag_sz);
	if (!tag_buf)
	{
		fclose(f);
		return NONE;
	}
	if (fseek(f, (long)tag_off, SEEK_SET) != 0 || fread(tag_buf, 1, tag_sz, f) != tag_sz)
	{
		free(tag_buf);
		fclose(f);
		return NONE;
	}

	inst_addr = *(uint32_t const *)(tag_buf + 0x00);
	tag_count = *(int32_t const *)(tag_buf + 0x0C);
	if (inst_addr < 0x40440000u || tag_count <= 0 || tag_count > 16384)
	{
		free(tag_buf);
		fclose(f);
		return NONE;
	}
	inst_off = inst_addr - 0x40440000u;

	for (i = 0; i < tag_count; i++)
	{
		uint32_t io = inst_off + (uint32_t)i * 32u;
		uint32_t grp, name_addr, data_addr, is_ext;
		char const *tname;

		if (io + 32u > tag_sz)
			break;
		grp = *(uint32_t const *)(tag_buf + io + 0x00);
		if (grp != 'bitm')
			continue;
		name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
		data_addr = *(uint32_t const *)(tag_buf + io + 0x14);
		is_ext = *(uint32_t const *)(tag_buf + io + 0x18);
		if (is_ext != 0 || name_addr < 0x40440000u || (name_addr - 0x40440000u) >= tag_sz)
			continue;
		tname = (char const *)(tag_buf + (name_addr - 0x40440000u));
		if (_stricmp(tname, bitm_name) == 0 && data_addr >= 0x40440000u)
		{
			uint32_t doff = data_addr - 0x40440000u;
			if (doff + 0x6Cu <= tag_sz)
			{
				int32_t bmp_count = *(int32_t const *)(tag_buf + doff + 0x60);
				uint32_t bmp_ptr = *(uint32_t const *)(tag_buf + doff + 0x64);
				if (bmp_count > 0 && bmp_count <= 64 && bmp_ptr >= 0x40440000u)
				{
					uint32_t boff = bmp_ptr - 0x40440000u;
					if (boff + 48u <= tag_sz)
					{
						uint8_t const *bdata = tag_buf + boff;
						int16_t width = *(int16_t const *)(bdata + 0x04);
						int16_t height = *(int16_t const *)(bdata + 0x06);
						int16_t type = *(int16_t const *)(bdata + 0x0A);
						int16_t format = *(int16_t const *)(bdata + 0x0C);
						uint32_t pixels_off = *(uint32_t const *)(bdata + 0x18);
						uint32_t pixels_sz = *(uint32_t const *)(bdata + 0x1C);

						if (type == 0 && width > 0 && height > 0 && width <= 2048 && height <= 2048 &&
							(width & (width - 1)) == 0 && (height & (height - 1)) == 0 &&
							format >= 0 && format < 18 && format != 4 && format != 5 && format != 7 && format != 12 && format != 13)
						{
							struct bitmap_data *bmp = bitmap_2d_new(width, height, 0, format);
							if (bmp && bmp->base_address)
							{
								long top_mip_bytes = bitmap_get_pixel_data_size(bmp);
								if (top_mip_bytes > 0 && (uint32_t)top_mip_bytes <= pixels_sz &&
									fseek(f, (long)pixels_off, SEEK_SET) == 0 &&
									fread(bmp->base_address, 1, (size_t)top_mip_bytes, f) == (size_t)top_mip_bytes)
								{
									struct bitmap_group *bgrp;
									/* Clear power-of-two flag bit 0 before bitmap_rebuild so rasterizer_bitmap_new
									 * keeps mipmap_count = 0 (since base_address is allocated for top mip only!) */
									bmp->flags &= (uint16_t)~1u;
									bitmap_rebuild(bmp);

									bgrp = (struct bitmap_group *)calloc(1, sizeof(struct bitmap_group));
									if (bgrp)
									{
										bgrp->bitmaps.count = 1;
										bgrp->bitmaps.address = bmp;
										result_idx = append_runtime_tag_instance('bitm', NONE, NONE, bitm_name, bgrp);
									}
								}
							}
						}
					}
				}
			}
			break;
		}
	}

	free(tag_buf);
	fclose(f);
	return result_idx;
}

static long load_dynamic_bitmap_from_resource_map(char const *bitm_name)
{
	long existing_idx;
	uint32_t i;
	uint32_t found_data_off = 0, found_size = 0;
	FILE *f;
	uint8_t *tag_bytes;
	int32_t bmp_count;
	uint32_t bmp_rel;
	uint8_t const *bdata;
	int16_t width, height, type, format;
	uint32_t pixels_off, pixels_sz;
	struct bitmap_data *bmp;
	long top_mip_bytes;
	struct bitmap_group *bgrp;

	if (!bitm_name || !bitm_name[0])
		return NONE;

	existing_idx = tag_loaded('bitm', bitm_name);
	if (existing_idx != NONE)
		return existing_idx;

	/* 1. Check embedded Campaign / custom bitmaps in OpenCE Forge/cached_tags/ first! */
	existing_idx = load_dynamic_bitmap_from_forgeasset_cache("OpenCE Forge/cached_tags/bitmaps_cache.bin", bitm_name);
	if (existing_idx != NONE)
		return existing_idx;
	existing_idx = load_dynamic_bitmap_from_forgeasset_cache("OpenCE Forge/cached_tags/custom_bitmaps_cache.bin", bitm_name);
	if (existing_idx != NONE)
		return existing_idx;
	existing_idx = load_dynamic_bitmap_from_forgeasset_cache("OpenCE Forge/forge_maps/forgeasset.map", bitm_name);
	if (existing_idx != NONE)
		return existing_idx;
	existing_idx = load_dynamic_bitmap_from_forgeasset_cache("OpenCE Forge/forge_maps/customforgeasset.map", bitm_name);
	if (existing_idx != NONE)
		return existing_idx;

	/* 2. Check Custom Edition bitmaps.map resource index */
	ensure_bitmaps_resource_index_loaded();
	if (!s_bitmaps_res.names_buf || !s_bitmaps_res.index_buf)
		return NONE;

	/* In bitmaps.map, odd indices (or the 2nd matching entry) hold the s_bitmap_group tag definition */
	for (i = 0; i < s_bitmaps_res.item_count; i++)
	{
		uint8_t const *ent = s_bitmaps_res.index_buf + i * 12u;
		uint32_t n_off = *(uint32_t const *)(ent + 0);
		uint32_t sz = *(uint32_t const *)(ent + 4);
		uint32_t d_off = *(uint32_t const *)(ent + 8);
		if (n_off < s_bitmaps_res.names_bytes &&
			_stricmp(s_bitmaps_res.names_buf + n_off, bitm_name) == 0 &&
			sz >= 0x6Cu + 0x30u)
		{
			found_data_off = d_off;
			found_size = sz;
			if (i & 1u)
				break;
		}
	}
	if (!found_data_off || found_size < 0x9Cu || found_size > 65536u)
		return NONE;

	f = fopen(s_bitmaps_res.path, "rb");
	if (!f)
		return NONE;

	tag_bytes = (uint8_t *)malloc(found_size);
	if (!tag_bytes)
	{
		fclose(f);
		return NONE;
	}
	if (fseek(f, (long)found_data_off, SEEK_SET) != 0 ||
		fread(tag_bytes, 1, found_size, f) != found_size)
	{
		free(tag_bytes);
		fclose(f);
		return NONE;
	}

	bmp_count = *(int32_t const *)(tag_bytes + 0x60);
	bmp_rel = *(uint32_t const *)(tag_bytes + 0x64);
	if (bmp_count <= 0 || bmp_count > 64 || bmp_rel < 0x6Cu || bmp_rel + 48u > found_size)
	{
		/* Fallback: s_bitmap_data array at end of resource tag block */
		if (bmp_count > 0 && bmp_count <= 64 && found_size >= 0x6Cu + (uint32_t)bmp_count * 48u)
			bmp_rel = found_size - (uint32_t)bmp_count * 48u;
		else
		{
			free(tag_bytes);
			fclose(f);
			return NONE;
		}
	}

	bdata = tag_bytes + bmp_rel;
	width = *(int16_t const *)(bdata + 0x04);
	height = *(int16_t const *)(bdata + 0x06);
	type = *(int16_t const *)(bdata + 0x0A);
	format = *(int16_t const *)(bdata + 0x0C);
	pixels_off = *(uint32_t const *)(bdata + 0x18);
	pixels_sz = *(uint32_t const *)(bdata + 0x1C);

	/* Validate 2D power-of-two bitmap format */
	if (type != 0 || width <= 0 || height <= 0 || width > 2048 || height > 2048 ||
		(width & (width - 1)) != 0 || (height & (height - 1)) != 0 ||
		format < 0 || format >= 18 || format == 4 || format == 5 || format == 7 || format == 12 || format == 13)
	{
		free(tag_bytes);
		fclose(f);
		return NONE;
	}

	bmp = bitmap_2d_new(width, height, 0, format);
	if (!bmp || !bmp->base_address)
	{
		free(tag_bytes);
		fclose(f);
		return NONE;
	}

	top_mip_bytes = bitmap_get_pixel_data_size(bmp);
	if (top_mip_bytes <= 0 || (uint32_t)top_mip_bytes > pixels_sz ||
		fseek(f, (long)pixels_off, SEEK_SET) != 0 ||
		fread(bmp->base_address, 1, (size_t)top_mip_bytes, f) != (size_t)top_mip_bytes)
	{
		free(tag_bytes);
		fclose(f);
		return NONE;
	}

	free(tag_bytes);
	fclose(f);

	/* Clear power-of-two flag bit 0 before bitmap_rebuild so rasterizer_bitmap_new
	 * keeps mipmap_count = 0 (since base_address is allocated for top mip only!) */
	bmp->flags &= (uint16_t)~1u;

	/* Upload to D3D8/OpenGL hardware texture */
	bitmap_rebuild(bmp);

	bgrp = (struct bitmap_group *)calloc(1, sizeof(struct bitmap_group));
	if (!bgrp)
		return NONE;
	bgrp->bitmaps.count = 1;
	bgrp->bitmaps.address = bmp;

	return append_runtime_tag_instance('bitm', NONE, NONE, bitm_name, bgrp);
}

static long get_or_create_dynamic_bitmap_shader(char const *shdr_name, char const *bitm_name)
{
	long shdr_idx, bitm_idx, tmpl_soso_idx;
	struct tag_iterator iter;
	void *tmpl_ptr;
	uint8_t *soso_def;
	uint16_t model_flags;

	if (!shdr_name || !shdr_name[0] || !bitm_name || !bitm_name[0])
		return NONE;

	shdr_idx = tag_loaded('soso', shdr_name);
	if (shdr_idx != NONE)
		return shdr_idx;

	bitm_idx = load_dynamic_bitmap_from_resource_map(bitm_name);
	if (bitm_idx == NONE)
		return NONE;

	tag_iterator_new(&iter, 'soso');
	tmpl_soso_idx = tag_iterator_next(&iter);
	if (tmpl_soso_idx == NONE)
		return NONE;
	tmpl_ptr = shader_definition_get(tmpl_soso_idx);
	if (!tmpl_ptr)
		return NONE;

	soso_def = (uint8_t *)calloc(1, 512);
	if (!soso_def)
		return NONE;
	memcpy(soso_def, tmpl_ptr, 440);

	/* Point 6 & 12: Set _shader_model_not_alpha_tested_bit (bit 2 = 0x04) on opaque model shaders!
	 * Halo diffuse bitmaps often store specular/reflection masks in the alpha channel; without bit 2,
	 * rasterizer_xbox_models.c enables D3DRS_ALPHATESTENABLE (ALPHAREF=0x7F) and discards faces! */
	model_flags = (strstr(shdr_name, "leaf") || strstr(shdr_name, "foliage") ||
				   strstr(shdr_name, "fern") || strstr(shdr_name, "vine") ||
				   strstr(shdr_name, "thorn") || strstr(shdr_name, "decal"))
		? 0u
		: (1u << 2);

	/* Set exact shader_model_definition fields (verified against rasterizer_xbox_models.c:465-521) */
	*(int16_t *)(soso_def + 0x24) = 4;            /* _shader_type_model */
	*(uint16_t *)(soso_def + 0x28) = model_flags; /* model.flags (_shader_model_not_alpha_tested_bit) */
	*(float *)(soso_def + 0x38) = 1.0f;           /* model.translucency */
	*(int16_t *)(soso_def + 0x4C) = 0;            /* model.change_color_source = none */
	*(uint16_t *)(soso_def + 0x6C) = 0;           /* model.self_illumination_flags = 0 */
	*(int16_t *)(soso_def + 0x70) = 0;            /* model.self_illumination_color_source = none */
	*(float *)(soso_def + 0x74) = 1.0f;           /* model.self_illumination_animation_period != 0 */
	*(float *)(soso_def + 0x9C) = 1.0f;           /* model.map_u_scale */
	*(float *)(soso_def + 0xA0) = 1.0f;           /* model.map_v_scale */
	*(int32_t *)(soso_def + 0xA4) = 'bitm';       /* model.base_map.group_tag */
	*(int32_t *)(soso_def + 0xA8) = 0;
	*(int32_t *)(soso_def + 0xAC) = 0;
	*(int32_t *)(soso_def + 0xB0) = bitm_idx;     /* model.base_map.index */
	*(int32_t *)(soso_def + 0xC8) = NONE;         /* model.multipurpose_map.index */
	*(int16_t *)(soso_def + 0xD4) = 0;            /* model.detail_function = none */
	*(int16_t *)(soso_def + 0xD6) = 0;            /* model.detail_mask = none */
	*(float *)(soso_def + 0xD8) = 1.0f;           /* model.detail_map_scale */
	*(int32_t *)(soso_def + 0xE8) = NONE;         /* model.detail_map.index */
	*(float *)(soso_def + 0xEC) = 1.0f;           /* model.detail_map_v_scale */
	*(float *)(soso_def + 0x144) = 0.0f;          /* model.perpendicular_brightness */
	*(float *)(soso_def + 0x154) = 0.0f;          /* model.parallel_brightness */
	*(int32_t *)(soso_def + 0x170) = NONE;        /* model.reflection_cube_map.index */

	return append_runtime_tag_instance('soso', 'shdr', NONE, shdr_name, soso_def);
}

static long find_best_loaded_shader(char const *shader_hint)
{
	struct tag_iterator iter;
	long idx;
	char shdr_name[128];
	char bitm_name[128];

	shdr_name[0] = '\0';
	bitm_name[0] = '\0';

	if (shader_hint && shader_hint[0])
	{
		char const *bar = strchr(shader_hint, '|');
		if (bar)
		{
			size_t slen = (size_t)(bar - shader_hint);
			if (slen >= sizeof(shdr_name))
				slen = sizeof(shdr_name) - 1;
			memcpy(shdr_name, shader_hint, slen);
			shdr_name[slen] = '\0';
			snprintf(bitm_name, sizeof(bitm_name), "%s", bar + 1);
		}
		else
		{
			snprintf(shdr_name, sizeof(shdr_name), "%s", shader_hint);
		}

		/* 1. Exact shader match in current map */
		idx = tag_loaded('shdr', shdr_name);
		if (idx != NONE) return idx;
		idx = tag_loaded('soso', shdr_name);
		if (idx != NONE) return idx;
		idx = tag_loaded('senv', shdr_name);
		if (idx != NONE) return idx;
		idx = tag_loaded('schi', shdr_name);
		if (idx != NONE) return idx;
		idx = tag_loaded('sgla', shdr_name);
		if (idx != NONE) return idx;

		/* 2. Dynamically create model shader with real texture skin from forgeasset.map / bitmaps.map! */
		if (bitm_name[0])
		{
			idx = get_or_create_dynamic_bitmap_shader(shdr_name, bitm_name);
			if (idx != NONE)
				return idx;
		}

		/* 3. Keyword match against loaded shaders in current map */
		{
			char const *keyword = NULL;
			if (strstr(shdr_name, "leaf") || strstr(shdr_name, "foliage") || strstr(shdr_name, "vine"))
				keyword = "leaf";
			else if (strstr(shdr_name, "bark") || strstr(shdr_name, "trunk") || strstr(shdr_name, "wood") || strstr(shdr_name, "tree"))
				keyword = "bark";
			else if (strstr(shdr_name, "rock") || strstr(shdr_name, "boulder") || strstr(shdr_name, "stone") || strstr(shdr_name, "cliff"))
				keyword = "boulder";
			else if (strstr(shdr_name, "c_") || strstr(shdr_name, "cov") || strstr(shdr_name, "wraith") || strstr(shdr_name, "dropship") || strstr(shdr_name, "banshee") || strstr(shdr_name, "ghost"))
				keyword = "ghost";
			else if (strstr(shdr_name, "pelican") || strstr(shdr_name, "warthog") || strstr(shdr_name, "scorpion") || strstr(shdr_name, "hull"))
				keyword = "warthog";

			if (keyword)
			{
				tag_iterator_new(&iter, 'shdr');
				while ((idx = tag_iterator_next(&iter)) != NONE)
				{
					char const *sname = tag_get_name(idx);
					if (sname && strstr(sname, keyword))
						return idx;
				}
			}
		}
	}

	/* 4. Default metallic model shader */
	{
		static char const *const preferred[] = {
			"characters\\cyborg\\shaders\\armor",
			"vehicles\\warthog\\shaders\\warthog hull",
			"vehicles\\ghost\\shaders\\ghost_hull",
			"weapons\\assault rifle\\fp\\shaders\\metal"
		};
		int i;
		for (i = 0; i < 4; i++)
		{
			idx = tag_loaded('soso', preferred[i]);
			if (idx != NONE)
				return idx;
		}
	}
	tag_iterator_new(&iter, 'soso');
	idx = tag_iterator_next(&iter);
	if (idx != NONE)
		return idx;
	tag_iterator_new(&iter, 'shdr');
	return tag_iterator_next(&iter);
}

static void ensure_monitor_hardware_mesh_loaded(void)
{
	if (s_monitor_hw.initialized)
		return;
	s_monitor_hw.initialized = 1;
	s_monitor_hw.valid = 0;

	if (load_hw_parts_from_ftag_file(
			"OpenCE Forge/cached_tags/bipeds/characters_monitor_monitor.ftag",
			s_monitor_hw.parts,
			FORGE_MAX_HW_PARTS,
			&s_monitor_hw.part_count,
			&s_monitor_hw.radius,
			1.35f,
			0.55f))
	{
		s_monitor_hw.valid = 1;
		return;
	}
	if (load_hw_parts_from_cache_file(
			"OpenCE Forge/forge_maps/forgeasset.map",
			'mod2',
			"characters\\monitor\\monitor",
			s_monitor_hw.parts,
			FORGE_MAX_HW_PARTS,
			&s_monitor_hw.part_count,
			&s_monitor_hw.radius,
			1.35f,
			0.55f))
	{
		s_monitor_hw.valid = 1;
		return;
	}
	if (load_hw_parts_from_cache_file(
			"FORGE CODE/GAME MAPS/PC MAPS/c10.map",
			'mod2',
			"characters\\monitor\\monitor",
			s_monitor_hw.parts,
			FORGE_MAX_HW_PARTS,
			&s_monitor_hw.part_count,
			&s_monitor_hw.radius,
			1.35f,
			0.55f))
	{
		s_monitor_hw.valid = 1;
		return;
	}
}

static void draw_custom_hw_parts(
	long object_index,
	void const *lighting_ptr,
	union real_point3d const *pos,
	union real_vector3d const *forward,
	union real_vector3d const *up,
	float radius,
	float scale,
	struct forge_hw_part *parts,
	int part_count,
	int is_monitor,
	int is_preview)
{
	struct rasterizer_model_begin_parameters model_parameters;
	union real_rgb_color change_colors[4];
	real function_values[4];
	struct real_matrix4x3 node_matrix;
	long fallback_shader_idx;
	struct shader *fallback_shader_def;
	int p;

	fallback_shader_idx = find_best_loaded_shader(NULL);
	if (fallback_shader_idx == NONE)
		return;
	fallback_shader_def = shader_definition_get(fallback_shader_idx);
	if (!fallback_shader_def)
		return;

	matrix4x3_from_point_and_vectors(&node_matrix, pos, forward, up);
	if (scale > 0.001f && scale != 1.0f)
		node_matrix.scale = scale;

	if (is_monitor)
	{
		change_colors[0].red = 0.28f;
		change_colors[0].green = 0.72f;
		change_colors[0].blue = 1.0f;
		change_colors[1].red = 0.65f;
		change_colors[1].green = 0.90f;
		change_colors[1].blue = 1.0f;
	}
	else
	{
		change_colors[0].red = 0.85f;
		change_colors[0].green = 0.85f;
		change_colors[0].blue = 0.85f;
		change_colors[1].red = 0.75f;
		change_colors[1].green = 0.75f;
		change_colors[1].blue = 0.75f;
	}
	change_colors[2] = change_colors[0];
	change_colors[3] = change_colors[1];

	function_values[0] = 1.0f;
	function_values[1] = 1.0f;
	function_values[2] = 1.0f;
	function_values[3] = 1.0f;

	memset(&model_parameters, 0, sizeof(model_parameters));
	model_parameters.unique_identifier = object_index;
	if (lighting_ptr)
	{
		model_parameters.lighting = *(struct render_lighting const *)lighting_ptr;
	}
	else
	{
		/* Bright studio lighting for 3D Spawn Palette Preview */
		model_parameters.lighting.ambient_color.red = 0.72f;
		model_parameters.lighting.ambient_color.green = 0.74f;
		model_parameters.lighting.ambient_color.blue = 0.78f;
		model_parameters.lighting.distant_light_count = 1;
		model_parameters.lighting.distant_lights[0].color.red = 0.95f;
		model_parameters.lighting.distant_lights[0].color.green = 0.95f;
		model_parameters.lighting.distant_lights[0].color.blue = 0.92f;
		model_parameters.lighting.distant_lights[0].direction.i = -0.577f;
		model_parameters.lighting.distant_lights[0].direction.j = -0.577f;
		model_parameters.lighting.distant_lights[0].direction.k = -0.577f;
	}
	model_parameters.centroid = *pos;
	model_parameters.radius = (radius > 0.1f) ? (radius * ((scale > 0.001f) ? scale : 1.0f)) : 1.5f;
	model_parameters.animation.colors = change_colors;
	model_parameters.animation.values = function_values;
	model_parameters.skinning.node_matrices = &node_matrix;
	model_parameters.skinning.node_matrix_count = 1;
	/* Point 12: Use first-person depth range (bit 4 = 0x10) for 3D preview so large models never clip into world geometry or sky! */
	model_parameters.geometry_flags = is_preview ? (1u << 4) : 0;
	model_parameters.base_map_scale.i = 1.0f;
	model_parameters.base_map_scale.j = 1.0f;

	rasterizer_model_begin(&model_parameters, FALSE);
	for (p = 0; p < part_count; p++)
	{
		long shdr_idx = find_best_loaded_shader(parts[p].shader_hint);
		struct shader *shdr_def = (shdr_idx != NONE) ? shader_definition_get(shdr_idx) : fallback_shader_def;
		if (!shdr_def)
			shdr_def = fallback_shader_def;

		rasterizer_model_draw(
			shdr_def,
			0,
			&parts[p].triangle_buffer,
			NONE,
			parts[p].triangle_buffer.count,
			&parts[p].vertex_buffer,
			NONE);
	}
	rasterizer_model_end();
}

static struct
{
	int loaded_catalog_idx;
	int valid;
	int part_count;
	float radius;
	struct forge_hw_part parts[FORGE_MAX_HW_PARTS];
} s_preview_hw = { -1, 0, 0, 1.5f };

static void render_spawn_palette_3d_preview(long object_index, void const *lighting_ptr)
{
	int i, match_count = 0, chosen_idx = -1;
	struct forge_catalog_entry const *entry;
	union real_point3d preview_pos;
	union real_vector3d preview_fwd, preview_up, cam_right;
	float dist, angle, preview_scale;

	if (g_forge.active_modal != _forge_modal_spawn_palette || g_forge.h3_menu_stage != 1)
		return;

	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (g_forge.catalog[i].category == g_forge.current_category &&
			forge_catalog_entry_allowed(&g_forge.catalog[i]))
		{
			if (match_count == g_forge.palette_cursor)
			{
				chosen_idx = i;
				break;
			}
			match_count++;
		}
	}
	if (chosen_idx < 0)
		return;

	entry = &g_forge.catalog[chosen_idx];
	if (s_preview_hw.loaded_catalog_idx != chosen_idx)
	{
		char ftag_path[512];
		s_preview_hw.loaded_catalog_idx = chosen_idx;
		s_preview_hw.valid = 0;
		s_preview_hw.part_count = 0;
		s_preview_hw.radius = 1.5f;

		make_cached_ftag_path(entry->group_tag, entry->tag_path, ftag_path, sizeof(ftag_path));
		if (load_hw_parts_from_ftag_file(
				ftag_path,
				s_preview_hw.parts,
				FORGE_MAX_HW_PARTS,
				&s_preview_hw.part_count,
				&s_preview_hw.radius,
				1.0f,
				0.0f) ||
			load_hw_parts_from_cache_file(
				"OpenCE Forge/forge_maps/forgeasset.map",
				entry->group_tag,
				entry->tag_path,
				s_preview_hw.parts,
				FORGE_MAX_HW_PARTS,
				&s_preview_hw.part_count,
				&s_preview_hw.radius,
				1.0f,
				0.0f) ||
			load_hw_parts_from_cache_file(
				"OpenCE Forge/forge_maps/customforgeasset.map",
				entry->group_tag,
				entry->tag_path,
				s_preview_hw.parts,
				FORGE_MAX_HW_PARTS,
				&s_preview_hw.part_count,
				&s_preview_hw.radius,
				1.0f,
				0.0f))
		{
			s_preview_hw.valid = (s_preview_hw.part_count > 0) ? 1 : 0;
		}
	}

	if (!s_preview_hw.valid || s_preview_hw.part_count <= 0)
		return;

	/* Point 12: Normalize preview model scale so small weapons and huge Pelicans/trees
	 * fit cleanly at a fixed 1.55m studio distance in the right-side viewport! */
	preview_scale = 0.52f / ((s_preview_hw.radius > 0.15f) ? s_preview_hw.radius : 0.52f);
	if (preview_scale < 0.035f) preview_scale = 0.035f;
	if (preview_scale > 1.35f) preview_scale = 1.35f;
	dist = 1.55f;

	/* Position the 3D rotating preview directly in front of the active render camera,
	 * offset slightly to the right so it sits in the open viewport area beside the left menu! */
	cross_product3d(&render.camera.forward, &render.camera.up, &cam_right);
	preview_pos.x = render.camera.position.x + render.camera.forward.i * dist + cam_right.i * 0.42f - render.camera.up.i * 0.14f;
	preview_pos.y = render.camera.position.y + render.camera.forward.j * dist + cam_right.j * 0.42f - render.camera.up.j * 0.14f;
	preview_pos.z = render.camera.position.z + render.camera.forward.k * dist + cam_right.k * 0.42f - render.camera.up.k * 0.14f;

	angle = (float)(system_milliseconds() % 6000u) * (6.2831853f / 6000.0f);
	preview_fwd.i = (float)cosine(angle);
	preview_fwd.j = (float)sine(angle);
	preview_fwd.k = 0.0f;
	preview_up.i = 0.0f;
	preview_up.j = 0.0f;
	preview_up.k = 1.0f;

	draw_custom_hw_parts(
		object_index,
		lighting_ptr,
		&preview_pos,
		&preview_fwd,
		&preview_up,
		s_preview_hw.radius,
		preview_scale,
		s_preview_hw.parts,
		s_preview_hw.part_count,
		0,
		1);
}

void forge_render_world_3d_overlays(void const *lighting_ptr)
{
	if (!g_forge_session_enabled || !g_forge.initialized)
		return;
	render_spawn_palette_3d_preview(NONE, lighting_ptr);
}

int forge_monitor_render_player_unit(long object_index, void const *lighting_ptr, unsigned long flags)
{
	struct object_datum *obj;
	struct unit_datum *unit;
	int i;

	if (!g_forge_session_enabled || object_index == NONE)
		return 0;

	/* Safely verify object datum before accessing (prevents stale salt assert in data.c:412) */
	obj = (struct object_datum *)object_try_and_get_and_verify_type(object_index, (unsigned long)-1);
	if (!obj)
		return 0;

	/* 1. Check if this object is one of our dynamically loaded Cached Tag objects! */
	for (i = 0; i < s_dyn_tags.count; i++)
	{
		if (s_dyn_tags.entries[i].definition_index == obj->definition_index &&
			s_dyn_tags.entries[i].part_count > 0)
		{
			if (flags & (1u << 1)) /* Skip shadow pass for custom hardware mesh */
				return 1;
			draw_custom_hw_parts(
				object_index,
				lighting_ptr,
				&obj->object.position,
				&obj->object.forward,
				&obj->object.up,
				s_dyn_tags.entries[i].bounding_radius,
				1.0f,
				s_dyn_tags.entries[i].parts,
				s_dyn_tags.entries[i].part_count,
				0,
				0);
			return 1;
		}
	}

	/* 2. Check if this object is a player biped */
	unit = (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_biped);
	if (!unit || unit->object.type != _object_type_biped || unit->unit.player_index == NONE)
		return 0;

	if (g_forge.mode == _forge_mode_play)
		return 0;

	if (flags & (1u << 1)) /* Skip shadow pass for Monitor hardware mesh */
		return 1;

	ensure_monitor_hardware_mesh_loaded();
	if (!s_monitor_hw.valid || s_monitor_hw.part_count <= 0)
		return 0;

	{
		union real_point3d pos = unit->object.position;
		float t = (float)(system_milliseconds() % 4000u) * (6.2831853f / 4000.0f);
		pos.z += 0.06f * (t < 3.1415926f ? (t / 3.1415926f - 0.5f) : (1.5f - t / 3.1415926f));

		draw_custom_hw_parts(
			object_index,
			lighting_ptr,
			&pos,
			&unit->object.forward,
			&unit->object.up,
			1.0f,
			1.0f,
			s_monitor_hw.parts,
			s_monitor_hw.part_count,
			1,
			0);
	}
	return 1;
}


