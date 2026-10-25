/*
 * OpenCE Forge ("HaloCE Forge.exe")
 * Collaborative P2P Forge Network Synchronization:
 * - Bidirectional Spawn (!FS: / !FH:), Transform (!FM:), and Delete (!FD:) replication
 * - Per-Player Monitor vs. Spartan mode & 3D Monitor flight state synchronization (!FT:)
 * - Late-join / full-session state synchronization (!FQ)
 * - Automatic client-side external tag & .ocetags faulting on object creation
 * Strictly modular inside FORGE CODE/ with zero modifications to source/ or port/.
 */

#include "cseries.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"
#include "items/items.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_groups.h"
#include "network_distributed.h"
#include "network_chat.h"

#include "forge_engine.h"

extern unsigned long system_milliseconds(void);
extern void orig_network_objects_handle_changes(void const *entries, short count);

struct forge_chat_line_payload
{
	uint8_t player_index;
	uint8_t flags;
	uint8_t pad[2];
	char text[64];
};

struct forge_chat_packet
{
	struct distributed_message_header header;
	struct forge_chat_line_payload line;
};

struct forge_distributed_object_change
{
	byte change;
	byte flags;
	byte owner_player_index;
	byte forced_shader_permutation_index;
	long object_index;
	long definition_index;
	short owner_team_index;
	short variant_number;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	real_vector3d translational_velocity;
	real_vector3d angular_velocity;
	real_rgb_color change_colors[4];
	byte region_permutations[8];
};

#define FORGE_MAX_TRACKED_NET_OBJECTS 2048
#define FORGE_MAX_CLIENT_MACHINES     32

static unsigned long g_last_transform_send_ms = 0;
static unsigned long g_last_monitor_send_ms = 0;
static int g_client_requested_full_sync = 0;
static unsigned long g_client_join_ms = 0;

/* Maps absolute object index -> resolved local tag definition_index & Forge live_idx */
static long g_net_obj_def_index[FORGE_MAX_TRACKED_NET_OBJECTS];
static int32_t g_net_obj_live_index[FORGE_MAX_TRACKED_NET_OBJECTS];
static uint8_t g_net_obj_has_deferred_create[FORGE_MAX_TRACKED_NET_OBJECTS];
static struct forge_distributed_object_change g_net_obj_deferred_create[FORGE_MAX_TRACKED_NET_OBJECTS];

/* Host per-machine incremental full-sync cursor (-1 = idle, >= 0 = next live_idx to send) */
static int g_host_machine_known[FORGE_MAX_CLIENT_MACHINES];
static int g_host_machine_sync_cursor[FORGE_MAX_CLIENT_MACHINES];
static int g_host_machine_mon_synced[FORGE_MAX_CLIENT_MACHINES];

uint32_t forge_catalog_entry_hash(uint32_t group_tag, char const *tag_path)
{
	uint32_t h = 2166136261u;
	char const *p;

	h = (h ^ ((group_tag >> 24) & 0xFF)) * 16777619u;
	h = (h ^ ((group_tag >> 16) & 0xFF)) * 16777619u;
	h = (h ^ ((group_tag >> 8) & 0xFF)) * 16777619u;
	h = (h ^ (group_tag & 0xFF)) * 16777619u;

	if (tag_path)
	{
		for (p = tag_path; *p; p++)
		{
			unsigned char c = (unsigned char)*p;
			if (c == '/')
				c = '\\';
			else if (c >= 'A' && c <= 'Z')
				c = (unsigned char)(c - 'A' + 'a');
			h = (h ^ c) * 16777619u;
		}
	}
	return h ? h : 1u;
}

struct forge_catalog_entry const *forge_find_catalog_entry_by_hash(uint32_t hash)
{
	int i;
	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (forge_catalog_entry_hash(g_forge.catalog[i].group_tag, g_forge.catalog[i].tag_path) == hash)
			return &g_forge.catalog[i];
	}
	return NULL;
}

static void forge_send_packet_to_host(char const *text)
{
	struct forge_chat_packet pkt;
	if (!text || !text[0])
		return;
	memset(&pkt, 0, sizeof(pkt));
	pkt.line.player_index = 0;
	pkt.line.flags = 0;
	snprintf(pkt.line.text, sizeof(pkt.line.text), "%s", text);
	distributed_send(&pkt, _distributed_message_chat_up, 1, sizeof(pkt), _distributed_to_host_reliably);
}

static void forge_send_packet_to_machine(long machine_index, char const *text)
{
	struct forge_chat_packet pkt;
	if (!text || !text[0] || machine_index < 0)
		return;
	memset(&pkt, 0, sizeof(pkt));
	pkt.line.player_index = 0;
	pkt.line.flags = 0;
	snprintf(pkt.line.text, sizeof(pkt.line.text), "%s", text);
	distributed_send_to_machine_reliably(machine_index, &pkt, _distributed_message_chat_down, 1, sizeof(pkt));
}

static void forge_broadcast_packet_to_clients(char const *text, long exclude_machine_index)
{
	long machines[FORGE_MAX_CLIENT_MACHINES];
	short count, i;

	if (!text || !text[0])
		return;
	count = distributed_client_machines(machines, FORGE_MAX_CLIENT_MACHINES);
	for (i = 0; i < count; i++)
	{
		if (machines[i] != exclude_machine_index)
			forge_send_packet_to_machine(machines[i], text);
	}
}

void forge_network_reset_session(void)
{
	int i;
	g_last_transform_send_ms = 0;
	g_last_monitor_send_ms = 0;
	g_client_requested_full_sync = 0;
	g_client_join_ms = system_milliseconds();

	memset(g_forge.player_is_monitor, 0, sizeof(g_forge.player_is_monitor));
	memset(g_forge.player_has_monitor_pos, 0, sizeof(g_forge.player_has_monitor_pos));
	memset(g_forge.player_monitor_pos, 0, sizeof(g_forge.player_monitor_pos));
	memset(g_forge.player_monitor_yaw, 0, sizeof(g_forge.player_monitor_yaw));
	memset(g_forge.player_monitor_pitch, 0, sizeof(g_forge.player_monitor_pitch));

	for (i = 0; i < FORGE_MAX_TRACKED_NET_OBJECTS; i++)
	{
		g_net_obj_def_index[i] = NONE;
		g_net_obj_live_index[i] = -1;
		g_net_obj_has_deferred_create[i] = 0;
	}
	for (i = 0; i < FORGE_MAX_CLIENT_MACHINES; i++)
	{
		g_host_machine_known[i] = 0;
		g_host_machine_sync_cursor[i] = -1;
		g_host_machine_mon_synced[i] = 0;
	}
}

static void forge_format_host_spawn_packet(
	char *out_buf,
	size_t out_size,
	int32_t live_idx,
	uint32_t group_tag,
	char const *tag_path,
	long obj_idx,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int16_t team_designator)
{
	uint32_t hash = forge_catalog_entry_hash(group_tag, tag_path);
	unsigned int obj_enc = (obj_idx != NONE) ? (unsigned int)(DATUM_INDEX_TO_ABSOLUTE_INDEX(obj_idx) + 1) : 0u;
	int yaw_cr = (int)(yaw * 100.0f);
	int pitch_cr = (int)(pitch * 100.0f);
	int roll_cr = (int)(roll * 100.0f);

	snprintf(
		out_buf,
		out_size,
		"!FH:%X:%08X:%X:%.2f:%.2f:%.2f:%d:%d:%d:%d",
		(unsigned int)live_idx,
		(unsigned int)hash,
		obj_enc,
		x, y, z,
		yaw_cr, pitch_cr, roll_cr,
		(int)team_designator);
}

void forge_network_send_monitor_mode(int is_monitor, float x, float y, float z, float yaw, float pitch)
{
	char msg[64];
	int local_p_abs;
	int yaw_cr = (int)(yaw * 100.0f);
	int pitch_cr = (int)(pitch * 100.0f);

	if (!forge_is_multiplayer_session())
		return;

	local_p_abs = forge_get_local_player_abs_index();
	if (local_p_abs < 0 || local_p_abs >= 128)
		local_p_abs = 0;

	g_last_monitor_send_ms = system_milliseconds();

	snprintf(
		msg,
		sizeof(msg),
		"!FT:%d:%d:%.2f:%.2f:%.2f:%d:%d",
		local_p_abs,
		is_monitor ? 1 : 0,
		x, y, z,
		yaw_cr, pitch_cr);

	if (game_connection() == _game_connection_network_client)
	{
		forge_send_packet_to_host(msg);
	}
	else if (game_connection() == _game_connection_network_server)
	{
		forge_broadcast_packet_to_clients(msg, -1);
	}
}

void forge_network_sync_tick(void)
{
	unsigned long now;

	if (!forge_is_multiplayer_session())
		return;

	now = system_milliseconds();

	/* Periodically broadcast local Monitor flight position/aiming (at ~7 Hz) so other players see smooth 3D Monitor flight */
	if (g_forge.mode == _forge_mode_3d_monitor && (now - g_last_monitor_send_ms >= 140))
	{
		forge_network_send_monitor_mode(1, g_forge.cam_x, g_forge.cam_y, g_forge.cam_z, g_forge.cam_yaw, g_forge.cam_pitch);
	}

	if (game_connection() == _game_connection_network_client)
	{
		if (!g_client_requested_full_sync && g_forge.monitor_cam_initialized && (now - g_client_join_ms >= 250))
		{
			g_client_requested_full_sync = 1;
			forge_send_packet_to_host("!FQ");
			if (g_forge.mode == _forge_mode_3d_monitor)
				forge_network_send_monitor_mode(1, g_forge.cam_x, g_forge.cam_y, g_forge.cam_z, g_forge.cam_yaw, g_forge.cam_pitch);
		}
	}
	else if (game_connection() == _game_connection_network_server)
	{
		long machines[FORGE_MAX_CLIENT_MACHINES];
		uint8_t active_mask[FORGE_MAX_CLIENT_MACHINES];
		short count, i;
		int m;

		memset(active_mask, 0, sizeof(active_mask));
		count = distributed_client_machines(machines, FORGE_MAX_CLIENT_MACHINES);
		for (i = 0; i < count; i++)
		{
			long midx = machines[i];
			if (midx >= 0 && midx < FORGE_MAX_CLIENT_MACHINES)
			{
				active_mask[midx] = 1;
				if (!g_host_machine_known[midx])
				{
					g_host_machine_known[midx] = 1;
					g_host_machine_sync_cursor[midx] = 0;
					g_host_machine_mon_synced[midx] = 0;
				}
			}
		}

		for (m = 0; m < FORGE_MAX_CLIENT_MACHINES; m++)
		{
			int packets_sent = 0;
			if (!active_mask[m])
			{
				g_host_machine_known[m] = 0;
				g_host_machine_sync_cursor[m] = -1;
				g_host_machine_mon_synced[m] = 0;
				continue;
			}

			/* Step 1: Sync all active Monitor players to newly joined client machine */
			if (!g_host_machine_mon_synced[m])
			{
				int p_abs;
				for (p_abs = 0; p_abs < 128; p_abs++)
				{
					if (g_forge.player_is_monitor[p_abs])
					{
						char msg[64];
						snprintf(
							msg,
							sizeof(msg),
							"!FT:%d:1:%.2f:%.2f:%.2f:%d:%d",
							p_abs,
							g_forge.player_monitor_pos[p_abs][0],
							g_forge.player_monitor_pos[p_abs][1],
							g_forge.player_monitor_pos[p_abs][2],
							(int)(g_forge.player_monitor_yaw[p_abs] * 100.0f),
							(int)(g_forge.player_monitor_pitch[p_abs] * 100.0f));
						forge_send_packet_to_machine(m, msg);
						packets_sent++;
					}
				}
				g_host_machine_mon_synced[m] = 1;
			}

			/* Step 2: Drip-feed up to 6 live_instances per tick to newly joined client machine */
			while (g_host_machine_sync_cursor[m] >= 0 &&
				   g_host_machine_sync_cursor[m] < g_forge.live_instance_count &&
				   packets_sent < 6)
			{
				int32_t idx = g_host_machine_sync_cursor[m]++;
				struct forge_live_instance const *inst = &g_forge.live_instances[idx];
				char msg[64];

				if (!inst->active)
				{
					if (inst->is_default_scenario_object)
					{
						snprintf(msg, sizeof(msg), "!FD:%d:-1", (int)idx);
						forge_send_packet_to_machine(m, msg);
						packets_sent++;
					}
				}
				else if (!inst->is_default_scenario_object)
				{
					forge_format_host_spawn_packet(
						msg,
						sizeof(msg),
						idx,
						inst->group_tag,
						inst->tag_path,
						inst->object_index,
						inst->x, inst->y, inst->z,
						inst->yaw, inst->pitch, inst->roll,
						inst->team_designator);
					forge_send_packet_to_machine(m, msg);
					packets_sent++;
				}
				else if (inst->edit_entry_index >= 0)
				{
					int obj_abs = (inst->object_index != NONE) ? (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(inst->object_index) : -1;
					snprintf(
						msg,
						sizeof(msg),
						"!FM:%d:%d:%.2f:%.2f:%.2f:%d:%d:%d",
						(int)idx,
						obj_abs,
						inst->x, inst->y, inst->z,
						(int)(inst->yaw * 100.0f),
						(int)(inst->pitch * 100.0f),
						(int)(inst->roll * 100.0f));
					forge_send_packet_to_machine(m, msg);
					packets_sent++;
				}
			}

			if (g_host_machine_sync_cursor[m] >= g_forge.live_instance_count)
				g_host_machine_sync_cursor[m] = -1;
		}
	}
}

void forge_network_send_spawn_ext(
	int32_t live_idx,
	uint32_t group_tag,
	char const *tag_path,
	uint8_t source_cache_id,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int16_t team_designator,
	long host_obj_idx,
	long host_def_idx)
{
	char msg[64];
	uint32_t hash;

	(void)source_cache_id;
	if (!forge_is_multiplayer_session() || !tag_path || !tag_path[0])
		return;

	hash = forge_catalog_entry_hash(group_tag, tag_path);

	if (host_obj_idx != NONE)
	{
		int obj_abs = (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(host_obj_idx);
		if (obj_abs >= 0 && obj_abs < FORGE_MAX_TRACKED_NET_OBJECTS)
		{
			if (host_def_idx != NONE)
				g_net_obj_def_index[obj_abs] = host_def_idx;
			if (live_idx >= 0)
				g_net_obj_live_index[obj_abs] = live_idx;
		}
	}

	if (game_connection() == _game_connection_network_client)
	{
		snprintf(
			msg,
			sizeof(msg),
			"!FS:%08X:%.2f:%.2f:%.2f:%d:%d:%d:%d",
			(unsigned int)hash,
			x, y, z,
			(int)(yaw * 100.0f),
			(int)(pitch * 100.0f),
			(int)(roll * 100.0f),
			(int)team_designator);
		forge_send_packet_to_host(msg);
	}
	else if (game_connection() == _game_connection_network_server)
	{
		forge_format_host_spawn_packet(
			msg,
			sizeof(msg),
			live_idx,
			group_tag,
			tag_path,
			host_obj_idx,
			x, y, z,
			yaw, pitch, roll,
			team_designator);
		forge_broadcast_packet_to_clients(msg, -1);
	}
}

void forge_network_send_spawn(
	uint32_t group_tag,
	char const *tag_path,
	uint8_t source_cache_id,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int16_t team_designator)
{
	forge_network_send_spawn_ext(
		-1,
		group_tag,
		tag_path,
		source_cache_id,
		x, y, z,
		yaw, pitch, roll,
		team_designator,
		NONE,
		NONE);
}

void forge_network_send_transform_ext(
	int32_t live_idx,
	long obj_idx,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int immediate)
{
	unsigned long now;
	char msg[64];
	int obj_abs;

	if (!forge_is_multiplayer_session())
		return;

	now = system_milliseconds();
	if (!immediate && (now - g_last_transform_send_ms < 50))
		return;
	g_last_transform_send_ms = now;

	obj_abs = (obj_idx != NONE) ? (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(obj_idx) : -1;
	snprintf(
		msg,
		sizeof(msg),
		"!FM:%d:%d:%.2f:%.2f:%.2f:%d:%d:%d",
		(int)live_idx,
		obj_abs,
		x, y, z,
		(int)(yaw * 100.0f),
		(int)(pitch * 100.0f),
		(int)(roll * 100.0f));

	if (game_connection() == _game_connection_network_client)
	{
		forge_send_packet_to_host(msg);
	}
	else if (game_connection() == _game_connection_network_server)
	{
		forge_broadcast_packet_to_clients(msg, -1);
	}
}

void forge_network_send_transform(
	int32_t live_idx,
	float x, float y, float z,
	float yaw, float pitch, float roll)
{
	long obj_idx = NONE;
	if (live_idx >= 0 && live_idx < g_forge.live_instance_count)
		obj_idx = g_forge.live_instances[live_idx].object_index;
	forge_network_send_transform_ext(live_idx, obj_idx, x, y, z, yaw, pitch, roll, 0);
}

void forge_network_send_delete_ext(int32_t live_idx, long obj_idx)
{
	char msg[64];
	int obj_abs;

	if (!forge_is_multiplayer_session())
		return;

	obj_abs = (obj_idx != NONE) ? (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(obj_idx) : -1;
	snprintf(msg, sizeof(msg), "!FD:%d:%d", (int)live_idx, obj_abs);

	if (game_connection() == _game_connection_network_client)
	{
		forge_send_packet_to_host(msg);
	}
	else if (game_connection() == _game_connection_network_server)
	{
		forge_broadcast_packet_to_clients(msg, -1);
	}
}

void forge_network_send_delete(int32_t live_idx)
{
	long obj_idx = NONE;
	if (live_idx >= 0 && live_idx < g_forge.live_instance_count)
		obj_idx = g_forge.live_instances[live_idx].object_index;
	forge_network_send_delete_ext(live_idx, obj_idx);
}

static long forge_find_live_object_by_abs_index(int obj_abs)
{
	if (obj_abs < 0 || !object_header_data)
		return NONE;
	if (obj_abs < object_header_data->count)
	{
		long cand;
		for (cand = data_next_index(object_header_data, NONE);
			 cand != NONE;
			 cand = data_next_index(object_header_data, cand))
		{
			if ((int)DATUM_INDEX_TO_ABSOLUTE_INDEX(cand) == obj_abs)
				return cand;
		}
	}
	return NONE;
}

static int32_t forge_resolve_target_live_idx(int live_idx, int obj_abs)
{
	if (live_idx >= 0 && live_idx < g_forge.live_instance_count && g_forge.live_instances[live_idx].active)
		return live_idx;
	if (obj_abs >= 0)
	{
		long obj_idx = forge_find_live_object_by_abs_index(obj_abs);
		if (obj_idx != NONE)
			return forge_3d_adopt_or_find_object_instance(obj_idx);
	}
	if (live_idx >= 0 && live_idx < g_forge.live_instance_count)
		return live_idx;
	return -1;
}

static void forge_client_handle_host_spawn(
	int host_live_idx,
	uint32_t hash,
	int obj_abs,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int16_t team_designator)
{
	struct forge_catalog_entry const *ce = forge_find_catalog_entry_by_hash(hash);
	uint32_t actual_grp;
	long def_idx = NONE;
	int32_t match_idx = -1;
	int i;

	if (!ce)
		return;

	actual_grp = ce->group_tag;
	if (ce->group_tag != 'spwn' && ce->group_tag != 'tele' && ce->group_tag != 'flag')
	{
		/* Fault in external Xbox campaign tags or .ocetags packages on the client before object creation! */
		def_idx = forge_resolve_engine_tag(ce->group_tag, ce->tag_path, &actual_grp);
	}

	if (obj_abs >= 0 && obj_abs < FORGE_MAX_TRACKED_NET_OBJECTS && def_idx != NONE)
	{
		g_net_obj_def_index[obj_abs] = def_idx;
	}

	/* Check if this Client already has this live_instance (e.g. if this Client initiated the spawn) */
	if (host_live_idx >= 0 && host_live_idx < g_forge.live_instance_count)
	{
		struct forge_live_instance const *cand = &g_forge.live_instances[host_live_idx];
		if (cand->active && cand->group_tag == ce->group_tag &&
			_stricmp(cand->tag_path, ce->tag_path) == 0)
		{
			match_idx = host_live_idx;
		}
	}
	if (match_idx < 0)
	{
		for (i = g_forge.live_instance_count - 1; i >= 0; i--)
		{
			struct forge_live_instance const *cand = &g_forge.live_instances[i];
			float dx, dy, dz;
			if (!cand->active || cand->is_default_scenario_object)
				continue;
			if (cand->group_tag != ce->group_tag || _stricmp(cand->tag_path, ce->tag_path) != 0)
				continue;
			dx = cand->x - x;
			dy = cand->y - y;
			dz = cand->z - z;
			if (dx * dx + dy * dy + dz * dz < 4.0f)
			{
				match_idx = i;
				break;
			}
		}
	}

	if (match_idx < 0)
	{
		match_idx = forge_3d_spawn_object_at(
			ce->group_tag,
			ce->tag_path,
			ce->source_cache_id,
			x, y, z,
			yaw, pitch, roll,
			team_designator,
			0);
	}

	if (obj_abs >= 0 && obj_abs < FORGE_MAX_TRACKED_NET_OBJECTS)
	{
		long existing_obj;
		if (match_idx >= 0)
			g_net_obj_live_index[obj_abs] = match_idx;

		/* If _distributed_message_object_changes arrived BEFORE !FH: and failed because the external tag
		 * was not yet resolved on the client, replay its deferred creation right now! */
		existing_obj = forge_find_live_object_by_abs_index(obj_abs);
		if (existing_obj == NONE && g_net_obj_has_deferred_create[obj_abs] && def_idx != NONE)
		{
			struct forge_distributed_object_change re_change = g_net_obj_deferred_create[obj_abs];
			g_net_obj_has_deferred_create[obj_abs] = 0;
			re_change.definition_index = def_idx;
			orig_network_objects_handle_changes(&re_change, 1);
			existing_obj = forge_find_live_object_by_abs_index(obj_abs);
		}

		if (existing_obj != NONE && match_idx >= 0 && match_idx < g_forge.live_instance_count)
		{
			g_forge.live_instances[match_idx].object_index = existing_obj;
			if (g_forge.grabbed_live_index == match_idx)
				g_forge.grabbed_object_index = existing_obj;
			if (ce->group_tag == 'vehi')
			{
				unit_set_enterable_by_player(existing_obj, TRUE);
				unit_set_controllable(existing_obj, TRUE);
			}
		}
	}
}

int forge_network_intercept_chat_packet(long machine_index, char const *text, int is_from_client_to_host)
{
	if (!text || text[0] != '!' || text[1] != 'F')
		return 0;

	/* 1. Per-Player Monitor Mode & 3D Flight State (!FT:) */
	if (strncmp(text, "!FT:", 4) == 0)
	{
		int p_abs = -1, is_mon = 0, yaw_cr = 0, pitch_cr = 0;
		float x = 0.0f, y = 0.0f, z = 0.0f;
		if (sscanf(text + 4, "%d:%d:%f:%f:%f:%d:%d", &p_abs, &is_mon, &x, &y, &z, &yaw_cr, &pitch_cr) == 7)
		{
			if (is_from_client_to_host && game_connection() == _game_connection_network_server)
			{
				/* Verify or resolve the player index belonging to this client machine */
				if (p_abs < 0 || p_abs >= 128 || !distributed_machine_has_player(machine_index, (short)p_abs))
				{
					short cand;
					for (cand = 0; cand < 16; cand++)
					{
						if (distributed_machine_has_player(machine_index, cand))
						{
							p_abs = cand;
							break;
						}
					}
				}
				if (p_abs >= 0 && p_abs < 128)
				{
					char relay[64];
					forge_apply_host_player_monitor_state(
						p_abs,
						is_mon,
						x, y, z,
						(float)yaw_cr / 100.0f,
						(float)pitch_cr / 100.0f);
					snprintf(
						relay,
						sizeof(relay),
						"!FT:%d:%d:%.2f:%.2f:%.2f:%d:%d",
						p_abs,
						is_mon ? 1 : 0,
						x, y, z,
						yaw_cr,
						pitch_cr);
					forge_broadcast_packet_to_clients(relay, machine_index);
				}
			}
			else if (!is_from_client_to_host && game_connection() == _game_connection_network_client)
			{
				if (p_abs >= 0 && p_abs < 128 && p_abs != forge_get_local_player_abs_index())
				{
					forge_apply_host_player_monitor_state(
						p_abs,
						is_mon,
						x, y, z,
						(float)yaw_cr / 100.0f,
						(float)pitch_cr / 100.0f);
				}
			}
		}
		return 1;
	}

	/* 2. Client Full-State Sync Request (!FQ) */
	if (strncmp(text, "!FQ", 3) == 0)
	{
		if (is_from_client_to_host && game_connection() == _game_connection_network_server)
		{
			if (machine_index >= 0 && machine_index < FORGE_MAX_CLIENT_MACHINES)
			{
				g_host_machine_known[machine_index] = 1;
				g_host_machine_sync_cursor[machine_index] = 0;
				g_host_machine_mon_synced[machine_index] = 0;
			}
		}
		return 1;
	}

	/* 3. Client -> Host Packets (!FS:, !FM:, !FD:) */
	if (is_from_client_to_host && game_connection() == _game_connection_network_server)
	{
		if (strncmp(text, "!FS:", 4) == 0)
		{
			unsigned int hash = 0;
			float x = 0.0f, y = 0.0f, z = 0.0f;
			int yaw_cr = 0, pitch_cr = 0, roll_cr = 0, team = 0;
			if (sscanf(text + 4, "%X:%f:%f:%f:%d:%d:%d:%d", &hash, &x, &y, &z, &yaw_cr, &pitch_cr, &roll_cr, &team) == 8)
			{
				struct forge_catalog_entry const *ce = forge_find_catalog_entry_by_hash(hash);
				if (ce)
				{
					/* Host spawns authoritatively and broadcasts !FH: to all Clients (broadcast_net = 1) */
					forge_3d_spawn_object_at(
						ce->group_tag,
						ce->tag_path,
						ce->source_cache_id,
						x, y, z,
						(float)yaw_cr / 100.0f,
						(float)pitch_cr / 100.0f,
						(float)roll_cr / 100.0f,
						(int16_t)team,
						1);
				}
			}
			return 1;
		}
		else if (strncmp(text, "!FM:", 4) == 0)
		{
			int live_idx = -1, obj_abs = -1, yaw_cr = 0, pitch_cr = 0, roll_cr = 0;
			float x = 0.0f, y = 0.0f, z = 0.0f;
			if (sscanf(text + 4, "%d:%d:%f:%f:%f:%d:%d:%d", &live_idx, &obj_abs, &x, &y, &z, &yaw_cr, &pitch_cr, &roll_cr) == 8)
			{
				int32_t target_idx = forge_resolve_target_live_idx(live_idx, obj_abs);
				if (target_idx >= 0)
				{
					forge_3d_move_instance(
						target_idx,
						x, y, z,
						(float)yaw_cr / 100.0f,
						(float)pitch_cr / 100.0f,
						(float)roll_cr / 100.0f,
						0);
					forge_broadcast_packet_to_clients(text, machine_index);
				}
			}
			return 1;
		}
		else if (strncmp(text, "!FD:", 4) == 0)
		{
			int live_idx = -1, obj_abs = -1;
			if (sscanf(text + 4, "%d:%d", &live_idx, &obj_abs) >= 1)
			{
				int32_t target_idx = forge_resolve_target_live_idx(live_idx, obj_abs);
				if (target_idx >= 0)
				{
					forge_3d_delete_instance(target_idx, 0);
					forge_broadcast_packet_to_clients(text, machine_index);
				}
			}
			return 1;
		}
	}
	/* 4. Host -> Client Packets (!FH:, !FM:, !FD:) */
	else if (!is_from_client_to_host && game_connection() == _game_connection_network_client)
	{
		if (strncmp(text, "!FH:", 4) == 0)
		{
			unsigned int live_idx_u = 0, hash = 0, obj_enc = 0;
			float x = 0.0f, y = 0.0f, z = 0.0f;
			int yaw_cr = 0, pitch_cr = 0, roll_cr = 0, team = 0;
			if (sscanf(
					text + 4,
					"%X:%X:%X:%f:%f:%f:%d:%d:%d:%d",
					&live_idx_u,
					&hash,
					&obj_enc,
					&x, &y, &z,
					&yaw_cr, &pitch_cr, &roll_cr,
					&team) == 10)
			{
				int obj_abs = (obj_enc > 0) ? (int)(obj_enc - 1) : -1;
				forge_client_handle_host_spawn(
					(int)live_idx_u,
					(uint32_t)hash,
					obj_abs,
					x, y, z,
					(float)yaw_cr / 100.0f,
					(float)pitch_cr / 100.0f,
					(float)roll_cr / 100.0f,
					(int16_t)team);
			}
			return 1;
		}
		else if (strncmp(text, "!FM:", 4) == 0)
		{
			int live_idx = -1, obj_abs = -1, yaw_cr = 0, pitch_cr = 0, roll_cr = 0;
			float x = 0.0f, y = 0.0f, z = 0.0f;
			if (sscanf(text + 4, "%d:%d:%f:%f:%f:%d:%d:%d", &live_idx, &obj_abs, &x, &y, &z, &yaw_cr, &pitch_cr, &roll_cr) == 8)
			{
				int32_t target_idx = forge_resolve_target_live_idx(live_idx, obj_abs);
				if (target_idx >= 0 && target_idx != g_forge.grabbed_live_index)
				{
					forge_3d_move_instance(
						target_idx,
						x, y, z,
						(float)yaw_cr / 100.0f,
						(float)pitch_cr / 100.0f,
						(float)roll_cr / 100.0f,
						0);
				}
			}
			return 1;
		}
		else if (strncmp(text, "!FD:", 4) == 0)
		{
			int live_idx = -1, obj_abs = -1;
			if (sscanf(text + 4, "%d:%d", &live_idx, &obj_abs) >= 1)
			{
				int32_t target_idx = forge_resolve_target_live_idx(live_idx, obj_abs);
				if (target_idx >= 0)
				{
					forge_3d_delete_instance(target_idx, 0);
				}
			}
			return 1;
		}
	}

	return 1;
}

void forge_network_pre_handle_object_changes(void const *entries, short count)
{
	struct forge_distributed_object_change *changes = (struct forge_distributed_object_change *)entries;
	short i;

	if (!changes || count <= 0)
		return;

	for (i = 0; i < count; i++)
	{
		if (changes[i].change == 0 /* _object_change_create */ && changes[i].object_index != NONE)
		{
			int obj_abs = (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(changes[i].object_index);
			if (obj_abs >= 0 && obj_abs < FORGE_MAX_TRACKED_NET_OBJECTS)
			{
				g_net_obj_deferred_create[obj_abs] = changes[i];
				g_net_obj_has_deferred_create[obj_abs] = 1;
				if (g_net_obj_def_index[obj_abs] != NONE)
				{
					changes[i].definition_index = g_net_obj_def_index[obj_abs];
				}
			}
		}
		else if (changes[i].change == 1 /* _object_change_delete */ && changes[i].object_index != NONE)
		{
			int obj_abs = (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(changes[i].object_index);
			if (obj_abs >= 0 && obj_abs < FORGE_MAX_TRACKED_NET_OBJECTS)
			{
				g_net_obj_has_deferred_create[obj_abs] = 0;
			}
		}
	}
}

void forge_network_post_handle_object_changes(void const *entries, short count)
{
	struct forge_distributed_object_change const *changes = (struct forge_distributed_object_change const *)entries;
	short i;

	if (!changes || count <= 0)
		return;

	for (i = 0; i < count; i++)
	{
		if (changes[i].change == 0 /* _object_change_create */ && changes[i].object_index != NONE)
		{
			int obj_abs = (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(changes[i].object_index);
			if (obj_abs >= 0 && obj_abs < FORGE_MAX_TRACKED_NET_OBJECTS)
			{
				long existing_obj = forge_find_live_object_by_abs_index(obj_abs);
				if (existing_obj != NONE)
				{
					int32_t live_idx = g_net_obj_live_index[obj_abs];
					g_net_obj_has_deferred_create[obj_abs] = 0;
					if (live_idx >= 0 && live_idx < g_forge.live_instance_count && g_forge.live_instances[live_idx].active)
					{
						g_forge.live_instances[live_idx].object_index = existing_obj;
						if (g_forge.grabbed_live_index == live_idx)
							g_forge.grabbed_object_index = existing_obj;
						if (g_forge.live_instances[live_idx].group_tag == 'vehi')
						{
							unit_set_enterable_by_player(existing_obj, TRUE);
							unit_set_controllable(existing_obj, TRUE);
						}
					}
				}
			}
		}
	}
}
