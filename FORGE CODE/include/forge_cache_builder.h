/*
FORGE_CACHE_BUILDER.H

Standalone and runtime Custom Edition (v609) and Halo PC (v7) cache parser,
schema-driven tag cloner, PC-to-CE bitmap/sound internalizer, gbxmodel
geometry packer, scenario modifier, and "Save Copy As..." exporter (.map,
140x114 .bmp, and .txt).
*/

#ifndef __FORGE_CACHE_BUILDER_H
#define __FORGE_CACHE_BUILDER_H
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- constants */

#define FORGE_MAX_PLACED_OBJECTS 512
#define FORGE_MAX_PLAYER_SPAWNS 512
#define FORGE_MAX_NETGAME_FLAGS 512
#define FORGE_MAX_NETGAME_EQUIPMENT 200
#define FORGE_MAX_AI_SPAWNS 128
#define FORGE_MAX_DEFAULT_OBJECT_EDITS 256
#define FORGE_TAG_PATH_MAX 256

enum forge_placed_object_category
{
	_forge_object_scenery = 0,
	_forge_object_vehicle,
	_forge_object_weapon,
	_forge_object_equipment,
	_forge_object_machine,
	_forge_object_control,
	_forge_object_light_fixture,
	_forge_object_biped,
	NUMBER_OF_FORGE_OBJECT_CATEGORIES
};

enum forge_ai_team_designator
{
	_forge_ai_team_friendly = 0, /* Allies with players (_game_team_player / _game_team_human) */
	_forge_ai_team_enemy_covenant = 1, /* Hostile Covenant (_game_team_covenant) */
	_forge_ai_team_enemy_flood = 2, /* Hostile Flood (_game_team_flood) */
	_forge_ai_team_enemy_sentinel = 3 /* Hostile Sentinel (_game_team_sentinel) */
};

/* ---------- structures */

struct forge_placed_object_entry
{
	int32_t category; /* enum forge_placed_object_category */
	uint32_t group_tag; /* 'scen', 'vehi', 'weap', 'eqip', 'mach', 'ctrl', 'lifi', 'bipd' */
	char tag_path[FORGE_TAG_PATH_MAX];
	float x, y, z;
	float yaw, pitch, roll;
	int16_t team_index;
	int16_t bsp_index;
	uint16_t placement_flags;
	uint16_t gametype_flags; /* 0 = All Modes, bit0=CTF, bit1=Slayer, bit2=Oddball, bit3=KOTH, bit4=Race */
	uint8_t source_cache_id;
	uint8_t active;
};

struct forge_default_object_edit
{
	uint32_t group_tag;
	uint8_t deleted;
	uint8_t active;
	uint16_t gametype_flags; /* 0 = All Modes, bit0=CTF, bit1=Slayer, bit2=Oddball, bit3=KOTH, bit4=Race */
	char tag_path[FORGE_TAG_PATH_MAX];
	float orig_x, orig_y, orig_z;
	float new_x, new_y, new_z;
	float new_yaw, new_pitch, new_roll;
};

struct forge_player_spawn_entry
{
	float x, y, z;
	float yaw;
	int16_t team_index; /* 0 = Red, 1 = Blue, -1 = FFA/Any */
	int16_t bsp_index;
	int16_t type0; /* game mode filter (0 = all, 1 = ctf, 2 = slayer, 3 = oddball, 4 = king, 5 = race) */
	int16_t type1;
	int16_t type2;
	int16_t type3;
};

struct forge_netgame_flag_entry
{
	float x, y, z;
	float yaw;
	int16_t type; /* _netgame_flag_ctf_flag, _netgame_flag_hill_flag, _netgame_flag_oddball_ball_spawn, _netgame_flag_race_track, _netgame_flag_teleport_from, _netgame_flag_teleport_to */
	int16_t usage_id; /* team index (0=Red, 1=Blue) or teleporter pair ID / hill order */
	char weapon_group_path[FORGE_TAG_PATH_MAX];
};

struct forge_netgame_equipment_entry
{
	uint32_t flags; /* bit 0 = levitate */
	int16_t type0;
	int16_t type1;
	int16_t type2;
	int16_t type3;
	int16_t team_index;
	int16_t spawn_time_seconds;
	float x, y, z;
	float yaw;
	char item_collection_path[FORGE_TAG_PATH_MAX]; /* 'itmc' tag path */
};

struct forge_ai_spawn_entry
{
	char actor_variant_path[FORGE_TAG_PATH_MAX]; /* 'actv' tag path */
	float x, y, z;
	float yaw;
	int16_t team_designator; /* enum forge_ai_team_designator (0=Friendly, 1=Enemy Covenant, 2=Enemy Flood, etc.) */
	int16_t unit_count; /* Number of units in this squad marker (1..8) */
	int16_t respawn_time_ticks; /* 0 = no respawn, >0 = respawn delay */
};

struct forge_scenario_edit_state
{
	/* If true, replaces the base map's existing spawns/flags/equipment/objects with these arrays;
	   if false, appends these entries onto the base map's existing scenario blocks. */
	int replace_player_spawns;
	int replace_netgame_flags;
	int replace_netgame_equipment;
	int replace_placed_objects;
	int replace_ai_spawns;

	int32_t player_spawn_count;
	struct forge_player_spawn_entry player_spawns[FORGE_MAX_PLAYER_SPAWNS];

	int32_t netgame_flag_count;
	struct forge_netgame_flag_entry netgame_flags[FORGE_MAX_NETGAME_FLAGS];

	int32_t netgame_equipment_count;
	struct forge_netgame_equipment_entry netgame_equipment[FORGE_MAX_NETGAME_EQUIPMENT];

	int32_t placed_object_count;
	struct forge_placed_object_entry placed_objects[FORGE_MAX_PLACED_OBJECTS];

	int32_t ai_spawn_count;
	struct forge_ai_spawn_entry ai_spawns[FORGE_MAX_AI_SPAWNS];

	int32_t default_edit_count;
	struct forge_default_object_edit default_edits[FORGE_MAX_DEFAULT_OBJECT_EDITS];
};

/* Summary of a map's 2D floorplan & existing placements for 2D Blueprint Mode */
struct forge_bsp_edge_2d
{
	float x0, y0, z0;
	float x1, y1, z1;
};

struct forge_map_blueprint_data
{
	char map_name[64];
	int16_t scenario_type; /* 0 = solo/co-op, 1 = multiplayer */
	float min_x, max_x;
	float min_y, max_y;
	float min_z, max_z;

	int32_t edge_count;
	struct forge_bsp_edge_2d *edges; /* heap-allocated array */

	struct forge_scenario_edit_state initial_state;
};

/* Catalog entry for the in-game & 2D Forge spawn palette */
struct forge_catalog_item
{
	uint32_t group_tag; /* 'weap', 'vehi', 'eqip', 'itmc', 'scen', 'mach', 'ctrl', 'lifi', 'bipd', 'actv' */
	char tag_path[FORGE_TAG_PATH_MAX];
	char display_name[64];
	int32_t source_cache; /* 0 = current map, 1 = forgeasset.map, 2 = customforgeasset.map */
};

/* ---------- prototypes */

/* Builds OpenCE Forge/forge_maps/forgeasset.map from CUSTOM EDITION DEFAULT MAPS + PC MAPS
   and records all harvested tag attributions into credits_md_path ("OpenCE Forge/credits.md") */
int forge_build_master_asset_cache(
	char const *ce_maps_dir,
	char const *pc_maps_dir,
	char const *output_map_path,
	char const *credits_md_path);

/* Builds OpenCE Forge/forge_maps/customforgeasset.map from user/community CE custom maps
   and appends custom tag attributions to credits_md_path ("OpenCE Forge/credits.md") */
int forge_build_custom_asset_cache(
	char const *base_ce_map_path,
	char const *ce_res_dir,
	char const *forgeasset_path,
	char const *const *custom_map_paths,
	int custom_map_count,
	char const *output_map_path,
	char const *credits_md_path);

/* Injects unique tags from a single custom map into customforgeasset.map ("Inject Custom Tags" UI action)
   and automatically appends the new tag attributions to credits_md_path ("OpenCE Forge/credits.md") */
int forge_inject_custom_map_into_cache(
	char const *source_custom_map_path,
	char const *ce_res_dir,
	char const *forgeasset_path,
	char const *customforgeasset_path,
	char const *credits_md_path,
	int *out_injected_root_count);

/* Exports a modified scenario map to custom_maps/<output_map_name>.map (.bmp, .txt) ("Save Copy As..." UI action) */
int forge_export_scenario_map(
	char const *base_map_path,
	char const *ce_res_dir,
	char const *pc_res_dir,
	char const *const *donor_map_paths,
	int donor_map_count,
	struct forge_scenario_edit_state const *edits,
	char const *output_map_name,
	char const *output_dir);

/* Loads 2D floorplan collision edges and initial scenario placements from any CE (v609) or PC (v7) .map file */
int forge_load_map_blueprint(
	char const *map_path,
	struct forge_map_blueprint_data *out_blueprint);

void forge_free_map_blueprint(
	struct forge_map_blueprint_data *blueprint);

/* Enumerates spawnable tags from a .map file (such as forgeasset.map or customforgeasset.map) */
int forge_enumerate_map_catalog(
	char const *map_path,
	int32_t source_cache_id,
	struct forge_catalog_item *out_items,
	int max_items);

/* Validates a Custom Edition v609 .map file using OpenCE's custom_edition_cache_load + tag_validate_custom_edition_tags */
int forge_validate_custom_edition_map_file(
	char const *map_path,
	char const *ce_res_dir,
	int32_t *out_tag_count,
	int32_t *out_corrections);

#ifdef __cplusplus
}
#endif

#endif /* __FORGE_CACHE_BUILDER_H */
