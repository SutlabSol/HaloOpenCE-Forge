/*
 * OpenCE Forge ("HaloCE Forge.exe")
 * Engine Symbol Bridge Hooks (linked via llvm-objcopy --redefine-sym).
 * Strictly modular inside FORGE CODE/ with zero modifications to source/ or port/.
 */

#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "camera/director.h"
#include "interface/hud.h"
#include "interface/hud_draw.h"
#include "interface/ui_widget.h"
#include "main/main.h"
#include "main/console.h"
#include "models/models.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "saved games/player_profile.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "network_chat.h"
#include "custom_edition_maps.h"
#include <xtl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge_engine.h"

#undef fopen
#undef fclose
#undef fread
#undef fseek
#undef malloc
#undef free

#include "game/game_engine.h"
#include "objects/objects.h"
#include "units/units.h"

extern unsigned long system_milliseconds(void);
extern void *cache_files_tag_instances(long *count);
extern void cache_files_set_tag_instances(void *instances, long count);
extern char const *pc_menu_function_name(long function_index);
extern char const *pc_menu_game_data_input_name(long function_index);
extern void main_menu_switch_to_single_player(void);
extern void game_connection_set(short connection);
extern long player_ui_get_active_player_profile_index(short local_player_index);
extern boolean player_ui_get_active_player_profile(short local_player_index, struct player_profile *profile);
extern void player_ui_set_active_player_profile(short local_player_index, long profile_index, struct player_profile const *profile);
extern void player_ui_set_single_player_local_player_controller(short local_player_index, short controller_index);
extern void player_ui_remember_player1_profile(boolean remember);
extern void ui_play_audio_feedback_sound(short index);
extern void display_error_text_deferred(wchar_t const *error_text, short text_style);
extern short ui_widget_port_gametypes(long *indices, short maximum, short *last_used);
extern boolean ui_widget_port_gametype_choose(long profile_index);
extern boolean ui_widget_port_open(void *widget, char const *name, boolean *widget_deleted);
extern boolean ui_widget_port_game_variant_set(struct game_variant *variant, struct game_variant_options const *options);
extern struct game_variant *build_game_variant_slayer(struct game_variant *variant);
extern void game_variant_options_default(struct game_variant const *variant, struct game_variant_options *options);

static short s_forge_last_map_kind = 1; /* 1 = MAP_KIND_MULTIPLAYER, 3 = MAP_KIND_CUSTOM_MULTIPLAYER, 4 = MAP_KIND_FORGE_MAPS */
static short s_forge_active_map_kind = 1;
static int s_is_forge_map_cache_valid = 0;
static short s_non_forge_ce_indices[4096];
static short s_non_forge_ce_count = 0;
static short s_forge_only_ce_indices[4096];
static short s_forge_only_ce_count = 0;
static long s_save_forge_btn_tag = NONE;

/* Original engine symbols redefined via llvm-objcopy in FORGE CODE/build/wrapped_obj/ */
extern void orig_network_chat_update(void);
extern void orig_network_chat_draw(short local_player_index);
extern boolean orig_network_chat_typing(void);
extern void orig_network_chat_handle_up(long machine_index, void const *entries, boolean from_stream);
extern void orig_director_update(real time_delta_sec);
extern short orig_director_get_perspective(short local_player_index);
extern struct observer_result const *orig_render_interpolation_camera(
	short local_player_index,
	struct observer_result const *fallback);
extern void orig_first_person_weapon_draw(void);
extern void orig_hud_render_weapon_interface(struct player_datum *player);
extern void orig_render_model(
	long model_index,
	real level_of_detail_pixels,
	struct real_matrix4x3 const *node_matrices,
	char const *region_permutation_indices,
	union real_rgb_color const *change_colors,
	real const *function_values,
	struct render_lighting const *lighting,
	union real_point3d const *centroid,
	real radius,
	struct render_model_effect const *model_effect,
	long unique_identifier,
	short forced_shader_permutation_index,
	unsigned long flags);
extern boolean orig_hs_compile_and_evaluate(char const *source);
extern void orig_hs_runtime_update(void);
extern void orig_cinematic_start(void);
extern boolean orig_scenario_trigger_volume_test_object(short trigger_volume_index, long object_index);
extern long orig_game_engine_remap_object_definition(long definition_index);
extern void orig_game_engine_post_rasterize_objects(void);
extern void rasterizer_models_begin(boolean sky);
extern void rasterizer_models_end(void);
extern void orig_menu_tags_loaded(char const *map_name);
extern void orig_menu_tags_unloaded(void);
extern boolean orig_pc_menu_event_function_invoke(
	void *widget,
	void *event,
	long function_index,
	boolean *widget_deleted);
extern void orig_pc_menu_game_data_function_invoke(
	void *widget,
	long function);

static unsigned long g_last_forge_frame_ms = 0;
static char g_last_forge_map_name[128] = "";

/* ---------- Custom Menu Folder Redirect for menu_files.c */

int forge_menu_config_folder(char *buf, unsigned int size)
{
	if (!buf || size == 0)
		return 0;
	snprintf(buf, size, "OpenCE Forge/");
	return (int)strlen(buf);
}

extern char **__cdecl SDL_GlobDirectory(char const *path, char const *pattern, unsigned int flags, int *count);

char **__cdecl forge_menu_SDL_GlobDirectory(char const *path, char const *pattern, unsigned int flags, int *count)
{
	char **names = SDL_GlobDirectory(path, pattern, flags, count);
	if (names && count)
	{
		int i;
		for (i = 0; i < *count && names[i]; i++)
		{
			char *p;
			for (p = names[i]; *p; p++)
			{
				if (*p == '\\')
					*p = '/';
			}
		}
	}
	return names;
}

void *__imp__forge_menu_SDL_GlobDirectory = (void *)&forge_menu_SDL_GlobDirectory;

/* ---------- Widget Instance Layout (matches port/linux/game/menu_functions.c) */

struct forge_widget_instance
{
	long definition_tag_index;
	char const *name;
	short local_player_index;
	short horizontal_offset;
	short vertical_offset;
	short type;
	boolean visible;
	boolean render_regardless_of_controller_index;
	boolean disabled;
	boolean pause_game_time;
	boolean delete_recursion_lock;
	boolean widget_is_error_dialog;
	boolean close_if_local_player_controller_present;
	byte pad17;
	long creation_time;
	unsigned long milliseconds_to_auto_close;
	unsigned long auto_close_fade_time;
	real alpha_modifier;
	struct forge_widget_instance *previous;
	struct forge_widget_instance *next;
	struct forge_widget_instance *parent;
	struct forge_widget_instance *child;
	struct forge_widget_instance *focused_child;
	union
	{
		struct
		{
			wchar_t *text;
			short string_list_index;
		} text_box;
		struct
		{
			short selected_index;
			short last_list_tab_direction;
			void *list_items;
			word number_of_items;
			struct forge_widget_instance *extended_description;
			wchar_t *item_text;
		} list;
	} parameters;
	struct
	{
		short current_frame_index;
		short first_frame_index;
		short last_frame_index;
		short number_of_sprite_frames;
	} animation;
};

static struct forge_widget_instance *fw_descendant(struct forge_widget_instance *w, char const *name, long *nth)
{
	struct forge_widget_instance *c;
	for (c = w ? w->child : NULL; c; c = c->next)
	{
		struct forge_widget_instance *found;
		if (c->name && !strcmp(c->name, name) && (*nth)-- == 0)
			return c;
		found = fw_descendant(c, name, nth);
		if (found)
			return found;
	}
	return NULL;
}

static struct forge_widget_instance *fw_named(struct forge_widget_instance *w, char const *name, long nth)
{
	return w ? fw_descendant(w, name, &nth) : NULL;
}

static void fw_text_set_ascii(struct forge_widget_instance *tb, char const *ascii)
{
	int i;
	if (!tb || !ascii)
		return;
	if (!tb->parameters.text_box.text)
	{
		tb->parameters.text_box.text = (wchar_t *)ui_widget_realloc(NULL, 128 * sizeof(wchar_t), __FILE__, __LINE__);
	}
	if (tb->parameters.text_box.text)
	{
		for (i = 0; ascii[i] && i < 126; i++)
			tb->parameters.text_box.text[i] = (wchar_t)(unsigned char)ascii[i];
		tb->parameters.text_box.text[i] = 0;
	}
}

/* ---------- Forge Edit Maps (User Created / Downloaded) & Inject Custom Tags Native Menus */

#define FORGE_MAX_EDIT_MAPS 64

static struct
{
	int tab; /* 0 = USER CREATED, 1 = DOWNLOADED */
	int count;
	int first;
	int chosen;
	char map_leaves[FORGE_MAX_EDIT_MAPS][64];
	char map_titles[FORGE_MAX_EDIT_MAPS][64];
	char map_authors[FORGE_MAX_EDIT_MAPS][64];
	int injected_flags[FORGE_MAX_DONOR_MAPS_LIST];
	int inject_first;
	int inject_chosen;
} s_forge_menu;

static int is_forge_created_map_file(char const *map_path, char const *leaf_no_ext, int *out_is_user_created, char *out_author, size_t author_sz)
{
	FILE *f;
	uint8_t hdr[2048];
	uint32_t marker = 0;
	char txt_path[512];
	int has_flg1 = 0;
	int user_created = 0;

	if (out_author && author_sz > 0)
		snprintf(out_author, author_sz, "Community Creator");

	f = fopen(map_path, "rb");
	if (f)
	{
		if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr))
		{
			uint32_t state_off = *(uint32_t const *)(hdr + 0x1E4);
			uint32_t state_sz = *(uint32_t const *)(hdr + 0x1E8);
			marker = *(uint32_t const *)(hdr + 0x1FC);
			if (marker == 0x464C4731u) /* 'FLG1' */
			{
				has_flg1 = 1;
				user_created = 1;
				if (state_off >= 0x800u && state_sz > 0 && state_sz < 4u * 1024u * 1024u)
				{
					char buf[1024];
					uint32_t rsz = (state_sz < sizeof(buf) - 1u) ? state_sz : (uint32_t)(sizeof(buf) - 1u);
					if (fseek(f, (long)state_off, SEEK_SET) == 0 && fread(buf, 1, rsz, f) == rsz)
					{
						buf[rsz] = 0;
						if (strstr(buf, "[OpenCE Forge Map]") != NULL)
							has_flg1 = 1;
						if (strstr(buf, "ForgeSource=UserCreated") != NULL)
							user_created = 1;
					}
				}
			}
		}
		fclose(f);
	}

	snprintf(txt_path, sizeof(txt_path), "forge_maps/%s/%s.txt", leaf_no_ext, leaf_no_ext);
	f = fopen(txt_path, "rb");
	if (!f)
	{
		snprintf(txt_path, sizeof(txt_path), "custom_maps/%s.txt", leaf_no_ext);
		f = fopen(txt_path, "rb");
	}
	if (f)
	{
		char buf[1024];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		fclose(f);
		if (strstr(buf, "[OpenCE Forge Map]") != NULL)
			has_flg1 = 1;
		if (strstr(buf, "ForgeSource=UserCreated") != NULL)
			user_created = 1;
	}

	if (out_is_user_created)
		*out_is_user_created = user_created;
	return has_flg1;
}

static int is_custom_map_leaf_forge_made(char const *leaf_no_ext)
{
	char check_path[512];
	FILE *f;

	if (!leaf_no_ext || !leaf_no_ext[0])
		return 0;

	/* Any map with metadata in forge_maps/<name>/<name>.txt or legacy forge_maps/<name>/<name>.map is a Forge map */
	snprintf(check_path, sizeof(check_path), "forge_maps/%s/%s.txt", leaf_no_ext, leaf_no_ext);
	f = fopen(check_path, "rb");
	if (f)
	{
		fclose(f);
		return 1;
	}

	snprintf(check_path, sizeof(check_path), "forge_maps/%s/%s.map", leaf_no_ext, leaf_no_ext);
	f = fopen(check_path, "rb");
	if (f)
	{
		fclose(f);
		return 1;
	}

	/* Also check if custom_maps/<name>.map has the 'FLG1' Forge header marker */
	snprintf(check_path, sizeof(check_path), "custom_maps/%s.map", leaf_no_ext);
	return is_forge_created_map_file(check_path, leaf_no_ext, NULL, NULL, 0);
}

static void ensure_forge_ce_map_partition_built(void)
{
	short total_ce, i;

	if (s_is_forge_map_cache_valid)
		return;
	s_is_forge_map_cache_valid = 1;
	s_non_forge_ce_count = 0;
	s_forge_only_ce_count = 0;

	total_ce = custom_edition_maps_count(FALSE);
	for (i = 0; i < total_ce; i++)
	{
		short disp = custom_edition_maps_display_index_of(FALSE, i);
		char const *lvl = custom_edition_maps_level_name(disp);
		char const *leaf = lvl;
		if (lvl)
		{
			char const *bs = strrchr(lvl, '\\');
			char const *fs = strrchr(lvl, '/');
			if (bs && (!fs || bs > fs))
				leaf = bs + 1;
			else if (fs)
				leaf = fs + 1;
		}
		if (leaf && is_custom_map_leaf_forge_made(leaf))
		{
			if (s_forge_only_ce_count < 4096)
				s_forge_only_ce_indices[s_forge_only_ce_count++] = i;
		}
		else
		{
			if (s_non_forge_ce_count < 4096)
				s_non_forge_ce_indices[s_non_forge_ce_count++] = i;
		}
	}
}

void forge_menu_custom_edition_maps_look_again(void)
{
	s_is_forge_map_cache_valid = 0;
	custom_edition_maps_look_again();
}

short forge_menu_custom_edition_maps_count(boolean campaign)
{
	if (campaign)
		return custom_edition_maps_count(TRUE);
	ensure_forge_ce_map_partition_built();
	if (s_forge_active_map_kind == 4)
		return s_forge_only_ce_count;
	return s_non_forge_ce_count;
}

short forge_menu_custom_edition_maps_display_index_of(boolean campaign, short index)
{
	if (campaign)
		return custom_edition_maps_display_index_of(TRUE, index);
	ensure_forge_ce_map_partition_built();
	if (s_forge_active_map_kind == 4)
	{
		if (index < 0 || index >= s_forge_only_ce_count)
			return NONE;
		return custom_edition_maps_display_index_of(FALSE, s_forge_only_ce_indices[index]);
	}
	if (index < 0 || index >= s_non_forge_ce_count)
		return NONE;
	return custom_edition_maps_display_index_of(FALSE, s_non_forge_ce_indices[index]);
}

extern short ui_widget_port_multiplayer_maps(char const *const **names, short *last_used);
extern boolean ui_widget_port_multiplayer_map_choose(short level_index);

static short s_last_mp_maps_returned = 0;

short forge_menu_multiplayer_maps(char const *const **names, short *last_used)
{
	short total = ui_widget_port_multiplayer_maps(names, last_used);
	short all_ce = custom_edition_maps_count(FALSE);
	short xbox_cnt = (short)(total > all_ce ? (total - all_ce) : 13);
	short part_cnt;

	/* Map screen always initializes on MAP_KIND_MULTIPLAYER (1) */
	s_forge_active_map_kind = 1;
	s_forge_last_map_kind = 1;
	part_cnt = forge_menu_custom_edition_maps_count(FALSE);
	if (last_used && *last_used >= xbox_cnt)
		*last_used = 0;
	s_last_mp_maps_returned = (short)(xbox_cnt + part_cnt);
	return s_last_mp_maps_returned;
}

boolean forge_menu_multiplayer_map_choose(short level_index)
{
	char const *const *names = NULL;
	short last_used = 0;
	short total = ui_widget_port_multiplayer_maps(&names, &last_used);
	short all_ce = custom_edition_maps_count(FALSE);
	short xbox_cnt = (short)(total > all_ce ? (total - all_ce) : 13);

	ensure_forge_ce_map_partition_built();

	/* In menu_functions.c (map_list_choose), when kind is CUSTOM MULTIPLAYER (3) or FORGE MAPS (4),
	 * it passes: xbox_multiplayer_map_count(map_list.map_count) + chosen, where
	 * xbox_multiplayer_map_count(c) = MAX(c - forge_menu_custom_edition_maps_count(FALSE), 0).
	 * Recover the exact 0-based `chosen` index regardless of how many Forge or non-Forge CE maps exist! */
	if (s_forge_active_map_kind == 4 || s_forge_active_map_kind == 3)
	{
		short active_ce = forge_menu_custom_edition_maps_count(FALSE);
		short mp_count = (s_last_mp_maps_returned > 0) ? s_last_mp_maps_returned : (short)(xbox_cnt + s_non_forge_ce_count);
		short base = (short)((mp_count > active_ce) ? (mp_count - active_ce) : 0);
		short chosen = (short)(level_index - base);

		if (s_forge_active_map_kind == 4)
		{
			if (chosen < 0 || chosen >= s_forge_only_ce_count)
				return FALSE;
			level_index = (short)(xbox_cnt + s_forge_only_ce_indices[chosen]);
		}
		else
		{
			if (chosen < 0 || chosen >= s_non_forge_ce_count)
				return FALSE;
			level_index = (short)(xbox_cnt + s_non_forge_ce_indices[chosen]);
		}
	}
	return ui_widget_port_multiplayer_map_choose(level_index);
}

static void scan_forge_edit_maps(int wanted_tab)
{
	WIN32_FIND_DATAA fd;
	HANDLE h;

	s_forge_menu.tab = wanted_tab;
	s_forge_menu.count = 0;
	s_forge_menu.first = 0;
	s_forge_menu.chosen = 0;

	/* Scan forge_maps/<name>/ subfolders (where .txt and .bmp live, with .map in custom_maps/<name>.map or legacy forge_maps/<name>/<name>.map) */
	h = FindFirstFileA("forge_maps\\*", &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			char full_path[512];
			char leaf[64];
			char author[64];
			int is_user = 0;
			FILE *tf;

			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				continue;
			if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
				continue;

			snprintf(leaf, sizeof(leaf), "%s", fd.cFileName);
			snprintf(full_path, sizeof(full_path), "custom_maps/%s.map", leaf);
			tf = fopen(full_path, "rb");
			if (tf)
			{
				fclose(tf);
			}
			else
			{
				snprintf(full_path, sizeof(full_path), "forge_maps/%s/%s.map", leaf, leaf);
			}
			if (is_forge_created_map_file(full_path, leaf, &is_user, author, sizeof(author)))
			{
				if ((wanted_tab == 0 && is_user) || (wanted_tab == 1 && !is_user))
				{
					if (s_forge_menu.count < FORGE_MAX_EDIT_MAPS)
					{
						int idx = s_forge_menu.count++;
						snprintf(s_forge_menu.map_leaves[idx], sizeof(s_forge_menu.map_leaves[0]), "%s", leaf);
						snprintf(s_forge_menu.map_titles[idx], sizeof(s_forge_menu.map_titles[0]), "%s", leaf);
						snprintf(s_forge_menu.map_authors[idx], sizeof(s_forge_menu.map_authors[0]), "%s",
							is_user ? "User Created Forge Map" : "Downloaded Forge Map");
					}
				}
			}
		} while (FindNextFileA(h, &fd));
		CloseHandle(h);
	}

	/* Also scan legacy custom_maps\*.map Forge maps */
	h = FindFirstFileA("custom_maps\\*.map", &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		char full_path[512];
		char leaf[64];
		char author[64];
		char *dot;
		int is_user = 0;
		int k, already_listed = 0;

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (_stricmp(fd.cFileName, "bitmaps.map") == 0 ||
			_stricmp(fd.cFileName, "sounds.map") == 0 ||
			_stricmp(fd.cFileName, "loc.map") == 0)
		{
			continue;
		}

		snprintf(leaf, sizeof(leaf), "%s", fd.cFileName);
		dot = strrchr(leaf, '.');
		if (dot)
			*dot = 0;

		for (k = 0; k < s_forge_menu.count; k++)
		{
			if (_stricmp(s_forge_menu.map_leaves[k], leaf) == 0)
			{
				already_listed = 1;
				break;
			}
		}
		if (already_listed)
			continue;

		snprintf(full_path, sizeof(full_path), "custom_maps/%s", fd.cFileName);
		if (is_forge_created_map_file(full_path, leaf, &is_user, author, sizeof(author)))
		{
			if ((wanted_tab == 0 && is_user) || (wanted_tab == 1 && !is_user))
			{
				if (s_forge_menu.count < FORGE_MAX_EDIT_MAPS)
				{
					int idx = s_forge_menu.count++;
					snprintf(s_forge_menu.map_leaves[idx], sizeof(s_forge_menu.map_leaves[0]), "%s", leaf);
					snprintf(s_forge_menu.map_titles[idx], sizeof(s_forge_menu.map_titles[0]), "%s", leaf);
					snprintf(s_forge_menu.map_authors[idx], sizeof(s_forge_menu.map_authors[0]), "%s",
						is_user ? "User Created Forge Map" : "Downloaded Forge Map");
				}
			}
		}
	} while (FindNextFileA(h, &fd));
	CloseHandle(h);
}

static void update_forge_edit_maps_widget(struct forge_widget_instance *list)
{
	struct forge_widget_instance *spinner = fw_named(list, "edit_maps_kind_spinner", 0);
	struct forge_widget_instance *desc = list->parameters.list.extended_description;
	struct forge_widget_instance *child;
	int kind = spinner ? (int)spinner->parameters.list.selected_index : 0;
	int row_idx = 0;
	int focused_entry = -1;

	if (kind < 0) kind = 0;
	if (kind > 1) kind = 1;
	if (kind != s_forge_menu.tab)
	{
		scan_forge_edit_maps(kind);
	}

	for (child = list->child; child; child = child->next, row_idx++)
	{
		int item_slot;
		if (row_idx < 1)
			continue;
		if (!child->name || strncmp(child->name, "list_item_", 10) != 0)
			break;
		item_slot = s_forge_menu.first + (row_idx - 1);
		if (s_forge_menu.count == 0)
		{
			child->visible = (row_idx == 1) ? TRUE : FALSE;
			if (row_idx == 1)
			{
				fw_text_set_ascii(
					fw_named(child, "list_item_text", 0),
					kind == 0 ? "(No User Created Maps Yet)" : "(No Downloaded Forge Maps)");
			}
		}
		else
		{
			child->visible = (item_slot < s_forge_menu.count) ? TRUE : FALSE;
			if (item_slot < s_forge_menu.count)
			{
				fw_text_set_ascii(fw_named(child, "list_item_text", 0), s_forge_menu.map_titles[item_slot]);
				if (child == list->focused_child)
					focused_entry = item_slot;
			}
		}
		if (fw_named(child, "list_item_arrows", 0))
			fw_named(child, "list_item_arrows", 0)->visible = (child == list->focused_child);
		if (fw_named(child, "scroll_up_button", 0))
			fw_named(child, "scroll_up_button", 0)->visible = FALSE;
		if (fw_named(child, "scroll_down_button", 0))
			fw_named(child, "scroll_down_button", 0)->visible = FALSE;
	}

	if (focused_entry >= 0 && focused_entry < s_forge_menu.count)
		s_forge_menu.chosen = focused_entry;

	if (desc)
	{
		struct forge_widget_instance *sp_name = fw_named(desc, "replay_level_right_name", 0);
		struct forge_widget_instance *sp_pic = fw_named(desc, "replay_level_right_pic", 0);
		struct forge_widget_instance *sp_data = fw_named(desc, "replay_level_right_data", 0);
		struct forge_widget_instance *mp_name = fw_named(desc, "mp_map_right_name", 0);
		struct forge_widget_instance *mp_pic = fw_named(desc, "mp_map_right_pic", 0);
		struct forge_widget_instance *mp_data = fw_named(desc, "mp_map_right_data", 0);
		long d_idx;

		/* Always hide the single-player right-hand widgets ('Truth and Reconciliation' default) and difficulty icons */
		if (sp_name) sp_name->visible = FALSE;
		if (sp_pic) sp_pic->visible = FALSE;
		if (sp_data) sp_data->visible = FALSE;
		for (d_idx = 0; d_idx < 3; d_idx++)
		{
			struct forge_widget_instance *di = fw_named(desc, "difficulty_indicator", d_idx);
			if (di) di->visible = FALSE;
		}

		if (s_forge_menu.count > 0 && s_forge_menu.chosen >= 0 && s_forge_menu.chosen < s_forge_menu.count)
		{
			char level_name[128];
			char info[192];
			short disp_idx;

			snprintf(level_name, sizeof(level_name), "custom_maps\\%s", s_forge_menu.map_leaves[s_forge_menu.chosen]);
			disp_idx = custom_edition_maps_display_index(level_name);
			snprintf(info, sizeof(info), "%s\r\nforge_maps\\%s\\%s.map\r\nPress OK to Edit in Forge",
				s_forge_menu.map_authors[s_forge_menu.chosen],
				s_forge_menu.map_leaves[s_forge_menu.chosen],
				s_forge_menu.map_leaves[s_forge_menu.chosen]);

			if (disp_idx != NONE)
			{
				wchar_t *desc_w = custom_edition_maps_description(disp_idx);
				if (desc_w)
				{
					int k;
					for (k = 0; info[k] && k < 254; k++)
						desc_w[k] = (wchar_t)(unsigned char)info[k];
					desc_w[k] = 0;
				}
				if (mp_name)
				{
					mp_name->visible = TRUE;
					mp_name->parameters.text_box.string_list_index = disp_idx;
				}
				if (mp_pic)
				{
					mp_pic->visible = TRUE;
					mp_pic->animation.current_frame_index = disp_idx;
				}
				if (mp_data)
				{
					mp_data->visible = TRUE;
					mp_data->parameters.text_box.string_list_index = disp_idx;
				}
			}
			else
			{
				if (mp_name) mp_name->visible = FALSE;
				if (mp_pic) mp_pic->visible = FALSE;
				if (mp_data) mp_data->visible = FALSE;
			}
		}
		else
		{
			if (mp_name) mp_name->visible = FALSE;
			if (mp_pic) mp_pic->visible = FALSE;
			if (mp_data) mp_data->visible = FALSE;
		}
	}
}

static void update_forge_inject_widget(struct forge_widget_instance *list)
{
	struct forge_widget_instance *desc = list->parameters.list.extended_description;
	struct forge_widget_instance *row;
	int r = 0;

	for (row = list->child; row; row = row->next)
	{
		int idx;
		if (!row->name || strncmp(row->name, "op_mod_", 7) != 0)
			continue;
		idx = s_forge_menu.inject_first + r;
		row->animation.current_frame_index = (row == list->focused_child) ? 1 : 0;
		if (idx < g_forge.donor_map_count)
		{
			char const *full = g_forge.donor_map_files[idx];
			row->visible = TRUE;
			if (strcmp(full, "__GEARBOX_MAPS__") == 0)
			{
				fw_text_set_ascii(fw_named(row, "mod_name", 0), "ADD GEARBOX MAP TAGS");
			}
			else if (strcmp(full, "__OCETAGS_FOLDER__") == 0)
			{
				fw_text_set_ascii(fw_named(row, "mod_name", 0), "ADD .OCETAGS PACKAGES");
			}
			else
			{
				char const *slash = strrchr(full, '/');
				if (!slash) slash = strrchr(full, '\\');
				fw_text_set_ascii(fw_named(row, "mod_name", 0), slash ? (slash + 1) : full);
			}
			fw_text_set_ascii(fw_named(row, "mod_state", 0), s_forge_menu.injected_flags[idx] ? "ADDED" : "READY");
			if (row == list->focused_child)
				s_forge_menu.inject_chosen = idx;
		}
		else
		{
			row->visible = FALSE;
		}
		if (fw_named(row, "mod_status", 0))
			fw_named(row, "mod_status", 0)->visible = FALSE;
		r++;
	}

	if (desc)
	{
		if (g_forge.donor_map_count > 0 && s_forge_menu.inject_chosen < g_forge.donor_map_count)
		{
			char details[384];
			char const *path = g_forge.donor_map_files[s_forge_menu.inject_chosen];
			if (strcmp(path, "__GEARBOX_MAPS__") == 0)
			{
				fw_text_set_ascii(fw_named(desc, "mod_details_name", 0), "ADD GEARBOX MAP TAGS");
				snprintf(details, sizeof(details),
					"Extracts all unique Vehicle, Scenery, Weapon & Equipment tags from all 19 Gearbox MP maps:\r\n"
					"Battle Creek, Blood Gulch, Boarding Action, Chill Out, Chiron TL-34, Damnation, Danger Canyon, Death Island, Derelict, Gephyrophobia, Hang 'Em High, Ice Fields, Infinity, Longest, Prisoner, Rat Race, Sidewinder, Timberland, Wizard.");
				fw_text_set_ascii(fw_named(desc, "mod_details_text", 0), details);
			}
			else if (strcmp(path, "__OCETAGS_FOLDER__") == 0)
			{
				fw_text_set_ascii(fw_named(desc, "mod_details_name", 0), "ADD .OCETAGS PACKAGES");
				snprintf(details, sizeof(details),
					"Scans OpenCE Forge/ocetags/ and extracts all custom .ocetags packages into cached_tags/.\r\n"
					"Custom items appear in the Forge HUD Spawn Menu prefixed with [c] and are baked into saved .map files.");
				fw_text_set_ascii(fw_named(desc, "mod_details_text", 0), details);
			}
			else
			{
				fw_text_set_ascii(fw_named(desc, "mod_details_name", 0), "ADD TAG PACKAGE TO CACHE");
				snprintf(details, sizeof(details),
					"Package:\r\n%s\r\n\r\nPress ENTER or click row to extract this .ocetags package into the Forge cache (prefixed with [c] in the Forge Spawn Menu).",
					path);
				fw_text_set_ascii(fw_named(desc, "mod_details_text", 0), details);
			}
		}
		else
		{
			fw_text_set_ascii(fw_named(desc, "mod_details_name", 0), "NO PACKAGES FOUND");
			fw_text_set_ascii(fw_named(desc, "mod_details_text", 0),
				"Place .ocetags packages in OpenCE Forge/ocetags/ to add custom tags to cache.");
		}
	}
}

boolean forge_player_profile_get(long player_profile_index, struct player_profile *profile)
{
	boolean ok = player_profile_get(player_profile_index, profile);
	if (ok && profile && g_forge_session_enabled)
	{
		int lv;
		for (lv = 0; lv < NUMBER_OF_SINGLE_PLAYER_LEVELS; lv++)
			profile->single_player_map_flags[lv] |= 1;
	}
	return ok;
}

boolean pc_menu_event_function_invoke(
	void *widget_ptr,
	void *event_ptr,
	long function_index,
	boolean *widget_deleted)
{
	struct forge_widget_instance *widget = (struct forge_widget_instance *)widget_ptr;
	char const *fn_name = pc_menu_function_name(function_index);
	char const *w_name = (widget && widget->name) ? widget->name : "";
	char const *p_name = (widget && widget->parent && widget->parent->name) ? widget->parent->name : "";

	/* Keep Forge session enabled across Singleplayer, Multiplayer, and Edit Maps flows */
	(void)p_name;
	g_forge_session_enabled = 1;

	/* Forge -> Edit Maps screen initialization & activation */
	if (fn_name && strcmp(fn_name, "load game menu init") == 0 &&
		strcmp(w_name, "forge_edit_maps_list") == 0)
	{
		g_forge_session_enabled = 1;
		custom_edition_maps_look_again();
		scan_forge_edit_maps(0);
		return TRUE;
	}
	if (fn_name && strcmp(fn_name, "load game menu activated") == 0 &&
		(strcmp(w_name, "forge_edit_maps_list") == 0 ||
		 (widget && widget->parent && widget->parent->name && strcmp(widget->parent->name, "forge_edit_maps_list") == 0)))
	{
		if (s_forge_menu.count > 0 && s_forge_menu.chosen >= 0 && s_forge_menu.chosen < s_forge_menu.count)
		{
			char level_path[128];
			g_forge_session_enabled = 1;
			custom_edition_maps_look_again();
			snprintf(level_path, sizeof(level_path), "custom_maps\\%s", s_forge_menu.map_leaves[s_forge_menu.chosen]);
			player_ui_set_single_player_local_player_controller(0, 0);
			main_set_map_name(level_path);
			main_set_difficulty(1);
			game_connection_set(0);
			main_menu_switch_to_single_player();
			player_ui_remember_player1_profile(TRUE);
			ui_play_audio_feedback_sound(2);
			return TRUE;
		}
		ui_play_audio_feedback_sound(4);
		return FALSE;
	}

	/* Settings -> CLEAR CUSTOM CACHE & RESET TAG CACHE buttons */
	if (fn_name && strcmp(fn_name, "port mods close") == 0)
	{
		if (w_name && strcmp(w_name, "clear_custom_cache_profile_item") == 0)
		{
			forge_clear_custom_tag_cache();
			ui_play_audio_feedback_sound(2);
			display_error_text_deferred(
				L"Custom tag cache cleared!\r\nAll custom .ocetags packages in 'OpenCE Forge/custom_tags' have been deleted.",
				0);
			return TRUE;
		}
		if (w_name && strcmp(w_name, "reset_tag_cache_profile_item") == 0)
		{
			ui_play_audio_feedback_sound(2);
			forge_reset_all_tag_caches_and_restart();
			return TRUE;
		}
	}

	/* Forge -> REHASH CUSTOM TAGS button & screen activation */
	if (fn_name && strcmp(fn_name, "port mods init") == 0 &&
		strcmp(w_name, "forge_inject_list") == 0)
	{
		forge_runtime_init();
		forge_runtime_reload_catalogs();
		memset(s_forge_menu.injected_flags, 0, sizeof(s_forge_menu.injected_flags));
		s_forge_menu.inject_first = 0;
		s_forge_menu.inject_chosen = 0;
		return TRUE;
	}
	if (fn_name && (strcmp(fn_name, "port mods toggle") == 0 || strcmp(fn_name, "port mods save") == 0))
	{
		struct forge_widget_instance *p = widget;
		while (p && (!p->name || strcmp(p->name, "forge_inject_list") != 0))
			p = p->parent;

		/* Direct click on "REHASH CUSTOM TAGS" button in the Forge submenu */
		if (!p && w_name && strcmp(w_name, "forge_item_inject_tags") == 0)
		{
			forge_ui_inject_custom_map("__OCETAGS_FOLDER__");
			ui_play_audio_feedback_sound(2);
			display_error_text_deferred(
				L"Custom .ocetags packages in 'OpenCE Forge/custom_tags' rehashed!\r\nCustom items appear at the top of each category in the Forge Spawn Menu prefixed with [c].",
				0);
			return TRUE;
		}
		if (p)
		{
			char const *chosen_path = "__OCETAGS_FOLDER__";
			if (g_forge.donor_map_count > 0 &&
				s_forge_menu.inject_chosen >= 0 &&
				s_forge_menu.inject_chosen < g_forge.donor_map_count)
			{
				chosen_path = g_forge.donor_map_files[s_forge_menu.inject_chosen];
			}
			if (forge_ui_inject_custom_map(chosen_path))
			{
				if (s_forge_menu.inject_chosen >= 0 && s_forge_menu.inject_chosen < 128)
					s_forge_menu.injected_flags[s_forge_menu.inject_chosen] = 1;
				ui_play_audio_feedback_sound(2);
				display_error_text_deferred(
					L"Custom .ocetags packages in 'OpenCE Forge/custom_tags' rehashed!\r\nCustom items appear at the top of each category in the Forge Spawn Menu prefixed with [c].",
					0);
			}
			return TRUE;
		}
	}

	/* Refresh Custom Edition & saved Forge maps on Single Player / Multiplayer level list init, and default to Multiplayer maps */
	if (fn_name && strcmp(fn_name, "initialize sp level list solo") == 0)
	{
		boolean res;
		struct forge_widget_instance *spinner;
		s_is_forge_map_cache_valid = 0;
		custom_edition_maps_look_again();
		s_forge_active_map_kind = 1;
		s_forge_last_map_kind = -1;
		res = orig_pc_menu_event_function_invoke(widget_ptr, event_ptr, function_index, widget_deleted);
		spinner = fw_named(widget, "list_item_0_map_kind_spinner", 0);
		if (spinner)
			spinner->parameters.list.selected_index = 1;
		return res;
	}
	if (fn_name && strcmp(fn_name, "mp level list initialize") == 0)
	{
		boolean res;
		struct forge_widget_instance *spinner;
		s_is_forge_map_cache_valid = 0;
		custom_edition_maps_look_again();
		s_forge_active_map_kind = 1;
		s_forge_last_map_kind = -1;
		res = orig_pc_menu_event_function_invoke(widget_ptr, event_ptr, function_index, widget_deleted);
		spinner = fw_named(widget, "list_item_0_map_kind_spinner", 0);
		if (spinner)
			spinner->parameters.list.selected_index = 1;
		return res;
	}

	if (fn_name && strcmp(fn_name, "solo level set map") == 0)
	{
		boolean res;
		struct forge_widget_instance *spinner = fw_named(widget, "list_item_0_map_kind_spinner", 0);
		if (spinner && s_forge_last_map_kind == 4)
			spinner->parameters.list.selected_index = 3;
		s_forge_active_map_kind = s_forge_last_map_kind;
		res = orig_pc_menu_event_function_invoke(widget_ptr, event_ptr, function_index, widget_deleted);
		if (spinner && s_forge_last_map_kind == 4)
			spinner->parameters.list.selected_index = 4;
		if (g_forge_session_enabled && res)
		{
			char cur_map_copy[128];
			char const *cur = main_get_map_name();
			snprintf(cur_map_copy, sizeof(cur_map_copy), "%s", (cur && cur[0]) ? cur : "levels\\test\\bloodgulch\\bloodgulch");
			player_ui_set_single_player_local_player_controller(0, 0);
			main_set_map_name(cur_map_copy);
			main_set_difficulty(1);
			game_connection_set(0);
			main_menu_switch_to_single_player();
			player_ui_remember_player1_profile(TRUE);
			ui_play_audio_feedback_sound(2);
			return FALSE; /* Returning FALSE prevents opening difficulty_select_list_screen */
		}
		return res;
	}

	/* Point 7: Multiplayer Forge Hosting -> skip Gametype Select screen and configure endless Forge sandbox! */
	if (fn_name && strcmp(fn_name, "mp level select") == 0)
	{
		boolean res;
		struct forge_widget_instance *spinner = fw_named(widget, "list_item_0_map_kind_spinner", 0);
		if (spinner && s_forge_last_map_kind == 4)
			spinner->parameters.list.selected_index = 3;
		s_forge_active_map_kind = s_forge_last_map_kind;
		res = orig_pc_menu_event_function_invoke(widget_ptr, event_ptr, function_index, widget_deleted);
		if (spinner && s_forge_last_map_kind == 4)
			spinner->parameters.list.selected_index = 4;
		if (g_forge_session_enabled && res)
		{
			long gtypes[64];
			short last_gt = 0;
			struct game_variant forge_var;
			struct game_variant_options forge_opts;

			if (ui_widget_port_gametypes(gtypes, 64, &last_gt) > 0)
				ui_widget_port_gametype_choose(gtypes[0]);

			memset(&forge_var, 0, sizeof(forge_var));
			build_game_variant_slayer(&forge_var);
			ustrncpy(
				forge_var.human_readable_game_description,
				L"Forge",
				NUMBEROF(forge_var.human_readable_game_description) - 1);
			forge_var.universal_variant.teams = FALSE;
			forge_var.universal_variant.score_to_win = 0x3FFFFFFF; /* Endless Forge session: never ends on kills */
			forge_var.universal_variant.respawn_time = 90;
			forge_var.universal_variant.suicide_penalty = 0;

			game_variant_options_default(&forge_var, &forge_opts);
			forge_opts.time_limit = 0; /* No time limit */
			forge_opts.vehicle_set[0] = VARIANT_VEHICLE_SET_PC; /* All map vehicles enabled */
			forge_opts.vehicle_set[1] = VARIANT_VEHICLE_SET_PC;
			ui_widget_port_game_variant_set(&forge_var, &forge_opts);

			ui_widget_port_open(
				widget_ptr,
				"pc\\main_menu\\multiplayer_type_select\\server_settings\\server_settings_screen",
				widget_deleted);
			return FALSE; /* Returning FALSE prevents XML handler from opening gametype_select_screen_wrapper */
		}
		return res;
	}

	/* In-Game Escape Pause Menu -> "SAVE MAP.." button handler
	 * Note: ui_widget_event_handler_function_invoke subtracts PC_MENU_FUNCTION_BASE (0x100 = 256) before calling pc_menu_event_function_invoke */
	if (function_index == 0x7777 ||
		function_index == (0x7777 - 256) ||
		function_index == (0x7777 - 102) ||
		(widget && widget->definition_tag_index == s_save_forge_btn_tag && s_save_forge_btn_tag != NONE) ||
		(w_name && strcmp(w_name, "save_forge_button") == 0))
	{
		ui_widgets_close_all();
		if (widget_deleted)
			*widget_deleted = TRUE;
		ui_play_audio_feedback_sound(2);
		forge_trigger_save_map_action();
		return TRUE;
	}

	/* Settings -> Controls Setup ("FORGE" and "FORGE PAD" tabs) */
	if (fn_name &&
		(strcmp(fn_name, "controls screen init") == 0 ||
		 strcmp(fn_name, "controls screen defaults") == 0 ||
		 strcmp(fn_name, "controls screen change set") == 0 ||
		 strcmp(fn_name, "controls binding slot") == 0 ||
		 strcmp(fn_name, "controls begin binding") == 0))
	{
		extern boolean forge_controls_menu_event(struct forge_widget_instance *w, void *event_ptr, char const *fn_name, long function_index, boolean *widget_deleted);
		if (forge_controls_menu_event(widget, event_ptr, fn_name, function_index, widget_deleted))
			return TRUE;
	}

	return orig_pc_menu_event_function_invoke(widget_ptr, event_ptr, function_index, widget_deleted);
}

/* ---------- Settings -> Controls Setup ("FORGE" & "FORGE PAD" Tabs) Implementation */

static struct
{
	int initialized;
	int32_t pending_vk[FORGE_BIND_COUNT];
	int32_t pending_gp[FORGE_BIND_COUNT];
	float pending_look_sens;
	float pending_fly_speed;
	int slot; /* 0 = Keyboard/Mouse, 1 = Controller */
	int capturing_action; /* -1 when idle, 0..FORGE_BIND_COUNT-1 when waiting for input */
	int capturing_slot;
	unsigned long capture_start_ms;
	int prev_vk_down[256];
	int prev_gp_down[FORGE_GP_COUNT];
} s_forge_controls_ui = { 0, { 0 }, { 0 }, 1.0f, 1.0f, 0, -1, 0, 0, { 0 }, { 0 } };

static char const *const s_forge_bind_labels[FORGE_BIND_COUNT] = {
	"FORGE / SPARTAN",
	"OBJECT PALETTE",
	"3RD-PERSON VIEW",
	"QUICK SAVE MAP",
	"ROTATE ITEM",
	"SNAP TO FLOOR",
	"SNAP TO SURFACE",
	"ROTATION SNAP",
	"OBJECT OPTIONS",
	"DELETE OBJECT"
};

static struct forge_widget_instance *fw_screen_of(struct forge_widget_instance *w)
{
	while (w && w->parent)
		w = w->parent;
	return w;
}

static struct forge_widget_instance *fw_control_row(struct forge_widget_instance *w)
{
	for (; w; w = w->parent)
	{
		if (w->name && strncmp(w->name, "op_command_", 11) == 0)
			return w;
	}
	return NULL;
}

static int fw_controls_group(struct forge_widget_instance *w)
{
	struct forge_widget_instance *sp = fw_named(fw_screen_of(w), "group_spinner", 0);
	return sp ? (int)sp->parameters.list.selected_index : 0;
}

static void forge_controls_ui_ensure_loaded(void)
{
	int i;
	if (g_forge.bind_vk[0] == 0 && g_forge.bind_gp[0] == 0)
	{
		forge_controls_init_defaults();
		forge_controls_load();
	}
	if (!s_forge_controls_ui.initialized)
	{
		for (i = 0; i < FORGE_BIND_COUNT; i++)
		{
			s_forge_controls_ui.pending_vk[i] = g_forge.bind_vk[i];
			s_forge_controls_ui.pending_gp[i] = g_forge.bind_gp[i];
		}
		s_forge_controls_ui.pending_look_sens = (g_forge.controller_look_sens >= 0.25f) ? g_forge.controller_look_sens : 1.0f;
		s_forge_controls_ui.pending_fly_speed = (g_forge.controller_fly_speed >= 0.25f) ? g_forge.controller_fly_speed : 1.0f;
		s_forge_controls_ui.capturing_action = -1;
		s_forge_controls_ui.initialized = 1;
	}
}

static void forge_controls_snapshot_input(void)
{
	int vk, gp;
	float lx, ly, rx, ry, lt, rt;
	forge_poll_controller_state(&lx, &ly, &rx, &ry, &lt, &rt);
	for (vk = 1; vk < 255; vk++)
		s_forge_controls_ui.prev_vk_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
	for (gp = 1; gp < FORGE_GP_COUNT; gp++)
		s_forge_controls_ui.prev_gp_down[gp] = forge_gp_button_down(gp);
}

boolean forge_controls_menu_event(
	struct forge_widget_instance *widget,
	void *event_ptr,
	char const *fn_name,
	long function_index,
	boolean *widget_deleted)
{
	int group = fw_controls_group(widget);
	int i;

	if (strcmp(fn_name, "controls screen init") == 0)
	{
		s_forge_controls_ui.initialized = 0;
		forge_controls_ui_ensure_loaded();
		orig_pc_menu_event_function_invoke(widget, event_ptr, function_index, widget_deleted);
		return TRUE;
	}
	if (strcmp(fn_name, "controls screen defaults") == 0)
	{
		forge_controls_ui_ensure_loaded();
		if (group == 4 || group == 5)
		{
			int32_t saved_vk[FORGE_BIND_COUNT];
			int32_t saved_gp[FORGE_BIND_COUNT];
			float saved_ls = g_forge.controller_look_sens;
			float saved_fs = g_forge.controller_fly_speed;
			for (i = 0; i < FORGE_BIND_COUNT; i++)
			{
				saved_vk[i] = g_forge.bind_vk[i];
				saved_gp[i] = g_forge.bind_gp[i];
			}
			forge_controls_init_defaults();
			for (i = 0; i < FORGE_BIND_COUNT; i++)
			{
				s_forge_controls_ui.pending_vk[i] = g_forge.bind_vk[i];
				s_forge_controls_ui.pending_gp[i] = g_forge.bind_gp[i];
				g_forge.bind_vk[i] = saved_vk[i];
				g_forge.bind_gp[i] = saved_gp[i];
			}
			s_forge_controls_ui.pending_look_sens = 1.0f;
			s_forge_controls_ui.pending_fly_speed = 1.0f;
			g_forge.controller_look_sens = saved_ls;
			g_forge.controller_fly_speed = saved_fs;
			s_forge_controls_ui.capturing_action = -1;
			return TRUE;
		}
		return orig_pc_menu_event_function_invoke(widget, event_ptr, function_index, widget_deleted);
	}
	if (strcmp(fn_name, "controls screen change set") == 0)
	{
		forge_controls_ui_ensure_loaded();
		for (i = 0; i < FORGE_BIND_COUNT; i++)
		{
			g_forge.bind_vk[i] = s_forge_controls_ui.pending_vk[i];
			g_forge.bind_gp[i] = s_forge_controls_ui.pending_gp[i];
		}
		g_forge.controller_look_sens = s_forge_controls_ui.pending_look_sens;
		g_forge.controller_fly_speed = s_forge_controls_ui.pending_fly_speed;
		forge_controls_save();
		return orig_pc_menu_event_function_invoke(widget, event_ptr, function_index, widget_deleted);
	}
	if (strcmp(fn_name, "controls binding slot") == 0)
	{
		forge_controls_ui_ensure_loaded();
		if (group == 4)
		{
			s_forge_controls_ui.slot = !s_forge_controls_ui.slot;
			orig_pc_menu_event_function_invoke(widget, event_ptr, function_index, widget_deleted);
			return TRUE;
		}
		if (group == 5)
		{
			struct forge_widget_instance *row = fw_control_row(widget);
			int row_idx = row ? (atoi(row->name + 11) - 1) : -1;
			int dir = 1;
			if ((GetAsyncKeyState(VK_LEFT) & 0x8000) || (GetAsyncKeyState('A') & 0x8000) || forge_gp_button_down(FORGE_GP_DPAD_LEFT))
			{
				dir = -1;
			}
			else if (event_ptr)
			{
				short const *ev_s = (short const *)event_ptr;
				if ((ev_s[0] == 2 && ev_s[2] == 14) || (ev_s[0] == 0 && ev_s[2] < 0))
					dir = -1;
			}
			if (row_idx == 0)
			{
				s_forge_controls_ui.pending_look_sens += (float)dir * 0.25f;
				if (s_forge_controls_ui.pending_look_sens < 0.25f) s_forge_controls_ui.pending_look_sens = 0.25f;
				if (s_forge_controls_ui.pending_look_sens > 2.50f) s_forge_controls_ui.pending_look_sens = 2.50f;
				return TRUE;
			}
			if (row_idx == 1)
			{
				s_forge_controls_ui.pending_fly_speed += (float)dir * 0.25f;
				if (s_forge_controls_ui.pending_fly_speed < 0.25f) s_forge_controls_ui.pending_fly_speed = 0.25f;
				if (s_forge_controls_ui.pending_fly_speed > 2.50f) s_forge_controls_ui.pending_fly_speed = 2.50f;
				return TRUE;
			}
			s_forge_controls_ui.slot = 1;
			return TRUE;
		}
		s_forge_controls_ui.slot = !s_forge_controls_ui.slot;
		return orig_pc_menu_event_function_invoke(widget, event_ptr, function_index, widget_deleted);
	}
	if (strcmp(fn_name, "controls begin binding") == 0)
	{
		forge_controls_ui_ensure_loaded();
		if (group == 4)
		{
			struct forge_widget_instance *row = fw_control_row(widget);
			int row_idx = row ? (atoi(row->name + 11) - 1) : -1;
			if (row_idx >= 0 && row_idx < FORGE_BIND_COUNT)
			{
				s_forge_controls_ui.capturing_action = row_idx;
				s_forge_controls_ui.capturing_slot = s_forge_controls_ui.slot;
				s_forge_controls_ui.capture_start_ms = system_milliseconds();
				forge_controls_snapshot_input();
				return TRUE;
			}
		}
		else if (group == 5)
		{
			struct forge_widget_instance *row = fw_control_row(widget);
			int row_idx = row ? (atoi(row->name + 11) - 1) : -1;
			if (row_idx == 0)
			{
				s_forge_controls_ui.pending_look_sens += 0.25f;
				if (s_forge_controls_ui.pending_look_sens > 2.50f)
					s_forge_controls_ui.pending_look_sens = 0.25f;
				return TRUE;
			}
			if (row_idx == 1)
			{
				s_forge_controls_ui.pending_fly_speed += 0.25f;
				if (s_forge_controls_ui.pending_fly_speed > 2.50f)
					s_forge_controls_ui.pending_fly_speed = 0.25f;
				return TRUE;
			}
		}
		return orig_pc_menu_event_function_invoke(widget, event_ptr, function_index, widget_deleted);
	}
	return FALSE;
}

static void forge_controls_menu_update(struct forge_widget_instance *list)
{
	int group = fw_controls_group(list);
	struct forge_widget_instance *row;
	struct forge_widget_instance *focused_row = fw_control_row(list ? list->focused_child : NULL);
	float lx, ly, rx, ry, lt, rt;

	forge_controls_ui_ensure_loaded();

	if (group < 4)
	{
		s_forge_controls_ui.capturing_action = -1;
		for (row = list ? list->child : NULL; row; row = row->next)
		{
			if (row->name && strncmp(row->name, "op_command_", 11) == 0)
			{
				int idx = atoi(row->name + 11) - 1;
				if (idx >= 0 && idx < 7)
				{
					row->vertical_offset = (short)(103 + idx * 30);
				}
				else if (idx >= 7)
				{
					row->visible = FALSE;
					if (list->focused_child == row)
					{
						list->focused_child = list->child;
						list->parameters.list.selected_index = 0;
					}
				}
			}
		}
		return;
	}

	forge_poll_controller_state(&lx, &ly, &rx, &ry, &lt, &rt);

	/* Poll for new Keyboard/Mouse or Controller button press when capturing */
	if (s_forge_controls_ui.capturing_action >= 0 && s_forge_controls_ui.capturing_action < FORGE_BIND_COUNT)
	{
		unsigned long now_ms = system_milliseconds();
		int act = s_forge_controls_ui.capturing_action;
		int vk, gp;

		if (now_ms - s_forge_controls_ui.capture_start_ms >= 120)
		{
			int esc_down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
			int del_down = ((GetAsyncKeyState(VK_DELETE) & 0x8000) || (GetAsyncKeyState(VK_BACK) & 0x8000)) != 0;

			if (esc_down && !s_forge_controls_ui.prev_vk_down[VK_ESCAPE])
			{
				s_forge_controls_ui.capturing_action = -1;
			}
			else if (del_down && (!s_forge_controls_ui.prev_vk_down[VK_DELETE] && !s_forge_controls_ui.prev_vk_down[VK_BACK]))
			{
				if (s_forge_controls_ui.capturing_slot == 0)
					s_forge_controls_ui.pending_vk[act] = 0;
				else
					s_forge_controls_ui.pending_gp[act] = FORGE_GP_NONE;
				s_forge_controls_ui.capturing_action = -1;
			}
			else
			{
				/* Check physical gamepad buttons first -> binds Controller column (slot 1) */
				for (gp = 1; gp < FORGE_GP_COUNT; gp++)
				{
					int down = forge_gp_button_down(gp);
					if (down && !s_forge_controls_ui.prev_gp_down[gp])
					{
						int other;
						for (other = 0; other < FORGE_BIND_COUNT; other++)
						{
							if (s_forge_controls_ui.pending_gp[other] == gp)
								s_forge_controls_ui.pending_gp[other] = FORGE_GP_NONE;
						}
						s_forge_controls_ui.pending_gp[act] = gp;
						s_forge_controls_ui.capturing_action = -1;
						break;
					}
				}

				/* Check keyboard & mouse buttons -> binds Key/Mouse column (slot 0) */
				if (s_forge_controls_ui.capturing_action >= 0)
				{
					for (vk = 1; vk < 254; vk++)
					{
						int down;
						if (vk == VK_ESCAPE || vk == VK_DELETE || vk == VK_BACK)
							continue;
						if (vk == VK_SHIFT || vk == VK_CONTROL || vk == 0x12 /* VK_MENU */)
							continue;
						if ((vk == VK_LBUTTON || vk == VK_RETURN) && (now_ms - s_forge_controls_ui.capture_start_ms < 260))
							continue;
						down = (GetAsyncKeyState(vk) & 0x8000) != 0;
						if (down && !s_forge_controls_ui.prev_vk_down[vk])
						{
							char const *vname = forge_vk_display_name(vk);
							if (vname && strcmp(vname, "-") != 0)
							{
								int other;
								for (other = 0; other < FORGE_BIND_COUNT; other++)
								{
									if (s_forge_controls_ui.pending_vk[other] == vk)
										s_forge_controls_ui.pending_vk[other] = 0;
								}
								s_forge_controls_ui.pending_vk[act] = vk;
								s_forge_controls_ui.capturing_action = -1;
								break;
							}
						}
					}
				}
			}
		}

		for (vk = 1; vk < 255; vk++)
			s_forge_controls_ui.prev_vk_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
		for (gp = 1; gp < FORGE_GP_COUNT; gp++)
			s_forge_controls_ui.prev_gp_down[gp] = forge_gp_button_down(gp);
	}

	/* Populate rows for group == 4 (FORGE) or group == 5 (FORGE PAD) */
	for (row = list ? list->child : NULL; row; row = row->next)
	{
		int idx;
		struct forge_widget_instance *lbl_w;
		struct forge_widget_instance *bind0_w;
		struct forge_widget_instance *bind1_w;
		char col0[64], col1[64];

		if (!row->name || strncmp(row->name, "op_command_", 11) != 0)
			continue;
		idx = atoi(row->name + 11) - 1;
		lbl_w = fw_named(row, "command_label", 0);
		bind0_w = fw_named(row, "command_binding", 0);
		bind1_w = fw_named(row, "command_binding", 1);

		if (group == 4)
		{
			if (idx < 0 || idx >= FORGE_BIND_COUNT)
			{
				row->visible = FALSE;
				continue;
			}
			row->visible = TRUE;
			row->vertical_offset = (short)(100 + idx * 26);
			fw_text_set_ascii(lbl_w, s_forge_bind_labels[idx]);

			if (s_forge_controls_ui.capturing_action == idx && s_forge_controls_ui.capturing_slot == 0)
				snprintf(col0, sizeof(col0), "PRESS KEY/PAD");
			else if (row == focused_row && s_forge_controls_ui.slot == 0)
				snprintf(col0, sizeof(col0), "> %s <", forge_vk_display_name(s_forge_controls_ui.pending_vk[idx]));
			else
				snprintf(col0, sizeof(col0), "%s", forge_vk_display_name(s_forge_controls_ui.pending_vk[idx]));

			if (s_forge_controls_ui.capturing_action == idx && s_forge_controls_ui.capturing_slot == 1)
				snprintf(col1, sizeof(col1), "PRESS KEY/PAD");
			else if (row == focused_row && s_forge_controls_ui.slot == 1)
				snprintf(col1, sizeof(col1), "> %s <", forge_gp_display_name(s_forge_controls_ui.pending_gp[idx]));
			else
				snprintf(col1, sizeof(col1), "%s", forge_gp_display_name(s_forge_controls_ui.pending_gp[idx]));

			fw_text_set_ascii(bind0_w, col0);
			fw_text_set_ascii(bind1_w, col1);
		}
		else if (group == 5)
		{
			if (idx == 0 || idx == 1)
			{
				int pct = (int)((idx == 0 ? s_forge_controls_ui.pending_look_sens : s_forge_controls_ui.pending_fly_speed) * 100.0f + 0.5f);
				row->visible = TRUE;
				row->vertical_offset = (short)(103 + idx * 30);
				fw_text_set_ascii(lbl_w, (idx == 0) ? "PAD LOOK SENS" : "PAD FLY SPEED");
				if (row == focused_row)
					snprintf(col0, sizeof(col0), "< %d%% >", pct);
				else
					snprintf(col0, sizeof(col0), "%d%%", pct);
				snprintf(col1, sizeof(col1), "LEFT / RIGHT");
				fw_text_set_ascii(bind0_w, col0);
				fw_text_set_ascii(bind1_w, col1);
			}
			else
			{
				row->visible = FALSE;
				if (list->focused_child == row)
				{
					list->focused_child = list->child;
					list->parameters.list.selected_index = 0;
				}
			}
		}
	}

	if (list && list->parameters.list.extended_description)
	{
		list->parameters.list.extended_description->parameters.text_box.string_list_index =
			(s_forge_controls_ui.capturing_action >= 0) ? 2 : (focused_row ? 1 : 0);
	}
}

void pc_menu_game_data_function_invoke(
	void *widget_ptr,
	long function)
{
	struct forge_widget_instance *widget = (struct forge_widget_instance *)widget_ptr;
	char const *fn_name = pc_menu_game_data_input_name(function);
	char const *w_name = (widget && widget->name) ? widget->name : "";
	char const *p_name = (widget && widget->parent && widget->parent->name) ? widget->parent->name : "";

	if (strncmp(w_name, "forge_", 6) == 0 || strncmp(p_name, "forge_", 6) == 0)
	{
		g_forge_session_enabled = 1;
	}

	if (fn_name && strcmp(fn_name, "load game list update") == 0 &&
		strcmp(w_name, "forge_edit_maps_list") == 0)
	{
		update_forge_edit_maps_widget(widget);
		return;
	}
	if (fn_name && strcmp(fn_name, "port mods update") == 0 &&
		strcmp(w_name, "forge_inject_list") == 0)
	{
		update_forge_inject_widget(widget);
		return;
	}
	if (fn_name && strcmp(fn_name, "controls update menu") == 0)
	{
		int group = fw_controls_group(widget);
		if (group >= 4)
		{
			forge_controls_menu_update(widget);
			return;
		}
		orig_pc_menu_game_data_function_invoke(widget_ptr, function);
		forge_controls_menu_update(widget);
		return;
	}

	/* Point 5: Cycle Forge map chooser across 3 tabs:
	 *   1 = MULTIPLAYER
	 *   3 = CUSTOM MULTIPLAYER (non-Forge custom maps only)
	 *   4 = FORGE MAPS (Forge-made/edited maps only)
	 */
	if (fn_name &&
		(strcmp(fn_name, "solo map list update") == 0 || strcmp(fn_name, "mp map list update") == 0))
	{
		struct forge_widget_instance *spinner = fw_named(widget, "list_item_0_map_kind_spinner", 0);
		short kind = spinner ? spinner->parameters.list.selected_index : 1;

		if (spinner)
		{
			if (s_forge_last_map_kind < 0)
			{
				kind = 1;
			}
			else if (kind != s_forge_last_map_kind)
			{
				if (s_forge_last_map_kind == 1)
					kind = (kind == 0) ? 4 : 3;
				else if (s_forge_last_map_kind == 3)
					kind = (kind == 2) ? 1 : 4;
				else if (s_forge_last_map_kind == 4)
					kind = (kind == 3) ? 3 : 1;
				else
					kind = 1;
			}
			else if (kind != 1 && kind != 3 && kind != 4)
			{
				kind = 1;
			}

			/* Whenever switching tabs, pulse menu_functions' internal kind for one call so s_levels resets first=chosen=0 */
			if (kind != s_forge_last_map_kind)
			{
				s_forge_active_map_kind = 1;
				spinner->parameters.list.selected_index = (kind == 1) ? 0 : 1;
				orig_pc_menu_game_data_function_invoke(widget_ptr, function);
				s_forge_last_map_kind = kind;
			}

			s_forge_active_map_kind = kind;
			spinner->parameters.list.selected_index = (kind == 4) ? 3 : kind;
			orig_pc_menu_game_data_function_invoke(widget_ptr, function);
			spinner->parameters.list.selected_index = kind;
			return;
		}
	}

	orig_pc_menu_game_data_function_invoke(widget_ptr, function);

	if (fn_name && strcmp(fn_name, "port gamepad layout preview") == 0 && widget && widget->parent && widget->parent->parent)
	{
		struct forge_widget_instance *sel_list = fw_named(widget->parent->parent, "profile_edit_select_list", 0);
		if (sel_list && sel_list->focused_child && sel_list->focused_child->name)
		{
			char const *fc_name = sel_list->focused_child->name;
			if (strcmp(fc_name, "clear_custom_cache_profile_item") == 0 ||
				strcmp(fc_name, "reset_tag_cache_profile_item") == 0)
			{
				widget->animation.current_frame_index = 8;
			}
		}
	}

	/* Point 7: Hide gametype rows on Server Setup screen when hosting a Multiplayer Forge session */
	if (g_forge_session_enabled && fn_name && strcmp(fn_name, "server settings update") == 0)
	{
		static char const *const gt_rows[] = {
			"op_game_type", "op_player_options", "op_item_options",
			"op_vehicle_options", "op_indicator_options", "op_team_options"
		};
		int r;
		for (r = 0; r < 6; r++)
		{
			struct forge_widget_instance *row_w = fw_named(widget, gt_rows[r], 0);
			if (row_w)
				row_w->visible = FALSE;
		}
	}
}

/* ---------- Frame Update, HUD, Camera, 343 Guilty Spark Model & Console Hooks */

int forge_is_main_menu_active(void)
{
	struct scenario *scen = global_scenario_get();
	char const *cur_map = main_get_map_name();
	if (!scen || scen->type == 2 /* _scenario_type_main_menu */)
		return 1;
	if (cur_map && (_stricmp(cur_map, "levels\\ui\\ui") == 0 || _stricmp(cur_map, "ui") == 0))
		return 1;
	return 0;
}

void network_chat_update(void)
{
	unsigned long now = system_milliseconds();
	float dt = 0.016f;
	char const *cur_map = main_get_map_name();

	orig_network_chat_update();

	/* When on the UI main menu (levels\ui\ui), main_load_ui_scenario leaves main_get_map_name()
	 * set to the previous playable map while global_scenario_get()->type == _scenario_type_main_menu (2).
	 * Reset Forge monitor/modal state and never run Forge camera/update hooks on the UI background scene! */
	if (forge_is_main_menu_active())
	{
		g_last_forge_map_name[0] = 0;
		g_forge.monitor_cam_initialized = 0;
		g_forge.active_modal = _forge_modal_none;
		g_forge.mode = _forge_mode_play;
		g_forge.grabbed_live_index = -1;
		g_forge.grabbed_object_index = NONE;
		g_forge.hovered_object_index = NONE;
		g_forge.hovered_live_index = -1;
		return;
	}

	/* Reset per-map monitor state when switching maps */
	if (cur_map && strcmp(cur_map, g_last_forge_map_name) != 0)
	{
		snprintf(g_last_forge_map_name, sizeof(g_last_forge_map_name), "%s", cur_map);
		g_forge.monitor_cam_initialized = 0;
		g_forge.active_modal = _forge_modal_none;
		g_forge.grabbed_live_index = -1;
		g_forge.grabbed_object_index = NONE;
		g_forge.hovered_object_index = NONE;
		g_forge.hovered_live_index = -1;
		g_forge.hovered_object_label[0] = 0;
		g_forge.live_instance_count = 0;
		g_forge.has_saved_map_once = 0;
		g_forge.save_map_name[0] = 0;
		g_forge.save_map_name_len = 0;
		memset(&g_forge.edit_state, 0, sizeof(g_forge.edit_state));
	}

	if (g_last_forge_frame_ms != 0)
	{
		unsigned long diff = now - g_last_forge_frame_ms;
		if (diff > 0 && diff < 250)
			dt = (float)diff / 1000.0f;
	}
	g_last_forge_frame_ms = now;

	forge_3d_update(dt);
}

void network_chat_draw(short local_player_index)
{
	long font_index;
	struct font_header *font;
	short line_height = 13;
	rectangle2d window;

	orig_network_chat_draw(local_player_index);

	/* Never draw Forge HUD overlay on the main menu (levels\ui\ui) or outside a Forge session */
	if (!g_forge_session_enabled || forge_is_main_menu_active())
		return;

	font_index = hud_get_font_index();
	if (font_index != NONE)
	{
		font = font_definition_get(font_index);
		if (font)
			line_height = (short)(font->ascending_height + font->descending_height + font->leading_height);
	}
	if (line_height <= 0)
		line_height = 13;

	window = render.camera.window_bounds;
	offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);

	forge_3d_draw_hud(
		local_player_index,
		window.x0,
		window.y0,
		window.x1,
		window.y1,
		font_index,
		line_height);
}

boolean network_chat_typing(void)
{
	if (orig_network_chat_typing())
		return TRUE;
	/* Only block native keyboard/Escape input when a Forge modal is open on screen! */
	if (g_forge_session_enabled && g_forge.initialized &&
		!forge_is_main_menu_active() &&
		g_forge.active_modal != _forge_modal_none)
	{
		return TRUE;
	}
	return FALSE;
}

void network_chat_handle_up(long machine_index, void const *entries, boolean from_stream)
{
	if (entries && from_stream)
	{
		char const *text = (char const *)entries + 4;
		if (forge_network_intercept_chat_packet(machine_index, text, 1))
			return;
	}
	orig_network_chat_handle_up(machine_index, entries, from_stream);
}

void director_update(real time_delta_sec)
{
	orig_director_update(time_delta_sec);
	if (g_forge_session_enabled && g_forge.initialized && !forge_is_main_menu_active())
	{
		forge_3d_apply_monitor_camera(0);
	}
}

short director_get_perspective(short local_player_index)
{
	if (g_forge_session_enabled && g_forge.initialized && !forge_is_main_menu_active())
	{
		if (g_forge.mode == _forge_mode_3d_monitor && g_forge.third_person_view)
		{
			return _director_perspective_third_person; /* 3rd-person perspective so Monitor is drawn */
		}
	}
	return orig_director_get_perspective(local_player_index);
}

struct observer_result const *render_interpolation_camera(
	short local_player_index,
	struct observer_result const *fallback)
{
	struct observer_result const *base = orig_render_interpolation_camera(local_player_index, fallback);
	if (base && g_forge_session_enabled && g_forge.initialized &&
		!forge_is_main_menu_active() && local_player_index == 0 &&
		g_forge.mode == _forge_mode_3d_monitor)
	{
		static struct observer_result s_live_obs;
		s_live_obs = *base;
		if (forge_3d_fill_live_observer_result(&s_live_obs))
			return &s_live_obs;
	}
	return base;
}

void first_person_weapon_draw(void)
{
	if (g_forge_session_enabled && g_forge.initialized &&
		g_forge.mode != _forge_mode_play &&
		!forge_is_main_menu_active())
	{
		/* Hide player's first-person hands & weapon model when in Forge Mode */
		return;
	}
	orig_first_person_weapon_draw();
}

extern void rasterizer_set_stencil_mode(long stencil_mode);
extern void rasterizer_set_frustum_z(real z_near, real z_far);

void game_engine_post_rasterize_objects(void)
{
	orig_game_engine_post_rasterize_objects();

	if (g_forge_session_enabled && g_forge.initialized && !forge_is_main_menu_active())
	{
		/* Render 3D Translucent Objective/Spawn/Teleporter Markers and 3D Spawn Menu Preview
		 * AFTER sky, opaque objects, BSP lightmaps, BSP diffuse textures, and fog have been drawn!
		 * Reset stencil mode to NONE and restore camera frustum constants so world markers never get rejected! */
		rasterizer_set_stencil_mode(0);
		rasterizer_set_frustum_z(0.0f, 0.0f);
		rasterizer_models_begin(FALSE);
		forge_render_world_3d_overlays(NULL);
		rasterizer_models_end();
	}
}

void hud_render_weapon_interface(struct player_datum *player)
{
	if (g_forge_session_enabled && g_forge.initialized &&
		g_forge.mode != _forge_mode_play &&
		!forge_is_main_menu_active())
	{
		/* Hide weapon HUD, weapon reticle, and 'No Weapon' prompt in Forge mode (unarmed reticle drawn in forge_3d_draw_hud) */
		return;
	}
	orig_hud_render_weapon_interface(player);
}

void render_model(
	long model_index,
	real level_of_detail_pixels,
	struct real_matrix4x3 const *node_matrices,
	char const *region_permutation_indices,
	union real_rgb_color const *change_colors,
	real const *function_values,
	struct render_lighting const *lighting,
	union real_point3d const *centroid,
	real radius,
	struct render_model_effect const *model_effect,
	long unique_identifier,
	short forced_shader_permutation_index,
	unsigned long flags)
{
	if (g_forge_session_enabled && g_forge.initialized && !forge_is_main_menu_active())
	{
		if (unique_identifier != NONE)
		{
			/* Point 10: Hide floating gun or child items attached to player bipeds in Forge Mode (both 1st and 3rd person) */
			if (g_forge.mode == _forge_mode_3d_monitor)
			{
				struct object_datum *obj = (struct object_datum *)object_try_and_get_and_verify_type(unique_identifier, (unsigned long)-1);
				if (obj && obj->object.parent_object_index != NONE)
				{
					struct unit_datum *parent_u = (struct unit_datum *)object_try_and_get_and_verify_type(
						obj->object.parent_object_index,
						(1 << _object_type_biped));
					if (parent_u && parent_u->unit.player_index != NONE)
						return;
				}
				if (!g_forge.third_person_view && player_control_get_unit_index(0) == unique_identifier)
					return;
			}

			/* Render dynamic Cached Tag objects and Monitor player units */
			if (forge_monitor_render_player_unit(unique_identifier, lighting, flags))
				return;

			/* For the currently held Forge object, bypass 30 Hz render interpolation and use live 60+ Hz node matrices */
			if (unique_identifier == g_forge.grabbed_object_index && g_forge.grabbed_live_index >= 0)
			{
				struct object_datum *grab_obj = (struct object_datum *)object_try_and_get_and_verify_type(unique_identifier, (unsigned long)-1);
				if (grab_obj && grab_obj->object.node_matrices.size > 0)
				{
					struct real_matrix4x3 const *live_mats =
						(struct real_matrix4x3 const *)object_header_block_get(unique_identifier, &grab_obj->object.node_matrices);
					if (live_mats)
						node_matrices = live_mats;
				}
			}
		}
	}

	orig_render_model(
		model_index,
		level_of_detail_pixels,
		node_matrices,
		region_permutation_indices,
		change_colors,
		function_values,
		lighting,
		centroid,
		radius,
		model_effect,
		unique_identifier,
		forced_shader_permutation_index,
		flags);
}

boolean hs_compile_and_evaluate(char const *source)
{
	if (source)
	{
		char const *s = source;
		while (*s == ' ' || *s == '\t')
			s++;
		if (_stricmp(s, "third_person") == 0 ||
			_stricmp(s, "forge_third_person") == 0 ||
			_stricmp(s, "3rd_person") == 0)
		{
			g_forge.third_person_view = !g_forge.third_person_view;
			console_printf(
				FALSE,
				"Forge 3rd-Person View: %s",
				g_forge.third_person_view ? "ON" : "OFF");
			forge_set_status(
				"3rd-Person Forge View: %s",
				g_forge.third_person_view ? "ON" : "OFF");
			return TRUE;
		}
		if (_stricmp(s, "forge") == 0 || _stricmp(s, "forge_mode") == 0)
		{
			g_forge_session_enabled = 1;
			g_forge.mode = (g_forge.mode == _forge_mode_play) ? _forge_mode_3d_monitor : _forge_mode_play;
			console_printf(
				FALSE,
				"Mode: %s",
				g_forge.mode == _forge_mode_3d_monitor ? "Forge Mode" : "Spartan Play");
			return TRUE;
		}
	}
	return orig_hs_compile_and_evaluate(source);
}

/* ---------- Point 1: Singleplayer Maps Load Straight In (No Cutscenes, No Script Trigger Points) */

void hs_runtime_update(void)
{
	if (g_forge_session_enabled && !forge_is_main_menu_active())
	{
		/* Suppress scenario startup/continuous scripts in Forge mode so SP maps load straight in with no cutscenes */
		return;
	}
	orig_hs_runtime_update();
}

void cinematic_start(void)
{
	if (g_forge_session_enabled && !forge_is_main_menu_active())
	{
		return;
	}
	orig_cinematic_start();
}

boolean scenario_trigger_volume_test_object(short trigger_volume_index, long object_index)
{
	if (g_forge_session_enabled && !forge_is_main_menu_active())
	{
		/* Suppress all script trigger volumes and BSP switch trigger volumes in Forge mode */
		return FALSE;
	}
	return orig_scenario_trigger_volume_test_object(trigger_volume_index, object_index);
}

/* ---------- Point 5: Bypass Multiplayer Gametype Vehicle/Weapon Remapping in Forge Mode */

extern boolean custom_edition_cache_tags_loaded(void);

long game_engine_remap_object_definition(long definition_index)
{
	if (g_forge_session_enabled || g_forge.has_saved_map_once || custom_edition_cache_tags_loaded())
	{
		/* Never remap or reject Scorpion, Banshee, Rocket Warthog, Shade Turret, or custom Forge vehicles! */
		return definition_index;
	}
	return orig_game_engine_remap_object_definition(definition_index);
}

/* ---------- Point 2 & 6: In-Game Escape Pause Menu "SAVE FORGE MAP" Patch & Map Unload Cleanup */

struct forge_pause_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

static void *s_pause_saved_instances = NULL;
static long s_pause_saved_count = 0;
static void *s_pause_alloc_instances = NULL;

void menu_tags_unloaded(void)
{
	forge_runtime_on_map_unload();
	s_save_forge_btn_tag = NONE;
	g_last_forge_map_name[0] = 0;
	g_forge.monitor_cam_initialized = 0;
	g_forge.live_instance_count = 0;
	g_forge.grabbed_live_index = -1;
	g_forge.grabbed_object_index = NONE;
	g_forge.hovered_live_index = -1;
	g_forge.hovered_object_index = NONE;
	if (s_pause_saved_instances)
	{
		cache_files_set_tag_instances(s_pause_saved_instances, s_pause_saved_count);
		s_pause_saved_instances = NULL;
		s_pause_saved_count = 0;
	}
	if (s_pause_alloc_instances)
	{
		free(s_pause_alloc_instances);
		s_pause_alloc_instances = NULL;
	}
	orig_menu_tags_unloaded();
}

void menu_tags_loaded(char const *map_name)
{
	orig_menu_tags_loaded(map_name);

	/* Ensure local tag cache and .ocetags packages are extracted and loaded on startup */
	forge_runtime_init();

	s_save_forge_btn_tag = NONE;
	g_last_forge_map_name[0] = 0;
	g_forge.monitor_cam_initialized = 0;
	g_forge.live_instance_count = 0;
	g_forge.grabbed_live_index = -1;
	g_forge.grabbed_object_index = NONE;
	g_forge.hovered_live_index = -1;
	g_forge.hovered_object_index = NONE;

	if (map_name && (_stricmp(map_name, "ui") == 0 || _stricmp(map_name, "levels\\ui\\ui") == 0))
	{
		g_forge_session_enabled = 0;
		g_forge.mode = _forge_mode_play;
		g_forge.active_modal = _forge_modal_none;
	}

	/* Patch "SAVE FORGE MAP" button into the in-game Escape Pause Menu when in a Forge session */
	if (g_forge_session_enabled && map_name && strcmp(map_name, "ui") != 0)
	{
		long settings_btn_tag = tag_loaded('DeLa', "pc\\pause\\settings_button");
		long existing_count = 0;
		struct forge_pause_tag_instance *instances =
			(struct forge_pause_tag_instance *)cache_files_tag_instances(&existing_count);

		if (settings_btn_tag != NONE && instances && existing_count > 0 && existing_count < 65000)
		{
			uint8_t const *settings_def = (uint8_t const *)tag_get('DeLa', settings_btn_tag);
			struct forge_pause_tag_instance *grown =
				(struct forge_pause_tag_instance *)malloc((size_t)(existing_count + 2) * sizeof(*grown));
			uint8_t *btn_def = (uint8_t *)malloc(0x3EC);
			struct tag_block *ustr_def = (struct tag_block *)calloc(1, sizeof(struct tag_block));
			struct tag_data *ustr_entry = (struct tag_data *)calloc(1, sizeof(struct tag_data));
			wchar_t *wtext = (wchar_t *)calloc(32, sizeof(wchar_t));

			if (grown && btn_def && ustr_def && ustr_entry && wtext && settings_def)
			{
				static char const *const label = "SAVE MAP..";
				long max_salt = 0xE174;
				long i, ustr_tag, btn_tag;
				int32_t h_count;
				uint8_t const *h_src;

				for (i = 0; label[i]; i++)
					wtext[i] = (wchar_t)(unsigned char)label[i];
				wtext[i] = 0;

				ustr_entry->size = (long)((i + 1) * sizeof(wchar_t));
				ustr_entry->address = wtext;
				ustr_def->count = 1;
				ustr_def->address = ustr_entry;

				memcpy(grown, instances, (size_t)existing_count * sizeof(*grown));
				for (i = 0; i < existing_count; i++)
				{
					if (instances[i].tag_index != NONE)
					{
						long s = (long)(((unsigned long)instances[i].tag_index >> 16) & 0x7FFFu);
						if (s > max_salt)
							max_salt = s;
					}
				}

				ustr_tag = ((max_salt + 1) << 16) | (existing_count & 0xFFFF);
				btn_tag = ((max_salt + 2) << 16) | ((existing_count + 1) & 0xFFFF);
				s_save_forge_btn_tag = btn_tag;

				memset(&grown[existing_count], 0, sizeof(grown[0]));
				grown[existing_count].group_tag = 'ustr';
				grown[existing_count].parent_group_tags[0] = NONE;
				grown[existing_count].parent_group_tags[1] = NONE;
				grown[existing_count].tag_index = ustr_tag;
				grown[existing_count].name = "pc\\pause\\save_forge_button text";
				grown[existing_count].base_address = ustr_def;

				memcpy(btn_def, settings_def, 0x3EC);
				snprintf((char *)(btn_def + 0x04), 32, "save_forge_button");
				/* Set text_label_string_list at offset 0xEC to our ustr_tag */
				*(int32_t *)(btn_def + 0xEC + 0x00) = 'ustr';
				*(char const **)(btn_def + 0xEC + 0x04) = "pc\\pause\\save_forge_button text";
				*(int32_t *)(btn_def + 0xEC + 0x08) = 0;
				*(int32_t *)(btn_def + 0xEC + 0x0C) = ustr_tag;

				/* Clone event_handlers at offset 0x54 -> run function 0x7777 & close all pause widgets */
				h_count = *(int32_t const *)(settings_def + 0x54);
				h_src = *(uint8_t const *const *)(settings_def + 0x58);
				if (h_count > 0 && h_count <= 16 && h_src)
				{
					uint8_t *h_dst = (uint8_t *)malloc((size_t)h_count * 0x48u);
					if (h_dst)
					{
						int h;
						memcpy(h_dst, h_src, (size_t)h_count * 0x48u);
						for (h = 0; h < h_count; h++)
						{
							uint8_t *hp = h_dst + (size_t)h * 0x48u;
							/* _event_handler_run_function_bit (bit 7) | _event_handler_close_all_widgets_bit (bit 2) */
							*(int32_t *)(hp + 0x00) = (1 << 7) | (1 << 2);
							*(int16_t *)(hp + 0x06) = 0x7777;
							*(int32_t *)(hp + 0x08) = 'DeLa';
							*(void **)(hp + 0x0C) = NULL;
							*(int32_t *)(hp + 0x10) = 0;
							*(int32_t *)(hp + 0x14) = NONE;
						}
						*(uint8_t **)(btn_def + 0x58) = h_dst;
					}
				}

				memset(&grown[existing_count + 1], 0, sizeof(grown[0]));
				grown[existing_count + 1].group_tag = 'DeLa';
				grown[existing_count + 1].parent_group_tags[0] = NONE;
				grown[existing_count + 1].parent_group_tags[1] = NONE;
				grown[existing_count + 1].tag_index = btn_tag;
				grown[existing_count + 1].name = "pc\\pause\\save_forge_button";
				grown[existing_count + 1].base_address = btn_def;

				s_pause_saved_instances = instances;
				s_pause_saved_count = existing_count;
				s_pause_alloc_instances = grown;
				cache_files_set_tag_instances(grown, existing_count + 2);

				/* Insert save_forge_button into the pause menu's column list right before settings_button */
				for (i = 0; i < existing_count; i++)
				{
					uint8_t *w_def;
					int32_t c_count, c, found_at = -1;
					uint8_t *c_arr;

					if (grown[i].group_tag != 'DeLa' || !grown[i].base_address)
						continue;
					w_def = (uint8_t *)grown[i].base_address;
					if (*(int16_t const *)(w_def + 0x00) != 3) /* _widget_type_column_list */
						continue;
					c_count = *(int32_t const *)(w_def + 0x3E0);
					c_arr = *(uint8_t *const *)(w_def + 0x3E4);
					if (c_count <= 0 || c_count > 16 || !c_arr)
						continue;
					for (c = 0; c < c_count; c++)
					{
						int32_t child_tag = *(int32_t const *)(c_arr + (size_t)c * 0x50u + 0x0C);
						long abs_idx = child_tag & 0xFFFF;
						if (child_tag == settings_btn_tag ||
							(child_tag != NONE && abs_idx >= 0 && abs_idx < existing_count &&
							 grown[abs_idx].name && strcmp(grown[abs_idx].name, "pc\\pause\\settings_button") == 0))
						{
							found_at = c;
							break;
						}
					}
					if (found_at >= 0)
					{
						uint8_t *new_children = (uint8_t *)malloc((size_t)(c_count + 1) * 0x50u);
						if (new_children)
						{
							int16_t old_y0 = *(int16_t const *)(c_arr + 0 * 0x50u + 0x36);
							int16_t spacing = (c_count + 1 <= 4) ? 29 : 25;
							int16_t base_y = (old_y0 >= 60) ? (int16_t)(old_y0 - 20) : 0;
							long list_tag_idx = grown[i].tag_index;
							long s_idx;

							if (found_at > 0)
								memcpy(new_children, c_arr, (size_t)found_at * 0x50u);
							memcpy(new_children + (size_t)found_at * 0x50u, c_arr + (size_t)found_at * 0x50u, 0x50u);
							*(int32_t *)(new_children + (size_t)found_at * 0x50u + 0x00) = 'DeLa';
							*(char const **)(new_children + (size_t)found_at * 0x50u + 0x04) = "pc\\pause\\save_forge_button";
							*(int32_t *)(new_children + (size_t)found_at * 0x50u + 0x08) = 0;
							*(int32_t *)(new_children + (size_t)found_at * 0x50u + 0x0C) = btn_tag;

							for (c = found_at; c < c_count; c++)
							{
								uint8_t *dst_c = new_children + (size_t)(c + 1) * 0x50u;
								memcpy(dst_c, c_arr + (size_t)c * 0x50u, 0x50u);
							}
							for (c = 0; c < c_count + 1; c++)
							{
								uint8_t *dst_c = new_children + (size_t)c * 0x50u;
								*(int16_t *)(dst_c + 0x36) = (int16_t)(base_y + c * spacing);
							}
							*(int32_t *)(w_def + 0x3E0) = c_count + 1;
							*(uint8_t **)(w_def + 0x3E4) = new_children;
							*(int16_t *)(w_def + 0x28) = (int16_t)(*(int16_t const *)(w_def + 0x28) + spacing); /* bounds.y1 */

							/* Shift the pause list widget itself upward inside its parent screen widget so all 4 options sit centered inside the box */
							if (old_y0 < 60)
							{
								int16_t lift = (old_y0 > 10) ? 14 : 22;
								for (s_idx = 0; s_idx < existing_count; s_idx++)
								{
									uint8_t *sw_def;
									int32_t sc_count, sc;
									uint8_t *sc_arr;
									if (grown[s_idx].group_tag != 'DeLa' || !grown[s_idx].base_address)
										continue;
									sw_def = (uint8_t *)grown[s_idx].base_address;
									sc_count = *(int32_t const *)(sw_def + 0x3E0);
									sc_arr = *(uint8_t *const *)(sw_def + 0x3E4);
									if (sc_count <= 0 || sc_count > 16 || !sc_arr)
										continue;
									for (sc = 0; sc < sc_count; sc++)
									{
										if (*(int32_t const *)(sc_arr + (size_t)sc * 0x50u + 0x0C) == list_tag_idx)
										{
											*(int16_t *)(sc_arr + (size_t)sc * 0x50u + 0x36) =
												(int16_t)(*(int16_t const *)(sc_arr + (size_t)sc * 0x50u + 0x36) - lift);
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

/* ---------- Point 11: Redirect custom_maps\<name>.(map|bmp|txt) to forge_maps\<name>\<name>.(map|bmp|txt) */

extern void *file_reference_create_from_path(void *reference, char const *full_path, boolean is_directory);
extern FILE *halo_windows_fopen(char const *filename, char const *mode);

static char const *resolve_forge_maps_redirect_path(char const *path, char *out_buf, size_t out_sz)
{
	char const *leaf = NULL;
	char const *dot;
	char sub_dir[64];
	char disk_check[512];
	size_t base_len;
	int has_d = 0;
	FILE *tf;

	if (!path || !path[0])
		return path;

	if (_strnicmp(path, "d:\\custom_maps\\", 15) == 0 || _strnicmp(path, "d:/custom_maps/", 15) == 0)
	{
		has_d = 1;
		leaf = path + 15;
	}
	else if (_strnicmp(path, "custom_maps\\", 12) == 0 || _strnicmp(path, "custom_maps/", 12) == 0)
	{
		has_d = 0;
		leaf = path + 12;
	}
	else
	{
		return path;
	}

	if (strchr(leaf, '\\') != NULL || strchr(leaf, '/') != NULL)
		return path;

	if (_stricmp(leaf, "bitmaps.map") == 0 ||
		_stricmp(leaf, "sounds.map") == 0 ||
		_stricmp(leaf, "loc.map") == 0)
	{
		snprintf(disk_check, sizeof(disk_check), "custom_maps\\%s", leaf);
		tf = fopen(disk_check, "rb");
		if (tf)
		{
			fclose(tf);
			return path;
		}
		snprintf(disk_check, sizeof(disk_check), "OpenCE Forge\\forge_maps\\%s", leaf);
		tf = fopen(disk_check, "rb");
		if (tf)
		{
			fclose(tf);
			snprintf(out_buf, out_sz, "%sOpenCE Forge\\forge_maps\\%s", has_d ? "d:\\" : "", leaf);
			return out_buf;
		}
		return path;
	}

	dot = strrchr(leaf, '.');
	base_len = dot ? (size_t)(dot - leaf) : strlen(leaf);
	if (base_len == 0 || base_len >= sizeof(sub_dir))
		return path;

	memcpy(sub_dir, leaf, base_len);
	sub_dir[base_len] = '\0';

	/* If custom.bmp exists inside forge_maps\<name>\custom.bmp, prefer it as the custom map preview! */
	if (dot && _stricmp(dot, ".bmp") == 0)
	{
		snprintf(disk_check, sizeof(disk_check), "forge_maps\\%s\\custom.bmp", sub_dir);
		tf = fopen(disk_check, "rb");
		if (tf)
		{
			fclose(tf);
			snprintf(out_buf, out_sz, "%sforge_maps\\%s\\custom.bmp", has_d ? "d:\\" : "", sub_dir);
			return out_buf;
		}
	}

	/* If the file exists directly in custom_maps\<leaf> (e.g. custom_maps\<name>.map or custom_maps\<name>.bmp), use it */
	snprintf(disk_check, sizeof(disk_check), "custom_maps\\%s", leaf);
	tf = fopen(disk_check, "rb");
	if (tf)
	{
		fclose(tf);
		return path;
	}

	snprintf(disk_check, sizeof(disk_check), "forge_maps\\%s\\%s", sub_dir, leaf);
	tf = fopen(disk_check, "rb");
	if (tf)
	{
		fclose(tf);
		snprintf(out_buf, out_sz, "%sforge_maps\\%s\\%s", has_d ? "d:\\" : "", sub_dir, leaf);
		return out_buf;
	}
	return path;
}

void *forge_ce_file_reference_create_from_path(void *reference, char const *full_path, boolean is_directory)
{
	char redirected[512];
	char const *actual = resolve_forge_maps_redirect_path(full_path, redirected, sizeof(redirected));
	return file_reference_create_from_path(reference, actual, is_directory);
}

#define FORGE_EMBED_FILE_SENTINEL ((FILE *)(uintptr_t)0x7F046E99u)

static struct
{
	int active;
	uint8_t *data;
	size_t size;
	size_t pos;
} s_embed_vfile;

static FILE *try_open_embedded_map_sidecar(char const *filename)
{
	char const *leaf;
	char const *dot;
	size_t stem_len;
	char stem[128];
	char map_path[512];
	int is_bmp = 0, is_txt = 0;
	FILE *mf;
	uint8_t hdr[0x200];
	uint32_t sig = 0, emb_off = 0, emb_sz = 0;

	if (!filename || !filename[0])
		return NULL;
	leaf = strrchr(filename, '\\');
	if (!leaf)
		leaf = strrchr(filename, '/');
	leaf = leaf ? (leaf + 1) : filename;
	dot = strrchr(leaf, '.');
	if (!dot)
		return NULL;
	if (_stricmp(dot, ".bmp") == 0)
		is_bmp = 1;
	else if (_stricmp(dot, ".txt") == 0)
		is_txt = 1;
	else
		return NULL;

	stem_len = (size_t)(dot - leaf);
	if (stem_len == 0 || stem_len >= sizeof(stem))
		return NULL;
	memcpy(stem, leaf, stem_len);
	stem[stem_len] = 0;

	snprintf(map_path, sizeof(map_path), "custom_maps/%s.map", stem);
	mf = fopen(map_path, "rb");
	if (!mf)
		return NULL;
	if (fread(hdr, 1, sizeof(hdr), mf) != sizeof(hdr))
	{
		fclose(mf);
		return NULL;
	}
	memcpy(&sig, hdr + 0x1FCu, 4);
	if (sig == 0x464C4731u)
	{
		if (is_bmp)
		{
			memcpy(&emb_off, hdr + 0x1ECu, 4);
			memcpy(&emb_sz, hdr + 0x1F0u, 4);
		}
		else if (is_txt)
		{
			memcpy(&emb_off, hdr + 0x1F4u, 4);
			memcpy(&emb_sz, hdr + 0x1F8u, 4);
		}
	}
	if (emb_off >= 0x800u && emb_sz > 0 && emb_sz <= 4u * 1024u * 1024u)
	{
		uint8_t *buf = (uint8_t *)malloc(emb_sz);
		if (buf)
		{
			if (fseek(mf, (long)emb_off, SEEK_SET) == 0 && fread(buf, 1, emb_sz, mf) == emb_sz)
			{
				fclose(mf);
				if (s_embed_vfile.data)
					free(s_embed_vfile.data);
				s_embed_vfile.active = 1;
				s_embed_vfile.data = buf;
				s_embed_vfile.size = emb_sz;
				s_embed_vfile.pos = 0;
				return FORGE_EMBED_FILE_SENTINEL;
			}
			free(buf);
		}
	}
	fclose(mf);
	return NULL;
}

FILE *forge_ce_fopen(char const *filename, char const *mode)
{
	char redirected[512];
	char const *actual = resolve_forge_maps_redirect_path(filename, redirected, sizeof(redirected));
	FILE *fp = halo_windows_fopen(actual, mode);
	if (!fp)
		fp = try_open_embedded_map_sidecar(filename);
	return fp;
}

size_t forge_ce_fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if (stream == FORGE_EMBED_FILE_SENTINEL)
	{
		size_t total = size * nmemb;
		size_t avail;
		if (!s_embed_vfile.active || !s_embed_vfile.data || size == 0 || nmemb == 0)
			return 0;
		avail = (s_embed_vfile.pos < s_embed_vfile.size) ? (s_embed_vfile.size - s_embed_vfile.pos) : 0;
		if (total > avail)
			total = (avail / size) * size;
		if (total > 0)
		{
			memcpy(ptr, s_embed_vfile.data + s_embed_vfile.pos, total);
			s_embed_vfile.pos += total;
		}
		return total / size;
	}
	return fread(ptr, size, nmemb, stream);
}

int forge_ce_fseek(FILE *stream, long offset, int whence)
{
	if (stream == FORGE_EMBED_FILE_SENTINEL)
	{
		long target = 0;
		if (!s_embed_vfile.active)
			return -1;
		if (whence == SEEK_SET)
			target = offset;
		else if (whence == SEEK_CUR)
			target = (long)s_embed_vfile.pos + offset;
		else if (whence == SEEK_END)
			target = (long)s_embed_vfile.size + offset;
		else
			return -1;
		if (target < 0 || (size_t)target > s_embed_vfile.size)
			return -1;
		s_embed_vfile.pos = (size_t)target;
		return 0;
	}
	return fseek(stream, offset, whence);
}

long forge_ce_ftell(FILE *stream)
{
	if (stream == FORGE_EMBED_FILE_SENTINEL)
	{
		if (!s_embed_vfile.active)
			return -1L;
		return (long)s_embed_vfile.pos;
	}
	return ftell(stream);
}

int forge_ce_fclose(FILE *stream)
{
	if (stream == FORGE_EMBED_FILE_SENTINEL)
	{
		if (s_embed_vfile.data)
		{
			free(s_embed_vfile.data);
			s_embed_vfile.data = NULL;
		}
		s_embed_vfile.active = 0;
		s_embed_vfile.size = 0;
		s_embed_vfile.pos = 0;
		return 0;
	}
	return fclose(stream);
}

#define FORGE_CE_FIND_SENTINEL_HANDLE ((HANDLE)(uintptr_t)0x7F046E01u)

static struct
{
	boolean in_use;
	HANDLE primary_handle;
	HANDLE forge_dirs_handle;
} s_forge_ce_find = { FALSE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };

static BOOL forge_ce_advance_forge_dirs(LPWIN32_FIND_DATAA data)
{
	WIN32_FIND_DATAA dir_data;
	BOOL got_entry;

	if (s_forge_ce_find.forge_dirs_handle == INVALID_HANDLE_VALUE)
	{
		s_forge_ce_find.forge_dirs_handle = FindFirstFileA("d:\\forge_maps\\*.*", &dir_data);
		if (s_forge_ce_find.forge_dirs_handle == INVALID_HANDLE_VALUE)
			return FALSE;
		got_entry = TRUE;
	}
	else
	{
		got_entry = FindNextFileA(s_forge_ce_find.forge_dirs_handle, &dir_data);
	}

	while (got_entry)
	{
		if ((dir_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
			dir_data.cFileName[0] != '.' &&
			strlen(dir_data.cFileName) > 0 &&
			strlen(dir_data.cFileName) < 64)
		{
			char map_check[512];
			FILE *tf;
			/* Skip if custom_maps\<name>.map already exists (already enumerated by primary_handle) */
			snprintf(map_check, sizeof(map_check), "custom_maps\\%s.map", dir_data.cFileName);
			tf = fopen(map_check, "rb");
			if (tf)
			{
				fclose(tf);
				got_entry = FindNextFileA(s_forge_ce_find.forge_dirs_handle, &dir_data);
				continue;
			}
			snprintf(map_check, sizeof(map_check), "forge_maps\\%s\\%s.map", dir_data.cFileName, dir_data.cFileName);
			tf = fopen(map_check, "rb");
			if (tf)
			{
				fclose(tf);
				memset(data, 0, sizeof(*data));
				data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
				snprintf(data->cFileName, sizeof(data->cFileName), "%s.map", dir_data.cFileName);
				return TRUE;
			}
		}
		got_entry = FindNextFileA(s_forge_ce_find.forge_dirs_handle, &dir_data);
	}

	CloseHandle(s_forge_ce_find.forge_dirs_handle);
	s_forge_ce_find.forge_dirs_handle = INVALID_HANDLE_VALUE;
	return FALSE;
}

HANDLE WINAPI forge_ce_FindFirstFileA(LPCSTR file_name, LPWIN32_FIND_DATAA data)
{
	if (!s_forge_ce_find.in_use && file_name &&
		(_stricmp(file_name, "d:\\custom_maps\\*.*") == 0 ||
		 _stricmp(file_name, "custom_maps\\*.*") == 0))
	{
		s_forge_ce_find.in_use = TRUE;
		s_forge_ce_find.primary_handle = FindFirstFileA(file_name, data);
		s_forge_ce_find.forge_dirs_handle = INVALID_HANDLE_VALUE;
		if (s_forge_ce_find.primary_handle != INVALID_HANDLE_VALUE)
			return FORGE_CE_FIND_SENTINEL_HANDLE;
		if (forge_ce_advance_forge_dirs(data))
			return FORGE_CE_FIND_SENTINEL_HANDLE;
		s_forge_ce_find.in_use = FALSE;
		return INVALID_HANDLE_VALUE;
	}
	return FindFirstFileA(file_name, data);
}

BOOL WINAPI forge_ce_FindNextFileA(HANDLE find_file, LPWIN32_FIND_DATAA data)
{
	if (find_file == FORGE_CE_FIND_SENTINEL_HANDLE)
	{
		if (s_forge_ce_find.primary_handle != INVALID_HANDLE_VALUE)
		{
			if (FindNextFileA(s_forge_ce_find.primary_handle, data))
				return TRUE;
			CloseHandle(s_forge_ce_find.primary_handle);
			s_forge_ce_find.primary_handle = INVALID_HANDLE_VALUE;
		}
		return forge_ce_advance_forge_dirs(data);
	}
	return FindNextFileA(find_file, data);
}

BOOL WINAPI forge_ce_CloseHandle(HANDLE hObject)
{
	if (hObject == FORGE_CE_FIND_SENTINEL_HANDLE)
	{
		if (s_forge_ce_find.primary_handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(s_forge_ce_find.primary_handle);
			s_forge_ce_find.primary_handle = INVALID_HANDLE_VALUE;
		}
		if (s_forge_ce_find.forge_dirs_handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(s_forge_ce_find.forge_dirs_handle);
			s_forge_ce_find.forge_dirs_handle = INVALID_HANDLE_VALUE;
		}
		s_forge_ce_find.in_use = FALSE;
		return TRUE;
	}
	return CloseHandle(hObject);
}

/* ---------- Disable Menu, Mouse, Keyboard & Controller Interaction When HaloCE Forge.exe Is Unfocused */

struct forge_sdl_ui_pointer
{
	float x, y;
	float click_x, click_y;
	BOOL moved;
	int left_clicks, right_clicks;
	int wheel_steps;
	int downs;
	float down_x, down_y;
	BOOL touch;
};

struct forge_sdl_input_state
{
	unsigned char keys[512];
	unsigned char mouse_buttons[8];
	float mouse_dx, mouse_dy;
	float mouse_wheel;
	BOOL focused;
	BOOL mouse_released;
	BOOL ui_pointer;
	BOOL menus;
};

struct forge_sdl_keystroke
{
	BYTE virtual_key;
	CHAR ascii;
	BYTE flags;
};

extern BOOL orig_platform_ui_pointer_read(struct forge_sdl_ui_pointer *pointer);
extern void orig_platform_input_read(struct forge_sdl_input_state *state, BOOL consume_motion);
extern BOOL orig_platform_next_keystroke(struct forge_sdl_keystroke *keystroke);
extern DWORD WINAPI orig_XInputGetState(HANDLE hDevice, PXINPUT_STATE pState);

#ifdef _WIN32
__declspec(dllimport) void *WINAPI GetForegroundWindow(void);
__declspec(dllimport) DWORD WINAPI GetWindowThreadProcessId(void *hWnd, LPDWORD lpdwProcessId);
__declspec(dllimport) DWORD WINAPI GetCurrentProcessId(void);
#endif

int forge_window_is_focused(void)
{
#ifdef _WIN32
	void *fg = GetForegroundWindow();
	DWORD fg_pid = 0;
	if (!fg)
		return 0;
	GetWindowThreadProcessId(fg, &fg_pid);
	return (fg_pid == GetCurrentProcessId()) ? 1 : 0;
#else
	return 1;
#endif
}

#undef GetAsyncKeyState
__declspec(dllimport) SHORT WINAPI GetAsyncKeyState(int vKey);

short forge_key_state(int vk)
{
	if (!forge_window_is_focused())
		return 0;
	return GetAsyncKeyState(vk);
}

BOOL platform_ui_pointer_read(struct forge_sdl_ui_pointer *pointer)
{
	BOOL active = orig_platform_ui_pointer_read(pointer);
	if (!forge_window_is_focused())
	{
		if (pointer)
		{
			pointer->moved = FALSE;
			pointer->left_clicks = 0;
			pointer->right_clicks = 0;
			pointer->wheel_steps = 0;
			pointer->downs = 0;
		}
		return FALSE;
	}
	return active;
}

void platform_input_read(struct forge_sdl_input_state *state, BOOL consume_motion)
{
	orig_platform_input_read(state, consume_motion);
	if (state && (!forge_window_is_focused() || !state->focused))
	{
		memset(state->keys, 0, sizeof(state->keys));
		memset(state->mouse_buttons, 0, sizeof(state->mouse_buttons));
		state->mouse_dx = 0.0f;
		state->mouse_dy = 0.0f;
		state->mouse_wheel = 0.0f;
		state->focused = FALSE;
	}
}

BOOL platform_next_keystroke(struct forge_sdl_keystroke *keystroke)
{
	if (!forge_window_is_focused())
	{
		struct forge_sdl_keystroke dummy;
		while (orig_platform_next_keystroke(&dummy))
		{
		}
		return FALSE;
	}
	return orig_platform_next_keystroke(keystroke);
}

DWORD WINAPI XInputGetState(HANDLE hDevice, PXINPUT_STATE pState)
{
	DWORD rc = orig_XInputGetState(hDevice, pState);
	if (!forge_window_is_focused() && pState)
	{
		memset(&pState->Gamepad, 0, sizeof(pState->Gamepad));
	}
	return rc;
}




