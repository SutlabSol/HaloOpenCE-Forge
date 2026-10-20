/*
FORGE_CACHE_BUILDER.C

Custom Edition (v609) and Halo PC (v7) cache parser, schema-driven tag cloner,
PC-to-CE bitmap/sound internalizer, gbxmodel geometry packer, credits.md
attribution generator, 2D blueprint collision extractor, and "Save Copy As..."
scenario exporter (.map, 140x114 .bmp, .txt).
*/

#include "cseries/cseries.h"
#include "forge_cache_builder.h"
#include "cache_file_formats.h"
#include "tag_schema.h"
#include "math/real_math.h"
#include "render/render.h"
#include "scenario/scenario_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "physics/collision_bsp_definitions.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define FORGE_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define FORGE_MKDIR(p) mkdir((p), 0755)
#endif

#undef malloc
#undef calloc
#undef realloc
#undef free
#undef fopen
#undef fclose
#undef fread
#undef fwrite
#undef fseek
#undef ftell
#undef remove
#undef rename

/* ---------- constants */

#define CACHE_HEADER_SIGNATURE 'head'
#define CACHE_FOOTER_SIGNATURE 'foot'
#define TAG_INDEX_SIGNATURE 'tags'
#define CACHE_VERSION_PC 7
#define CACHE_VERSION_CE 609

#define CE_TAG_BASE_ADDR 0x40440000UL
#define CE_TAG_MAX_BYTES 0x01700000UL

#define TAG_HEADER_SIZE 0x28
#define TAG_INSTANCE_SIZE 0x20

#define BITMAP_DATA_FLAGS_OFFSET 0x0E
#define BITMAP_DATA_PIXELS_OFFSET_OFFSET 0x18
#define BITMAP_DATA_PIXELS_SIZE_OFFSET 0x1C
#define BITMAP_DATA_TAG_INDEX_OFFSET 0x20
#define BITMAP_DATA_IN_RESOURCE_MAP_BIT 8
#define BITMAP_DATA_SIZE 0x30

#define SOUND_DEFINITON_SIZE 0xA4
#define SOUND_PROMOTION_SOUND_OFFSET 0x70
#define SOUND_PITCH_RANGES_OFFSET 0x98
#define SOUND_PITCH_RANGE_SIZE 0x48
#define SOUND_PITCH_RANGE_PERMUTATIONS_OFFSET 0x3C
#define SOUND_PERMUTATION_SIZE 0x7C
#define SOUND_PERMUTATION_SAMPLES_OFFSET 0x40
#define SOUND_PERMUTATION_TAG_INDEX0_OFFSET 0x34
#define SOUND_PERMUTATION_TAG_INDEX1_OFFSET 0x38
#define SOUND_SAMPLES_IN_RESOURCE_MAP_BIT 0

#define GBXMODEL_GEOMETRIES_OFFSET 0xD0
#define GBXMODEL_SHADERS_OFFSET 0xDC
#define GBXMODEL_NODES_OFFSET 0xB8
#define GBXMODEL_GEOMETRY_SIZE 0x30
#define GBXMODEL_GEOMETRY_PARTS_OFFSET 0x24
#define GBXMODEL_PART_SIZE 0x84
#define GBXMODEL_PART_UNCOMPRESSED_VERTS_OFFSET 0x20
#define GBXMODEL_PART_COMPRESSED_VERTS_OFFSET 0x2C
#define GBXMODEL_PART_TRIANGLES_OFFSET 0x38
#define GBXMODEL_PART_STRIP_COUNT_OFFSET 0x48
#define GBXMODEL_PART_STRIP_OFFSET_OFFSET 0x4C
#define GBXMODEL_PART_VERTEX_COUNT_OFFSET 0x58
#define GBXMODEL_PART_VERTEX_OFFSET_OFFSET 0x64
#define GBXMODEL_PART_LOCAL_NODE_COUNT_OFFSET 0x6B
#define GBXMODEL_PART_LOCAL_NODES_OFFSET 0x6C
#define GBXMODEL_VERTEX_SIZE 68

#define MAX_BUILDER_TAGS 16384
#define MAX_CREDITS_ENTRIES 4096

/* ---------- little-endian memory helpers */

static uint16_t read_u16(void const *p)
{
	uint8_t const *b = (uint8_t const *)p;
	return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static int16_t read_s16(void const *p)
{
	return (int16_t)read_u16(p);
}

static uint32_t read_u32(void const *p)
{
	uint8_t const *b = (uint8_t const *)p;
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int32_t read_s32(void const *p)
{
	return (int32_t)read_u32(p);
}

static float read_f32(void const *p)
{
	uint32_t bits = read_u32(p);
	float val;
	memcpy(&val, &bits, sizeof(val));
	return val;
}

static void write_u16(void *p, uint16_t v)
{
	uint8_t *b = (uint8_t *)p;
	b[0] = (uint8_t)(v & 0xFF);
	b[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_s16(void *p, int16_t v)
{
	write_u16(p, (uint16_t)v);
}

static void write_u32(void *p, uint32_t v)
{
	uint8_t *b = (uint8_t *)p;
	b[0] = (uint8_t)(v & 0xFF);
	b[1] = (uint8_t)((v >> 8) & 0xFF);
	b[2] = (uint8_t)((v >> 16) & 0xFF);
	b[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_s32(void *p, int32_t v)
{
	write_u32(p, (uint32_t)v);
}

static void write_f32(void *p, float v)
{
	uint32_t bits;
	memcpy(&bits, &v, sizeof(bits));
	write_u32(p, bits);
}

static uint32_t align4(uint32_t v)
{
	return (v + 3u) & ~3u;
}

/* ---------- CRC-32 (IEEE 802.3, matching OpenCE cache_file_formats.c) */

static uint32_t forge_crc32_update(uint32_t crc, void const *data, size_t size)
{
	uint8_t const *bytes = (uint8_t const *)data;
	size_t i;
	int bit;

	for (i = 0; i < size; i++)
	{
		crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++)
		{
			crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
		}
	}
	return crc;
}

/* ---------- growable byte buffer */

struct forge_byte_buf
{
	uint8_t *data;
	uint32_t size;
	uint32_t capacity;
};

static void buf_init(struct forge_byte_buf *b, uint32_t initial_cap)
{
	b->size = 0;
	b->capacity = initial_cap ? initial_cap : 65536;
	b->data = (uint8_t *)calloc(1, b->capacity);
}

static void buf_free(struct forge_byte_buf *b)
{
	if (b->data)
		free(b->data);
	b->data = NULL;
	b->size = 0;
	b->capacity = 0;
}

static uint32_t buf_alloc(struct forge_byte_buf *b, uint32_t bytes, uint32_t align)
{
	uint32_t offset = b->size;
	uint32_t needed;

	if (align > 1)
		offset = (offset + (align - 1)) & ~(align - 1);
	needed = offset + bytes;
	if (needed > b->capacity)
	{
		uint32_t new_cap = b->capacity ? b->capacity : 65536;
		uint8_t *new_data;
		while (new_cap < needed)
			new_cap *= 2;
		new_data = (uint8_t *)realloc(b->data, new_cap);
		if (!new_data)
			return 0;
		memset(new_data + b->capacity, 0, new_cap - b->capacity);
		b->data = new_data;
		b->capacity = new_cap;
	}
	if (offset > b->size)
		memset(b->data + b->size, 0, offset - b->size);
	memset(b->data + offset, 0, bytes);
	b->size = needed;
	return offset;
}

/* ---------- schema lookup */

static struct tag_schema_group const *forge_schema_group_get(uint32_t group_tag)
{
	struct tag_schema_group const *const *list;
	struct tag_schema_group const *group;

	for (group = tag_schema_custom_edition_groups; group->group_tag; group++)
	{
		if (group->group_tag == group_tag)
			return group;
	}
	for (list = tag_schema_group_lists; *list; list++)
	{
		for (group = *list; group->group_tag; group++)
		{
			if (group->group_tag == group_tag)
				return group;
		}
	}
	return NULL;
}

static int forge_groups_compatible(uint32_t group_a, uint32_t group_b)
{
	if (group_a == group_b)
		return 1;
	if ((group_a == 'mod2' && group_b == 'mode') || (group_a == 'mode' && group_b == 'mod2'))
		return 1;
	if ((group_a == 'scex' && group_b == 'schi') || (group_a == 'schi' && group_b == 'scex'))
		return 1;
	return 0;
}

static char const *forge_group_tag_ext(uint32_t group_tag)
{
	switch (group_tag)
	{
	case 'bipd': return "bipd";
	case 'vehi': return "vehi";
	case 'weap': return "weap";
	case 'eqip': return "eqip";
	case 'itmc': return "itmc";
	case 'scen': return "scen";
	case 'mach': return "mach";
	case 'ctrl': return "ctrl";
	case 'lifi': return "lifi";
	case 'actv': return "actv";
	case 'actr': return "actr";
	case 'antr': return "antr";
	case 'mod2':
	case 'mode': return "gbxmodel";
	case 'coll': return "model_collision_geometry";
	case 'phys': return "physics";
	case 'jpt!': return "damage_effect";
	case 'proj': return "projectile";
	case 'effe': return "effect";
	default: return "tag";
	}
}

/* ---------- source .map loader */

struct forge_source_map
{
	char path[512];
	char filename[128];
	char map_name[36];
	char author_credit[128];
	uint8_t *data;
	uint32_t file_size;
	int32_t version;
	int16_t scenario_type;
	uint32_t tag_data_offset;
	uint32_t tag_data_size;
	uint32_t instances_offset;
	int32_t scenario_tag_index;
	int32_t tag_count;
	uint32_t model_vertex_data_offset;
	uint32_t model_index_data_offset;
	uint32_t model_data_size;
};

static void forge_extract_basename(char const *path, char *out_name, size_t max_len)
{
	char const *slash1 = strrchr(path, '/');
	char const *slash2 = strrchr(path, '\\');
	char const *base = slash1 > slash2 ? slash1 + 1 : (slash2 ? slash2 + 1 : path);
	snprintf(out_name, max_len, "%s", base);
}

static void forge_discover_map_author(char const *map_path, char const *filename, int32_t version, char *out_author, size_t max_len)
{
	char txt_path[512];
	char *dot;
	FILE *f;

	/* Stock Halo PC / Custom Edition retail maps */
	if (version == CACHE_VERSION_PC ||
		_stricmp(filename, "beavercreek.map") == 0 ||
		_stricmp(filename, "bloodgulch.map") == 0 ||
		_stricmp(filename, "boardingaction.map") == 0 ||
		_stricmp(filename, "chillout.map") == 0 ||
		_stricmp(filename, "chirone34.map") == 0 ||
		_stricmp(filename, "damnation.map") == 0 ||
		_stricmp(filename, "danger_canyon.map") == 0 ||
		_stricmp(filename, "deathisland.map") == 0 ||
		_stricmp(filename, "derelict.map") == 0 ||
		_stricmp(filename, "gephyrophobia.map") == 0 ||
		_stricmp(filename, "hangemhigh.map") == 0 ||
		_stricmp(filename, "icefields.map") == 0 ||
		_stricmp(filename, "infinity.map") == 0 ||
		_stricmp(filename, "longest.map") == 0 ||
		_stricmp(filename, "prisoner.map") == 0 ||
		_stricmp(filename, "putput.map") == 0 ||
		_stricmp(filename, "ratrace.map") == 0 ||
		_stricmp(filename, "sidewinder.map") == 0 ||
		_stricmp(filename, "timberland.map") == 0 ||
		_stricmp(filename, "wizard.map") == 0)
	{
		snprintf(out_author, max_len, "Bungie / Gearbox Software");
		return;
	}

	/* Check accompanying .txt file next to the custom map */
	snprintf(txt_path, sizeof(txt_path), "%s", map_path);
	dot = strrchr(txt_path, '.');
	if (dot)
	{
		strcpy(dot, ".txt");
		f = fopen(txt_path, "rb");
		if (f)
		{
			char buf[1024];
			size_t n = fread(buf, 1, sizeof(buf) - 1, f);
			fclose(f);
			buf[n] = 0;
			/* Look for "Author:" or "By:" or "Creator:" */
			{
				char const *keys[] = { "Author:", "author:", "By:", "by:", "Created by:", "Creator:" };
				int k;
				for (k = 0; k < 6; k++)
				{
					char const *hit = strstr(buf, keys[k]);
					if (hit)
					{
						char const *p = hit + strlen(keys[k]);
						size_t i = 0;
						while (*p == ' ' || *p == '\t')
							p++;
						while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < max_len)
						{
							out_author[i] = p[i];
							i++;
						}
						out_author[i] = 0;
						if (i > 0)
							return;
					}
				}
			}
		}
	}
	snprintf(out_author, max_len, "its original creator");
}

static int forge_source_map_open(char const *path, struct forge_source_map *map)
{
	FILE *f;
	long sz;
	uint32_t tag_hdr_off;
	uint32_t inst_addr;

	memset(map, 0, sizeof(*map));
	snprintf(map->path, sizeof(map->path), "%s", path);
	forge_extract_basename(path, map->filename, sizeof(map->filename));

	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0x800)
	{
		fclose(f);
		return 0;
	}
	map->file_size = (uint32_t)sz;
	map->data = (uint8_t *)malloc(map->file_size);
	if (!map->data)
	{
		fclose(f);
		return 0;
	}
	fseek(f, 0, SEEK_SET);
	if (fread(map->data, 1, map->file_size, f) != map->file_size)
	{
		free(map->data);
		map->data = NULL;
		fclose(f);
		return 0;
	}
	fclose(f);

	if (read_u32(map->data) != CACHE_HEADER_SIGNATURE ||
		read_u32(map->data + 0x7FC) != CACHE_FOOTER_SIGNATURE)
	{
		free(map->data);
		map->data = NULL;
		return 0;
	}
	map->version = read_s32(map->data + 0x04);
	if (map->version != CACHE_VERSION_PC && map->version != CACHE_VERSION_CE)
	{
		free(map->data);
		map->data = NULL;
		return 0;
	}
	map->tag_data_offset = read_u32(map->data + 0x10);
	map->tag_data_size = read_u32(map->data + 0x14);
	memcpy(map->map_name, map->data + 0x20, 32);
	map->map_name[31] = 0;
	map->scenario_type = read_s16(map->data + 0x60);

	if (map->tag_data_offset < 0x800 ||
		map->tag_data_size < TAG_HEADER_SIZE ||
		map->tag_data_offset + map->tag_data_size > map->file_size)
	{
		free(map->data);
		map->data = NULL;
		return 0;
	}
	tag_hdr_off = map->tag_data_offset;
	if (read_u32(map->data + tag_hdr_off + 0x24) != TAG_INDEX_SIGNATURE)
	{
		free(map->data);
		map->data = NULL;
		return 0;
	}
	inst_addr = read_u32(map->data + tag_hdr_off + 0x00);
	map->scenario_tag_index = read_s32(map->data + tag_hdr_off + 0x04) & 0xFFFF;
	map->tag_count = read_s32(map->data + tag_hdr_off + 0x0C);
	map->model_vertex_data_offset = read_u32(map->data + tag_hdr_off + 0x14);
	map->model_index_data_offset = read_u32(map->data + tag_hdr_off + 0x1C);
	map->model_data_size = read_u32(map->data + tag_hdr_off + 0x20);

	if (inst_addr < CE_TAG_BASE_ADDR ||
		map->tag_count <= 0 ||
		map->tag_count > 65535 ||
		(inst_addr - CE_TAG_BASE_ADDR) + (uint32_t)map->tag_count * TAG_INSTANCE_SIZE > map->tag_data_size)
	{
		free(map->data);
		map->data = NULL;
		return 0;
	}
	map->instances_offset = inst_addr - CE_TAG_BASE_ADDR;
	forge_discover_map_author(path, map->filename, map->version, map->author_credit, sizeof(map->author_credit));
	return 1;
}

static void forge_source_map_close(struct forge_source_map *map)
{
	if (map->data)
		free(map->data);
	memset(map, 0, sizeof(*map));
}

static uint8_t const *src_tag_instance(struct forge_source_map const *map, int32_t idx)
{
	if (idx < 0 || idx >= map->tag_count)
		return NULL;
	return map->data + map->tag_data_offset + map->instances_offset + (uint32_t)idx * TAG_INSTANCE_SIZE;
}

static char const *src_tag_name(struct forge_source_map const *map, int32_t idx)
{
	uint8_t const *inst = src_tag_instance(map, idx);
	uint32_t name_addr;
	uint32_t off;
	if (!inst)
		return NULL;
	name_addr = read_u32(inst + 0x10);
	if (name_addr < CE_TAG_BASE_ADDR)
		return NULL;
	off = name_addr - CE_TAG_BASE_ADDR;
	if (off >= map->tag_data_size)
		return NULL;
	if (!memchr(map->data + map->tag_data_offset + off, 0, map->tag_data_size - off))
		return NULL;
	return (char const *)(map->data + map->tag_data_offset + off);
}

static uint8_t const *src_ptr(struct forge_source_map const *map, uint32_t addr, uint32_t bytes)
{
	uint32_t off;
	if (addr < CE_TAG_BASE_ADDR)
		return NULL;
	off = addr - CE_TAG_BASE_ADDR;
	if (off > map->tag_data_size || bytes > map->tag_data_size - off)
		return NULL;
	return map->data + map->tag_data_offset + off;
}

/* ---------- resource map (bitmaps.map, sounds.map, loc.map) reader */

struct forge_resource_map
{
	uint8_t *data;
	uint32_t file_size;
	uint32_t resource_type;
	uint32_t names_offset;
	uint32_t items_offset;
	int32_t item_count;
};

static int forge_resource_map_open_file(char const *path, struct forge_resource_map *rm)
{
	FILE *f;
	long sz;

	memset(rm, 0, sizeof(*rm));
	if (!path || !path[0])
		return 0;
	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 16)
	{
		fclose(f);
		return 0;
	}
	rm->file_size = (uint32_t)sz;
	rm->data = (uint8_t *)malloc(rm->file_size);
	if (!rm->data)
	{
		fclose(f);
		return 0;
	}
	fseek(f, 0, SEEK_SET);
	if (fread(rm->data, 1, rm->file_size, f) != rm->file_size)
	{
		free(rm->data);
		rm->data = NULL;
		fclose(f);
		return 0;
	}
	fclose(f);

	rm->resource_type = read_u32(rm->data + 0x00);
	rm->names_offset = read_u32(rm->data + 0x04);
	rm->items_offset = read_u32(rm->data + 0x08);
	rm->item_count = read_s32(rm->data + 0x0C);
	if (rm->resource_type < 1 || rm->resource_type > 3 || rm->item_count <= 0 ||
		rm->names_offset >= rm->file_size ||
		(uint64_t)rm->items_offset + (uint64_t)rm->item_count * 12u > rm->file_size)
	{
		free(rm->data);
		memset(rm, 0, sizeof(*rm));
		return 0;
	}
	return 1;
}

static void forge_resource_map_close_file(struct forge_resource_map *rm)
{
	if (!rm)
		return;
	if (rm->data)
		free(rm->data);
	memset(rm, 0, sizeof(*rm));
}

static int32_t forge_resource_map_find_item(struct forge_resource_map const *rm, char const *tag_name)
{
	int32_t i;
	if (!rm || !rm->data || !tag_name)
		return -1;
	for (i = 0; i < rm->item_count; i++)
	{
		uint32_t item_off = rm->items_offset + (uint32_t)i * 12u;
		uint32_t name_off = read_u32(rm->data + item_off + 0x00);
		if (rm->names_offset + name_off < rm->file_size)
		{
			char const *entry_name = (char const *)(rm->data + rm->names_offset + name_off);
			if (strcmp(entry_name, tag_name) == 0)
				return i;
		}
	}
	return -1;
}

/* ---------- destination cache builder state */

#define BUILDER_INSTANCES_OFFSET 0x000028u
#define BUILDER_MAX_TAGS         8192
#define BUILDER_STRINGS_OFFSET   (BUILDER_INSTANCES_OFFSET + (uint32_t)BUILDER_MAX_TAGS * TAG_INSTANCE_SIZE)
#define BUILDER_STRINGS_CAPACITY 0x080000u
#define BUILDER_BODY_OFFSET      (BUILDER_STRINGS_OFFSET + BUILDER_STRINGS_CAPACITY)

struct forge_raw_patch_entry
{
	uint32_t tag_buf_offset;
	uint32_t raw_buf_offset;
};

struct forge_credit_record
{
	uint32_t group_tag;
	char tag_path[FORGE_TAG_PATH_MAX];
	char source_map[128];
	char author[128];
};

struct forge_dest_tag
{
	uint32_t primary_group;
	uint32_t secondary_group;
	uint32_t tertiary_group;
	uint32_t handle;
	uint32_t name_address;
	uint32_t base_address;
	int32_t in_resource_map;
	int32_t pad;
	char *name;
	uint8_t cloned;
};

struct forge_builder
{
	struct forge_source_map const *base_map;
	struct forge_resource_map ce_bitmaps;
	struct forge_resource_map ce_sounds;
	struct forge_resource_map ce_loc;
	struct forge_resource_map pc_bitmaps;
	struct forge_resource_map pc_sounds;

	struct forge_dest_tag *tags;
	int32_t tag_count;
	uint32_t strings_used;

	struct forge_byte_buf tag_buf;
	struct forge_byte_buf raw_buf;
	struct forge_byte_buf model_verts_buf;
	struct forge_byte_buf model_strips_buf;
	uint32_t model_part_count;

	struct forge_raw_patch_entry *raw_patches;
	int32_t raw_patch_count;
	int32_t raw_patch_cap;

	struct forge_credit_record *credits;
	int32_t credit_count;
	int32_t credit_cap;
};

static void builder_add_raw_patch(struct forge_builder *b, uint32_t tag_buf_off, uint32_t raw_buf_off)
{
	if (b->raw_patch_count >= b->raw_patch_cap)
	{
		int32_t ncap = b->raw_patch_cap ? b->raw_patch_cap * 2 : 512;
		struct forge_raw_patch_entry *np = (struct forge_raw_patch_entry *)realloc(
			b->raw_patches, (size_t)ncap * sizeof(*np));
		if (!np)
			return;
		b->raw_patches = np;
		b->raw_patch_cap = ncap;
	}
	b->raw_patches[b->raw_patch_count].tag_buf_offset = tag_buf_off;
	b->raw_patches[b->raw_patch_count].raw_buf_offset = raw_buf_off;
	b->raw_patch_count++;
}

static void builder_add_credit(
	struct forge_builder *b,
	uint32_t group_tag,
	char const *tag_path,
	char const *source_map,
	char const *author)
{
	int32_t i;
	struct forge_credit_record *rec;
	if (!b || !tag_path || !tag_path[0])
		return;
	for (i = 0; i < b->credit_count; i++)
	{
		if (b->credits[i].group_tag == group_tag &&
			_stricmp(b->credits[i].tag_path, tag_path) == 0)
			return;
	}
	if (b->credit_count >= b->credit_cap)
	{
		int32_t ncap = b->credit_cap ? b->credit_cap * 2 : 512;
		struct forge_credit_record *nc = (struct forge_credit_record *)realloc(
			b->credits, (size_t)ncap * sizeof(*nc));
		if (!nc)
			return;
		b->credits = nc;
		b->credit_cap = ncap;
	}
	rec = &b->credits[b->credit_count++];
	memset(rec, 0, sizeof(*rec));
	rec->group_tag = group_tag;
	snprintf(rec->tag_path, sizeof(rec->tag_path), "%s", tag_path);
	snprintf(rec->source_map, sizeof(rec->source_map), "%s", source_map ? source_map : "unknown.map");
	snprintf(rec->author, sizeof(rec->author), "%s", (author && author[0]) ? author : "its original creator");
}

static uint32_t builder_alloc_string(struct forge_builder *b, char const *str)
{
	uint32_t len = str ? (uint32_t)strlen(str) + 1u : 1u;
	uint32_t off;
	if (b->strings_used + len > BUILDER_STRINGS_CAPACITY)
		return CE_TAG_BASE_ADDR + BUILDER_STRINGS_OFFSET;
	off = BUILDER_STRINGS_OFFSET + b->strings_used;
	if (str && len > 1u)
		memcpy(b->tag_buf.data + off, str, len);
	else
		b->tag_buf.data[off] = 0;
	b->strings_used += len;
	return CE_TAG_BASE_ADDR + off;
}

static int32_t builder_find_tag(struct forge_builder const *b, uint32_t group_tag, char const *name)
{
	int32_t i;
	if (!b || !name)
		return -1;
	for (i = 0; i < b->tag_count; i++)
	{
		if (forge_groups_compatible(b->tags[i].primary_group, group_tag) &&
			b->tags[i].name &&
			_stricmp(b->tags[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

static int32_t src_map_find_tag(struct forge_source_map const *map, uint32_t group_tag, char const *name)
{
	int32_t i;
	if (!map || !map->data || !name)
		return -1;
	for (i = 0; i < map->tag_count; i++)
	{
		uint8_t const *inst = src_tag_instance(map, i);
		char const *tname = src_tag_name(map, i);
		if (inst && tname &&
			forge_groups_compatible(read_u32(inst + 0x00), group_tag) &&
			_stricmp(tname, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

static void builder_init(
	struct forge_builder *b,
	struct forge_source_map const *base_map,
	char const *ce_res_dir,
	char const *pc_res_dir)
{
	char path[512];
	int32_t i;

	memset(b, 0, sizeof(*b));
	b->base_map = base_map;
	b->tags = (struct forge_dest_tag *)calloc(BUILDER_MAX_TAGS, sizeof(*b->tags));

	if (ce_res_dir && ce_res_dir[0])
	{
		snprintf(path, sizeof(path), "%s/bitmaps.map", ce_res_dir);
		forge_resource_map_open_file(path, &b->ce_bitmaps);
		snprintf(path, sizeof(path), "%s/sounds.map", ce_res_dir);
		forge_resource_map_open_file(path, &b->ce_sounds);
		snprintf(path, sizeof(path), "%s/loc.map", ce_res_dir);
		forge_resource_map_open_file(path, &b->ce_loc);
	}
	if (pc_res_dir && pc_res_dir[0])
	{
		snprintf(path, sizeof(path), "%s/bitmaps.map", pc_res_dir);
		forge_resource_map_open_file(path, &b->pc_bitmaps);
		snprintf(path, sizeof(path), "%s/sounds.map", pc_res_dir);
		forge_resource_map_open_file(path, &b->pc_sounds);
	}

	buf_init(&b->tag_buf, BUILDER_BODY_OFFSET + 0x200000u);
	buf_alloc(&b->tag_buf, BUILDER_BODY_OFFSET, 4);
	buf_init(&b->raw_buf, 0x100000u);
	buf_init(&b->model_verts_buf, 0x100000u);
	buf_init(&b->model_strips_buf, 0x80000u);

	if (base_map)
	{
		for (i = 0; i < base_map->tag_count && i < BUILDER_MAX_TAGS; i++)
		{
			uint8_t const *inst = src_tag_instance(base_map, i);
			char const *tname = src_tag_name(base_map, i);
			struct forge_dest_tag *dt = &b->tags[i];
			dt->primary_group = read_u32(inst + 0x00);
			dt->secondary_group = read_u32(inst + 0x04);
			dt->tertiary_group = read_u32(inst + 0x08);
			dt->handle = read_u32(inst + 0x0C);
			dt->name = _strdup(tname ? tname : "");
			dt->name_address = builder_alloc_string(b, dt->name);
			dt->base_address = read_u32(inst + 0x14);
			dt->in_resource_map = read_s32(inst + 0x18);
			dt->pad = 0;
			dt->cloned = 0;
			b->tag_count++;
		}
	}
}

static void builder_free(struct forge_builder *b)
{
	int32_t i;
	if (!b)
		return;
	for (i = 0; i < b->tag_count; i++)
	{
		if (b->tags[i].name)
			free(b->tags[i].name);
	}
	forge_resource_map_close_file(&b->ce_bitmaps);
	forge_resource_map_close_file(&b->ce_sounds);
	forge_resource_map_close_file(&b->ce_loc);
	forge_resource_map_close_file(&b->pc_bitmaps);
	forge_resource_map_close_file(&b->pc_sounds);
	buf_free(&b->tag_buf);
	buf_free(&b->raw_buf);
	buf_free(&b->model_verts_buf);
	buf_free(&b->model_strips_buf);
	if (b->tags) free(b->tags);
	if (b->raw_patches)
		free(b->raw_patches);
	if (b->credits)
		free(b->credits);
	memset(b, 0, sizeof(*b));
}

/* ---------- schema-driven recursive cloner & resource internalizers */

static int32_t builder_ensure_tag(
	struct forge_builder *b,
	struct forge_source_map const *src_map,
	int32_t src_idx);

static void builder_clone_definition(
	struct forge_builder *b,
	struct forge_source_map const *src_map,
	struct tag_schema_definition const *def,
	uint32_t src_off,
	uint32_t dst_off,
	int32_t element_count)
{
	int32_t elem;
	struct tag_schema_field const *field;

	if (!def || !def->fields || element_count <= 0)
		return;

	for (elem = 0; elem < element_count; elem++)
	{
		uint32_t e_src = src_off + (uint32_t)elem * (uint32_t)def->size;
		uint32_t e_dst = dst_off + (uint32_t)elem * (uint32_t)def->size;

		for (field = def->fields; field->type != _tag_schema_terminator; field++)
		{
			int16_t k;
			int16_t fcount = field->count > 0 ? field->count : 1;
			uint32_t fstride = field->size > 0 ? (uint32_t)field->size : 0;

			for (k = 0; k < fcount; k++)
			{
				uint32_t f_src = e_src + (uint32_t)field->offset + (uint32_t)k * fstride;
				uint32_t f_dst = e_dst + (uint32_t)field->offset + (uint32_t)k * fstride;

				if (field->type == _tag_schema_struct)
				{
					struct tag_schema_definition const *sub_def =
						(struct tag_schema_definition const *)field->definition;
					if (sub_def)
						builder_clone_definition(b, src_map, sub_def, f_src, f_dst, 1);
				}
				else if (field->type == _tag_schema_block)
				{
					struct tag_schema_definition const *child_def =
						(struct tag_schema_definition const *)field->definition;
					int32_t cnt = read_s32(src_map->data + src_map->tag_data_offset + f_src + 0);
					uint32_t addr = read_u32(src_map->data + src_map->tag_data_offset + f_src + 4);
					write_u32(b->tag_buf.data + f_dst + 8, 0);

					if (cnt <= 0 || !child_def || child_def->size <= 0 || addr < CE_TAG_BASE_ADDR)
					{
						write_s32(b->tag_buf.data + f_dst + 0, 0);
						write_u32(b->tag_buf.data + f_dst + 4, 0);
					}
					else
					{
						uint32_t child_src_off = addr - CE_TAG_BASE_ADDR;
						uint64_t total_bytes = (uint64_t)(uint32_t)cnt * (uint64_t)(uint32_t)child_def->size;
						if (field->maximum > 0 && !(field->flags & (1 << _tag_schema_tool_maximum_bit)) &&
							cnt > field->maximum)
						{
							cnt = field->maximum;
							total_bytes = (uint64_t)(uint32_t)cnt * (uint64_t)(uint32_t)child_def->size;
						}
						if ((uint64_t)child_src_off + total_bytes > src_map->tag_data_size)
						{
							write_s32(b->tag_buf.data + f_dst + 0, 0);
							write_u32(b->tag_buf.data + f_dst + 4, 0);
						}
						else
						{
							uint32_t child_dst_off = buf_alloc(&b->tag_buf, (uint32_t)total_bytes, 4);
							memcpy(
								b->tag_buf.data + child_dst_off,
								src_map->data + src_map->tag_data_offset + child_src_off,
								(size_t)total_bytes);
							write_s32(b->tag_buf.data + f_dst + 0, cnt);
							write_u32(b->tag_buf.data + f_dst + 4, CE_TAG_BASE_ADDR + child_dst_off);
							builder_clone_definition(b, src_map, child_def, child_src_off, child_dst_off, cnt);
						}
					}
				}
				else if (field->type == _tag_schema_data)
				{
					int32_t sz = read_s32(src_map->data + src_map->tag_data_offset + f_src + 0);
					uint32_t addr = read_u32(src_map->data + src_map->tag_data_offset + f_src + 12);
					write_u32(b->tag_buf.data + f_dst + 16, 0);

					if (sz <= 0 || addr < CE_TAG_BASE_ADDR)
					{
						memset(b->tag_buf.data + f_dst, 0, 20);
					}
					else
					{
						uint32_t td_src_off = addr - CE_TAG_BASE_ADDR;
						if ((uint64_t)td_src_off + (uint32_t)sz > src_map->tag_data_size)
						{
							memset(b->tag_buf.data + f_dst, 0, 20);
						}
						else
						{
							uint32_t td_dst_off = buf_alloc(&b->tag_buf, (uint32_t)sz, 4);
							memcpy(
								b->tag_buf.data + td_dst_off,
								src_map->data + src_map->tag_data_offset + td_src_off,
								(size_t)sz);
							write_s32(b->tag_buf.data + f_dst + 0, sz);
							write_u32(b->tag_buf.data + f_dst + 12, CE_TAG_BASE_ADDR + td_dst_off);
						}
					}
				}
				else if (field->type == _tag_schema_reference)
				{
					uint32_t ref_group = read_u32(src_map->data + src_map->tag_data_offset + f_src + 0);
					int32_t ref_handle = read_s32(src_map->data + src_map->tag_data_offset + f_src + 12);

					if (ref_handle == -1 || ref_group == 0xFFFFFFFFu || ref_group == 0 ||
						(ref_group == 'sbsp' && src_map != b->base_map) ||
						(ref_group == 'udlg' && src_map != b->base_map))
					{
						write_u32(b->tag_buf.data + f_dst + 4, 0);
						write_s32(b->tag_buf.data + f_dst + 8, 0);
						write_s32(b->tag_buf.data + f_dst + 12, -1);
					}
					else
					{
						int32_t src_ref_idx = ref_handle & 0xFFFF;
						int32_t dst_ref_idx = builder_ensure_tag(b, src_map, src_ref_idx);
						if (dst_ref_idx < 0)
						{
							write_u32(b->tag_buf.data + f_dst + 4, 0);
							write_s32(b->tag_buf.data + f_dst + 8, 0);
							write_s32(b->tag_buf.data + f_dst + 12, -1);
						}
						else
						{
							write_u32(b->tag_buf.data + f_dst + 0, b->tags[dst_ref_idx].primary_group);
							write_u32(b->tag_buf.data + f_dst + 4, b->tags[dst_ref_idx].name_address);
							write_s32(b->tag_buf.data + f_dst + 8, 0);
							write_u32(b->tag_buf.data + f_dst + 12, b->tags[dst_ref_idx].handle);
						}
					}
				}
				else if (field->type == _tag_schema_tag_index)
				{
					int32_t ref_handle = read_s32(src_map->data + src_map->tag_data_offset + f_src);
					if (ref_handle == -1)
					{
						write_s32(b->tag_buf.data + f_dst, -1);
					}
					else
					{
						int32_t src_ref_idx = ref_handle & 0xFFFF;
						int32_t dst_ref_idx = builder_ensure_tag(b, src_map, src_ref_idx);
						write_s32(b->tag_buf.data + f_dst, dst_ref_idx >= 0 ? (int32_t)b->tags[dst_ref_idx].handle : -1);
					}
				}
			}
		}
	}
}

static void builder_pack_mod2_geometry(
	struct forge_builder *b,
	struct forge_source_map const *src_map,
	uint32_t dst_mod2_off)
{
	int32_t node_count = read_s32(b->tag_buf.data + dst_mod2_off + GBXMODEL_NODES_OFFSET);
	int32_t node_limit = node_count > 0 ? node_count : 1;
	int32_t geom_count = read_s32(b->tag_buf.data + dst_mod2_off + GBXMODEL_GEOMETRIES_OFFSET + 0);
	uint32_t geom_addr = read_u32(b->tag_buf.data + dst_mod2_off + GBXMODEL_GEOMETRIES_OFFSET + 4);
	int32_t g;

	if (geom_count <= 0 || geom_addr < CE_TAG_BASE_ADDR)
		return;

	for (g = 0; g < geom_count; g++)
	{
		uint32_t geom_off = (geom_addr - CE_TAG_BASE_ADDR) + (uint32_t)g * GBXMODEL_GEOMETRY_SIZE;
		int32_t part_count;
		uint32_t part_addr;
		int32_t p;

		if (geom_off + GBXMODEL_GEOMETRY_SIZE > b->tag_buf.size)
			break;
		part_count = read_s32(b->tag_buf.data + geom_off + GBXMODEL_GEOMETRY_PARTS_OFFSET + 0);
		part_addr = read_u32(b->tag_buf.data + geom_off + GBXMODEL_GEOMETRY_PARTS_OFFSET + 4);
		if (part_count <= 0 || part_addr < CE_TAG_BASE_ADDR)
			continue;

		for (p = 0; p < part_count; p++)
		{
			uint32_t part_off = (part_addr - CE_TAG_BASE_ADDR) + (uint32_t)p * GBXMODEL_PART_SIZE;
			uint32_t strip_tri_count, strip_src_rel, vert_count, vert_src_rel;
			uint32_t vert_bytes, strip_index_count, strip_bytes;
			uint64_t src_vert_foff, src_strip_foff;

			if (part_off + GBXMODEL_PART_SIZE > b->tag_buf.size)
				break;

			strip_tri_count = read_u32(b->tag_buf.data + part_off + GBXMODEL_PART_STRIP_COUNT_OFFSET);
			strip_src_rel = read_u32(b->tag_buf.data + part_off + GBXMODEL_PART_STRIP_OFFSET_OFFSET);
			vert_count = read_u32(b->tag_buf.data + part_off + GBXMODEL_PART_VERTEX_COUNT_OFFSET);
			vert_src_rel = read_u32(b->tag_buf.data + part_off + GBXMODEL_PART_VERTEX_OFFSET_OFFSET);

			/* Zero embedded vertex/triangle tag_blocks at +0x24..+0x47 */
			memset(b->tag_buf.data + part_off + GBXMODEL_PART_UNCOMPRESSED_VERTS_OFFSET, 0, 0x24u);

			if (vert_count > 0 && strip_tri_count > 0)
			{
				vert_bytes = vert_count * GBXMODEL_VERTEX_SIZE;
				strip_index_count = strip_tri_count + 2u;
				strip_bytes = strip_index_count * 2u;
				src_vert_foff = (uint64_t)src_map->model_vertex_data_offset + vert_src_rel;
				src_strip_foff = (uint64_t)src_map->model_vertex_data_offset +
					(uint64_t)src_map->model_index_data_offset + strip_src_rel;

				if (src_vert_foff + vert_bytes <= src_map->file_size &&
					src_strip_foff + strip_bytes <= src_map->file_size)
				{
					uint32_t dst_vert_rel = buf_alloc(&b->model_verts_buf, vert_bytes, 4);
					uint32_t dst_strip_rel;
					uint32_t v;

					memcpy(b->model_verts_buf.data + dst_vert_rel, src_map->data + src_vert_foff, vert_bytes);
					for (v = 0; v < vert_count; v++)
					{
						uint8_t *vp = b->model_verts_buf.data + dst_vert_rel + v * GBXMODEL_VERTEX_SIZE;
						int c;
						int16_t n0, n1;
						float w0, w1;
						for (c = 3; c < 12; c++)
						{
							float f = read_f32(vp + c * 4);
							if (f != f) f = 0.0f;
							if (f > 1.0f) f = 1.0f;
							if (f < -1.0f) f = -1.0f;
							write_f32(vp + c * 4, f);
						}
						n0 = read_s16(vp + 0x38);
						n1 = read_s16(vp + 0x3A);
						if (n0 < 0 || n0 >= node_limit) write_s16(vp + 0x38, 0);
						if (n1 < 0 || n1 >= node_limit) write_s16(vp + 0x3A, 0);
						w0 = read_f32(vp + 0x3C);
						w1 = read_f32(vp + 0x40);
						if (w0 != w0 || w0 < 0.0f) write_f32(vp + 0x3C, 1.0f);
						if (w0 > 1.0f) write_f32(vp + 0x3C, 1.0f);
						if (w1 != w1 || w1 < 0.0f) write_f32(vp + 0x40, 0.0f);
						if (w1 > 1.0f) write_f32(vp + 0x40, 1.0f);
					}

					dst_strip_rel = buf_alloc(&b->model_strips_buf, align4(strip_bytes), 4);
					memcpy(b->model_strips_buf.data + dst_strip_rel, src_map->data + src_strip_foff, strip_bytes);
					if (strip_bytes & 2u)
					{
						uint16_t last_idx = read_u16(b->model_strips_buf.data + dst_strip_rel + strip_bytes - 2u);
						write_u16(b->model_strips_buf.data + dst_strip_rel + strip_bytes, last_idx);
					}

					write_u32(b->tag_buf.data + part_off + GBXMODEL_PART_VERTEX_OFFSET_OFFSET, dst_vert_rel);
					write_u32(b->tag_buf.data + part_off + GBXMODEL_PART_STRIP_OFFSET_OFFSET, dst_strip_rel);
					write_u32(b->tag_buf.data + part_off + 0x50u, dst_strip_rel);
					b->model_part_count++;
				}
			}
		}
	}
}

static void builder_internalize_bitm(
	struct forge_builder *b,
	struct forge_source_map const *src_map,
	int32_t dst_tag_idx,
	uint32_t dst_bitm_off)
{
	uint32_t dst_handle = b->tags[dst_tag_idx].handle;
	int32_t seq_count = read_s32(b->tag_buf.data + dst_bitm_off + 0x54u);
	uint32_t seq_addr = read_u32(b->tag_buf.data + dst_bitm_off + 0x58u);
	int32_t bd_count = read_s32(b->tag_buf.data + dst_bitm_off + 0x60u);
	uint32_t bd_addr = read_u32(b->tag_buf.data + dst_bitm_off + 0x64u);
	int32_t s, i;

	if (seq_count > 0 && seq_addr >= CE_TAG_BASE_ADDR)
	{
		for (s = 0; s < seq_count; s++)
		{
			uint32_t seq_off = (seq_addr - CE_TAG_BASE_ADDR) + (uint32_t)s * 0x40u;
			int32_t perm_count, p;
			uint32_t perm_addr;
			if (seq_off + 0x40u > b->tag_buf.size)
				break;
			perm_count = read_s32(b->tag_buf.data + seq_off + 0x34u);
			perm_addr = read_u32(b->tag_buf.data + seq_off + 0x38u);
			if (perm_count <= 0 || perm_addr < CE_TAG_BASE_ADDR)
				continue;
			for (p = 0; p < perm_count; p++)
			{
				uint32_t perm_off = (perm_addr - CE_TAG_BASE_ADDR) + (uint32_t)p * 0x40u;
				if (perm_off + 0x40u > b->tag_buf.size)
					break;
				write_u32(b->tag_buf.data + perm_off + 0x34u, dst_handle);
				write_u32(b->tag_buf.data + perm_off + 0x38u, dst_handle);
			}
		}
	}

	if (bd_count <= 0 || bd_addr < CE_TAG_BASE_ADDR)
		return;

	for (i = 0; i < bd_count; i++)
	{
		uint32_t bd_off = (bd_addr - CE_TAG_BASE_ADDR) + (uint32_t)i * BITMAP_DATA_SIZE;
		uint16_t flags;
		uint32_t px_off, px_size;
		uint8_t const *src_pixels = NULL;

		if (bd_off + BITMAP_DATA_SIZE > b->tag_buf.size)
			break;

		flags = read_u16(b->tag_buf.data + bd_off + BITMAP_DATA_FLAGS_OFFSET);
		px_off = read_u32(b->tag_buf.data + bd_off + BITMAP_DATA_PIXELS_OFFSET_OFFSET);
		px_size = read_u32(b->tag_buf.data + bd_off + BITMAP_DATA_PIXELS_SIZE_OFFSET);

		write_u32(b->tag_buf.data + bd_off + BITMAP_DATA_TAG_INDEX_OFFSET, dst_handle);
		write_u32(b->tag_buf.data + bd_off + 0x24u, 0);
		write_u32(b->tag_buf.data + bd_off + 0x28u, 0);
		write_u32(b->tag_buf.data + bd_off + 0x2Cu, 0);

		if (px_size > 0)
		{
			uint32_t raw_dst;
			if (flags & (1u << BITMAP_DATA_IN_RESOURCE_MAP_BIT))
			{
				if (src_map->version == CACHE_VERSION_PC && b->pc_bitmaps.data &&
					(uint64_t)px_off + px_size <= b->pc_bitmaps.file_size)
				{
					src_pixels = b->pc_bitmaps.data + px_off;
				}
				else if (b->ce_bitmaps.data && (uint64_t)px_off + px_size <= b->ce_bitmaps.file_size)
				{
					src_pixels = b->ce_bitmaps.data + px_off;
				}
				flags &= (uint16_t)~(1u << BITMAP_DATA_IN_RESOURCE_MAP_BIT);
				write_u16(b->tag_buf.data + bd_off + BITMAP_DATA_FLAGS_OFFSET, flags);
			}
			else if ((uint64_t)px_off + px_size <= src_map->file_size)
			{
				src_pixels = src_map->data + px_off;
			}

			raw_dst = buf_alloc(&b->raw_buf, px_size, 4);
			if (src_pixels)
				memcpy(b->raw_buf.data + raw_dst, src_pixels, px_size);
			builder_add_raw_patch(b, bd_off + BITMAP_DATA_PIXELS_OFFSET_OFFSET, raw_dst);
		}
	}
}

static void builder_internalize_snd(
	struct forge_builder *b,
	struct forge_source_map const *src_map,
	int32_t dst_tag_idx,
	uint32_t dst_snd_off)
{
	uint32_t dst_handle = b->tags[dst_tag_idx].handle;
	int32_t pr_count = read_s32(b->tag_buf.data + dst_snd_off + SOUND_PITCH_RANGES_OFFSET + 0);
	uint32_t pr_addr = read_u32(b->tag_buf.data + dst_snd_off + SOUND_PITCH_RANGES_OFFSET + 4);
	int32_t p;

	if (pr_count <= 0 || pr_addr < CE_TAG_BASE_ADDR)
		return;

	for (p = 0; p < pr_count; p++)
	{
		uint32_t pr_off = (pr_addr - CE_TAG_BASE_ADDR) + (uint32_t)p * SOUND_PITCH_RANGE_SIZE;
		int32_t perm_count, m;
		uint32_t perm_addr;
		if (pr_off + SOUND_PITCH_RANGE_SIZE > b->tag_buf.size)
			break;
		perm_count = read_s32(b->tag_buf.data + pr_off + SOUND_PITCH_RANGE_PERMUTATIONS_OFFSET + 0);
		perm_addr = read_u32(b->tag_buf.data + pr_off + SOUND_PITCH_RANGE_PERMUTATIONS_OFFSET + 4);
		if (perm_count <= 0 || perm_addr < CE_TAG_BASE_ADDR)
			continue;

		for (m = 0; m < perm_count; m++)
		{
			uint32_t perm_off = (perm_addr - CE_TAG_BASE_ADDR) + (uint32_t)m * SOUND_PERMUTATION_SIZE;
			uint32_t td_off;
			int32_t td_size;
			uint32_t td_flags, td_foff, td_addr;

			if (perm_off + SOUND_PERMUTATION_SIZE > b->tag_buf.size)
				break;

			write_u32(b->tag_buf.data + perm_off + SOUND_PERMUTATION_TAG_INDEX0_OFFSET, dst_handle);
			write_u32(b->tag_buf.data + perm_off + 0x3Cu, dst_handle);

			td_off = perm_off + SOUND_PERMUTATION_SAMPLES_OFFSET;
			td_size = read_s32(b->tag_buf.data + td_off + 0);
			td_flags = read_u32(b->tag_buf.data + td_off + 4);
			td_foff = read_u32(b->tag_buf.data + td_off + 8);
			td_addr = read_u32(b->tag_buf.data + td_off + 12);

			if (td_size > 0 && td_addr == 0)
			{
				uint8_t const *src_samples = NULL;
				uint32_t raw_dst;
				if (td_flags & 1u)
				{
					if (src_map->version == CACHE_VERSION_PC && b->pc_sounds.data &&
						(uint64_t)td_foff + (uint32_t)td_size <= b->pc_sounds.file_size)
					{
						src_samples = b->pc_sounds.data + td_foff;
					}
					else if (b->ce_sounds.data &&
						(uint64_t)td_foff + (uint32_t)td_size <= b->ce_sounds.file_size)
					{
						src_samples = b->ce_sounds.data + td_foff;
					}
					td_flags &= ~1u;
					write_u32(b->tag_buf.data + td_off + 4, td_flags);
				}
				else if ((uint64_t)td_foff + (uint32_t)td_size <= src_map->file_size)
				{
					src_samples = src_map->data + td_foff;
				}

				raw_dst = buf_alloc(&b->raw_buf, (uint32_t)td_size, 4);
				if (src_samples)
					memcpy(b->raw_buf.data + raw_dst, src_samples, (size_t)td_size);
				builder_add_raw_patch(b, td_off + 8, raw_dst);
			}
		}
	}
}

static int builder_clone_single_tag(
	struct forge_builder *b,
	struct forge_source_map const *src_map,
	int32_t src_idx,
	int32_t dst_idx)
{
	uint8_t const *inst = src_tag_instance(src_map, src_idx);
	char const *tname = src_tag_name(src_map, src_idx);
	uint32_t group;
	uint32_t src_base_addr;
	int32_t src_in_res;
	struct tag_schema_group const *schema;
	uint32_t src_root_off, dst_root_off;

	if (!inst || !tname)
		return 0;
	if (b->tags[dst_idx].cloned)
		return 1;
	b->tags[dst_idx].cloned = 1;

	group = read_u32(inst + 0x00);
	src_base_addr = read_u32(inst + 0x14);
	src_in_res = read_s32(inst + 0x18);

	/* Never clone donor structure BSPs; base_map's sbsp stays in the BSP section */
	if (group == 'sbsp')
	{
		if (src_map == b->base_map)
		{
			b->tags[dst_idx].base_address = 0;
			b->tags[dst_idx].in_resource_map = 0;
			return 1;
		}
		return 0;
	}

	/* Link to Custom Edition resource maps (bitmaps.map, sounds.map, loc.map) whenever available */
	if (group == 'bitm')
	{
		int32_t ce_item = forge_resource_map_find_item(&b->ce_bitmaps, tname);
		if (ce_item >= 0)
		{
			b->tags[dst_idx].in_resource_map = 1;
			b->tags[dst_idx].base_address = (uint32_t)ce_item;
			return 1;
		}
	}
	else if (group == 'font' || group == 'ustr' || group == 'hmt ')
	{
		int32_t ce_item = forge_resource_map_find_item(&b->ce_loc, tname);
		if (ce_item >= 0)
		{
			b->tags[dst_idx].in_resource_map = 1;
			b->tags[dst_idx].base_address = (uint32_t)ce_item;
			return 1;
		}
	}
	else if (group == 'snd!')
	{
		int32_t ce_item = forge_resource_map_find_item(&b->ce_sounds, tname);
		if (ce_item >= 0)
		{
			uint32_t item_off = b->ce_sounds.items_offset + (uint32_t)ce_item * 12u;
			uint32_t data_size = read_u32(b->ce_sounds.data + item_off + 0x04);
			uint32_t data_off = read_u32(b->ce_sounds.data + item_off + 0x08);
			if (data_size >= SOUND_DEFINITON_SIZE &&
				(uint64_t)data_off + SOUND_DEFINITON_SIZE <= b->ce_sounds.file_size)
			{
				dst_root_off = buf_alloc(&b->tag_buf, SOUND_DEFINITON_SIZE, 4);
				memcpy(b->tag_buf.data + dst_root_off, b->ce_sounds.data + data_off, SOUND_DEFINITON_SIZE);
				/* Clear promotion_sound reference (+0x70) and pitch_ranges address (+0x9C) */
				write_u32(b->tag_buf.data + dst_root_off + SOUND_PROMOTION_SOUND_OFFSET + 0, 'snd!');
				write_u32(b->tag_buf.data + dst_root_off + SOUND_PROMOTION_SOUND_OFFSET + 4, 0);
				write_s32(b->tag_buf.data + dst_root_off + SOUND_PROMOTION_SOUND_OFFSET + 8, 0);
				write_s32(b->tag_buf.data + dst_root_off + SOUND_PROMOTION_SOUND_OFFSET + 12, -1);
				write_u32(b->tag_buf.data + dst_root_off + SOUND_PITCH_RANGES_OFFSET + 4, 0);
				write_u32(b->tag_buf.data + dst_root_off + SOUND_PITCH_RANGES_OFFSET + 8, 0);
				b->tags[dst_idx].in_resource_map = 1;
				b->tags[dst_idx].base_address = CE_TAG_BASE_ADDR + dst_root_off;
				return 1;
			}
		}
	}

	if (src_in_res && group != 'snd!')
	{
		b->tags[dst_idx].in_resource_map = 1;
		b->tags[dst_idx].base_address = src_base_addr;
		return 1;
	}

	schema = forge_schema_group_get(group);
	if (!schema || !schema->definition || schema->definition->size <= 0)
		return 0;

	if (src_base_addr < CE_TAG_BASE_ADDR)
		return 0;
	src_root_off = src_base_addr - CE_TAG_BASE_ADDR;
	if ((uint64_t)src_root_off + (uint32_t)schema->definition->size > src_map->tag_data_size)
		return 0;

	b->tags[dst_idx].in_resource_map = 0;
	dst_root_off = buf_alloc(&b->tag_buf, (uint32_t)schema->definition->size, 4);
	memcpy(
		b->tag_buf.data + dst_root_off,
		src_map->data + src_map->tag_data_offset + src_root_off,
		(size_t)schema->definition->size);
	b->tags[dst_idx].base_address = CE_TAG_BASE_ADDR + dst_root_off;

	builder_clone_definition(b, src_map, schema->definition, src_root_off, dst_root_off, 1);

	if (group == 'mod2' || group == 'mode')
	{
		b->tags[dst_idx].primary_group = 'mod2';
		builder_pack_mod2_geometry(b, src_map, dst_root_off);
	}
	else if (group == 'bitm')
	{
		builder_internalize_bitm(b, src_map, dst_idx, dst_root_off);
	}
	else if (group == 'snd!')
	{
		builder_internalize_snd(b, src_map, dst_idx, dst_root_off);
	}
	return 1;
}

static int32_t builder_ensure_tag(
	struct forge_builder *b,
	struct forge_source_map const *src_map,
	int32_t src_idx)
{
	uint8_t const *inst;
	char const *tname;
	uint32_t group;
	int32_t dst_idx;

	if (!b || !src_map || src_idx < 0 || src_idx >= src_map->tag_count)
		return -1;

	inst = src_tag_instance(src_map, src_idx);
	tname = src_tag_name(src_map, src_idx);
	if (!inst || !tname)
		return -1;

	group = read_u32(inst + 0x00);
	if (group == 'sbsp' && src_map != b->base_map)
		return -1;

	if (src_map == b->base_map && src_idx < b->tag_count)
	{
		dst_idx = src_idx;
	}
	else
	{
		dst_idx = builder_find_tag(b, group, tname);
		if (dst_idx < 0)
		{
			uint16_t salt;
			struct forge_dest_tag *dt;
			if (b->tag_count >= BUILDER_MAX_TAGS)
				return -1;
			dst_idx = b->tag_count++;
			salt = (uint16_t)(0xE174u + (uint16_t)dst_idx);
			if (salt == 0 || salt == 0xFFFFu)
				salt = 0xE175u;
			dt = &b->tags[dst_idx];
			dt->primary_group = (group == 'mode') ? 'mod2' : group;
			dt->secondary_group = read_u32(inst + 0x04);
			dt->tertiary_group = read_u32(inst + 0x08);
			dt->handle = ((uint32_t)salt << 16) | (uint32_t)dst_idx;
			dt->name = _strdup(tname);
			dt->name_address = builder_alloc_string(b, dt->name);
			dt->base_address = 0;
			dt->in_resource_map = 0;
			dt->pad = 0;
			dt->cloned = 0;
		}
	}

	if (!b->tags[dst_idx].cloned)
	{
		if (!builder_clone_single_tag(b, src_map, src_idx, dst_idx))
			return -1;
		if (src_map != b->base_map)
		{
			builder_add_credit(b, b->tags[dst_idx].primary_group, tname, src_map->filename, src_map->author_credit);
		}
	}
	return dst_idx;
}

/* ---------- Custom Edition v609 cache serializer */

static int builder_serialize_cache_file(
	struct forge_builder *b,
	char const *map_name,
	int16_t scenario_type,
	char const *output_path)
{
	struct forge_byte_buf out;
	int32_t i;
	uint32_t bsp_section_offset = 0x800u;
	uint32_t bsp_section_size = 0;
	uint32_t raw_section_offset;
	uint32_t model_verts_offset;
	uint32_t model_strips_rel_offset;
	uint32_t model_total_size;
	uint32_t tag_data_offset;
	uint32_t scnr_off = 0;
	uint32_t crc = 0xFFFFFFFFu;
	FILE *f;

	if (!b || !b->base_map || !output_path)
		return 0;

	for (i = 0; i < b->base_map->tag_count; i++)
	{
		builder_ensure_tag(b, b->base_map, i);
	}

	buf_alloc(&b->raw_buf, 0, 32);
	buf_alloc(&b->model_verts_buf, 0, 32);
	buf_alloc(&b->model_strips_buf, 0, 32);
	buf_alloc(&b->tag_buf, 0, 32);

	if (b->tags[b->base_map->scenario_tag_index].base_address >= CE_TAG_BASE_ADDR)
	{
		scnr_off = b->tags[b->base_map->scenario_tag_index].base_address - CE_TAG_BASE_ADDR;
	}

	/* Compute BSP section size from scenario structure_bsp_references (+0x5A4) */
	if (scnr_off > 0 && scnr_off + 0x5B0u <= b->tag_buf.size)
	{
		int32_t bsp_count = read_s32(b->tag_buf.data + scnr_off + 0x5A4u);
		uint32_t bsp_addr = read_u32(b->tag_buf.data + scnr_off + 0x5A8u);
		if (bsp_count > 0 && bsp_addr >= CE_TAG_BASE_ADDR)
		{
			uint32_t bsp_arr_off = bsp_addr - CE_TAG_BASE_ADDR;
			uint32_t cur_off = bsp_section_offset;
			for (i = 0; i < bsp_count; i++)
			{
				uint32_t ref_off = bsp_arr_off + (uint32_t)i * 0x20u;
				uint32_t old_foff, bsp_sz;
				if (ref_off + 0x20u > b->tag_buf.size)
					break;
				old_foff = read_u32(b->tag_buf.data + ref_off + 0x00u);
				bsp_sz = read_u32(b->tag_buf.data + ref_off + 0x04u);
				if (bsp_sz > 0 && (uint64_t)old_foff + bsp_sz <= b->base_map->file_size)
				{
					cur_off = (cur_off + 31u) & ~31u;
					cur_off += bsp_sz;
				}
			}
			cur_off = (cur_off + 31u) & ~31u;
			bsp_section_size = cur_off - bsp_section_offset;
		}
	}

	raw_section_offset = bsp_section_offset + bsp_section_size;
	for (i = 0; i < b->raw_patch_count; i++)
	{
		uint32_t tpos = b->raw_patches[i].tag_buf_offset;
		uint32_t abs_foff = raw_section_offset + b->raw_patches[i].raw_buf_offset;
		if (tpos + 4u <= b->tag_buf.size)
			write_u32(b->tag_buf.data + tpos, abs_foff);
	}

	model_verts_offset = raw_section_offset + b->raw_buf.size;
	model_strips_rel_offset = b->model_verts_buf.size;
	model_total_size = b->model_verts_buf.size + b->model_strips_buf.size;
	tag_data_offset = model_verts_offset + model_total_size;

	/* Write cache_tag_header (0x28 bytes) at start of tag_buf */
	memset(b->tag_buf.data, 0, TAG_HEADER_SIZE);
	write_u32(b->tag_buf.data + 0x00u, CE_TAG_BASE_ADDR + BUILDER_INSTANCES_OFFSET);
	write_u32(b->tag_buf.data + 0x04u, b->tags[b->base_map->scenario_tag_index].handle);
	write_u32(b->tag_buf.data + 0x08u, 0x12345678u);
	write_s32(b->tag_buf.data + 0x0Cu, b->tag_count);
	write_u32(b->tag_buf.data + 0x10u, b->model_part_count);
	write_u32(b->tag_buf.data + 0x14u, model_verts_offset);
	write_u32(b->tag_buf.data + 0x18u, b->model_part_count);
	write_u32(b->tag_buf.data + 0x1Cu, model_strips_rel_offset);
	write_u32(b->tag_buf.data + 0x20u, model_total_size);
	write_u32(b->tag_buf.data + 0x24u, TAG_INDEX_SIGNATURE);

	/* Write cache_tag_instance table */
	for (i = 0; i < b->tag_count; i++)
	{
		uint8_t *inst = b->tag_buf.data + BUILDER_INSTANCES_OFFSET + (uint32_t)i * TAG_INSTANCE_SIZE;
		write_u32(inst + 0x00u, b->tags[i].primary_group);
		write_u32(inst + 0x04u, b->tags[i].secondary_group);
		write_u32(inst + 0x08u, b->tags[i].tertiary_group);
		write_u32(inst + 0x0Cu, b->tags[i].handle);
		write_u32(inst + 0x10u, b->tags[i].name_address);
		write_u32(inst + 0x14u, b->tags[i].base_address);
		write_s32(inst + 0x18u, b->tags[i].in_resource_map);
		write_s32(inst + 0x1Cu, 0);
	}

	buf_init(&out, tag_data_offset + b->tag_buf.size + 64u);
	buf_alloc(&out, 0x800u, 1);

	/* Copy structure BSPs from base_map and update scenario structure_bsp_references */
	if (scnr_off > 0 && scnr_off + 0x5B0u <= b->tag_buf.size)
	{
		int32_t bsp_count = read_s32(b->tag_buf.data + scnr_off + 0x5A4u);
		uint32_t bsp_addr = read_u32(b->tag_buf.data + scnr_off + 0x5A8u);
		if (bsp_count > 0 && bsp_addr >= CE_TAG_BASE_ADDR)
		{
			uint32_t bsp_arr_off = bsp_addr - CE_TAG_BASE_ADDR;
			for (i = 0; i < bsp_count; i++)
			{
				uint32_t ref_off = bsp_arr_off + (uint32_t)i * 0x20u;
				uint32_t old_foff, bsp_sz;
				if (ref_off + 0x20u > b->tag_buf.size)
					break;
				old_foff = read_u32(b->tag_buf.data + ref_off + 0x00u);
				bsp_sz = read_u32(b->tag_buf.data + ref_off + 0x04u);
				if (bsp_sz > 0 && (uint64_t)old_foff + bsp_sz <= b->base_map->file_size)
				{
					uint32_t new_foff = buf_alloc(&out, bsp_sz, 32);
					memcpy(out.data + new_foff, b->base_map->data + old_foff, bsp_sz);
					write_u32(b->tag_buf.data + ref_off + 0x00u, new_foff);
					crc = forge_crc32_update(crc, out.data + new_foff, bsp_sz);
				}
			}
			buf_alloc(&out, 0, 32);
		}
	}

	if (b->raw_buf.size > 0)
	{
		uint32_t roff = buf_alloc(&out, b->raw_buf.size, 32);
		memcpy(out.data + roff, b->raw_buf.data, b->raw_buf.size);
	}
	if (b->model_verts_buf.size > 0)
	{
		uint32_t voff = buf_alloc(&out, b->model_verts_buf.size, 32);
		memcpy(out.data + voff, b->model_verts_buf.data, b->model_verts_buf.size);
	}
	if (b->model_strips_buf.size > 0)
	{
		uint32_t soff = buf_alloc(&out, b->model_strips_buf.size, 32);
		memcpy(out.data + soff, b->model_strips_buf.data, b->model_strips_buf.size);
	}
	if (model_total_size > 0)
	{
		crc = forge_crc32_update(crc, out.data + model_verts_offset, model_total_size);
	}
	{
		uint32_t toff = buf_alloc(&out, b->tag_buf.size, 32);
		memcpy(out.data + toff, b->tag_buf.data, b->tag_buf.size);
		crc = forge_crc32_update(crc, out.data + toff, b->tag_buf.size);
	}
	crc = ~crc;

	/* Populate 0x800-byte cache file header */
	memset(out.data, 0, 0x800u);
	write_u32(out.data + 0x00u, CACHE_HEADER_SIGNATURE);
	write_s32(out.data + 0x04u, CACHE_VERSION_CE);
	write_u32(out.data + 0x08u, out.size);
	write_u32(out.data + 0x0Cu, 0);
	write_u32(out.data + 0x10u, tag_data_offset);
	write_u32(out.data + 0x14u, b->tag_buf.size);
	snprintf((char *)(out.data + 0x20u), 32, "%s", map_name ? map_name : "forgeasset");
	snprintf((char *)(out.data + 0x40u), 32, "01.00.00.0609");
	write_s16(out.data + 0x60u, scenario_type);
	write_u32(out.data + 0x64u, crc);
	if (map_name && strcmp(map_name, "forgeasset") != 0 && strcmp(map_name, "customforgeasset") != 0)
	{
		write_u32(out.data + 0x1FCu, 0x464C4731u); /* 'FLG1' OpenCE Forge map signature */
	}
	write_u32(out.data + 0x7FCu, CACHE_FOOTER_SIGNATURE);

	f = fopen(output_path, "wb");
	if (!f)
	{
		buf_free(&out);
		return 0;
	}
	if (fwrite(out.data, 1, out.size, f) != out.size)
	{
		fclose(f);
		buf_free(&out);
		return 0;
	}
	fclose(f);
	buf_free(&out);
	return 1;
}

/* ---------- credits.md attribution writer */

static void forge_write_credits_file(
	char const *credits_md_path,
	struct forge_credit_record const *records,
	int32_t count,
	int append_section,
	char const *section_title)
{
	FILE *f;
	int32_t i;
	if (!credits_md_path || !credits_md_path[0] || !records)
		return;

	f = fopen(credits_md_path, append_section ? "ab" : "wb");
	if (!f)
		return;

	if (!append_section)
	{
		fprintf(f, "# OpenCE Forge - Asset Attribution & Community Credits\n\n");
		fprintf(f, "Every tag extracted and compiled into `forgeasset.map` and `customforgeasset.map` is recorded below with its tag class, path, source `.map` file, and original creator attribution.\n\n");
	}

	fprintf(f, "\n## %s (%d tags)\n\n", section_title ? section_title : "Extracted Assets", (int)count);
	fprintf(f, "| Tag Class & Name | Source Map | Attribution |\n");
	fprintf(f, "| :--- | :--- | :--- |\n");

	for (i = 0; i < count; i++)
	{
		fprintf(
			f,
			"| `%s.%s` | `%s` | *%s* and its original creator (%s) |\n",
			records[i].tag_path,
			forge_group_tag_ext(records[i].group_tag),
			records[i].source_map,
			records[i].source_map,
			records[i].author);
	}
	fclose(f);
}

/* ---------- Master & Custom Asset Cache Harvesters */

static int is_allowed_forge_palette_group(uint32_t g)
{
	return (
		g == 'bipd' ||
		g == 'actv' ||
		g == 'actr' ||
		g == 'antr' ||
		g == 'vehi' ||
		g == 'weap' ||
		g == 'eqip' ||
		g == 'itmc' ||
		g == 'scen' ||
		g == 'mach' ||
		g == 'ctrl' ||
		g == 'lifi' ||
		g == 'phys' ||
		g == 'coll' ||
		g == 'jpt!');
}

static void harvest_donor_map_into_builder(struct forge_builder *b, char const *donor_path)
{
	struct forge_source_map donor;
	int32_t i;

	if (!forge_source_map_open(donor_path, &donor))
		return;

	for (i = 0; i < donor.tag_count; i++)
	{
		uint8_t const *inst = src_tag_instance(&donor, i);
		char const *tname = src_tag_name(&donor, i);
		uint32_t g;
		if (!inst || !tname || !tname[0])
			continue;
		g = read_u32(inst + 0x00);
		if (!is_allowed_forge_palette_group(g))
			continue;
		/* Skip campaign cinematic-only animation graphs */
		if (g == 'antr' && strstr(tname, "cinematic") != NULL)
			continue;
		if (b->tag_buf.size < 0x00A00000u) builder_ensure_tag(b, &donor, i);
	}

	forge_source_map_close(&donor);
}

int forge_build_master_asset_cache(
	char const *ce_maps_dir,
	char const *pc_maps_dir,
	char const *output_map_path,
	char const *credits_md_path)
{
	char anchor_path[512];
	struct forge_source_map base_map;
	struct forge_builder builder;
	int32_t i;
	int ok;

	snprintf(anchor_path, sizeof(anchor_path), "%s/bloodgulch.map", ce_maps_dir);
	if (!forge_source_map_open(anchor_path, &base_map))
		return 0;

	builder_init(&builder, &base_map, ce_maps_dir, pc_maps_dir);

	/* Record base map's own palette tags in credits */
	for (i = 0; i < base_map.tag_count; i++)
	{
		uint8_t const *inst = src_tag_instance(&base_map, i);
		char const *tname = src_tag_name(&base_map, i);
		if (inst && tname)
		{
			uint32_t g = read_u32(inst + 0x00);
			if (is_allowed_forge_palette_group(g))
			{
				builder_add_credit(&builder, g, tname, base_map.filename, base_map.author_credit);
			}
		}
	}

	/* 1. Harvest all stock Custom Edition default multiplayer maps */
	{
		static char const *const ce_maps[] = {
			"beavercreek.map", "boardingaction.map", "carousel.map", "chillout.map",
			"damnation.map", "dangercanyon.map", "deathisland.map", "gephyrophobia.map",
			"hangemhigh.map", "icefields.map", "infinity.map", "longest.map",
			"prisoner.map", "putput.map", "ratrace.map", "sidewinder.map",
			"timberland.map", "wizard.map"
		};
		size_t m;
		for (m = 0; m < sizeof(ce_maps) / sizeof(ce_maps[0]); m++)
		{
			char mpath[512];
			snprintf(mpath, sizeof(mpath), "%s/%s", ce_maps_dir, ce_maps[m]);
			harvest_donor_map_into_builder(&builder, mpath);
		}
	}

	/* 2. Harvest PC campaign maps (343 Guilty Spark Monitor in c10/c20 first, plus Marines, Elites, Grunts, Jackals, Hunters, Flood, Sentinels, Wraiths, Pelicans, Crates) */
	if (pc_maps_dir && pc_maps_dir[0])
	{
		static char const *const pc_maps[] = {
			"c10.map", "c20.map", "b30.map", "b40.map", "a10.map",
			"a30.map", "a50.map", "c40.map", "d20.map", "d40.map"
		};
		size_t m;
		for (m = 0; m < sizeof(pc_maps) / sizeof(pc_maps[0]); m++)
		{
			char mpath[512];
			snprintf(mpath, sizeof(mpath), "%s/%s", pc_maps_dir, pc_maps[m]);
			harvest_donor_map_into_builder(&builder, mpath);
		}
	}

	ok = builder_serialize_cache_file(&builder, "forgeasset", 1, output_map_path);
	if (ok && credits_md_path)
	{
		forge_write_credits_file(
			credits_md_path,
			builder.credits,
			builder.credit_count,
			0,
			"Base Master Asset Cache (`forgeasset.map`)");
	}

	builder_free(&builder);
	forge_source_map_close(&base_map);
	return ok;
}

int forge_build_custom_asset_cache(
	char const *base_ce_map_path,
	char const *ce_res_dir,
	char const *forgeasset_path,
	char const *const *custom_map_paths,
	int custom_map_count,
	char const *output_map_path,
	char const *credits_md_path)
{
	struct forge_source_map base_map;
	struct forge_source_map master_map;
	struct forge_builder builder;
	int m, ok;

	if (!forge_source_map_open(base_ce_map_path, &base_map))
		return 0;
	memset(&master_map, 0, sizeof(master_map));
	if (forgeasset_path && forgeasset_path[0])
		forge_source_map_open(forgeasset_path, &master_map);

	builder_init(&builder, &base_map, ce_res_dir, NULL);

	for (m = 0; m < custom_map_count; m++)
	{
		struct forge_source_map donor;
		int32_t i;
		if (!custom_map_paths[m] || !forge_source_map_open(custom_map_paths[m], &donor))
			continue;
		for (i = 0; i < donor.tag_count; i++)
		{
			uint8_t const *inst = src_tag_instance(&donor, i);
			char const *tname = src_tag_name(&donor, i);
			uint32_t g;
			if (!inst || !tname || !tname[0])
				continue;
			g = read_u32(inst + 0x00);
			if (!is_allowed_forge_palette_group(g))
				continue;
			if (master_map.data && src_map_find_tag(&master_map, g, tname) >= 0)
				continue;
			if (builder_find_tag(&builder, g, tname) >= 0)
				continue;
			builder_ensure_tag(&builder, &donor, i);
		}
		forge_source_map_close(&donor);
	}

	ok = builder_serialize_cache_file(&builder, "customforgeasset", 1, output_map_path);
	if (ok && credits_md_path && builder.credit_count > 0)
	{
		forge_write_credits_file(
			credits_md_path,
			builder.credits,
			builder.credit_count,
			1,
			"Custom Asset Cache (`customforgeasset.map`)");
	}

	builder_free(&builder);
	forge_source_map_close(&master_map);
	forge_source_map_close(&base_map);
	return ok;
}

int forge_inject_custom_map_into_cache(
	char const *source_custom_map_path,
	char const *ce_res_dir,
	char const *forgeasset_path,
	char const *customforgeasset_path,
	char const *credits_md_path,
	int *out_injected_root_count)
{
	struct forge_source_map base_map;
	struct forge_source_map master_map;
	struct forge_source_map donor;
	struct forge_builder builder;
	char const *anchor_path = forgeasset_path;
	FILE *test_f;
	int32_t i;
	int injected_roots = 0;
	int ok;

	if (out_injected_root_count)
		*out_injected_root_count = 0;

	memset(&master_map, 0, sizeof(master_map));
	if (forgeasset_path && forgeasset_path[0])
		forge_source_map_open(forgeasset_path, &master_map);

	test_f = fopen(customforgeasset_path, "rb");
	if (test_f)
	{
		fclose(test_f);
		anchor_path = customforgeasset_path;
	}

	if (!forge_source_map_open(anchor_path, &base_map))
	{
		forge_source_map_close(&master_map);
		return 0;
	}

	builder_init(&builder, &base_map, ce_res_dir, NULL);

	if (source_custom_map_path && forge_source_map_open(source_custom_map_path, &donor))
	{
		for (i = 0; i < donor.tag_count; i++)
		{
			uint8_t const *inst = src_tag_instance(&donor, i);
			char const *tname = src_tag_name(&donor, i);
			uint32_t g;
			if (!inst || !tname || !tname[0])
				continue;
			g = read_u32(inst + 0x00);
			if (!is_allowed_forge_palette_group(g))
				continue;
			if (master_map.data && src_map_find_tag(&master_map, g, tname) >= 0)
				continue;
			if (builder_find_tag(&builder, g, tname) >= 0)
				continue;
			if (builder_ensure_tag(&builder, &donor, i) >= 0)
				injected_roots++;
		}
		forge_source_map_close(&donor);
	}

	ok = builder_serialize_cache_file(&builder, "customforgeasset", 1, customforgeasset_path);
	if (ok && credits_md_path && builder.credit_count > 0)
	{
		char title[320];
		snprintf(title, sizeof(title), "Custom Injected Tags (`%s`)", source_custom_map_path ? source_custom_map_path : "custom map");
		forge_write_credits_file(credits_md_path, builder.credits, builder.credit_count, 1, title);
	}
	if (out_injected_root_count)
		*out_injected_root_count = injected_roots;

	builder_free(&builder);
	forge_source_map_close(&base_map);
	forge_source_map_close(&master_map);
	return ok;
}

/* ---------- OpenCE custom_edition_cache_load + tag_validate verifier */

static int mem_source_read(void *context, uint32_t offset, uint32_t size, void *buffer)
{
	struct forge_resource_map const *rm = (struct forge_resource_map const *)context;
	if (!rm || !rm->data || (uint64_t)offset + size > rm->file_size)
		return 0;
	memcpy(buffer, rm->data + offset, size);
	return 1;
}

int forge_validate_custom_edition_map_file(
	char const *map_path,
	char const *ce_res_dir,
	int32_t *out_tag_count,
	int32_t *out_corrections)
{
	struct forge_resource_map map_file, bitm_file, snd_file, loc_file;
	struct cache_file_source map_src, bitm_src, snd_src, loc_src;
	struct resource_map rm_bitm, rm_snd, rm_loc;
	struct resource_map *rm_list[NUMBER_OF_RESOURCE_MAP_TYPES];
	struct custom_edition_load_report load_rep;
	struct custom_edition_conversion_report conv_rep;
	struct tag_validate_file_range ranges[3];
	uint8_t *tag_cache;
	char path[512];
	enum cache_file_status st;
	boolean valid;

	if (out_tag_count) *out_tag_count = 0;
	if (out_corrections) *out_corrections = 0;

	memset(&map_file, 0, sizeof(map_file));
	memset(&bitm_file, 0, sizeof(bitm_file));
	memset(&snd_file, 0, sizeof(snd_file));
	memset(&loc_file, 0, sizeof(loc_file));
	memset(&rm_bitm, 0, sizeof(rm_bitm));
	memset(&rm_snd, 0, sizeof(rm_snd));
	memset(&rm_loc, 0, sizeof(rm_loc));

	if (!forge_resource_map_open_file(map_path, &map_file))
	{
		/* map_path is a .map file, not a resource map; read raw bytes */
		FILE *f = fopen(map_path, "rb");
		long sz;
		if (!f)
			return 0;
		if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0x800)
		{
			fclose(f);
			return 0;
		}
		map_file.file_size = (uint32_t)sz;
		map_file.data = (uint8_t *)malloc(map_file.file_size);
		fseek(f, 0, SEEK_SET);
		fread(map_file.data, 1, map_file.file_size, f);
		fclose(f);
	}

	map_src.context = &map_file;
	map_src.read = mem_source_read;
	map_src.size = map_file.file_size;

	if (ce_res_dir && ce_res_dir[0])
	{
		snprintf(path, sizeof(path), "%s/bitmaps.map", ce_res_dir);
		if (forge_resource_map_open_file(path, &bitm_file))
		{
			bitm_src.context = &bitm_file;
			bitm_src.read = mem_source_read;
			bitm_src.size = bitm_file.file_size;
			resource_map_open(&bitm_src, _resource_map_bitmaps, &rm_bitm);
		}
		snprintf(path, sizeof(path), "%s/sounds.map", ce_res_dir);
		if (forge_resource_map_open_file(path, &snd_file))
		{
			snd_src.context = &snd_file;
			snd_src.read = mem_source_read;
			snd_src.size = snd_file.file_size;
			resource_map_open(&snd_src, _resource_map_sounds, &rm_snd);
		}
		snprintf(path, sizeof(path), "%s/loc.map", ce_res_dir);
		if (forge_resource_map_open_file(path, &loc_file))
		{
			loc_src.context = &loc_file;
			loc_src.read = mem_source_read;
			loc_src.size = loc_file.file_size;
			resource_map_open(&loc_src, _resource_map_locale, &rm_loc);
		}
	}

	rm_list[_resource_map_bitmaps] = rm_bitm.items ? &rm_bitm : NULL;
	rm_list[_resource_map_sounds] = rm_snd.items ? &rm_snd : NULL;
	rm_list[_resource_map_locale] = rm_loc.items ? &rm_loc : NULL;

#ifdef _WIN32
	{
		extern void *__stdcall VirtualAlloc(void *, size_t, unsigned long, unsigned long);
		tag_cache = (uint8_t *)VirtualAlloc((void *)CE_TAG_BASE_ADDR, CE_TAG_MAX_BYTES, 0x3000u, 0x04u);
	}
#else
	tag_cache = (uint8_t *)calloc(1, CE_TAG_MAX_BYTES);
#endif
	if (!tag_cache)
		return 0;

	st = custom_edition_cache_load(&map_src, rm_list, tag_cache, CE_TAG_MAX_BYTES, &load_rep);
	if (st != _cache_file_status_ok)
	{
		printf("[Forge Validate] custom_edition_cache_load status=%s (%d) at tag %d loc 0x%X\n",
			cache_file_status_describe(st), (int)st, (int)load_rep.problem_tag_index, (unsigned)load_rep.problem_location);
#ifdef _WIN32
		{
			extern int __stdcall VirtualFree(void *, size_t, unsigned long);
			VirtualFree(tag_cache, 0, 0x8000u);
		}
#else
		free(tag_cache);
#endif
		resource_map_close(&rm_bitm);
		resource_map_close(&rm_snd);
		resource_map_close(&rm_loc);
		forge_resource_map_close_file(&map_file);
		forge_resource_map_close_file(&bitm_file);
		forge_resource_map_close_file(&snd_file);
		forge_resource_map_close_file(&loc_file);
		return 0;
	}

	{
		uint32_t loaded_bytes = load_rep.tag_data_bytes + load_rep.resource_tag_bytes;
		custom_edition_cache_combine_resource_offsets(
			tag_cache,
			loaded_bytes,
			map_file.file_size,
			map_file.file_size + bitm_file.file_size);

		st = custom_edition_cache_convert(tag_cache, loaded_bytes, load_rep.identity.name, &conv_rep);
		if (st != _cache_file_status_ok)
		{
			printf("[Forge Validate] custom_edition_cache_convert status=%s (%d) at tag %d\n",
				cache_file_status_describe(st), (int)st, (int)conv_rep.problem_tag_index);
#ifdef _WIN32
			{
				extern int __stdcall VirtualFree(void *, size_t, unsigned long);
				VirtualFree(tag_cache, 0, 0x8000u);
			}
#else
			free(tag_cache);
#endif
			return 0;
		}

		ranges[0].offset = 0;
		ranges[0].size = map_file.file_size;
		ranges[1].offset = map_file.file_size;
		ranges[1].size = bitm_file.file_size;
		ranges[2].offset = map_file.file_size + bitm_file.file_size;
		ranges[2].size = snd_file.file_size;

		valid = tag_validate_custom_edition_tags(
			tag_cache,
			(long)loaded_bytes,
			load_rep.tag_cache_bytes,
			ranges,
			3,
			load_rep.identity.name);
	}

	if (out_tag_count)
		*out_tag_count = load_rep.tag_count;
	if (out_corrections)
		*out_corrections = (int32_t)tag_validate_corrections();

#ifdef _WIN32
	{
		extern int __stdcall VirtualFree(void *, size_t, unsigned long);
		VirtualFree(tag_cache, 0, 0x8000u);
	}
#else
	free(tag_cache);
#endif
	resource_map_close(&rm_bitm);
	resource_map_close(&rm_snd);
	resource_map_close(&rm_loc);
	forge_resource_map_close_file(&map_file);
	forge_resource_map_close_file(&bitm_file);
	forge_resource_map_close_file(&snd_file);
	forge_resource_map_close_file(&loc_file);
	return valid ? 1 : 0;
}

/* ---------- 2D Blueprint Floorplan & Scenario Exporter ("Save Copy As...") */

int forge_load_map_blueprint(
	char const *map_path,
	struct forge_map_blueprint_data *out_blueprint)
{
	struct forge_source_map sm;
	uint8_t const *scnr_inst;
	uint32_t scnr_addr, scnr_off;
	int32_t bsp_count;
	uint32_t bsp_addr, ref_off, foff, bsp_sz, bsp_base, bsp_ptr, bsp_root_off;
	int32_t cbsp_count, edge_count, vert_count, e;
	uint32_t cbsp_addr, cbsp_off, edge_addr, vert_addr, edge_off, vert_off;

	if (!map_path || !out_blueprint)
		return 0;
	memset(out_blueprint, 0, sizeof(*out_blueprint));

	if (!forge_source_map_open(map_path, &sm))
		return 0;

	snprintf(out_blueprint->map_name, sizeof(out_blueprint->map_name), "%s", sm.map_name);
	out_blueprint->scenario_type = sm.scenario_type;

	scnr_inst = src_tag_instance(&sm, sm.scenario_tag_index);
	if (!scnr_inst)
	{
		forge_source_map_close(&sm);
		return 0;
	}
	scnr_addr = read_u32(scnr_inst + 0x14);
	if (scnr_addr < CE_TAG_BASE_ADDR)
	{
		forge_source_map_close(&sm);
		return 0;
	}
	scnr_off = scnr_addr - CE_TAG_BASE_ADDR;
	if (scnr_off + sizeof(struct scenario) > sm.tag_data_size)
	{
		forge_source_map_close(&sm);
		return 0;
	}

	/* Extract existing player spawns (offsetof(struct scenario, players), 52 bytes each) */
	{
		uint32_t pl_field = (uint32_t)offsetof(struct scenario, players);
		int32_t sp_count = read_s32(sm.data + sm.tag_data_offset + scnr_off + pl_field + 0);
		uint32_t sp_addr = read_u32(sm.data + sm.tag_data_offset + scnr_off + pl_field + 4);
		if (sp_count > 0 && sp_addr >= CE_TAG_BASE_ADDR)
		{
			uint32_t sp_off = sp_addr - CE_TAG_BASE_ADDR;
			int32_t i;
			for (i = 0; i < sp_count && out_blueprint->initial_state.player_spawn_count < FORGE_MAX_PLAYER_SPAWNS; i++)
			{
				uint32_t eo = sp_off + (uint32_t)i * 52u;
				struct forge_player_spawn_entry *dst;
				if (eo + 52u > sm.tag_data_size)
					break;
				dst = &out_blueprint->initial_state.player_spawns[out_blueprint->initial_state.player_spawn_count++];
				dst->x = read_f32(sm.data + sm.tag_data_offset + eo + 0x00u);
				dst->y = read_f32(sm.data + sm.tag_data_offset + eo + 0x04u);
				dst->z = read_f32(sm.data + sm.tag_data_offset + eo + 0x08u);
				dst->yaw = read_f32(sm.data + sm.tag_data_offset + eo + 0x0Cu);
				dst->team_index = read_s16(sm.data + sm.tag_data_offset + eo + 0x10u);
				dst->bsp_index = read_s16(sm.data + sm.tag_data_offset + eo + 0x12u);
				dst->type0 = read_s16(sm.data + sm.tag_data_offset + eo + 0x14u);
			}
		}
	}

	/* Extract existing netgame flags (offsetof(struct scenario, netgame_flags), 148 bytes each) */
	{
		uint32_t nf_field = (uint32_t)offsetof(struct scenario, netgame_flags);
		int32_t nf_count = read_s32(sm.data + sm.tag_data_offset + scnr_off + nf_field + 0);
		uint32_t nf_addr = read_u32(sm.data + sm.tag_data_offset + scnr_off + nf_field + 4);
		if (nf_count > 0 && nf_addr >= CE_TAG_BASE_ADDR)
		{
			uint32_t nf_off = nf_addr - CE_TAG_BASE_ADDR;
			int32_t i;
			for (i = 0; i < nf_count && out_blueprint->initial_state.netgame_flag_count < FORGE_MAX_NETGAME_FLAGS; i++)
			{
				uint32_t eo = nf_off + (uint32_t)i * 148u;
				struct forge_netgame_flag_entry *dst;
				if (eo + 148u > sm.tag_data_size)
					break;
				dst = &out_blueprint->initial_state.netgame_flags[out_blueprint->initial_state.netgame_flag_count++];
				dst->x = read_f32(sm.data + sm.tag_data_offset + eo + 0x00u);
				dst->y = read_f32(sm.data + sm.tag_data_offset + eo + 0x04u);
				dst->z = read_f32(sm.data + sm.tag_data_offset + eo + 0x08u);
				dst->yaw = read_f32(sm.data + sm.tag_data_offset + eo + 0x0Cu);
				dst->type = read_s16(sm.data + sm.tag_data_offset + eo + 0x10u);
				dst->usage_id = read_s16(sm.data + sm.tag_data_offset + eo + 0x12u);
			}
		}
	}

	/* Extract collision BSP wireframe edges */
	bsp_count = read_s32(sm.data + sm.tag_data_offset + scnr_off + (uint32_t)offsetof(struct scenario, structure_bsp_references) + 0);
	bsp_addr = read_u32(sm.data + sm.tag_data_offset + scnr_off + (uint32_t)offsetof(struct scenario, structure_bsp_references) + 4);
	if (bsp_count > 0 && bsp_addr >= CE_TAG_BASE_ADDR)
	{
		ref_off = bsp_addr - CE_TAG_BASE_ADDR;
		foff = read_u32(sm.data + sm.tag_data_offset + ref_off + 0x00u);
		bsp_sz = read_u32(sm.data + sm.tag_data_offset + ref_off + 0x04u);
		bsp_base = read_u32(sm.data + sm.tag_data_offset + ref_off + 0x08u);
		if (bsp_sz >= 0x100u && (uint64_t)foff + bsp_sz <= sm.file_size)
		{
			bsp_ptr = read_u32(sm.data + foff);
			if (bsp_ptr >= bsp_base)
			{
				bsp_root_off = foff + (bsp_ptr - bsp_base);
				if ((uint64_t)bsp_root_off + sizeof(struct structure_bsp) <= sm.file_size)
				{
					uint32_t wb_off = bsp_root_off + (uint32_t)offsetof(struct structure_bsp, world_bounds);
					uint32_t cb_field = bsp_root_off + (uint32_t)offsetof(struct structure_bsp, collision_bsp);
					out_blueprint->min_x = read_f32(sm.data + wb_off + 0x00u);
					out_blueprint->max_x = read_f32(sm.data + wb_off + 0x04u);
					out_blueprint->min_y = read_f32(sm.data + wb_off + 0x08u);
					out_blueprint->max_y = read_f32(sm.data + wb_off + 0x0Cu);
					out_blueprint->min_z = read_f32(sm.data + wb_off + 0x10u);
					out_blueprint->max_z = read_f32(sm.data + wb_off + 0x14u);

					cbsp_count = read_s32(sm.data + cb_field + 0);
					cbsp_addr = read_u32(sm.data + cb_field + 4);
					if (cbsp_count > 0 && cbsp_addr >= bsp_base)
					{
						cbsp_off = foff + (cbsp_addr - bsp_base);
						if ((uint64_t)cbsp_off + sizeof(struct collision_bsp) <= sm.file_size)
						{
							edge_count = read_s32(sm.data + cbsp_off + (uint32_t)offsetof(struct collision_bsp, edges) + 0);
							edge_addr = read_u32(sm.data + cbsp_off + (uint32_t)offsetof(struct collision_bsp, edges) + 4);
							vert_count = read_s32(sm.data + cbsp_off + (uint32_t)offsetof(struct collision_bsp, vertices) + 0);
							vert_addr = read_u32(sm.data + cbsp_off + (uint32_t)offsetof(struct collision_bsp, vertices) + 4);
							if (edge_count > 0 && vert_count > 0 && edge_addr >= bsp_base && vert_addr >= bsp_base)
							{
								edge_off = foff + (edge_addr - bsp_base);
								vert_off = foff + (vert_addr - bsp_base);
								if ((uint64_t)edge_off + (uint64_t)edge_count * 24u <= sm.file_size &&
									(uint64_t)vert_off + (uint64_t)vert_count * 16u <= sm.file_size)
								{
									out_blueprint->edges = (struct forge_bsp_edge_2d *)malloc(
										(size_t)edge_count * sizeof(struct forge_bsp_edge_2d));
									if (out_blueprint->edges)
									{
										for (e = 0; e < edge_count; e++)
										{
											uint32_t eo = edge_off + (uint32_t)e * 24u;
											int32_t v0 = read_s32(sm.data + eo + 0x00u);
											int32_t v1 = read_s32(sm.data + eo + 0x04u);
											if (v0 >= 0 && v0 < vert_count && v1 >= 0 && v1 < vert_count)
											{
												uint32_t vo0 = vert_off + (uint32_t)v0 * 16u;
												uint32_t vo1 = vert_off + (uint32_t)v1 * 16u;
												struct forge_bsp_edge_2d *ed =
													&out_blueprint->edges[out_blueprint->edge_count++];
												ed->x0 = read_f32(sm.data + vo0 + 0x00u);
												ed->y0 = read_f32(sm.data + vo0 + 0x04u);
												ed->z0 = read_f32(sm.data + vo0 + 0x08u);
												ed->x1 = read_f32(sm.data + vo1 + 0x00u);
												ed->y1 = read_f32(sm.data + vo1 + 0x04u);
												ed->z1 = read_f32(sm.data + vo1 + 0x08u);
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

	forge_source_map_close(&sm);
	return 1;
}

void forge_free_map_blueprint(struct forge_map_blueprint_data *blueprint)
{
	if (!blueprint)
		return;
	if (blueprint->edges)
		free(blueprint->edges);
	memset(blueprint, 0, sizeof(*blueprint));
}

int forge_enumerate_map_catalog(
	char const *map_path,
	int32_t source_cache_id,
	struct forge_catalog_item *out_items,
	int max_items)
{
	struct forge_source_map sm;
	int32_t i;
	int count = 0;

	if (!map_path || !out_items || max_items <= 0)
		return 0;
	if (!forge_source_map_open(map_path, &sm))
		return 0;

	for (i = 0; i < sm.tag_count && count < max_items; i++)
	{
		uint8_t const *inst = src_tag_instance(&sm, i);
		char const *tname = src_tag_name(&sm, i);
		uint32_t g;
		char const *slash;
		struct forge_catalog_item *it;

		if (!inst || !tname || !tname[0])
			continue;
		g = read_u32(inst + 0x00);
		if (g != 'weap' && g != 'vehi' && g != 'eqip' && g != 'itmc' &&
			g != 'scen' && g != 'mach' && g != 'ctrl' && g != 'lifi' &&
			g != 'bipd' && g != 'actv')
		{
			continue;
		}
		it = &out_items[count++];
		memset(it, 0, sizeof(*it));
		it->group_tag = g;
		snprintf(it->tag_path, sizeof(it->tag_path), "%s", tname);
		slash = strrchr(tname, '\\');
		if (!slash) slash = strrchr(tname, '/');
		snprintf(it->display_name, sizeof(it->display_name), "%s", slash ? slash + 1 : tname);
		it->source_cache = source_cache_id;
	}

	forge_source_map_close(&sm);
	return count;
}

static int32_t builder_resolve_tag_from_donors(
	struct forge_builder *b,
	struct forge_source_map const *donors,
	int donor_count,
	uint32_t group_tag,
	char const *tag_path)
{
	int32_t idx;
	int d;
	if (!b || !tag_path || !tag_path[0])
		return -1;
	idx = builder_find_tag(b, group_tag, tag_path);
	if (idx >= 0)
	{
		if (!b->tags[idx].cloned && b->base_map && idx < b->base_map->tag_count)
			builder_ensure_tag(b, b->base_map, idx);
		return idx;
	}
	for (d = 0; d < donor_count; d++)
	{
		if (donors[d].data)
		{
			int32_t sidx = src_map_find_tag(&donors[d], group_tag, tag_path);
			if (sidx >= 0)
				return builder_ensure_tag(b, &donors[d], sidx);
		}
	}
	return -1;
}

static int16_t scenario_ensure_palette_entry(
	struct forge_builder *b,
	uint32_t scnr_off,
	uint32_t pal_field_off,
	uint32_t elem_size,
	int32_t dst_tag_idx)
{
	int32_t count = read_s32(b->tag_buf.data + scnr_off + pal_field_off + 0);
	uint32_t addr = read_u32(b->tag_buf.data + scnr_off + pal_field_off + 4);
	uint32_t target_handle = b->tags[dst_tag_idx].handle;
	int32_t i;
	uint32_t new_off;

	if (count > 0 && addr >= CE_TAG_BASE_ADDR)
	{
		uint32_t arr_off = addr - CE_TAG_BASE_ADDR;
		for (i = 0; i < count; i++)
		{
			uint32_t eo = arr_off + (uint32_t)i * elem_size;
			if (eo + 16u <= b->tag_buf.size && read_u32(b->tag_buf.data + eo + 12u) == target_handle)
				return (int16_t)i;
		}
	}

	new_off = buf_alloc(&b->tag_buf, (uint32_t)(count + 1) * elem_size, 4);
	if (count > 0 && addr >= CE_TAG_BASE_ADDR)
	{
		memcpy(b->tag_buf.data + new_off, b->tag_buf.data + (addr - CE_TAG_BASE_ADDR), (size_t)count * elem_size);
	}
	write_u32(b->tag_buf.data + new_off + (uint32_t)count * elem_size + 0u, b->tags[dst_tag_idx].primary_group);
	write_u32(b->tag_buf.data + new_off + (uint32_t)count * elem_size + 4u, b->tags[dst_tag_idx].name_address);
	write_s32(b->tag_buf.data + new_off + (uint32_t)count * elem_size + 8u, 0);
	write_u32(b->tag_buf.data + new_off + (uint32_t)count * elem_size + 12u, target_handle);

	write_s32(b->tag_buf.data + scnr_off + pal_field_off + 0u, count + 1);
	write_u32(b->tag_buf.data + scnr_off + pal_field_off + 4u, CE_TAG_BASE_ADDR + new_off);
	return (int16_t)count;
}

static uint32_t scenario_append_element(
	struct forge_builder *b,
	uint32_t scnr_off,
	uint32_t block_field_off,
	uint32_t elem_size)
{
	int32_t count = read_s32(b->tag_buf.data + scnr_off + block_field_off + 0);
	uint32_t addr = read_u32(b->tag_buf.data + scnr_off + block_field_off + 4);
	uint32_t new_off = buf_alloc(&b->tag_buf, (uint32_t)(count + 1) * elem_size, 4);

	if (count > 0 && addr >= CE_TAG_BASE_ADDR)
	{
		memcpy(b->tag_buf.data + new_off, b->tag_buf.data + (addr - CE_TAG_BASE_ADDR), (size_t)count * elem_size);
	}
	write_s32(b->tag_buf.data + scnr_off + block_field_off + 0u, count + 1);
	write_u32(b->tag_buf.data + scnr_off + block_field_off + 4u, CE_TAG_BASE_ADDR + new_off);
	return new_off + (uint32_t)count * elem_size;
}

static void write_blueprint_bmp(
	char const *bmp_path,
	struct forge_map_blueprint_data const *bp,
	struct forge_scenario_edit_state const *edits)
{
	enum { W = 140, H = 114, IMG_BYTES = 54 + 140 * 114 * 3 };
	uint8_t *img = (uint8_t *)calloc(1, IMG_BYTES);
	uint8_t *px;
	int x, y, i;
	float span_x = 1.0f, span_y = 1.0f;
	FILE *f;

	if (!img)
		return;
	px = img + 54;
	img[0] = 'B';
	img[1] = 'M';
	write_u32(img + 2, IMG_BYTES);
	write_u32(img + 10, 54);
	write_u32(img + 14, 40);
	write_s32(img + 18, W);
	write_s32(img + 22, H);
	write_u16(img + 26, 1);
	write_u16(img + 28, 24);
	write_u32(img + 34, W * H * 3);

	for (y = 0; y < H; y++)
	{
		for (x = 0; x < W; x++)
		{
			uint8_t *p = px + (y * W + x) * 3;
			int grid = ((x % 14) == 0) || ((y % 14) == 0);
			p[0] = grid ? 62 : 34;
			p[1] = grid ? 42 : 22;
			p[2] = grid ? 20 : 10;
		}
	}

	if (bp && bp->edge_count > 0)
	{
		span_x = bp->max_x - bp->min_x;
		span_y = bp->max_y - bp->min_y;
		if (span_x < 0.001f) span_x = 1.0f;
		if (span_y < 0.001f) span_y = 1.0f;

		for (i = 0; i < bp->edge_count; i++)
		{
			int x0 = 6 + (int)(((bp->edges[i].x0 - bp->min_x) / span_x) * (W - 12));
			int y0 = 6 + (int)(((bp->edges[i].y0 - bp->min_y) / span_y) * (H - 12));
			int x1 = 6 + (int)(((bp->edges[i].x1 - bp->min_x) / span_x) * (W - 12));
			int y1 = 6 + (int)(((bp->edges[i].y1 - bp->min_y) / span_y) * (H - 12));
			int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
			int s;
			if (steps > 200) steps = 200;
			for (s = 0; s <= steps; s++)
			{
				int cx = steps ? (x0 + (x1 - x0) * s / steps) : x0;
				int cy = steps ? (y0 + (y1 - y0) * s / steps) : y0;
				if (cx >= 0 && cx < W && cy >= 0 && cy < H)
				{
					uint8_t *p = px + (cy * W + cx) * 3;
					p[0] = 215; p[1] = 175; p[2] = 95;
				}
			}
		}
	}

	if (bp && edits)
	{
		for (i = 0; i < edits->placed_object_count; i++)
		{
			int cx = 6 + (int)(((edits->placed_objects[i].x - bp->min_x) / span_x) * (W - 12));
			int cy = 6 + (int)(((edits->placed_objects[i].y - bp->min_y) / span_y) * (H - 12));
			int dx, dy;
			for (dy = -1; dy <= 1; dy++)
				for (dx = -1; dx <= 1; dx++)
					if (cx + dx >= 0 && cx + dx < W && cy + dy >= 0 && cy + dy < H)
					{
						uint8_t *p = px + ((cy + dy) * W + (cx + dx)) * 3;
						p[0] = 80; p[1] = 240; p[2] = 255;
					}
		}
	}

	f = fopen(bmp_path, "wb");
	if (f)
	{
		fwrite(img, 1, IMG_BYTES, f);
		fclose(f);
	}
	free(img);
}

static int32_t src_map_find_tag_or_variant(
	struct forge_source_map const *map,
	uint32_t group_tag,
	char const *tag_path)
{
	int32_t idx;
	size_t i;

	if (!map || !tag_path || !tag_path[0])
		return -1;

	idx = src_map_find_tag(map, group_tag, tag_path);
	if (idx >= 0)
		return idx;

	if (group_tag == 'vehi')
	{
		static struct { char const *a; char const *b; } const vehi_pairs[] = {
			{ "vehicles\\warthog\\warthog", "vehicles\\warthog\\mp_warthog" },
			{ "vehicles\\ghost\\ghost", "vehicles\\ghost\\ghost_mp" },
			{ "vehicles\\scorpion\\scorpion", "vehicles\\scorpion\\scorpion_mp" },
			{ "vehicles\\banshee\\banshee", "vehicles\\banshee\\banshee_mp" },
			{ "vehicles\\c gun turret\\c gun turret", "vehicles\\c gun turret\\c gun turret_mp" },
			{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c gun turret\\c gun turret_mp" },
			{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c_gun_turret\\c_gun_turret_mp" }
		};
		for (i = 0; i < sizeof(vehi_pairs) / sizeof(vehi_pairs[0]); i++)
		{
			if (_stricmp(tag_path, vehi_pairs[i].a) == 0)
			{
				idx = src_map_find_tag(map, 'vehi', vehi_pairs[i].b);
				if (idx >= 0) return idx;
			}
			else if (_stricmp(tag_path, vehi_pairs[i].b) == 0)
			{
				idx = src_map_find_tag(map, 'vehi', vehi_pairs[i].a);
				if (idx >= 0) return idx;
			}
		}
	}
	return -1;
}

static uint32_t inplace_append_scenario_element(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	uint32_t scnr_foff,
	uint32_t block_field_off,
	uint32_t elem_size)
{
	int32_t count;
	uint32_t addr;
	uint32_t new_foff;
	uint32_t old_foff = 0;

	if (!out || !out->data || scnr_foff + block_field_off + 12u > out->size)
		return 0;

	count = read_s32(out->data + scnr_foff + block_field_off + 0u);
	addr = read_u32(out->data + scnr_foff + block_field_off + 4u);
	if (count < 0 || count > 8192)
		count = 0;
	if (count > 0 && addr >= CE_TAG_BASE_ADDR)
	{
		old_foff = tag_data_offset + (addr - CE_TAG_BASE_ADDR);
		if ((uint64_t)old_foff + (uint64_t)count * elem_size > out->size)
			count = 0;
	}
	else
	{
		count = 0;
	}

	new_foff = buf_alloc(out, (uint32_t)(count + 1) * elem_size, 4);
	if (!new_foff || !out->data)
		return 0;

	if (count > 0 && old_foff > 0)
	{
		memcpy(out->data + new_foff, out->data + old_foff, (size_t)count * elem_size);
	}

	write_s32(out->data + scnr_foff + block_field_off + 0u, count + 1);
	write_u32(out->data + scnr_foff + block_field_off + 4u, CE_TAG_BASE_ADDR + (new_foff - tag_data_offset));
	write_u32(out->data + scnr_foff + block_field_off + 8u, 0);
	return new_foff + (uint32_t)count * elem_size;
}

static int16_t inplace_ensure_scenario_palette_entry(
	struct forge_byte_buf *out,
	struct forge_source_map const *base_map,
	uint32_t tag_data_offset,
	uint32_t scnr_foff,
	uint32_t pal_field_off,
	uint32_t elem_size,
	int32_t tag_idx)
{
	uint8_t const *inst = src_tag_instance(base_map, tag_idx);
	uint32_t target_group, target_name_addr, target_handle;
	int32_t count, i;
	uint32_t addr;
	uint32_t new_eo;

	if (!inst || !out || !out->data || scnr_foff + pal_field_off + 12u > out->size)
		return -1;

	target_group = read_u32(inst + 0x00u);
	target_handle = read_u32(inst + 0x0Cu);
	target_name_addr = read_u32(inst + 0x10u);

	count = read_s32(out->data + scnr_foff + pal_field_off + 0u);
	addr = read_u32(out->data + scnr_foff + pal_field_off + 4u);
	if (count > 0 && addr >= CE_TAG_BASE_ADDR)
	{
		uint32_t arr_foff = tag_data_offset + (addr - CE_TAG_BASE_ADDR);
		for (i = 0; i < count; i++)
		{
			uint32_t eo = arr_foff + (uint32_t)i * elem_size;
			if (eo + 16u <= out->size && read_u32(out->data + eo + 12u) == target_handle)
				return (int16_t)i;
		}
	}
	else
	{
		count = 0;
	}

	new_eo = inplace_append_scenario_element(out, tag_data_offset, scnr_foff, pal_field_off, elem_size);
	if (!new_eo || !out->data)
		return -1;

	write_u32(out->data + new_eo + 0u, target_group);
	write_u32(out->data + new_eo + 4u, target_name_addr);
	write_s32(out->data + new_eo + 8u, 0);
	write_u32(out->data + new_eo + 12u, target_handle);
	return (int16_t)count;
}

static void inplace_apply_default_object_edit(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	uint32_t scnr_foff,
	struct forge_default_object_edit const *de)
{
	static struct
	{
		uint32_t blk_off;
		uint32_t elem_sz;
	} const obj_blocks[8] = {
		{ (uint32_t)offsetof(struct scenario, scenery), 72u },
		{ (uint32_t)offsetof(struct scenario, bipeds), 120u },
		{ (uint32_t)offsetof(struct scenario, vehicles), 120u },
		{ (uint32_t)offsetof(struct scenario, equipment), 40u },
		{ (uint32_t)offsetof(struct scenario, weapons), 92u },
		{ (uint32_t)offsetof(struct scenario, machines), 64u },
		{ (uint32_t)offsetof(struct scenario, controls), 64u },
		{ (uint32_t)offsetof(struct scenario, light_fixtures), 88u }
	};
	uint32_t best_obj_foff = 0;
	float best_obj_d2 = 36.0f; /* Within 6.0 world units of original spawn position */
	uint32_t best_ne_foff = 0;
	float best_ne_d2 = 36.0f;
	size_t b;
	int32_t i;

	if (!out || !out->data || !de || !de->active)
		return;

	/* 1. Search standard scenario object placement blocks */
	for (b = 0; b < 8; b++)
	{
		uint32_t f_off = scnr_foff + obj_blocks[b].blk_off;
		int32_t cnt;
		uint32_t addr, arr_foff;
		if (f_off + 12u > out->size)
			continue;
		cnt = read_s32(out->data + f_off + 0u);
		addr = read_u32(out->data + f_off + 4u);
		if (cnt <= 0 || addr < CE_TAG_BASE_ADDR)
			continue;
		arr_foff = tag_data_offset + (addr - CE_TAG_BASE_ADDR);
		for (i = 0; i < cnt; i++)
		{
			uint32_t eo = arr_foff + (uint32_t)i * obj_blocks[b].elem_sz;
			float ex, ey, ez, dx, dy, dz, d2;
			if (eo + obj_blocks[b].elem_sz > out->size)
				break;
			ex = read_f32(out->data + eo + 0x08u);
			ey = read_f32(out->data + eo + 0x0Cu);
			ez = read_f32(out->data + eo + 0x10u);
			dx = ex - de->orig_x;
			dy = ey - de->orig_y;
			dz = ez - de->orig_z;
			d2 = dx * dx + dy * dy + dz * dz;
			if (d2 < best_obj_d2)
			{
				best_obj_d2 = d2;
				best_obj_foff = eo;
			}
		}
	}

	if (best_obj_foff > 0)
	{
		if (de->deleted)
		{
			write_s16(out->data + best_obj_foff + 0x00u, -1);
			write_f32(out->data + best_obj_foff + 0x08u, -99999.0f);
			write_f32(out->data + best_obj_foff + 0x0Cu, -99999.0f);
			write_f32(out->data + best_obj_foff + 0x10u, -99999.0f);
		}
		else
		{
			write_f32(out->data + best_obj_foff + 0x08u, de->new_x);
			write_f32(out->data + best_obj_foff + 0x0Cu, de->new_y);
			write_f32(out->data + best_obj_foff + 0x10u, de->new_z);
			write_f32(out->data + best_obj_foff + 0x14u, de->new_yaw);
			write_f32(out->data + best_obj_foff + 0x18u, de->new_pitch);
			write_f32(out->data + best_obj_foff + 0x1Cu, de->new_roll);
		}
	}

	/* 2. Search multiplayer netgame_equipment block (MP weapons, powerups, grenades, vehicles) */
	{
		uint32_t ne_field = scnr_foff + (uint32_t)offsetof(struct scenario, netgame_equipment);
		if (ne_field + 12u <= out->size)
		{
			int32_t cnt = read_s32(out->data + ne_field + 0u);
			uint32_t addr = read_u32(out->data + ne_field + 4u);
			if (cnt > 0 && addr >= CE_TAG_BASE_ADDR)
			{
				uint32_t arr_foff = tag_data_offset + (addr - CE_TAG_BASE_ADDR);
				for (i = 0; i < cnt; i++)
				{
					uint32_t eo = arr_foff + (uint32_t)i * 144u;
					float ex, ey, ez, dx, dy, dz, d2;
					if (eo + 144u > out->size)
						break;
					ex = read_f32(out->data + eo + 0x40u);
					ey = read_f32(out->data + eo + 0x44u);
					ez = read_f32(out->data + eo + 0x48u);
					dx = ex - de->orig_x;
					dy = ey - de->orig_y;
					dz = ez - de->orig_z;
					d2 = dx * dx + dy * dy + dz * dz;
					if (d2 < best_ne_d2)
					{
						best_ne_d2 = d2;
						best_ne_foff = eo;
					}
				}
			}
		}
	}

	if (best_ne_foff > 0 && (best_obj_foff == 0 || best_ne_d2 <= best_obj_d2))
	{
		if (de->deleted)
		{
			write_s16(out->data + best_ne_foff + 0x04u, 0);
			write_s16(out->data + best_ne_foff + 0x06u, 0);
			write_s16(out->data + best_ne_foff + 0x08u, 0);
			write_s16(out->data + best_ne_foff + 0x0Au, 0);
			write_s32(out->data + best_ne_foff + 0x5Cu, -1);
			write_f32(out->data + best_ne_foff + 0x40u, -99999.0f);
			write_f32(out->data + best_ne_foff + 0x44u, -99999.0f);
			write_f32(out->data + best_ne_foff + 0x48u, -99999.0f);
		}
		else
		{
			write_f32(out->data + best_ne_foff + 0x40u, de->new_x);
			write_f32(out->data + best_ne_foff + 0x44u, de->new_y);
			write_f32(out->data + best_ne_foff + 0x48u, de->new_z);
			write_f32(out->data + best_ne_foff + 0x4Cu, de->new_yaw);
		}
	}
}

int forge_export_scenario_map(
	char const *base_map_path,
	char const *ce_res_dir,
	char const *pc_res_dir,
	char const *const *donor_map_paths,
	int donor_map_count,
	struct forge_scenario_edit_state const *edits,
	char const *output_map_name,
	char const *output_dir)
{
	struct forge_source_map base_map;
	struct forge_byte_buf out;
	struct forge_map_blueprint_data *bp = NULL;
	char map_path[512], bmp_path[512], txt_path[512];
	uint8_t const *scnr_inst;
	uint32_t scnr_addr, scnr_off, scnr_foff;
	int32_t i;
	FILE *f;

	(void)ce_res_dir;
	(void)pc_res_dir;
	(void)donor_map_paths;
	(void)donor_map_count;

	if (!base_map_path || !output_map_name || !output_dir)
		return 0;

#undef _mkdir
#undef mkdir
	FORGE_MKDIR("forge_maps");
	FORGE_MKDIR(output_dir);

	/* Ensure we open a Custom Edition (v609) base map container so custom_maps/ always receives a valid v609 cache */
	if (!forge_source_map_open(base_map_path, &base_map) || base_map.version != CACHE_VERSION_CE)
	{
		if (base_map.data)
			forge_source_map_close(&base_map);
		if (!forge_source_map_open("OpenCE Forge/forge_maps/bloodgulch.map", &base_map))
		{
			if (!forge_source_map_open("../FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bloodgulch.map", &base_map))
			{
				if (!forge_source_map_open("FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bloodgulch.map", &base_map))
					return 0;
			}
		}
	}

	scnr_inst = src_tag_instance(&base_map, base_map.scenario_tag_index);
	if (!scnr_inst)
	{
		forge_source_map_close(&base_map);
		return 0;
	}
	scnr_addr = read_u32(scnr_inst + 0x14u);
	if (scnr_addr < CE_TAG_BASE_ADDR)
	{
		forge_source_map_close(&base_map);
		return 0;
	}
	scnr_off = scnr_addr - CE_TAG_BASE_ADDR;
	if (scnr_off + sizeof(struct scenario) > base_map.tag_data_size)
	{
		forge_source_map_close(&base_map);
		return 0;
	}
	scnr_foff = base_map.tag_data_offset + scnr_off;

	/* Copy base_map into a single growable buffer (only ~14 MB, zero OOM risk in 32-bit address space!) */
	buf_init(&out, base_map.file_size + 262144u);
	if (!out.data)
	{
		forge_source_map_close(&base_map);
		return 0;
	}
	memcpy(out.data, base_map.data, base_map.file_size);
	out.size = base_map.file_size;

	if (edits)
	{
		/* 1. Apply edits/deletions to default scenario objects */
		for (i = 0; i < edits->default_edit_count; i++)
		{
			inplace_apply_default_object_edit(&out, base_map.tag_data_offset, scnr_foff, &edits->default_edits[i]);
		}

		/* 2. Apply player spawn and netgame flag additions/replacements */
		if (edits->replace_player_spawns)
		{
			uint32_t pl_field = (uint32_t)offsetof(struct scenario, players);
			if (edits->player_spawn_count > 0)
			{
				uint32_t arr_foff = buf_alloc(&out, (uint32_t)edits->player_spawn_count * 52u, 4);
				if (arr_foff > 0 && out.data)
				{
					write_s32(out.data + scnr_foff + pl_field + 0u, edits->player_spawn_count);
					write_u32(out.data + scnr_foff + pl_field + 4u, CE_TAG_BASE_ADDR + (arr_foff - base_map.tag_data_offset));
					write_u32(out.data + scnr_foff + pl_field + 8u, 0);
					for (i = 0; i < edits->player_spawn_count; i++)
					{
						struct forge_player_spawn_entry const *sp = &edits->player_spawns[i];
						uint32_t eo = arr_foff + (uint32_t)i * 52u;
						write_f32(out.data + eo + 0x00u, sp->x);
						write_f32(out.data + eo + 0x04u, sp->y);
						write_f32(out.data + eo + 0x08u, sp->z);
						write_f32(out.data + eo + 0x0Cu, sp->yaw);
						write_s16(out.data + eo + 0x10u, sp->team_index);
						write_s16(out.data + eo + 0x12u, sp->bsp_index);
						write_s16(out.data + eo + 0x14u, sp->type0);
						write_s16(out.data + eo + 0x16u, sp->type1);
						write_s16(out.data + eo + 0x18u, sp->type2);
						write_s16(out.data + eo + 0x1Au, sp->type3);
					}
				}
			}
			else
			{
				write_s32(out.data + scnr_foff + pl_field + 0u, 0);
			}
		}
		else
		{
			for (i = 0; i < edits->player_spawn_count; i++)
			{
				struct forge_player_spawn_entry const *sp = &edits->player_spawns[i];
				uint32_t eo = inplace_append_scenario_element(
					&out, base_map.tag_data_offset, scnr_foff, (uint32_t)offsetof(struct scenario, players), 52u);
				if (eo > 0 && out.data)
				{
					write_f32(out.data + eo + 0x00u, sp->x);
					write_f32(out.data + eo + 0x04u, sp->y);
					write_f32(out.data + eo + 0x08u, sp->z);
					write_f32(out.data + eo + 0x0Cu, sp->yaw);
					write_s16(out.data + eo + 0x10u, sp->team_index);
					write_s16(out.data + eo + 0x12u, sp->bsp_index);
					write_s16(out.data + eo + 0x14u, sp->type0 ? sp->type0 : 12);
					write_s16(out.data + eo + 0x16u, sp->type1);
					write_s16(out.data + eo + 0x18u, sp->type2);
					write_s16(out.data + eo + 0x1Au, sp->type3);
				}
			}
		}

		if (edits->replace_netgame_flags)
		{
			uint32_t nf_field = (uint32_t)offsetof(struct scenario, netgame_flags);
			if (edits->netgame_flag_count > 0)
			{
				uint32_t arr_foff = buf_alloc(&out, (uint32_t)edits->netgame_flag_count * 148u, 4);
				if (arr_foff > 0 && out.data)
				{
					write_s32(out.data + scnr_foff + nf_field + 0u, edits->netgame_flag_count);
					write_u32(out.data + scnr_foff + nf_field + 4u, CE_TAG_BASE_ADDR + (arr_foff - base_map.tag_data_offset));
					write_u32(out.data + scnr_foff + nf_field + 8u, 0);
					for (i = 0; i < edits->netgame_flag_count; i++)
					{
						struct forge_netgame_flag_entry const *nf = &edits->netgame_flags[i];
						uint32_t eo = arr_foff + (uint32_t)i * 148u;
						write_f32(out.data + eo + 0x00u, nf->x);
						write_f32(out.data + eo + 0x04u, nf->y);
						write_f32(out.data + eo + 0x08u, nf->z);
						write_f32(out.data + eo + 0x0Cu, nf->yaw);
						write_s16(out.data + eo + 0x10u, nf->type);
						write_s16(out.data + eo + 0x12u, nf->usage_id);
						write_s32(out.data + eo + 0x1Cu, -1);
					}
				}
			}
			else
			{
				write_s32(out.data + scnr_foff + nf_field + 0u, 0);
			}
		}
		else
		{
			for (i = 0; i < edits->netgame_flag_count; i++)
			{
				struct forge_netgame_flag_entry const *nf = &edits->netgame_flags[i];
				uint32_t eo = inplace_append_scenario_element(
					&out, base_map.tag_data_offset, scnr_foff, (uint32_t)offsetof(struct scenario, netgame_flags), 148u);
				if (eo > 0 && out.data)
				{
					write_f32(out.data + eo + 0x00u, nf->x);
					write_f32(out.data + eo + 0x04u, nf->y);
					write_f32(out.data + eo + 0x08u, nf->z);
					write_f32(out.data + eo + 0x0Cu, nf->yaw);
					write_s16(out.data + eo + 0x10u, nf->type);
					write_s16(out.data + eo + 0x12u, nf->usage_id);
					write_s32(out.data + eo + 0x1Cu, -1);
				}
			}
		}

		if (edits->replace_netgame_equipment)
			write_s32(out.data + scnr_foff + (uint32_t)offsetof(struct scenario, netgame_equipment), 0);

		/* 3. Append newly placed objects whose tags exist natively in base_map */
		for (i = 0; i < edits->placed_object_count; i++)
		{
			struct forge_placed_object_entry const *po = &edits->placed_objects[i];
			uint32_t g = po->group_tag;
			uint32_t pal_off = 0, blk_off = 0, elem_sz = 0;
			int32_t tag_idx;

			if (!po->active && po->tag_path[0] == 0)
				continue;
			if (po->active == 0 && (po->x < -90000.0f))
				continue;

			if (g == 'scen' || po->category == _forge_object_scenery)
			{
				g = 'scen'; blk_off = (uint32_t)offsetof(struct scenario, scenery); pal_off = (uint32_t)offsetof(struct scenario, scenery_palette); elem_sz = 72u;
			}
			else if (g == 'bipd' || po->category == _forge_object_biped)
			{
				g = 'bipd'; blk_off = (uint32_t)offsetof(struct scenario, bipeds); pal_off = (uint32_t)offsetof(struct scenario, biped_palette); elem_sz = 120u;
			}
			else if (g == 'vehi' || po->category == _forge_object_vehicle)
			{
				g = 'vehi'; blk_off = (uint32_t)offsetof(struct scenario, vehicles); pal_off = (uint32_t)offsetof(struct scenario, vehicle_palette); elem_sz = 120u;
			}
			else if (g == 'weap' || po->category == _forge_object_weapon)
			{
				g = 'weap'; blk_off = (uint32_t)offsetof(struct scenario, weapons); pal_off = (uint32_t)offsetof(struct scenario, weapon_palette); elem_sz = 92u;
			}
			else if (g == 'eqip' || po->category == _forge_object_equipment)
			{
				g = 'eqip'; blk_off = (uint32_t)offsetof(struct scenario, equipment); pal_off = (uint32_t)offsetof(struct scenario, equipment_palette); elem_sz = 40u;
			}

			if (elem_sz > 0)
			{
				tag_idx = src_map_find_tag_or_variant(&base_map, g, po->tag_path);
				if (tag_idx >= 0)
				{
					int16_t pal = inplace_ensure_scenario_palette_entry(
						&out, &base_map, base_map.tag_data_offset, scnr_foff, pal_off, 48u, tag_idx);
					if (pal >= 0)
					{
						uint32_t eo = inplace_append_scenario_element(
							&out, base_map.tag_data_offset, scnr_foff, blk_off, elem_sz);
						if (eo > 0 && out.data)
						{
							write_s16(out.data + eo + 0x00u, pal);
							write_s16(out.data + eo + 0x02u, -1);
							write_u16(out.data + eo + 0x04u, po->placement_flags);
							write_s16(out.data + eo + 0x06u, 0);
							write_f32(out.data + eo + 0x08u, po->x);
							write_f32(out.data + eo + 0x0Cu, po->y);
							write_f32(out.data + eo + 0x10u, po->z);
							write_f32(out.data + eo + 0x14u, po->yaw);
							write_f32(out.data + eo + 0x18u, po->pitch);
							write_f32(out.data + eo + 0x1Cu, po->roll);
							if (g == 'vehi')
								out.data[eo + 0x5Au] = 0x3Fu;
						}
					}
				}
			}
		}
	}

	/* Align file end to 32 bytes and update cache header fields */
	buf_alloc(&out, 0, 32);
	if (!out.data)
	{
		forge_source_map_close(&base_map);
		return 0;
	}

	write_s32(out.data + 0x04u, CACHE_VERSION_CE);
	write_u32(out.data + 0x08u, out.size);
	write_u32(out.data + 0x0Cu, 0);
	write_u32(out.data + 0x10u, base_map.tag_data_offset);
	write_u32(out.data + 0x14u, out.size - base_map.tag_data_offset);
	memset(out.data + 0x20u, 0, 32);
	snprintf((char *)(out.data + 0x20u), 32, "%s", output_map_name);
	write_s16(out.data + 0x60u, 1); /* _scenario_type_multiplayer */
	if (scnr_foff + 4u <= out.size)
		write_s16(out.data + scnr_foff + 0x00u, 1);
	write_u32(out.data + 0x1FCu, 0x464C4731u); /* 'FLG1' OpenCE Forge map signature */
	write_u32(out.data + 0x7FCu, CACHE_FOOTER_SIGNATURE);

	snprintf(map_path, sizeof(map_path), "%s/%s.map", output_dir, output_map_name);
	snprintf(bmp_path, sizeof(bmp_path), "%s/%s.bmp", output_dir, output_map_name);
	snprintf(txt_path, sizeof(txt_path), "%s/%s.txt", output_dir, output_map_name);

	f = fopen(map_path, "wb");
	if (!f)
	{
		buf_free(&out);
		forge_source_map_close(&base_map);
		return 0;
	}
	if (fwrite(out.data, 1, out.size, f) != out.size)
	{
		fclose(f);
		buf_free(&out);
		forge_source_map_close(&base_map);
		return 0;
	}
	fclose(f);
	buf_free(&out);

	bp = (struct forge_map_blueprint_data *)calloc(1, sizeof(*bp));
	if (bp)
	{
		if (forge_load_map_blueprint(map_path, bp))
		{
			write_blueprint_bmp(bmp_path, bp, edits);
			forge_free_map_blueprint(bp);
		}
		free(bp);
	}

	f = fopen(txt_path, "wb");
	if (f)
	{
		fprintf(f, "[OpenCE Forge Map]\r\n");
		fprintf(f, "ForgeSource=UserCreated\r\n");
		fprintf(f, "Map Name: %s\r\n", output_map_name);
		fprintf(f, "Base Map: %s\r\n", base_map.map_name);
		fprintf(f, "Author: OpenCE Forge Creator\r\n");
		fprintf(f, "Objects Placed: %d\r\n", edits ? (int)edits->placed_object_count : 0);
		fprintf(f, "AI Squads: %d\r\n", edits ? (int)edits->ai_spawn_count : 0);
		if (edits)
		{
			fprintf(f, "[ForgeState]\r\n");
			for (i = 0; i < edits->default_edit_count; i++)
			{
				struct forge_default_object_edit const *de = &edits->default_edits[i];
				if (!de->active)
					continue;
				fprintf(f, "DEFAULT_EDIT|%u|%d|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%s\r\n",
					(unsigned)de->group_tag,
					(int)de->deleted,
					de->orig_x, de->orig_y, de->orig_z,
					de->new_x, de->new_y, de->new_z,
					de->new_yaw, de->new_pitch, de->new_roll,
					de->tag_path);
			}
			for (i = 0; i < edits->placed_object_count; i++)
			{
				struct forge_placed_object_entry const *po = &edits->placed_objects[i];
				int baked = (src_map_find_tag_or_variant(&base_map, po->group_tag, po->tag_path) >= 0) ? 1 : 0;
				if (!po->tag_path[0] || po->x < -90000.0f)
					continue;
				fprintf(f, "OBJECT|%u|%d|%d|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%d|%s\r\n",
					(unsigned)po->group_tag,
					(int)po->source_cache_id,
					baked,
					po->x, po->y, po->z,
					po->yaw, po->pitch, po->roll,
					(int)po->team_index,
					po->tag_path);
			}
			for (i = 0; i < edits->ai_spawn_count; i++)
			{
				struct forge_ai_spawn_entry const *ai = &edits->ai_spawns[i];
				if (!ai->actor_variant_path[0])
					continue;
				fprintf(f, "AI|%.4f|%.4f|%.4f|%.4f|%d|%s\r\n",
					ai->x, ai->y, ai->z, ai->yaw,
					(int)ai->team_designator,
					ai->actor_variant_path);
			}
		}
		fclose(f);
	}

	forge_source_map_close(&base_map);
	return 1;
}

