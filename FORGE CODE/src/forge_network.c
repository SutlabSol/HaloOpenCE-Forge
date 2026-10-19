/*
 * OpenCE Forge ("HaloCE Forge.exe")
 * Collaborative P2P Forge Network Synchronization & Multiplayer Asset Guard Enforcement.
 * Uses OpenCE's distributed netcode and enforces FORGE_NETWORK_VERSION (0x461D) session isolation.
 * Strictly modular inside FORGE CODE/ with zero modifications to source/ or port/.
 */

#include "cseries.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/game.h"
#include "network_distributed.h"
#include "network_chat.h"

#include "forge_engine.h"

extern unsigned long system_milliseconds(void);
static unsigned long g_last_transform_send_ms = 0;

void forge_network_send_spawn(
	uint32_t group_tag,
	char const *tag_path,
	uint8_t source_cache_id,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int16_t team_designator)
{
	char msg[64];
	char g4[5];

	/* Multiplayer Asset Guard: Never transmit customforgeasset.map spawns over live network sessions */
	if (source_cache_id != 0)
		return;
	if (!forge_is_multiplayer_session())
		return;

	g4[0] = (char)((group_tag >> 24) & 0xFF);
	g4[1] = (char)((group_tag >> 16) & 0xFF);
	g4[2] = (char)((group_tag >> 8) & 0xFF);
	g4[3] = (char)(group_tag & 0xFF);
	g4[4] = 0;

	/*
	 * Note: OpenCE's network_objects.c and network_actors.c automatically replicate
	 * host object_new() and actor_create_for_unit() across all clients.
	 * When a client initiates a Forge spawn, we send a compact "!FS:" command packet
	 * to the host so the host authoritatively executes it and records it in edit_state.
	 */
	if (game_connection() == _game_connection_network_client)
	{
		int cat_idx = -1, i;
		for (i = 0; i < g_forge.catalog_count; i++)
		{
			if (g_forge.catalog[i].group_tag == group_tag &&
				g_forge.catalog[i].source_cache_id == 0 &&
				strcmp(g_forge.catalog[i].tag_path, tag_path) == 0)
			{
				cat_idx = i;
				break;
			}
		}
		if (cat_idx >= 0)
		{
			snprintf(msg, sizeof(msg), "!FS:%d:%.1f:%.1f:%.1f:%.2f:%d",
				cat_idx, x, y, z, yaw, (int)team_designator);
			network_chat_say_unchecked(_chat_channel_everyone, msg);
		}
	}
	(void)pitch;
	(void)roll;
	(void)g4;
}

void forge_network_send_transform(
	int32_t live_idx,
	float x, float y, float z,
	float yaw, float pitch, float roll)
{
	unsigned long now;
	char msg[64];

	if (!forge_is_multiplayer_session())
		return;
	now = system_milliseconds();
	if (now - g_last_transform_send_ms < 100)
		return;
	g_last_transform_send_ms = now;

	if (game_connection() == _game_connection_network_client)
	{
		snprintf(msg, sizeof(msg), "!FM:%d:%.1f:%.1f:%.1f:%.2f",
			(int)live_idx, x, y, z, yaw);
		network_chat_say_unchecked(_chat_channel_everyone, msg);
	}
	(void)pitch;
	(void)roll;
}

void forge_network_send_delete(int32_t live_idx)
{
	char msg[64];
	if (!forge_is_multiplayer_session())
		return;
	if (game_connection() == _game_connection_network_client)
	{
		snprintf(msg, sizeof(msg), "!FD:%d", (int)live_idx);
		network_chat_say_unchecked(_chat_channel_everyone, msg);
	}
}

int forge_network_intercept_chat_packet(long machine_index, char const *text, int is_from_client_to_host)
{
	(void)machine_index;
	if (!text || text[0] != '!' || text[1] != 'F')
		return 0;

	if (is_from_client_to_host && game_connection() == _game_connection_network_server)
	{
		if (strncmp(text, "!FS:", 4) == 0)
		{
			int cat_idx = -1, team = 0;
			float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
			if (sscanf(text + 4, "%d:%f:%f:%f:%f:%d", &cat_idx, &x, &y, &z, &yaw, &team) == 6)
			{
				if (cat_idx >= 0 && cat_idx < g_forge.catalog_count)
				{
					struct forge_catalog_entry const *ce = &g_forge.catalog[cat_idx];
					/* Enforce Multiplayer Asset Guard on authoritative host */
					if (ce->source_cache_id == 0)
					{
						forge_3d_spawn_object_at(
							ce->group_tag,
							ce->tag_path,
							0,
							x, y, z,
							yaw, 0.0f, 0.0f,
							(int16_t)team,
							0);
					}
				}
			}
			return 1;
		}
		else if (strncmp(text, "!FM:", 4) == 0)
		{
			int live_idx = -1;
			float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
			if (sscanf(text + 4, "%d:%f:%f:%f:%f", &live_idx, &x, &y, &z, &yaw) == 5)
			{
				forge_3d_move_instance(live_idx, x, y, z, yaw, 0.0f, 0.0f, 0);
			}
			return 1;
		}
		else if (strncmp(text, "!FD:", 4) == 0)
		{
			int live_idx = -1;
			if (sscanf(text + 4, "%d", &live_idx) == 1)
			{
				forge_3d_delete_instance(live_idx, 0);
			}
			return 1;
		}
	}

	return 1;
}
