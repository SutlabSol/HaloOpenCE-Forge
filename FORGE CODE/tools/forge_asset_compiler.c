/*
 * OpenCE : Forge - Standalone Asset Compiler & Map Validator Tool
 * Builds:
 *   1. OpenCE Forge/forge_maps/forgeasset.map
 *   2. OpenCE Forge/forge_maps/customforgeasset.map
 *   3. OpenCE Forge/credits.md
 * And validates them against OpenCE's custom_edition_cache_load + tag_validate_custom_edition_tags
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge_cache_builder.h"

#undef fopen
#undef remove
#undef memset
#undef memcpy
#undef strlen
#undef strcpy
#undef strcmp
#undef strcat

FILE *halo_windows_fopen(char const *path, char const *mode) { return fopen(path, mode); }
int halo_windows_remove(char const *path) { return remove(path); }
void *csmemset(void *dst, int val, size_t sz) { return memset(dst, val, sz); }
void *csmemcpy(void *dst, void const *src, size_t sz) { return memcpy(dst, src, sz); }
long csstrlen(char const *s) { return (long)strlen(s); }
char *csstrcpy(char *d, char const *s) { return strcpy(d, s); }
int csstrcmp(char const *a, char const *b) { return strcmp(a, b); }
char *csstrcat(char *d, char const *s) { return strcat(d, s); }
#undef malloc
#undef free
#undef realloc
void *debug_malloc(size_t sz, int clear, char const *f, long l) { void *p = malloc(sz); if (p && clear) memset(p, 0, sz); return p; }
void debug_free(void *p, char const *f, long l) { free(p); }
void *debug_realloc(void *p, size_t sz, char const *f, long l) { return realloc(p, sz); }

void tag_validate_report(char const *message)
{
	if (message)
		printf("  [tag_validate] %s\n", message);
}

static void copy_file_if_missing(char const *src_path, char const *dst_path)
{
	FILE *in, *out;
	uint8_t buf[65536];
	size_t n;

	out = fopen(dst_path, "rb");
	if (out)
	{
		fclose(out);
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
	{
		fwrite(buf, 1, n, out);
	}
	fclose(out);
	fclose(in);
}

int main(int argc, char **argv)
{
	char const *ce_default_dir = "FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS";
	char const *pc_maps_dir = "FORGE CODE/GAME MAPS/PC MAPS";
	char const *ce_custom_dir = "FORGE CODE/GAME MAPS/CUSTOM EDITION MAPS";
	char const *forgeasset_out = "OpenCE Forge/forge_maps/forgeasset.map";
	char const *customforgeasset_out = "OpenCE Forge/forge_maps/customforgeasset.map";
	char const *credits_out = "OpenCE Forge/credits.md";
	int32_t tag_count = 0, corrs = 0;
	int injected = 0;
	int valid;
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (argc >= 3 && strcmp(argv[1], "--inject") == 0)
	{
		char const *donor_path = argv[2];
		printf("[Forge Asset Compiler] Injecting custom tags from '%s'...\n", donor_path);
		if (!forge_inject_custom_map_into_cache(
			donor_path,
			ce_default_dir,
			forgeasset_out,
			customforgeasset_out,
			credits_out,
			&injected))
		{
			fprintf(stderr, "[Forge Asset Compiler] ERROR: Failed to inject from '%s'\n", donor_path);
			return 1;
		}
		valid = forge_validate_custom_edition_map_file(customforgeasset_out, ce_default_dir, &tag_count, &corrs);
		printf("[Forge Asset Compiler] Injected %d root tags -> %s (valid=%d, total_tags=%d, corrections=%d)\n",
			injected, customforgeasset_out, valid, (int)tag_count, (int)corrs);
		return valid ? 0 : 1;
	}

	if (argc >= 2 && strcmp(argv[1], "--step4") == 0)
		goto step4;

	printf("[Forge Asset Compiler] Step 1: Copying CE resource maps (bitmaps.map, sounds.map, loc.map)...\n");
	copy_file_if_missing("FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bitmaps.map", "OpenCE Forge/forge_maps/bitmaps.map");
	copy_file_if_missing("FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/sounds.map", "OpenCE Forge/forge_maps/sounds.map");
	copy_file_if_missing("FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/loc.map", "OpenCE Forge/forge_maps/loc.map");
	copy_file_if_missing("FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bitmaps.map", "custom_maps/bitmaps.map");
	copy_file_if_missing("FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/sounds.map", "custom_maps/sounds.map");
	copy_file_if_missing("FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/loc.map", "custom_maps/loc.map");

	printf("[Forge Asset Compiler] Step 2: Harvesting CE Default Maps + PC Campaign Maps -> %s...\n", forgeasset_out);
	if (!forge_build_master_asset_cache(ce_default_dir, pc_maps_dir, forgeasset_out, credits_out))
	{
		fprintf(stderr, "[Forge Asset Compiler] ERROR: Failed to build %s\n", forgeasset_out);
		return 1;
	}

	valid = forge_validate_custom_edition_map_file(forgeasset_out, ce_default_dir, &tag_count, &corrs);
	printf("[Forge Asset Compiler] Validated %s: valid=%d, tags=%d, corrections=%d\n",
		forgeasset_out, valid, (int)tag_count, (int)corrs);

	printf("[Forge Asset Compiler] Step 3: Building initial %s (with sample injection from CUSTOM EDITION MAPS)...\n",
		customforgeasset_out);
	{
		char sample_donor[512];
		snprintf(sample_donor, sizeof(sample_donor), "%s/Launch_Bay_X.map", ce_custom_dir);
		remove(customforgeasset_out);
		if (!forge_inject_custom_map_into_cache(
			sample_donor,
			ce_default_dir,
			forgeasset_out,
			customforgeasset_out,
			credits_out,
			&injected))
		{
			fprintf(stderr, "[Forge Asset Compiler] ERROR: Failed to build %s\n", customforgeasset_out);
			return 1;
		}
		valid = forge_validate_custom_edition_map_file(customforgeasset_out, ce_default_dir, &tag_count, &corrs);
		printf("[Forge Asset Compiler] Validated %s (+%d root tags from Launch_Bay_X.map): valid=%d, tags=%d, corrections=%d\n",
			customforgeasset_out, injected, valid, (int)tag_count, (int)corrs);
	}

step4:
	printf("[Forge Asset Compiler] Step 4: Testing 2D Blueprint floorplan extraction & Save Copy As export...\n");
	{
		struct forge_map_blueprint_data *bp = (struct forge_map_blueprint_data *)calloc(1, sizeof(*bp));
		struct forge_scenario_edit_state *edits;
		char const *donors[2];

		if (bp && forge_load_map_blueprint("FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bloodgulch.map", bp))
		{
			printf("[Forge Asset Compiler] Extracted 2D Blueprint floorplan for '%s': %d edges, %d spawns, %d flags, X[%.1f..%.1f] Y[%.1f..%.1f]\n",
				bp->map_name,
				(int)bp->edge_count,
				(int)bp->initial_state.player_spawn_count,
				(int)bp->initial_state.netgame_flag_count,
				bp->min_x, bp->max_x, bp->min_y, bp->max_y);
			forge_free_map_blueprint(bp);
		}
		if (bp) { free(bp);
		}

		edits = (struct forge_scenario_edit_state *)calloc(1, sizeof(*edits));
		if (edits)
		{
			edits->placed_object_count = 6;
			edits->placed_objects[0].active = 1;
			edits->placed_objects[0].category = _forge_object_scenery;
			edits->placed_objects[0].group_tag = 'scen';
			snprintf(edits->placed_objects[0].tag_path, FORGE_TAG_PATH_MAX, "scenery\\c_storage\\c_storage");
			edits->placed_objects[0].x = 65.0f;
			edits->placed_objects[0].y = -120.0f;
			edits->placed_objects[0].z = 1.8f;

			edits->placed_objects[1].active = 1;
			edits->placed_objects[1].category = _forge_object_vehicle;
			edits->placed_objects[1].group_tag = 'vehi';
			snprintf(edits->placed_objects[1].tag_path, FORGE_TAG_PATH_MAX, "vehicles\\wraith\\wraith");
			edits->placed_objects[1].x = 70.0f;
			edits->placed_objects[1].y = -115.0f;
			edits->placed_objects[1].z = 2.2f;

			edits->placed_objects[2].active = 1;
			edits->placed_objects[2].category = _forge_object_scenery;
			edits->placed_objects[2].group_tag = 'mach';
			snprintf(edits->placed_objects[2].tag_path, FORGE_TAG_PATH_MAX, "levels\\a10\\devices\\doors\\door small\\door small");
			edits->placed_objects[2].x = 62.0f;
			edits->placed_objects[2].y = -118.0f;
			edits->placed_objects[2].z = 1.8f;

			edits->placed_objects[3].active = 1;
			edits->placed_objects[3].category = _forge_object_scenery;
			edits->placed_objects[3].group_tag = 'mach';
			snprintf(edits->placed_objects[3].tag_path, FORGE_TAG_PATH_MAX, "levels\\a30\\devices\\beam emitter\\beam emitter");
			edits->placed_objects[3].x = 60.0f;
			edits->placed_objects[3].y = -116.0f;
			edits->placed_objects[3].z = 1.8f;

			edits->placed_objects[4].active = 1;
			edits->placed_objects[4].category = _forge_object_scenery;
			edits->placed_objects[4].group_tag = 'mach';
			snprintf(edits->placed_objects[4].tag_path, FORGE_TAG_PATH_MAX, "levels\\a30\\devices\\beam emitter\\beam emitter red");
			edits->placed_objects[4].x = 58.0f;
			edits->placed_objects[4].y = -114.0f;
			edits->placed_objects[4].z = 1.8f;

			edits->placed_objects[5].active = 1;
			edits->placed_objects[5].category = _forge_object_scenery;
			edits->placed_objects[5].group_tag = 'lifi';
			snprintf(edits->placed_objects[5].tag_path, FORGE_TAG_PATH_MAX, "levels\\a10\\devices\\lights\\light fixture cryo");
			edits->placed_objects[5].x = 64.0f;
			edits->placed_objects[5].y = -112.0f;
			edits->placed_objects[5].z = 2.8f;

			donors[0] = forgeasset_out;
			donors[1] = customforgeasset_out;

			if (forge_export_scenario_map(
				"OpenCE Forge/forge_maps/bloodgulch.map",
				"maps",
				"custom_maps",
				donors,
				2,
				edits,
				"forge_bloodgulch_demo",
				"custom_maps"))
			{
				valid = forge_validate_custom_edition_map_file(
					"custom_maps/forge_bloodgulch_demo.map", "custom_maps", &tag_count, &corrs);
				printf("[Forge Asset Compiler] Exported custom_maps/forge_bloodgulch_demo.map (.bmp + .txt): valid=%d, tags=%d, corrections=%d\n",
					valid, (int)tag_count, (int)corrs);
				remove("custom_maps/forge_bloodgulch_demo.map");
				remove("forge_maps/forge_bloodgulch_demo/forge_bloodgulch_demo.bmp");
				remove("forge_maps/forge_bloodgulch_demo/forge_bloodgulch_demo.txt");
			}
			else
			{
				fprintf(stderr, "[Forge Asset Compiler] ERROR: forge_export_scenario_map returned 0!\n");
				free(edits);
				return 1;
			}
			free(edits);
		}
	}

	return 0;
}
