/*
 * OpenCE Forge ("HaloCE Forge.exe")
 * Mode 1: Top-Down Overhead Editor View (F3)
 * Positions the in-engine camera floating above the map looking straight down (-Z),
 * sticks placement to the map floor by default via downward raycast, allows raising/lowering
 * height and rotating, and renders a shining target drop icon on the ground.
 */

#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "camera/observer.h"
#include "objects/objects.h"
#include "units/units.h"
#include "physics/collisions.h"
#include "render/render.h"
#include "rasterizer/rasterizer.h"
#include "interface/interface.h"
#include "text/draw_string.h"
#include "math/real_math.h"
#include <xtl.h>

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "forge_engine.h"

extern unsigned long system_milliseconds(void);
extern void halo_linux_mouse_look(short controller_index, float *yaw_delta, float *pitch_delta);

static int prev_bp_keys[256];

static int vkey_down(int vk)
{
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static int vkey_pressed(int vk)
{
	int down = vkey_down(vk);
	int prev = prev_bp_keys[vk & 0xFF];
	return (down && !prev);
}

static long get_small_ui_font(long fallback_font)
{
	long f = interface_get_tag_index(_interface_font_terminal);
	if (f == NONE)
		f = interface_get_tag_index(_interface_font_system);
	if (f == NONE)
		f = fallback_font;
	return f;
}

static void draw_ui_rect(short x0, short y0, short x1, short y1, uint32_t argb)
{
	forge_draw_screen_rect(x0, y0, x1, y1, argb);
}

static void draw_ui_box_outline(short x0, short y0, short x1, short y1, short thick, uint32_t argb)
{
	draw_ui_rect(x0, y0, x1, (short)(y0 + thick), argb);
	draw_ui_rect(x0, (short)(y1 - thick), x1, y1, argb);
	draw_ui_rect(x0, y0, (short)(x0 + thick), y1, argb);
	draw_ui_rect((short)(x1 - thick), y0, x1, y1, argb);
}

static void draw_small_text(long font_index, short x, short y, short w, short h, char const *ascii, real a, real r, real g, real b)
{
	wchar_t wbuf[256];
	rectangle2d bounds;
	real_argb_color col;
	int i = 0;

	if (!ascii || !ascii[0])
		return;
	while (ascii[i] && i < 254)
	{
		wbuf[i] = (wchar_t)(unsigned char)ascii[i];
		i++;
	}
	wbuf[i] = 0;

	col.alpha = a;
	col.red = r;
	col.green = g;
	col.blue = b;

	bounds.x0 = x;
	bounds.y0 = y;
	bounds.x1 = (short)(x + w);
	bounds.y1 = (short)(y + h);

	draw_string_set_draw_mode(get_small_ui_font(font_index), NONE, 0, 0, &col);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, wbuf);
}

static void compute_ground_height_at(float x, float y, float start_z, float *out_ground_z)
{
	struct collision_result collision;
	real_point3d origin;
	real_vector3d delta;
	unsigned long flags = FLAG(_collision_test_structure_bit) |
						  FLAG(_collision_test_front_facing_surfaces_bit);

	origin.x = x;
	origin.y = y;
	origin.z = start_z;
	delta.i = 0.0f;
	delta.j = 0.0f;
	delta.k = -250.0f;

	memset(&collision, 0, sizeof(collision));
	if (collision_test_vector(flags, &origin, &delta, NONE, &collision))
	{
		*out_ground_z = collision.point.z;
		return;
	}

	origin.z = start_z + 40.0f;
	memset(&collision, 0, sizeof(collision));
	if (collision_test_vector(flags, &origin, &delta, NONE, &collision))
	{
		*out_ground_z = collision.point.z;
		return;
	}
}

void forge_blueprint_enter(void)
{
	g_forge.mode = _forge_mode_2d_blueprint;
	g_forge.bp_center_x = g_forge.cam_x;
	g_forge.bp_center_y = g_forge.cam_y;
	g_forge.bp_ground_z = g_forge.cam_z - 1.0f;
	compute_ground_height_at(g_forge.bp_center_x, g_forge.bp_center_y, g_forge.cam_z + 5.0f, &g_forge.bp_ground_z);
	g_forge.bp_cam_z = g_forge.bp_ground_z + 14.0f;
	g_forge.bp_height_offset = 0.0f; /* Default to stick directly to the map floor */
	g_forge.bp_yaw = g_forge.cam_yaw;
	forge_set_status("Top-Down Overhead View: Mouse/WASD Move | Click/Enter Drop | Q/E Height | R Rotate | F2 Menu");
}

void forge_blueprint_update(float dt)
{
	float pan_speed;
	float dyaw = 0.0f, dpitch = 0.0f;
	float cy, sy;
	int vk;

	if (dt <= 0.0f || dt > 0.25f)
		dt = 0.016f;

	halo_linux_mouse_look(0, &dyaw, &dpitch);

	/* Right-mouse button + mouse X rotates camera yaw, otherwise mouse pans target smoothly */
	if (vkey_down(VK_RBUTTON))
	{
		g_forge.bp_yaw += dyaw;
	}
	else
	{
		cy = (float)cos((double)g_forge.bp_yaw);
		sy = (float)sin((double)g_forge.bp_yaw);
		g_forge.bp_center_x += (dpitch * cy + dyaw * sy) * 4.5f;
		g_forge.bp_center_y += (dpitch * sy - dyaw * cy) * 4.5f;
	}

	cy = (float)cos((double)g_forge.bp_yaw);
	sy = (float)sin((double)g_forge.bp_yaw);
	pan_speed = g_forge.cam_speed * (vkey_down(VK_SHIFT) ? 2.2f : 1.0f);

	if (vkey_down('W'))
	{
		g_forge.bp_center_x += cy * pan_speed * dt;
		g_forge.bp_center_y += sy * pan_speed * dt;
	}
	if (vkey_down('S'))
	{
		g_forge.bp_center_x -= cy * pan_speed * dt;
		g_forge.bp_center_y -= sy * pan_speed * dt;
	}
	if (vkey_down('A'))
	{
		g_forge.bp_center_x -= sy * pan_speed * dt;
		g_forge.bp_center_y += cy * pan_speed * dt;
	}
	if (vkey_down('D'))
	{
		g_forge.bp_center_x += sy * pan_speed * dt;
		g_forge.bp_center_y -= cy * pan_speed * dt;
	}

	/* Raycast straight down to stick target to map floor */
	compute_ground_height_at(g_forge.bp_center_x, g_forge.bp_center_y, g_forge.bp_cam_z, &g_forge.bp_ground_z);

	/* Adjust camera altitude above floor with [ / ] or - / = */
	if (vkey_down(VK_OEM_PLUS) || vkey_down(VK_OEM_6))
		g_forge.bp_cam_z -= 12.0f * dt;
	if (vkey_down(VK_OEM_MINUS) || vkey_down(VK_OEM_4))
		g_forge.bp_cam_z += 12.0f * dt;
	if (g_forge.bp_cam_z < g_forge.bp_ground_z + 3.5f)
		g_forge.bp_cam_z = g_forge.bp_ground_z + 3.5f;
	if (g_forge.bp_cam_z > g_forge.bp_ground_z + 65.0f)
		g_forge.bp_cam_z = g_forge.bp_ground_z + 65.0f;

	/* Move item height offset up/down relative to floor (defaults to 0 = stuck to floor) */
	if (vkey_down('E') || vkey_down(VK_UP) || vkey_down(VK_PRIOR))
		g_forge.bp_height_offset += 3.5f * dt;
	if (vkey_down('Q') || vkey_down(VK_DOWN) || vkey_down(VK_NEXT))
		g_forge.bp_height_offset -= 3.5f * dt;
	if (vkey_pressed('C') || vkey_pressed(VK_HOME))
	{
		g_forge.bp_height_offset = 0.0f;
		forge_set_status("Drop height snapped to map floor (Z +0.00)");
	}

	/* Rotate item before confirming placement */
	if (vkey_pressed('R'))
	{
		float step = (g_forge.snap_mode == _forge_snap_90_deg) ? 1.570796f :
					 (g_forge.snap_mode == _forge_snap_45_deg) ? 0.785398f :
					 (g_forge.snap_mode == _forge_snap_15_deg) ? 0.261799f : 0.087266f;
		g_forge.bp_item_yaw += step;
		if (g_forge.bp_item_yaw > 6.283185f)
			g_forge.bp_item_yaw -= 6.283185f;
	}
	if (vkey_pressed('G'))
	{
		g_forge.snap_mode = (enum forge_snap_angle_mode)((g_forge.snap_mode + 1) % NUMBER_OF_FORGE_SNAP_MODES);
	}

	/* Keep 3D camera synced to overhead position so switching back to F1 is seamless */
	g_forge.cam_x = g_forge.bp_center_x;
	g_forge.cam_y = g_forge.bp_center_y;
	g_forge.cam_z = g_forge.bp_ground_z + g_forge.bp_height_offset + 1.5f;

	/* Confirm placement with Enter or Left Mouse Button (when modal is closed) */
	if (g_forge.active_modal == _forge_modal_none &&
		(vkey_pressed(VK_RETURN) || vkey_pressed(VK_LBUTTON)))
	{
		int cur = 0;
		int i;
		for (i = 0; i < g_forge.catalog_count; i++)
		{
			struct forge_catalog_entry const *e = &g_forge.catalog[i];
			if (e->category != (uint8_t)g_forge.current_category)
				continue;
			if (!forge_catalog_entry_allowed(e))
				continue;
			if (cur == g_forge.palette_cursor)
			{
				float drop_z = g_forge.bp_ground_z + g_forge.bp_height_offset + 0.08f;
				forge_3d_spawn_object_at(
					e->group_tag,
					e->tag_path,
					e->source_cache_id,
					g_forge.bp_center_x,
					g_forge.bp_center_y,
					drop_z,
					g_forge.bp_item_yaw,
					0.0f,
					0.0f,
					(int16_t)g_forge.ai_team_mode,
					1);
				break;
			}
			cur++;
		}
	}

	for (vk = 0; vk < 256; vk++)
		prev_bp_keys[vk] = vkey_down(vk);
}

void forge_blueprint_apply_camera(void)
{
	static struct observer_command cmd;
	long unit_idx;

	memset(&cmd, 0, sizeof(cmd));
	cmd.flags = FLAG(_observer_command_valid_bit) | FLAG(_observer_command_ignore_obstructions_bit);
	cmd.focus_position.x = g_forge.bp_center_x;
	cmd.focus_position.y = g_forge.bp_center_y;
	cmd.focus_position.z = g_forge.bp_cam_z;
	cmd.focus_distance = 0.0f;
	cmd.field_of_view = 1.3962634f; /* 80 degrees */

	/* Look straight down (-Z) at the map */
	cmd.forward.i = 0.0f;
	cmd.forward.j = 0.0f;
	cmd.forward.k = -1.0f;

	cmd.up.i = (float)cos((double)g_forge.bp_yaw);
	cmd.up.j = (float)sin((double)g_forge.bp_yaw);
	cmd.up.k = 0.0f;

	observer_set_camera(0, &cmd);

	/* Keep player unit near the drop target so BSP PVS renders the ground directly below */
	unit_idx = player_control_get_unit_index(0);
	if (unit_idx != NONE)
	{
		struct unit_datum *u = (struct unit_datum *)object_get_and_verify_type(unit_idx, _object_mask_unit);
		if (u)
		{
			u->object.position.x = g_forge.bp_center_x;
			u->object.position.y = g_forge.bp_center_y;
			u->object.position.z = g_forge.bp_ground_z + g_forge.bp_height_offset + 0.35f;
			u->object.translational_velocity.i = 0.0f;
			u->object.translational_velocity.j = 0.0f;
			u->object.translational_velocity.k = 0.0f;
			SET_FLAG(u->object.flags, _object_connected_to_map_bit, FALSE);
		}
	}
}

void forge_blueprint_draw(
	short window_x0,
	short window_y0,
	short window_x1,
	short window_y1,
	long font_index,
	short line_height)
{
	uint32_t ms = (uint32_t)system_milliseconds();
	float pulse = (float)(ms % 1000u) / 1000.0f;
	short cx = (short)((window_x0 + window_x1) / 2);
	short cy = (short)((window_y0 + window_y1) / 2);
	short r_outer = (short)(10 + (int)(pulse * 12.0f));
	short r_inner = 4;
	short dir_dx, dir_dy;
	uint32_t pulse_alpha = (uint32_t)((0.92f - pulse * 0.65f) * 255.0f);
	uint32_t c_pulse = (pulse_alpha << 24) | 0x4CE0FFu;
	char line[256];
	char selected_name[80];
	int cur = 0, i;

	(void)line_height;

	snprintf(selected_name, sizeof(selected_name), "(None)");
	for (i = 0; i < g_forge.catalog_count; i++)
	{
		struct forge_catalog_entry const *e = &g_forge.catalog[i];
		if (e->category != (uint8_t)g_forge.current_category)
			continue;
		if (!forge_catalog_entry_allowed(e))
			continue;
		if (cur == g_forge.palette_cursor)
		{
			snprintf(selected_name, sizeof(selected_name), "%s", e->display_name);
			break;
		}
		cur++;
	}

	/* Shining, pulsing target drop icon at screen center (directly below overhead camera) */
	draw_ui_box_outline((short)(cx - r_outer), (short)(cy - r_outer), (short)(cx + r_outer), (short)(cy + r_outer), 2, c_pulse);
	draw_ui_box_outline((short)(cx - 9), (short)(cy - 9), (short)(cx + 9), (short)(cy + 9), 1, 0xFF73D9FFu);
	draw_ui_rect((short)(cx - r_inner), (short)(cy - r_inner), (short)(cx + r_inner), (short)(cy + r_inner), 0xFFFFEB66u);
	draw_ui_rect((short)(cx - 18), (short)(cy - 1), (short)(cx - 6), (short)(cy + 1), 0xFF73D9FFu);
	draw_ui_rect((short)(cx + 6), (short)(cy - 1), (short)(cx + 18), (short)(cy + 1), 0xFF73D9FFu);
	draw_ui_rect((short)(cx - 1), (short)(cy - 18), (short)(cx + 1), (short)(cy - 6), 0xFF73D9FFu);
	draw_ui_rect((short)(cx - 1), (short)(cy + 6), (short)(cx + 1), (short)(cy + 18), 0xFF73D9FFu);

	/* Facing direction indicator tick on the shining drop icon */
	dir_dx = (short)(cos((double)(g_forge.bp_item_yaw - g_forge.bp_yaw)) * 16.0);
	dir_dy = (short)(-sin((double)(g_forge.bp_item_yaw - g_forge.bp_yaw)) * 16.0);
	draw_ui_rect((short)(cx + dir_dx - 3), (short)(cy + dir_dy - 3), (short)(cx + dir_dx + 3), (short)(cy + dir_dy + 3), 0xFFFFD147u);

	/* Compact top HUD bar using native small terminal font */
	draw_ui_rect((short)(window_x0 + 12), (short)(window_y0 + 8), (short)(window_x1 - 12), (short)(window_y0 + 38), 0xC80A1428u);
	draw_ui_box_outline((short)(window_x0 + 12), (short)(window_y0 + 8), (short)(window_x1 - 12), (short)(window_y0 + 38), 1, 0xD84094EBu);

	snprintf(
		line,
		sizeof(line),
		"OVERHEAD DROP VIEW  |  Item: %s  |  Floor Z: %.2f  Height: %+.2f  Rot: %.0f deg",
		selected_name,
		g_forge.bp_ground_z,
		g_forge.bp_height_offset,
		(double)(g_forge.bp_item_yaw * 57.29578f));
	draw_small_text(font_index, (short)(window_x0 + 20), (short)(window_y0 + 11), (short)(window_x1 - window_x0 - 40), 14,
		line, 1.0f, 0.45f, 0.85f, 1.0f);

	draw_small_text(font_index, (short)(window_x0 + 20), (short)(window_y0 + 23), (short)(window_x1 - window_x0 - 40), 14,
		"Mouse/WASD: Move  |  Click/Enter: Drop Item  |  Q/E: Down/Up  |  C: Stick Floor  |  R: Rotate  |  F2: Menu  |  F1: 3D Monitor",
		1.0f, 0.90f, 0.94f, 1.0f);
}
