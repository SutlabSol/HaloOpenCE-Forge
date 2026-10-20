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
extern short ui_widget_port_gametypes(long *indices, short maximum, short *last_used);
extern boolean ui_widget_port_gametype_choose(long profile_index);
extern boolean ui_widget_port_open(void *widget, char const *name, boolean *widget_deleted);
extern boolean ui_widget_port_game_variant_set(struct game_variant *variant, struct game_variant_options const *options);
extern struct game_variant *build_game_variant_slayer(struct game_variant *variant);
extern void game_variant_options_default(struct game_variant const *variant, struct game_variant_options *options);

static short s_forge_last_map_kind = 1; /* 1 = MAP_KIND_MULTIPLAYER, 3 = MAP_KIND_CUSTOM_MULTIPLAYER */

/* Original engine symbols redefined via llvm-objcopy in FORGE CODE/build/wrapped_obj/ */
extern void orig_network_chat_update(void);
extern void orig_network_chat_draw(short local_player_index);
extern boolean orig_network_chat_typing(void);
extern void orig_network_chat_handle_up(long machine_index, void const *entries, boolean from_stream);
extern void orig_director_update(real time_delta_sec);
extern short orig_director_get_perspective(short local_player_index);
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
			marker = *(uint32_t const *)(hdr + 0x1FC);
			if (marker == 0x464C4731u) /* 'FLG1' */
				has_flg1 = 1;
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

static void scan_forge_edit_maps(int wanted_tab)
{
	WIN32_FIND_DATAA fd;
	HANDLE h;

	s_forge_menu.tab = wanted_tab;
	s_forge_menu.count = 0;
	s_forge_menu.first = 0;
	s_forge_menu.chosen = 0;

	/* Point 11: Scan forge_maps/<name>/<name>.map subfolders first! */
	h = FindFirstFileA("forge_maps\\*", &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			char full_path[512];
			char leaf[64];
			char author[64];
			int is_user = 0;

			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				continue;
			if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
				continue;

			snprintf(leaf, sizeof(leaf), "%s", fd.cFileName);
			snprintf(full_path, sizeof(full_path), "forge_maps/%s/%s.map", leaf, leaf);
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
		if (s_forge_menu.count > 0 && s_forge_menu.chosen < s_forge_menu.count)
		{
			char info[128];
			fw_text_set_ascii(fw_named(desc, "replay_level_right_name", 0), s_forge_menu.map_titles[s_forge_menu.chosen]);
			snprintf(info, sizeof(info), "%s\r\nforge_maps\\%s\\%s.map\r\nPress OK to Edit in Forge",
				s_forge_menu.map_authors[s_forge_menu.chosen],
				s_forge_menu.map_leaves[s_forge_menu.chosen],
				s_forge_menu.map_leaves[s_forge_menu.chosen]);
			fw_text_set_ascii(fw_named(desc, "replay_level_right_data", 0), info);
		}
		else
		{
			fw_text_set_ascii(fw_named(desc, "replay_level_right_name", 0),
				kind == 0 ? "USER CREATED" : "DOWNLOADED");
			fw_text_set_ascii(fw_named(desc, "replay_level_right_data", 0),
				"Save a map in Forge using F5\r\nto edit it here.");
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
			char const *slash = strrchr(full, '/');
			if (!slash) slash = strrchr(full, '\\');
			row->visible = TRUE;
			fw_text_set_ascii(fw_named(row, "mod_name", 0), slash ? (slash + 1) : full);
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
			char details[256];
			char const *path = g_forge.donor_map_files[s_forge_menu.inject_chosen];
			/* Point 10: Change 'INJECT CUSTOM TAGS' to 'ADD TAGS TO CACHE' */
			fw_text_set_ascii(fw_named(desc, "mod_details_name", 0), "ADD TAGS TO CACHE");
			snprintf(details, sizeof(details),
				"Source Map:\r\n%s\r\n\r\nPress ENTER or click row to add unique Vehicles, Weapons & Scenery into the Forge cache and record credits in credits.md.",
				path);
			fw_text_set_ascii(fw_named(desc, "mod_details_text", 0), details);
		}
		else
		{
			fw_text_set_ascii(fw_named(desc, "mod_details_name", 0), "NO MAPS FOUND");
			fw_text_set_ascii(fw_named(desc, "mod_details_text", 0),
				"Place Custom Edition .map files in custom_maps/ to add their tags to cache.");
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

	/* Track whether the player is entering standard Campaign/Multiplayer vs Forge from the Main Menu */
	if (strcmp(w_name, "main_menu_item_forge") == 0 ||
		strncmp(w_name, "forge_", 6) == 0 ||
		strncmp(p_name, "forge_", 6) == 0)
	{
		g_forge_session_enabled = 1;
	}
	else if (strcmp(w_name, "main_menu_item_load_camp") == 0 ||
			 strcmp(w_name, "main_menu_item_multiplayer") == 0)
	{
		g_forge_session_enabled = 0;
	}

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

	/* Forge -> Inject Custom Tags screen initialization & row activation */
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
		if (p)
		{
			if (g_forge.donor_map_count > 0 &&
				s_forge_menu.inject_chosen >= 0 &&
				s_forge_menu.inject_chosen < g_forge.donor_map_count)
			{
				if (forge_ui_inject_custom_map(g_forge.donor_map_files[s_forge_menu.inject_chosen]))
				{
					s_forge_menu.injected_flags[s_forge_menu.inject_chosen] = 1;
					ui_play_audio_feedback_sound(2);
				}
			}
			return TRUE;
		}
	}

	/* Point 5: Forge Single Player -> default to Multiplayer maps only (no Singleplayer campaign maps) */
	if (g_forge_session_enabled && fn_name && strcmp(fn_name, "initialize sp level list solo") == 0)
	{
		boolean res;
		struct forge_widget_instance *spinner;
		custom_edition_maps_look_again();
		res = orig_pc_menu_event_function_invoke(widget_ptr, event_ptr, function_index, widget_deleted);
		spinner = fw_named(widget, "list_item_0_map_kind_spinner", 0);
		s_forge_last_map_kind = 1; /* 1 = MAP_KIND_MULTIPLAYER */
		if (spinner)
			spinner->parameters.list.selected_index = 1;
		return res;
	}

	if (g_forge_session_enabled && fn_name && strcmp(fn_name, "solo level set map") == 0)
	{
		boolean res = orig_pc_menu_event_function_invoke(widget_ptr, event_ptr, function_index, widget_deleted);
		if (res)
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
	if (g_forge_session_enabled && fn_name && strcmp(fn_name, "mp level select") == 0)
	{
		boolean res = orig_pc_menu_event_function_invoke(widget_ptr, event_ptr, function_index, widget_deleted);
		if (res)
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

	/* In-Game Escape Pause Menu -> "SAVE FORGE MAP" button handler */
	if (function_index == 0x7777)
	{
		ui_widgets_close_all();
		if (widget_deleted)
			*widget_deleted = TRUE;
		ui_play_audio_feedback_sound(2);
		forge_trigger_save_map_action();
		return TRUE;
	}

	return orig_pc_menu_event_function_invoke(widget_ptr, event_ptr, function_index, widget_deleted);
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

	/* Point 5: Restrict Forge map chooser strictly to Multiplayer (1) and Custom Multiplayer (3) maps */
	if (g_forge_session_enabled && fn_name &&
		(strcmp(fn_name, "solo map list update") == 0 || strcmp(fn_name, "mp map list update") == 0))
	{
		struct forge_widget_instance *spinner = fw_named(widget, "list_item_0_map_kind_spinner", 0);
		if (spinner)
		{
			short kind = spinner->parameters.list.selected_index;
			if (kind != 1 && kind != 3)
			{
				kind = (s_forge_last_map_kind == 1 && kind != 0) ? 3 :
					   (s_forge_last_map_kind == 3 && kind != 0) ? 1 :
					   (s_forge_last_map_kind == 1 ? 3 : 1);
				spinner->parameters.list.selected_index = kind;
			}
			s_forge_last_map_kind = kind;
		}
	}

	orig_pc_menu_game_data_function_invoke(widget_ptr, function);

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

void network_chat_update(void)
{
	unsigned long now = system_milliseconds();
	float dt = 0.016f;
	char const *cur_map = main_get_map_name();

	orig_network_chat_update();

	/* Reset per-map monitor state when switching maps or returning to levels\ui\ui */
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
		if (strcmp(cur_map, "levels\\ui\\ui") == 0)
		{
			g_forge.mode = _forge_mode_play;
		}
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
	if (!g_forge_session_enabled)
		return;
	if (main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") == 0)
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
		main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") != 0 &&
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
	if (g_forge_session_enabled && g_forge.initialized)
	{
		forge_3d_apply_monitor_camera(0);
	}
}

short director_get_perspective(short local_player_index)
{
	if (g_forge_session_enabled && g_forge.initialized &&
		main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") != 0)
	{
		if (g_forge.mode == _forge_mode_3d_monitor && g_forge.third_person_view)
		{
			return _director_perspective_third_person; /* 3rd-person perspective so Monitor is drawn */
		}
	}
	return orig_director_get_perspective(local_player_index);
}

void first_person_weapon_draw(void)
{
	if (g_forge_session_enabled && g_forge.initialized &&
		g_forge.mode != _forge_mode_play &&
		main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") != 0)
	{
		/* Point 12: Render the 3D Spawn Palette Preview at the first-person weapon stage
		 * (after sky, BSP, and opaque objects have been drawn) so it renders with proper depth and lighting! */
		forge_render_world_3d_overlays(NULL);
		/* Hide player's first-person hands & weapon model when in Forge Mode */
		return;
	}
	orig_first_person_weapon_draw();
}

void hud_render_weapon_interface(struct player_datum *player)
{
	if (g_forge_session_enabled && g_forge.initialized &&
		g_forge.mode != _forge_mode_play &&
		main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") != 0)
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
	if (g_forge_session_enabled && g_forge.initialized &&
		main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") != 0)
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
	if (g_forge_session_enabled &&
		main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") != 0)
	{
		/* Suppress scenario startup/continuous scripts in Forge mode so SP maps load straight in with no cutscenes */
		return;
	}
	orig_hs_runtime_update();
}

void cinematic_start(void)
{
	if (g_forge_session_enabled &&
		main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") != 0)
	{
		return;
	}
	orig_cinematic_start();
}

boolean scenario_trigger_volume_test_object(short trigger_volume_index, long object_index)
{
	if (g_forge_session_enabled &&
		main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") != 0)
	{
		/* Suppress all script trigger volumes and BSP switch trigger volumes in Forge mode */
		return FALSE;
	}
	return orig_scenario_trigger_volume_test_object(trigger_volume_index, object_index);
}

/* ---------- Point 5: Bypass Multiplayer Gametype Vehicle/Weapon Remapping in Forge Mode */

long game_engine_remap_object_definition(long definition_index)
{
	if (g_forge_session_enabled)
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
				static char const *const label = "SAVE FORGE MAP";
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
					long s = (long)((unsigned long)instances[i].tag_index >> 16);
					if (s > max_salt)
						max_salt = s;
				}

				ustr_tag = ((max_salt + 1) << 16) | (existing_count & 0xFFFF);
				btn_tag = ((max_salt + 2) << 16) | ((existing_count + 1) & 0xFFFF);

				memset(&grown[existing_count], 0, sizeof(grown[0]));
				grown[existing_count].group_tag = 'ustr';
				grown[existing_count].parent_group_tags[0] = NONE;
				grown[existing_count].parent_group_tags[1] = NONE;
				grown[existing_count].tag_index = ustr_tag;
				grown[existing_count].name = "pc\\pause\\save_forge_button text";
				grown[existing_count].base_address = ustr_def;

				memcpy(btn_def, settings_def, 0x3EC);
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
						if (*(int32_t const *)(c_arr + (size_t)c * 0x50u + 0x0C) == settings_btn_tag)
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
							int16_t spacing = 26;
							if (c_count >= 2)
							{
								int16_t d = *(int16_t const *)(c_arr + 1 * 0x50u + 0x36) -
											*(int16_t const *)(c_arr + 0 * 0x50u + 0x36);
								if (d > 10 && d < 60)
									spacing = d;
							}
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
								*(int16_t *)(dst_c + 0x36) = (int16_t)(*(int16_t const *)(dst_c + 0x36) + spacing);
							}
							*(int32_t *)(w_def + 0x3E0) = c_count + 1;
							*(uint8_t **)(w_def + 0x3E4) = new_children;
							*(int16_t *)(w_def + 0x28) = (int16_t)(*(int16_t const *)(w_def + 0x28) + spacing); /* bounds.y1 */
						}
						break;
					}
				}
			}
		}
	}
}

/* ---------- Point 11: Redirect custom_maps\<name>.(map|bmp|txt) to forge_maps\<name>\<name>.(map|bmp|txt) */

extern void *file_reference_create_from_path(void *reference, char const *full_path, boolean is_directory);

static char const *resolve_forge_maps_redirect_path(char const *path, char *out_buf, size_t out_sz)
{
	char const *leaf;
	char const *dot;
	char sub_dir[64];
	size_t base_len;
	FILE *tf;

	if (!path || !path[0])
		return path;
	if (_strnicmp(path, "custom_maps\\", 12) != 0 && _strnicmp(path, "custom_maps/", 12) != 0)
		return path;

	leaf = path + 12;
	if (_stricmp(leaf, "bitmaps.map") == 0 ||
		_stricmp(leaf, "sounds.map") == 0 ||
		_stricmp(leaf, "loc.map") == 0 ||
		strchr(leaf, '\\') != NULL ||
		strchr(leaf, '/') != NULL)
	{
		return path;
	}

	dot = strrchr(leaf, '.');
	base_len = dot ? (size_t)(dot - leaf) : strlen(leaf);
	if (base_len == 0 || base_len >= sizeof(sub_dir))
		return path;

	memcpy(sub_dir, leaf, base_len);
	sub_dir[base_len] = '\0';

	snprintf(out_buf, out_sz, "forge_maps\\%s\\%s", sub_dir, leaf);
	tf = fopen(out_buf, "rb");
	if (tf)
	{
		fclose(tf);
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

FILE *forge_ce_fopen(char const *filename, char const *mode)
{
	char redirected[512];
	char const *actual = resolve_forge_maps_redirect_path(filename, redirected, sizeof(redirected));
	return fopen(actual, mode);
}


