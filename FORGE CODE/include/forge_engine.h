/*
 * OpenCE Forge ("HaloCE Forge.exe")
 * Runtime Engine Header - Shared across all Forge runtime modules.
 * Strictly modular inside FORGE CODE/ with zero modifications to source/ or port/.
 */

#ifndef FORGE_ENGINE_H
#define FORGE_ENGINE_H

#include <stdint.h>
#include "forge_cache_builder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Forge Network Protocol Isolation ('F' << 8 | 38 = 0x4626) */

#define FORGE_NETWORK_VERSION 0x4626
#define STANDARD_OPENCE_NETWORK_VERSION 38
#define _forge_ai_team_friendly_human _forge_ai_team_friendly

extern int g_forge_session_enabled;
int forge_get_network_version(void);

#ifdef _WIN32
__declspec(dllimport) short __stdcall GetAsyncKeyState(int vKey);
#else
static inline short GetAsyncKeyState(int vKey) { (void)vKey; return 0; }
#endif

#ifndef VK_LBUTTON
#define VK_LBUTTON   0x01
#define VK_RBUTTON   0x02
#endif

#ifndef VK_BACK
#define VK_BACK      0x08
#define VK_TAB       0x09
#define VK_RETURN    0x0D
#define VK_SHIFT     0x10
#define VK_CONTROL   0x11
#define VK_ESCAPE    0x1B
#define VK_SPACE     0x20
#define VK_PRIOR     0x21
#define VK_NEXT      0x22
#define VK_END       0x23
#define VK_HOME      0x24
#define VK_LEFT      0x25
#define VK_UP        0x26
#define VK_RIGHT     0x27
#define VK_DOWN      0x28
#define VK_DELETE    0x2E
#define VK_F1        0x70
#define VK_F2        0x71
#define VK_F3        0x72
#define VK_F4        0x73
#define VK_F5        0x74
#define VK_F6        0x75
#define VK_OEM_PLUS  0xBB
#define VK_OEM_MINUS 0xBD
#define VK_OEM_4     0xDB
#define VK_OEM_6     0xDD
#endif

/* ---------- Forge Editor Modes */

enum forge_editor_mode
{
	_forge_mode_play = 0,         /* Normal Spartan / Player control */
	_forge_mode_3d_monitor = 1,   /* Mode 2: 3D In-Engine 343 Guilty Spark Monitor & Physics Beam */
	_forge_mode_2d_blueprint = 2, /* Mode 1: Top-Down Overhead Camera & Shining Drop Marker */
	NUMBER_OF_FORGE_MODES
};

enum forge_ui_modal
{
	_forge_modal_none = 0,
	_forge_modal_spawn_palette,
	_forge_modal_object_properties,
	_forge_modal_inject_custom_tags,
	_forge_modal_save_copy_as
};

enum forge_palette_category
{
	_forge_cat_weapons = 0,
	_forge_cat_equipment,
	_forge_cat_vehicles,
	_forge_cat_bipeds,
	_forge_cat_scenery,
	_forge_cat_barriers_devices,
	_forge_cat_lights,
	_forge_cat_spawns_teleporters,
	_forge_cat_objectives,
	_forge_cat_ai_squads,
	NUMBER_OF_FORGE_PALETTE_CATEGORIES
};

#define _forge_cat_count NUMBER_OF_FORGE_PALETTE_CATEGORIES

enum forge_snap_angle_mode
{
	_forge_snap_off = 0,
	_forge_snap_15_deg,
	_forge_snap_45_deg,
	_forge_snap_90_deg,
	NUMBER_OF_FORGE_SNAP_MODES
};

/* ---------- Runtime Palette Catalog Entry */

struct forge_catalog_entry
{
	uint32_t group_tag;
	uint8_t category;
	uint8_t source_cache_id; /* 0 = forgeasset.map (universal), 1 = customforgeasset.map (SP only) */
	uint16_t pad;
	char tag_path[FORGE_TAG_PATH_MAX];
	char display_name[64];
};

#define FORGE_MAX_CATALOG_ENTRIES 2048
#define FORGE_MAX_DONOR_MAPS_LIST 128

/* ---------- Live Placed Instance Tracking */

struct forge_live_instance
{
	int32_t active;
	int32_t object_index;      /* Engine object datum index, or -1 if marker-only */
	int32_t actor_index;       /* Engine AI actor datum index, or -1 */
	int32_t edit_entry_index;  /* Index in edit_state */
	uint32_t group_tag;
	uint8_t source_cache_id;
	uint8_t is_ai_unit;
	uint8_t is_default_scenario_object; /* 1 if adopted from the base map's default scenario objects */
	uint8_t pad_inst;
	uint16_t gametype_flags;   /* 0 = All Modes, bit0=CTF, bit1=Slayer, bit2=Oddball, bit3=KOTH, bit4=Race */
	int16_t netgame_flag_type; /* Scenario netgame_flag type (6=tele_from, 7=tele_to, 8=hill, 2=oddball, 0=ctf) or -1 */
	int16_t team_designator;   /* Reference/Channel ID or Team Index */
	float orig_x, orig_y, orig_z; /* Original scenario position if is_default_scenario_object == 1 */
	float x, y, z;
	float yaw, pitch, roll;
	char tag_path[FORGE_TAG_PATH_MAX];
	char label[64];
};

#define FORGE_MAX_LIVE_INSTANCES 1024

/* ---------- Global Runtime Forge State */

struct forge_runtime_state
{
	int initialized;
	enum forge_editor_mode mode;
	enum forge_ui_modal active_modal;
	enum forge_palette_category current_category;
	int h3_menu_stage; /* 0 = Halo 3 category list, 1 = Halo 3 item list */
	int palette_cursor;
	int palette_scroll;
	int props_cursor;
	enum forge_snap_angle_mode snap_mode;
	int ai_team_mode; /* _forge_ai_team_friendly_human or _forge_ai_team_enemy_covenant */
	int third_person_view; /* Toggleable via console `third_person` or F4 to inspect 343 Guilty Spark */

	/* Monitor 6DOF Free-Cam State */
	int monitor_cam_initialized;
	float cam_x, cam_y, cam_z;
	float cam_yaw, cam_pitch, cam_roll;
	float cam_speed;

	/* Physics Beam / Object Grab State */
	int32_t grabbed_object_index;
	int32_t grabbed_live_index;
	int grabbed_is_new_spawn;
	float grab_orig_x, grab_orig_y, grab_orig_z;
	float grab_orig_yaw, grab_orig_pitch, grab_orig_roll;
	float grab_distance;
	float grab_yaw, grab_pitch, grab_roll;
	int32_t hovered_object_index;
	int32_t hovered_live_index;
	char hovered_object_label[128];
	float ray_hit_x, ray_hit_y, ray_hit_z;
	int ray_hit_valid;

	/* 10-Point Linked KOTH Hill Circle Placement State */
	int koth_group_placing;
	int koth_group_hill_id;
	float koth_group_radius;
	int32_t koth_group_live_indices[10];

	/* Mode 1: Top-Down Overhead Camera & Shining Drop Icon State */
	int overhead_initialized;
	float bp_center_x, bp_center_y, bp_cam_z;
	float bp_ground_z;
	float bp_height_offset;
	float bp_yaw;
	float bp_item_yaw;
	float bp_zoom;

	/* Catalogs from cached_tags / forgeasset.map (0) and customforgeasset.map (1) */
	struct forge_catalog_entry catalog[FORGE_MAX_CATALOG_ENTRIES];
	int catalog_count;

	/* Donor maps list for "Inject Custom Tags" modal */
	char donor_map_files[FORGE_MAX_DONOR_MAPS_LIST][256];
	int donor_map_count;
	int donor_map_cursor;

	/* Save Map state: prompts for name on initial save, then quick-updates on subsequent saves */
	int has_saved_map_once;
	char save_map_name[64];
	int save_map_name_len;

	/* Status banner message */
	char status_message[128];
	uint32_t status_message_until_ms;

	/* Scenario Edit State & Live Instances */
	struct forge_scenario_edit_state edit_state;
	struct forge_live_instance live_instances[FORGE_MAX_LIVE_INSTANCES];
	int live_instance_count;

	/* Forge Controls & Controller State */
	int using_controller;
	int bind_vk[10];
	int bind_gp[10];
	float controller_look_sens;
	float controller_fly_speed;
};

enum forge_bind_action
{
	FORGE_BIND_TOGGLE_MODE = 0,
	FORGE_BIND_PALETTE = 1,
	FORGE_BIND_THIRD_PERSON = 2,
	FORGE_BIND_QUICK_SAVE = 3,
	FORGE_BIND_ROTATE_HOLD = 4,
	FORGE_BIND_SNAP_FLOOR = 5,
	FORGE_BIND_SNAP_SURFACE = 6,
	FORGE_BIND_ROT_SNAP = 7,
	FORGE_BIND_OBJECT_OPTIONS = 8,
	FORGE_BIND_DELETE_OBJECT = 9,
	FORGE_BIND_COUNT = 10
};

enum forge_gamepad_button
{
	FORGE_GP_NONE = 0,
	FORGE_GP_A = 1,
	FORGE_GP_B = 2,
	FORGE_GP_X = 3,
	FORGE_GP_Y = 4,
	FORGE_GP_BLACK = 5,
	FORGE_GP_WHITE = 6,
	FORGE_GP_LTRIGGER = 7,
	FORGE_GP_RTRIGGER = 8,
	FORGE_GP_DPAD_UP = 9,
	FORGE_GP_DPAD_DOWN = 10,
	FORGE_GP_DPAD_LEFT = 11,
	FORGE_GP_DPAD_RIGHT = 12,
	FORGE_GP_START = 13,
	FORGE_GP_BACK = 14,
	FORGE_GP_LTHUMB = 15,
	FORGE_GP_RTHUMB = 16,
	FORGE_GP_COUNT = 17
};

extern struct forge_runtime_state g_forge;

void forge_controls_init_defaults(void);
void forge_controls_load(void);
void forge_controls_save(void);
char const *forge_vk_display_name(int vk);
char const *forge_gp_display_name(int gp);
char const *forge_gp_icon_token(int gp);
char const *forge_action_prompt(int action);
int forge_gp_button_down(int gp);
int forge_action_down(int action);
void forge_poll_controller_state(float *out_lx, float *out_ly, float *out_rx, float *out_ry, float *out_lt, float *out_rt);

/* ---------- Prototypes: forge_runtime_tags.c */

void forge_runtime_init(void);
void forge_runtime_reload_catalogs(void);
int forge_is_multiplayer_session(void);
int forge_catalog_entry_allowed(struct forge_catalog_entry const *entry);
long forge_resolve_engine_tag(uint32_t group_tag, char const *tag_path, uint32_t *out_actual_group);
int forge_ui_inject_custom_map(char const *donor_map_path);
int forge_clear_custom_tag_cache(void);
int forge_reset_all_tag_caches_and_restart(void);
int forge_ui_save_copy_as(char const *new_map_name);
void forge_trigger_save_map_action(void);
void forge_load_saved_map_state_if_present(char const *map_name);
void forge_set_status(char const *fmt, ...);
void forge_draw_screen_rect(short x0, short y0, short x1, short y1, uint32_t argb);
int forge_monitor_render_player_unit(long object_index, void const *lighting, unsigned long flags);
void forge_render_world_3d_overlays(void const *lighting);

/* ---------- Prototypes: forge_ai.c */

int32_t forge_ai_spawn_unit_live(
	char const *actor_variant_path,
	float x, float y, float z, float yaw,
	int16_t team_designator,
	int32_t *out_unit_index);
void forge_ai_clear_all_live(void);

/* ---------- Prototypes: forge_blueprint_2d.c */

void forge_blueprint_enter(void);
void forge_blueprint_update(float dt);
void forge_blueprint_apply_camera(void);
void forge_blueprint_draw(short window_x0, short window_y0, short window_x1, short window_y1, long font_index, short line_height);

/* ---------- Prototypes: forge_3d_mode.c */

void forge_3d_update(float dt);
void forge_3d_apply_monitor_camera(short local_player_index);
int forge_3d_fill_live_observer_result(void *out_observer_result);
int forge_is_main_menu_active(void);
void forge_3d_draw_hud(short local_player_index, short window_x0, short window_y0, short window_x1, short window_y1, long font_index, short line_height);
int32_t forge_3d_spawn_object_at(
	uint32_t group_tag,
	char const *tag_path,
	uint8_t source_cache_id,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int16_t team_designator,
	int broadcast_net);
void forge_3d_move_instance(int32_t live_idx, float x, float y, float z, float yaw, float pitch, float roll, int broadcast_net);
void forge_3d_delete_instance(int32_t live_idx, int broadcast_net);

/* ---------- Prototypes: forge_network.c */

void forge_network_send_spawn(
	uint32_t group_tag,
	char const *tag_path,
	uint8_t source_cache_id,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int16_t team_designator);
void forge_network_send_transform(
	int32_t live_idx,
	float x, float y, float z,
	float yaw, float pitch, float roll);
void forge_network_send_delete(int32_t live_idx);
int forge_network_intercept_chat_packet(long machine_index, char const *text, int is_from_client_to_host);
void forge_runtime_on_map_unload(void);
int forge_window_is_focused(void);
short forge_key_state(int vk);

#ifdef _WIN32
#undef GetAsyncKeyState
#define GetAsyncKeyState(vk) forge_key_state(vk)
#endif

#ifdef __cplusplus
}
#endif

#endif /* FORGE_ENGINE_H */
