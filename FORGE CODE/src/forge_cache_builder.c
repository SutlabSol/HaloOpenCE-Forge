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
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "rasterizer/rasterizer_swizzle.h"
#include "rasterizer/rasterizer_geometry.h"
#include "scenario/scenario_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "physics/collision_bsp_definitions.h"
#include "../../port/third_party/zlib/zlib_prefixed.h"

extern void *custom_edition_sounds_encode(
	void const *samples,
	long size,
	short compression,
	boolean big_endian_pcm,
	long channels,
	long rate,
	long encoded_rate,
	unsigned long *encoded_bytes);
extern int forge_ensure_all_xbox_maps_cached(void);
extern void platform_translate_path(char const *xbox_path, char *out_path, size_t out_size);

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
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
#undef _mkdir
#undef mkdir
#undef memcmp
#undef memcpy
#undef memmove
#undef memset

/* ---------- constants */

#define CACHE_HEADER_SIGNATURE 'head'
#define CACHE_FOOTER_SIGNATURE 'foot'
#define TAG_INDEX_SIGNATURE 'tags'
#define CACHE_VERSION_XBOX 5
#define CACHE_VERSION_PC 7
#define CACHE_VERSION_CE 609

#define CE_TAG_BASE_ADDR 0x40440000UL
#define XBOX_TAG_BASE_ADDR 0x803A6000UL
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
	uint32_t tag_base_addr;
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
	uint8_t hdr_peek[0x800];

	memset(map, 0, sizeof(*map));
	if (!path || !path[0])
		return 0;
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
	fseek(f, 0, SEEK_SET);
	if (fread(hdr_peek, 1, 0x800, f) != 0x800)
	{
		fclose(f);
		return 0;
	}
	if (read_u32(hdr_peek) != CACHE_HEADER_SIGNATURE ||
		read_u32(hdr_peek + 0x7FC) != CACHE_FOOTER_SIGNATURE)
	{
		fclose(f);
		return 0;
	}
	map->version = read_s32(hdr_peek + 0x04);
	if (map->version != CACHE_VERSION_XBOX &&
		map->version != CACHE_VERSION_PC &&
		map->version != CACHE_VERSION_CE)
	{
		fclose(f);
		return 0;
	}
	map->tag_data_offset = read_u32(hdr_peek + 0x10);
	map->tag_data_size = read_u32(hdr_peek + 0x14);
	memcpy(map->map_name, hdr_peek + 0x20, 32);
	map->map_name[31] = 0;
	map->scenario_type = read_s16(hdr_peek + 0x60);

	/* Transparently handle zlib-compressed Xbox v5 maps (or load from xbox_cache if already decompressed) */
	if (map->version == CACHE_VERSION_XBOX &&
		(uint64_t)map->tag_data_offset + (uint64_t)map->tag_data_size > (uint64_t)sz)
	{
		uint32_t decomp_len = read_u32(hdr_peek + 0x08);
		char cache_cand[512];
		FILE *cf;

		if (decomp_len < map->tag_data_offset + map->tag_data_size)
			decomp_len = map->tag_data_offset + map->tag_data_size;
		if (decomp_len < 0x800u || decomp_len > 128u * 1024u * 1024u)
		{
			fclose(f);
			return 0;
		}

		snprintf(cache_cand, sizeof(cache_cand), "OpenCE Forge/cached_tags/xbox_cache/%s", map->filename);
		cf = fopen(cache_cand, "rb");
		if (cf)
		{
			long csz = 0;
			if (fseek(cf, 0, SEEK_END) == 0 && (csz = ftell(cf)) >= (long)(map->tag_data_offset + map->tag_data_size))
			{
				map->file_size = (uint32_t)csz;
				map->data = (uint8_t *)malloc(map->file_size);
				if (map->data)
				{
					fseek(cf, 0, SEEK_SET);
					if (fread(map->data, 1, map->file_size, cf) == map->file_size)
					{
						fclose(cf);
						fclose(f);
						goto parse_tag_header;
					}
					free(map->data);
					map->data = NULL;
				}
			}
			fclose(cf);
		}

		{
			uint32_t comp_len = (uint32_t)sz - 0x800u;
			uint8_t *comp_buf = (uint8_t *)malloc(comp_len);
			uLongf dst_len = (uLongf)(decomp_len - 0x800u);
			if (!comp_buf)
			{
				fclose(f);
				return 0;
			}
			fseek(f, 0x800, SEEK_SET);
			if (fread(comp_buf, 1, comp_len, f) != comp_len)
			{
				free(comp_buf);
				fclose(f);
				return 0;
			}
			fclose(f);
			f = NULL;

			map->file_size = decomp_len;
			map->data = (uint8_t *)malloc(decomp_len);
			if (!map->data)
			{
				free(comp_buf);
				return 0;
			}
			memcpy(map->data, hdr_peek, 0x800u);
			if (uncompress(map->data + 0x800u, &dst_len, comp_buf, (uLong)comp_len) != Z_OK ||
				0x800u + (uint32_t)dst_len < map->tag_data_offset + map->tag_data_size)
			{
				free(comp_buf);
				free(map->data);
				map->data = NULL;
				return 0;
			}
			free(comp_buf);
			map->file_size = 0x800u + (uint32_t)dst_len;

			/* Cache the decompressed Xbox map to OpenCE Forge/cached_tags/xbox_cache/ for instant future opens */
			FORGE_MKDIR("OpenCE Forge");
			FORGE_MKDIR("OpenCE Forge/cached_tags");
			FORGE_MKDIR("OpenCE Forge/cached_tags/xbox_cache");
			cf = fopen(cache_cand, "wb");
			if (cf)
			{
				fwrite(map->data, 1, map->file_size, cf);
				fclose(cf);
			}
		}
	}
	else
	{
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
	}

parse_tag_header:
	if (map->tag_data_offset < 0x800 ||
		map->tag_data_size < 0x24u ||
		map->tag_data_offset + map->tag_data_size > map->file_size)
	{
		free(map->data);
		map->data = NULL;
		return 0;
	}
	tag_hdr_off = map->tag_data_offset;
	if (map->version == CACHE_VERSION_XBOX)
	{
		if (read_u32(map->data + tag_hdr_off + 0x20) != TAG_INDEX_SIGNATURE)
		{
			free(map->data);
			map->data = NULL;
			return 0;
		}
	}
	else if (read_u32(map->data + tag_hdr_off + 0x24) != TAG_INDEX_SIGNATURE)
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
	map->tag_base_addr = (map->version == CACHE_VERSION_XBOX)
		? ((inst_addr >= 0x24u) ? (inst_addr - 0x24u) : XBOX_TAG_BASE_ADDR)
		: CE_TAG_BASE_ADDR;

	if (inst_addr < map->tag_base_addr ||
		map->tag_count <= 0 ||
		map->tag_count > 65535 ||
		(inst_addr - map->tag_base_addr) + (uint32_t)map->tag_count * TAG_INSTANCE_SIZE > map->tag_data_size)
	{
		free(map->data);
		map->data = NULL;
		return 0;
	}
	map->instances_offset = inst_addr - map->tag_base_addr;
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
	if (name_addr < map->tag_base_addr)
		return NULL;
	off = name_addr - map->tag_base_addr;
	if (off >= map->tag_data_size)
		return NULL;
	if (!memchr(map->data + map->tag_data_offset + off, 0, map->tag_data_size - off))
		return NULL;
	return (char const *)(map->data + map->tag_data_offset + off);
}

static uint8_t const *src_ptr(struct forge_source_map const *map, uint32_t addr, uint32_t bytes)
{
	uint32_t off;
	if (addr < map->tag_base_addr)
		return NULL;
	off = addr - map->tag_base_addr;
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

static uint8_t *build_blueprint_bmp_buffer(
	char const *custom_bmp_hint_path,
	struct forge_map_blueprint_data const *bp,
	struct forge_scenario_edit_state const *edits,
	uint32_t *out_bmp_size)
{
	enum { W = 140, H = 114, IMG_BYTES = 54 + 140 * 114 * 3 };
	uint8_t *img;
	uint8_t *px;
	int x, y, i;
	float span_x = 1.0f, span_y = 1.0f;

	if (out_bmp_size)
		*out_bmp_size = 0;

	/* Allow a user-supplied custom .bmp (if present) to be embedded into the .map */
	if (custom_bmp_hint_path && custom_bmp_hint_path[0])
	{
		FILE *f = fopen(custom_bmp_hint_path, "rb");
		if (f)
		{
			uint8_t hdr[14];
			size_t nr = fread(hdr, 1, sizeof(hdr), f);
			if (nr == sizeof(hdr) && hdr[0] == 'B' && hdr[1] == 'M')
			{
				uint32_t sig = read_u32(hdr + 6);
				uint32_t fsz = read_u32(hdr + 2);
				if (sig != 0x504D4246u && fsz >= 54u && fsz <= 4u * 1024u * 1024u)
				{
					uint8_t *custom_buf = (uint8_t *)malloc(fsz);
					if (custom_buf)
					{
						fseek(f, 0, SEEK_SET);
						if (fread(custom_buf, 1, fsz, f) == fsz)
						{
							fclose(f);
							if (out_bmp_size)
								*out_bmp_size = fsz;
							return custom_buf;
						}
						free(custom_buf);
					}
				}
			}
			fclose(f);
		}
	}

	img = (uint8_t *)calloc(1, IMG_BYTES);
	if (!img)
		return NULL;
	px = img + 54;
	img[0] = 'B';
	img[1] = 'M';
	write_u32(img + 2, IMG_BYTES);
	write_u32(img + 6, 0x504D4246u); /* 'FBMP' auto-generated blueprint marker in bfReserved1/2 */
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

	if (out_bmp_size)
		*out_bmp_size = (uint32_t)IMG_BYTES;
	return img;
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

	if (group_tag == 'mach' && _stricmp(tag_path, "levels\\a30\\devices\\beam emitter\\beam emitter red") == 0)
	{
		idx = src_map_find_tag(map, 'mach', "levels\\a30\\devices\\beam emitter\\beam emitter");
		if (idx >= 0)
			return idx;
	}

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
	uint8_t const *inst;
	uint32_t inst_foff;
	uint32_t target_group, target_name_addr, target_handle;
	int32_t count, i, cur_tag_count;
	uint32_t addr;
	uint32_t new_eo;

	(void)base_map;
	if (!out || !out->data || tag_idx < 0 || scnr_foff + pal_field_off + 12u > out->size)
		return -1;
	cur_tag_count = read_s32(out->data + tag_data_offset + 0x0Cu);
	inst_foff = tag_data_offset + 0x28u + (uint32_t)tag_idx * 32u;
	if (tag_idx >= cur_tag_count || inst_foff + 32u > out->size)
		return -1;
	inst = out->data + inst_foff;

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

static void cb_sanitize_cached_leaf(char const *tag_path, char *out_name, size_t out_sz)
{
	size_t i = 0, j = 0;
	if (!out_name || out_sz == 0)
		return;
	for (i = 0; tag_path && tag_path[i] && j + 1 < out_sz; i++)
	{
		char c = tag_path[i];
		if (c == '\\' || c == '/' || c == ' ' || c == ':')
			out_name[j++] = '_';
		else
			out_name[j++] = c;
	}
	out_name[j] = 0;
}

static void cb_build_ftag_path(uint32_t group_tag, char const *tag_path, char *out_path, size_t out_sz)
{
	char sanitized[200];
	char const *sub_dir = "misc";
	if (group_tag == 'scen') sub_dir = "scenery";
	else if (group_tag == 'vehi') sub_dir = "vehicles";
	else if (group_tag == 'weap' || group_tag == 'eqip') sub_dir = "weapons";
	else if (group_tag == 'bipd') sub_dir = "bipeds";
	else if (group_tag == 'mach' || group_tag == 'ctrl' || group_tag == 'lifi') sub_dir = "devices";
	cb_sanitize_cached_leaf(tag_path, sanitized, sizeof(sanitized));
	snprintf(out_path, out_sz, "OpenCE Forge/cached_tags/%s/%s.ftag", sub_dir, sanitized);
}

static void cb_build_fcol_path(uint32_t group_tag, char const *tag_path, char *out_path, size_t out_sz)
{
	char sanitized[200];
	char const *sub_dir = "misc";
	if (group_tag == 'scen') sub_dir = "scenery";
	else if (group_tag == 'vehi') sub_dir = "vehicles";
	else if (group_tag == 'weap' || group_tag == 'eqip') sub_dir = "weapons";
	else if (group_tag == 'bipd') sub_dir = "bipeds";
	else if (group_tag == 'mach' || group_tag == 'ctrl' || group_tag == 'lifi') sub_dir = "devices";
	cb_sanitize_cached_leaf(tag_path, sanitized, sizeof(sanitized));
	snprintf(out_path, out_sz, "OpenCE Forge/cached_tags/%s/%s.fcol", sub_dir, sanitized);
}

static void cb_build_fbtm_path(char const *bitm_tag_path, char *out_path, size_t out_sz)
{
	char sanitized[200];
	cb_sanitize_cached_leaf(bitm_tag_path, sanitized, sizeof(sanitized));
	snprintf(out_path, out_sz, "OpenCE Forge/cached_tags/bitmaps/%s.fbtm", sanitized);
}

struct forge_pending_bake_part
{
	uint32_t vtx_count;
	uint32_t idx_count;
	uint32_t extra_vtx_rel_off;
	uint32_t extra_idx_rel_off;
	char shdr_path[128];
	char bitm_path[128];
};

struct forge_pending_ocetags_gpart
{
	uint32_t tag_idx;
	uint32_t geom_idx;
	uint32_t part_idx;
	uint32_t vtx_count;
	uint32_t idx_count;
	uint32_t extra_vtx_rel_off;
	uint32_t extra_idx_rel_off;
};

struct forge_pending_bake_obj
{
	uint32_t group_tag;
	char tag_path[256];
	float bounding_radius;
	float u_scale;
	float v_scale;
	int part_count;
	struct forge_pending_bake_part parts[32];
	char *ocetags_json;
	size_t ocetags_json_len;
	char xbox_map_path[512];
	int ocetags_gpart_count;
	struct forge_pending_ocetags_gpart ocetags_gparts[256];
	int32_t baked_tag_idx;
};

struct cb_deferred_media_patch
{
	uint32_t field_foff;
	uint32_t media_rel_off;
};

static struct forge_byte_buf s_cb_raw_media;
static struct cb_deferred_media_patch s_cb_media_patches[4096];
static int s_cb_media_patch_count = 0;

static void cb_reset_deferred_media(void)
{
	buf_free(&s_cb_raw_media);
	buf_init(&s_cb_raw_media, 65536u);
	s_cb_media_patch_count = 0;
}

static void cb_alloc_deferred_media(uint8_t const *bytes, uint32_t size, uint32_t align, uint32_t field_foff)
{
	uint32_t rel_off;
	if (!bytes || size == 0 || s_cb_media_patch_count >= 4096)
		return;
	rel_off = buf_alloc(&s_cb_raw_media, size, align);
	if (!s_cb_raw_media.data)
		return;
	memcpy(s_cb_raw_media.data + rel_off, bytes, size);
	s_cb_media_patches[s_cb_media_patch_count].field_foff = field_foff;
	s_cb_media_patches[s_cb_media_patch_count].media_rel_off = rel_off;
	s_cb_media_patch_count++;
}

static float cb_clamp_unit_float(float v)
{
	if (v != v) return 0.0f;
	if (v < -1.0f) return -1.0f;
	if (v > 1.0f) return 1.0f;
	return v;
}

static void cb_sanitize_vertex_68(uint8_t *v)
{
	float nx, ny, nz, bx, by, bz, tx, ty, tz;
	nx = cb_clamp_unit_float(read_f32(v + 0x0Cu));
	ny = cb_clamp_unit_float(read_f32(v + 0x10u));
	nz = cb_clamp_unit_float(read_f32(v + 0x14u));
	if (nx == 0.0f && ny == 0.0f && nz == 0.0f)
		nz = 1.0f;
	bx = cb_clamp_unit_float(read_f32(v + 0x18u));
	by = cb_clamp_unit_float(read_f32(v + 0x1Cu));
	bz = cb_clamp_unit_float(read_f32(v + 0x20u));
	tx = cb_clamp_unit_float(read_f32(v + 0x24u));
	ty = cb_clamp_unit_float(read_f32(v + 0x28u));
	tz = cb_clamp_unit_float(read_f32(v + 0x2Cu));
	write_f32(v + 0x0Cu, nx);
	write_f32(v + 0x10u, ny);
	write_f32(v + 0x14u, nz);
	write_f32(v + 0x18u, bx);
	write_f32(v + 0x1Cu, by);
	write_f32(v + 0x20u, bz);
	write_f32(v + 0x24u, tx);
	write_f32(v + 0x28u, ty);
	write_f32(v + 0x2Cu, tz);
	write_s16(v + 0x38u, 0);
	write_s16(v + 0x3Au, 0);
	write_f32(v + 0x3Cu, 1.0f);
	write_f32(v + 0x40u, 0.0f);
}

static int32_t out_map_find_tag(
	struct forge_byte_buf const *out,
	uint32_t tag_data_offset,
	uint32_t group_tag,
	char const *tag_path)
{
	int32_t count, i;
	if (!out || !out->data || !tag_path || !tag_path[0])
		return -1;
	count = read_s32(out->data + tag_data_offset + 0x0Cu);
	for (i = 0; i < count; i++)
	{
		uint32_t io = tag_data_offset + 0x28u + (uint32_t)i * 32u;
		uint32_t g, naddr;
		if (io + 32u > out->size)
			break;
		g = read_u32(out->data + io + 0x00u);
		if (group_tag != 0 && g != group_tag)
			continue;
		naddr = read_u32(out->data + io + 0x10u);
		if (naddr >= CE_TAG_BASE_ADDR && tag_data_offset + (naddr - CE_TAG_BASE_ADDR) < out->size)
		{
			char const *nm = (char const *)(out->data + tag_data_offset + (naddr - CE_TAG_BASE_ADDR));
			if (_stricmp(nm, tag_path) == 0)
				return i;
		}
	}
	return -1;
}

static int32_t out_map_alloc_tag_instance(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	uint32_t scnr_off,
	uint32_t g0,
	uint32_t g1,
	uint32_t g2,
	char const *tag_path,
	uint32_t def_foff,
	uint32_t in_res_map,
	uint32_t *out_handle,
	uint32_t *out_name_addr)
{
	int32_t idx;
	uint32_t io, name_foff, handle, name_addr;
	size_t nlen;

	if (!out || !out->data || !tag_path)
		return -1;
	idx = read_s32(out->data + tag_data_offset + 0x0Cu);
	if (idx < 0 || 0x28u + (uint32_t)(idx + 1) * 32u > scnr_off)
		return -1;

	nlen = strlen(tag_path);
	name_foff = buf_alloc(out, (uint32_t)nlen + 1u, 1);
	if (!name_foff || !out->data)
		return -1;
	memcpy(out->data + name_foff, tag_path, nlen + 1u);

	io = tag_data_offset + 0x28u + (uint32_t)idx * 32u;
	handle = (((uint32_t)(0xE174 + idx) & 0xFFFFu) << 16) | ((uint32_t)idx & 0xFFFFu);
	name_addr = CE_TAG_BASE_ADDR + (name_foff - tag_data_offset);

	write_u32(out->data + io + 0x00u, g0);
	write_u32(out->data + io + 0x04u, g1);
	write_u32(out->data + io + 0x08u, g2);
	write_u32(out->data + io + 0x0Cu, handle);
	write_u32(out->data + io + 0x10u, name_addr);
	write_u32(out->data + io + 0x14u, def_foff ? (CE_TAG_BASE_ADDR + (def_foff - tag_data_offset)) : 0u);
	write_u32(out->data + io + 0x18u, in_res_map);
	write_u32(out->data + io + 0x1Cu, 0u);

	write_s32(out->data + tag_data_offset + 0x0Cu, idx + 1);
	if (out_handle) *out_handle = handle;
	if (out_name_addr) *out_name_addr = name_addr;
	return idx;
}

static int32_t out_map_ensure_fbtm_bitmap_tag(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	uint32_t scnr_off,
	char const *bitm_path)
{
	int32_t existing;
	char fbtm_disk[512];
	FILE *bf;
	uint32_t bmagic = 0, bver = 0, top_mip = 0;
	uint16_t w = 0, h = 0, fmt = 0, bflags = 0;
	uint32_t def_foff, bdata_foff, pix_foff;
	uint32_t handle = 0, name_addr = 0;
	int32_t new_idx;
	uint16_t final_flags;

	if (!bitm_path || !bitm_path[0])
		return -1;

	/* Prefer synthesized or native bitmap if already in out AND backed by local pixels or resource map */
	cb_build_fbtm_path(bitm_path, fbtm_disk, sizeof(fbtm_disk));
	bf = fopen(fbtm_disk, "rb");
	existing = out_map_find_tag(out, tag_data_offset, 'bitm', bitm_path);
	if (existing >= 0)
	{
		uint32_t io = tag_data_offset + 0x28u + (uint32_t)existing * 32u;
		/* If it was already synthesized as an internal tag (in_resource_map == 0) or no .fbtm exists, reuse it */
		if (read_u32(out->data + io + 0x18u) == 0u || !bf)
		{
			if (bf) fclose(bf);
			return existing;
		}
	}
	if (!bf)
		return existing;

	if (fread(&bmagic, 4, 1, bf) != 1 || bmagic != 0x4D544246u ||
		fread(&bver, 4, 1, bf) != 1 || bver != 1u ||
		fread(&w, 2, 1, bf) != 1 ||
		fread(&h, 2, 1, bf) != 1 ||
		fread(&fmt, 2, 1, bf) != 1 ||
		fread(&bflags, 2, 1, bf) != 1 ||
		fread(&top_mip, 4, 1, bf) != 1 ||
		w == 0 || h == 0 || top_mip == 0 || top_mip > 16u * 1024u * 1024u)
	{
		fclose(bf);
		return existing;
	}

	def_foff = buf_alloc(out, 0x6Cu, 4);
	bdata_foff = buf_alloc(out, 0x30u, 4);
	if (!def_foff || !bdata_foff || !out->data)
	{
		fclose(bf);
		return existing;
	}
	{
		uint8_t *tmp_pix = (uint8_t *)malloc(top_mip);
		if (!tmp_pix || fread(tmp_pix, 1, top_mip, bf) != top_mip)
		{
			free(tmp_pix);
			fclose(bf);
			return existing;
		}
		fclose(bf);
		cb_alloc_deferred_media(tmp_pix, top_mip, 32, bdata_foff + 0x18u);
		free(tmp_pix);
	}
	pix_foff = 0u;

	new_idx = out_map_alloc_tag_instance(
		out, tag_data_offset, scnr_off,
		'bitm', 0xFFFFFFFFu, 0xFFFFFFFFu,
		bitm_path, def_foff, 0u,
		&handle, &name_addr);
	if (new_idx < 0)
		return existing;

	/* Populate bitmap_group (0x6C bytes) */
	memset(out->data + def_foff, 0, 0x6Cu);
	write_s32(out->data + def_foff + 0x60u, 1);
	write_u32(out->data + def_foff + 0x64u, CE_TAG_BASE_ADDR + (bdata_foff - tag_data_offset));

	/* Populate bitmap_data (0x30 bytes) with internal .map pixel offset */
	final_flags = 0x0080u; /* _bitmap_cached_bit */
	if ((w & (w - 1)) == 0 && (h & (h - 1)) == 0)
		final_flags |= 0x0001u; /* _bitmap_has_power_of_two_dimensions_bit */
	if (fmt >= 14 && fmt <= 16)
		final_flags |= 0x0002u; /* _bitmap_compressed_bit */

	memset(out->data + bdata_foff, 0, 0x30u);
	write_u32(out->data + bdata_foff + 0x00u, 'bitm');
	write_u16(out->data + bdata_foff + 0x04u, w);
	write_u16(out->data + bdata_foff + 0x06u, h);
	write_u16(out->data + bdata_foff + 0x08u, 1);
	write_u16(out->data + bdata_foff + 0x0Au, 0);
	write_u16(out->data + bdata_foff + 0x0Cu, fmt);
	write_u16(out->data + bdata_foff + 0x0Eu, final_flags);
	write_s16(out->data + bdata_foff + 0x14u, 0); /* mipmap_count = 0 */
	write_u32(out->data + bdata_foff + 0x18u, pix_foff);
	write_u32(out->data + bdata_foff + 0x1Cu, top_mip);
	write_u32(out->data + bdata_foff + 0x20u, handle);
	write_s16(out->data + bdata_foff + 0x24u, 0);
	write_u32(out->data + bdata_foff + 0x28u, 0xFFFFFFFFu);

	return new_idx;
}

static int32_t out_map_ensure_part_shader_tag(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	uint32_t scnr_off,
	char const *shdr_path,
	char const *bitm_path)
{
	int32_t bitm_idx = -1;
	int32_t shdr_idx = -1;
	int32_t count, i;
	uint32_t soso_foff, shdr_handle = 0, shdr_name_addr = 0;
	char synth_shdr_name[256];

	if (bitm_path && bitm_path[0])
		bitm_idx = out_map_ensure_fbtm_bitmap_tag(out, tag_data_offset, scnr_off, bitm_path);

	/* If no custom .fbtm was needed and the shader already exists in base_map, use it directly */
	if (shdr_path && shdr_path[0])
	{
		static uint32_t const s_shdr_grps[] = { 'soso', 'senv', 'schi', 'scex', 'sgla', 'smet', 'spla', 'swat' };
		size_t k;
		for (k = 0; k < sizeof(s_shdr_grps) / sizeof(s_shdr_grps[0]); k++)
		{
			shdr_idx = out_map_find_tag(out, tag_data_offset, s_shdr_grps[k], shdr_path);
			if (shdr_idx >= 0)
			{
				if (bitm_idx < 0 || read_u32(out->data + tag_data_offset + 0x28u + (uint32_t)bitm_idx * 32u + 0x18u) != 0u)
					return shdr_idx;
				break;
			}
		}
	}

	/* If no custom .fbtm was needed, reuse an existing model shader ('soso') in base_map */
	count = read_s32(out->data + tag_data_offset + 0x0Cu);
	if (bitm_idx < 0)
	{
		static char const *const s_fallback_soso[] = {
			"vehicles\\warthog\\shaders\\warthog hull",
			"characters\\cyborg\\shaders\\armor",
			"vehicles\\ghost\\shaders\\ghost_hull",
			"weapons\\assault rifle\\fp\\shaders\\metal"
		};
		size_t k;
		for (k = 0; k < sizeof(s_fallback_soso) / sizeof(s_fallback_soso[0]); k++)
		{
			shdr_idx = out_map_find_tag(out, tag_data_offset, 'soso', s_fallback_soso[k]);
			if (shdr_idx >= 0)
				return shdr_idx;
		}
		for (i = 0; i < count; i++)
		{
			if (read_u32(out->data + tag_data_offset + 0x28u + (uint32_t)i * 32u) == 'soso')
				return i;
		}
		return -1;
	}

	snprintf(synth_shdr_name, sizeof(synth_shdr_name), "forge_baked\\shaders\\%s",
		(shdr_path && shdr_path[0]) ? shdr_path : (bitm_path ? bitm_path : "default"));
	shdr_idx = out_map_find_tag(out, tag_data_offset, 'soso', synth_shdr_name);
	if (shdr_idx >= 0)
		return shdr_idx;

	soso_foff = buf_alloc(out, 0x1B8u, 4);
	if (!soso_foff || !out->data)
		return -1;
	memset(out->data + soso_foff, 0, 0x1B8u);

	/* Copy baseline scalar properties from an existing 'soso' tag in base_map */
	for (i = 0; i < count; i++)
	{
		uint32_t io = tag_data_offset + 0x28u + (uint32_t)i * 32u;
		if (read_u32(out->data + io) == 'soso')
		{
			uint32_t daddr = read_u32(out->data + io + 0x14u);
			if (daddr >= CE_TAG_BASE_ADDR && tag_data_offset + (daddr - CE_TAG_BASE_ADDR) + 0x1B8u <= out->size)
			{
				memcpy(out->data + soso_foff, out->data + tag_data_offset + (daddr - CE_TAG_BASE_ADDR), 0x1B8u);
				break;
			}
		}
	}

	/* Clear all secondary tag_references in shader_model so only base_map (+0xA4) is active */
	{
		static uint32_t const s_ref_offs[] = { 0xA4u, 0xBCu, 0xDCu, 0x164u };
		size_t k;
		for (k = 0; k < sizeof(s_ref_offs) / sizeof(s_ref_offs[0]); k++)
		{
			uint32_t ro = soso_foff + s_ref_offs[k];
			write_u32(out->data + ro + 0x00u, 0xFFFFFFFFu);
			write_u32(out->data + ro + 0x04u, 0u);
			write_u32(out->data + ro + 0x08u, 0u);
			write_u32(out->data + ro + 0x0Cu, 0xFFFFFFFFu);
		}
	}

	/* Bind bitm_idx to shader_model.maps.base_map (+0xA4) */
	{
		uint32_t bio = tag_data_offset + 0x28u + (uint32_t)bitm_idx * 32u;
		uint32_t b_handle = read_u32(out->data + bio + 0x0Cu);
		uint32_t b_name_addr = read_u32(out->data + bio + 0x10u);
		write_u32(out->data + soso_foff + 0xA4u + 0x00u, 'bitm');
		write_u32(out->data + soso_foff + 0xA4u + 0x04u, b_name_addr);
		write_u32(out->data + soso_foff + 0xA4u + 0x08u, 0u);
		write_u32(out->data + soso_foff + 0xA4u + 0x0Cu, b_handle);
	}

	return out_map_alloc_tag_instance(
		out, tag_data_offset, scnr_off,
		'soso', 'shdr', 0xFFFFFFFFu,
		synth_shdr_name, soso_foff, 0u,
		&shdr_handle, &shdr_name_addr);
}

static void out_map_init_schema_null_refs(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	struct tag_schema_definition const *def,
	uint32_t elem_foff,
	int32_t elem_count)
{
	int32_t elem;
	struct tag_schema_field const *field;

	if (!out || !out->data || !def || !def->fields || elem_count <= 0)
		return;

	for (elem = 0; elem < elem_count; elem++)
	{
		uint32_t base_foff = elem_foff + (uint32_t)elem * (uint32_t)def->size;
		if (base_foff + (uint32_t)def->size > out->size)
			break;

		for (field = def->fields; field->type != _tag_schema_terminator; field++)
		{
			int16_t k;
			int16_t fcount = field->count > 0 ? field->count : 1;
			uint32_t fstride = field->size > 0 ? (uint32_t)field->size : 0;

			for (k = 0; k < fcount; k++)
			{
				uint32_t f_foff = base_foff + (uint32_t)field->offset + (uint32_t)k * fstride;

				if (field->type == _tag_schema_struct)
				{
					struct tag_schema_definition const *sub_def =
						(struct tag_schema_definition const *)field->definition;
					if (sub_def)
						out_map_init_schema_null_refs(out, tag_data_offset, sub_def, f_foff, 1);
				}
				else if (field->type == _tag_schema_reference)
				{
					if (f_foff + 16u <= out->size &&
						read_u32(out->data + f_foff + 0u) == 0u &&
						read_u32(out->data + f_foff + 12u) == 0u)
					{
						write_u32(out->data + f_foff + 0u, 0xFFFFFFFFu);
						write_u32(out->data + f_foff + 4u, 0u);
						write_u32(out->data + f_foff + 8u, 0u);
						write_u32(out->data + f_foff + 12u, 0xFFFFFFFFu);
					}
				}
				else if (field->type == _tag_schema_tag_index)
				{
					if (f_foff + 4u <= out->size && read_u32(out->data + f_foff) == 0u)
						write_u32(out->data + f_foff, 0xFFFFFFFFu);
				}
				else if (field->type == _tag_schema_block)
				{
					struct tag_schema_definition const *child_def =
						(struct tag_schema_definition const *)field->definition;
					int32_t cnt = read_s32(out->data + f_foff + 0u);
					uint32_t addr = read_u32(out->data + f_foff + 4u);
					if (child_def && cnt > 0 && addr >= CE_TAG_BASE_ADDR)
					{
						uint32_t child_foff = tag_data_offset + (addr - CE_TAG_BASE_ADDR);
						if ((uint64_t)child_foff + (uint64_t)(uint32_t)cnt * (uint32_t)child_def->size <= out->size)
							out_map_init_schema_null_refs(out, tag_data_offset, child_def, child_foff, cnt);
					}
				}
			}
		}
	}
}

static int32_t out_map_bake_collision_from_fcol(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	uint32_t scnr_off,
	uint32_t group_tag,
	char const *tag_path,
	uint8_t *out_offsets_24,
	int32_t *out_mnode_count,
	uint32_t *out_mnodes_foff)
{
	static uint32_t const s_bsp_strides[8] = { 12u, 16u, 8u, 8u, 20u, 12u, 24u, 16u };
	char fcol_disk[512];
	FILE *cf;
	char magic[4];
	uint32_t ver = 0;
	uint8_t pf_box[24];
	int32_t mnode_count = 0, cnode_count = 0, n;
	uint32_t mnodes_foff, cnodes_foff, coll_foff, mat_foff, reg_foff;
	char coll_name[256];

	if (out_mnode_count) *out_mnode_count = 0;
	if (out_mnodes_foff) *out_mnodes_foff = 0;
	cb_build_fcol_path(group_tag, tag_path, fcol_disk, sizeof(fcol_disk));
	cf = fopen(fcol_disk, "rb");
	if (!cf)
		return -1;

	if (fread(magic, 1, 4, cf) != 4 || memcmp(magic, "FCOL", 4) != 0 ||
		fread(&ver, 4, 1, cf) != 1 || ver != 1 ||
		fread(out_offsets_24, 1, 24, cf) != 24 ||
		fread(pf_box, 1, 24, cf) != 24 ||
		fread(&mnode_count, 4, 1, cf) != 1 ||
		mnode_count <= 0 || mnode_count > 64)
	{
		fclose(cf);
		return -1;
	}

	mnodes_foff = buf_alloc(out, (uint32_t)mnode_count * 156u, 4);
	if (!mnodes_foff || !out->data ||
		fread(out->data + mnodes_foff, 156, (size_t)mnode_count, cf) != (size_t)mnode_count ||
		fread(&cnode_count, 4, 1, cf) != 1 ||
		cnode_count <= 0 || cnode_count > 64)
	{
		fclose(cf);
		return -1;
	}

	/* Ensure every model_node has a valid unit quaternion and non-zero runtime_default_inverse_matrix */
	for (n = 0; n < mnode_count; n++)
	{
		uint32_t mn_foff = mnodes_foff + (uint32_t)n * 156u;
		float inv_scale = read_f32(out->data + mn_foff + 0x68u);
		float qi = read_f32(out->data + mn_foff + 0x34u);
		float qj = read_f32(out->data + mn_foff + 0x38u);
		float qk = read_f32(out->data + mn_foff + 0x3Cu);
		float qw = read_f32(out->data + mn_foff + 0x40u);
		if (qi * qi + qj * qj + qk * qk + qw * qw < 0.01f || (qk == -1.0f && qw == 0.0f))
		{
			write_f32(out->data + mn_foff + 0x34u, 0.0f);
			write_f32(out->data + mn_foff + 0x38u, 0.0f);
			write_f32(out->data + mn_foff + 0x3Cu, 0.0f);
			write_f32(out->data + mn_foff + 0x40u, 1.0f);
		}
		if (!(inv_scale > 0.01f && inv_scale < 100.0f))
		{
			memset(out->data + mn_foff + 0x68u, 0, 52u);
			write_f32(out->data + mn_foff + 0x68u, 1.0f);
			write_f32(out->data + mn_foff + 0x6Cu, 1.0f);
			write_f32(out->data + mn_foff + 0x7Cu, 1.0f);
			write_f32(out->data + mn_foff + 0x8Cu, 1.0f);
		}
	}
	{
		int pf_i;
		for (pf_i = 0; pf_i < 6; pf_i++)
		{
			float pfv = read_f32(pf_box + (uint32_t)pf_i * 4u);
			if (!(pfv >= -500.0f && pfv <= 500.0f))
				write_f32(pf_box + (uint32_t)pf_i * 4u, (pf_i & 1) ? 2.5f : -2.5f);
		}
	}

	cnodes_foff = buf_alloc(out, (uint32_t)cnode_count * 64u, 4);
	if (!cnodes_foff || !out->data ||
		fread(out->data + cnodes_foff, 64, (size_t)cnode_count, cf) != (size_t)cnode_count)
	{
		fclose(cf);
		return -1;
	}

	for (n = 0; n < cnode_count; n++)
	{
		uint32_t cn_foff = cnodes_foff + (uint32_t)n * 64u;
		int32_t has_bsp = 0;
		int16_t reg_idx = read_s16(out->data + cn_foff + 0x20u);
		if (reg_idx != -1 && (reg_idx < 0 || reg_idx >= 8))
			write_s16(out->data + cn_foff + 0x20u, 0);
		write_s16(out->data + cn_foff + 0x32u, -1);

		if (fread(&has_bsp, 4, 1, cf) != 1)
		{
			write_s32(out->data + cn_foff + 0x34u, 0);
			write_u32(out->data + cn_foff + 0x38u, 0u);
			write_u32(out->data + cn_foff + 0x3Cu, 0u);
			break;
		}
		if (has_bsp)
		{
			int32_t counts[8] = { 0 };
			uint32_t bsp_foff = buf_alloc(out, 96u, 4);
			int b, b_ok = 1;
			if (!bsp_foff || !out->data || fread(counts, 4, 8, cf) != 8)
			{
				write_s32(out->data + cn_foff + 0x34u, 0);
				write_u32(out->data + cn_foff + 0x38u, 0u);
				write_u32(out->data + cn_foff + 0x3Cu, 0u);
				break;
			}
			memset(out->data + bsp_foff, 0, 96u);
			for (b = 0; b < 8; b++)
			{
				uint32_t blk_foff = 0;
				if (counts[b] < 0 || counts[b] > 65536) { b_ok = 0; break; }
				if (counts[b] > 0)
				{
					blk_foff = buf_alloc(out, (uint32_t)counts[b] * s_bsp_strides[b], 4);
					if (!blk_foff || !out->data ||
						fread(out->data + blk_foff, s_bsp_strides[b], (size_t)counts[b], cf) != (size_t)counts[b])
					{
						b_ok = 0;
						break;
					}
					if (b == 5)
					{
						int32_t s;
						for (s = 0; s < counts[b]; s++)
						{
							uint32_t sf = blk_foff + (uint32_t)s * 12u;
							out->data[sf + 0x09u] = 0xFFu;
							write_s16(out->data + sf + 0x0Au, 0);
						}
					}
					else if (b == 7)
					{
						int32_t v_idx, c_idx;
						for (v_idx = 0; v_idx < counts[b]; v_idx++)
						{
							uint32_t cv = blk_foff + (uint32_t)v_idx * 16u;
							for (c_idx = 0; c_idx < 3; c_idx++)
							{
								float val = read_f32(out->data + cv + (uint32_t)c_idx * 4u);
								if (!(val >= -500.0f && val <= 500.0f))
								{
									uint32_t raw_bits = read_u32(out->data + cv + (uint32_t)c_idx * 4u);
									uint32_t fixed_bits = raw_bits - 0x0BDEA5CEu;
									float fixed_val = 0.0f;
									if (fixed_bits >= 0x40440000u && fixed_bits <= 0x41B40000u)
										memcpy(&fixed_val, &fixed_bits, 4);
									write_f32(out->data + cv + (uint32_t)c_idx * 4u, fixed_val);
								}
							}
						}
					}
				}
				write_s32(out->data + bsp_foff + (uint32_t)b * 12u + 0u, counts[b]);
				write_u32(out->data + bsp_foff + (uint32_t)b * 12u + 4u,
					blk_foff ? (CE_TAG_BASE_ADDR + (blk_foff - tag_data_offset)) : 0u);
				write_u32(out->data + bsp_foff + (uint32_t)b * 12u + 8u, 0u);
			}
			if (b_ok)
			{
				write_s32(out->data + cn_foff + 0x34u, 1);
				write_u32(out->data + cn_foff + 0x38u, CE_TAG_BASE_ADDR + (bsp_foff - tag_data_offset));
				write_u32(out->data + cn_foff + 0x3Cu, 0u);
			}
			else
			{
				write_s32(out->data + cn_foff + 0x34u, 0);
				write_u32(out->data + cn_foff + 0x38u, 0u);
				write_u32(out->data + cn_foff + 0x3Cu, 0u);
			}
		}
		else
		{
			write_s32(out->data + cn_foff + 0x34u, 0);
			write_u32(out->data + cn_foff + 0x38u, 0u);
			write_u32(out->data + cn_foff + 0x3Cu, 0u);
		}
	}
	fclose(cf);

	coll_foff = buf_alloc(out, 0x298u, 4);
	mat_foff = buf_alloc(out, 0x48u, 4);
	reg_foff = buf_alloc(out, 8u * 0x68u, 4);
	if (!coll_foff || !mat_foff || !reg_foff || !out->data)
		return -1;

	memset(out->data + coll_foff, 0, 0x298u);
	memset(out->data + mat_foff, 0, 0x48u);
	memset(out->data + reg_foff, 0, 8u * 0x68u);

	snprintf((char *)(out->data + mat_foff), 32, "default");
	write_f32(out->data + mat_foff + 0x28u, 1.0f);
	write_f32(out->data + mat_foff + 0x3Cu, 1.0f);
	{
		int r_idx;
		for (r_idx = 0; r_idx < 8; r_idx++)
		{
			snprintf((char *)(out->data + reg_foff + (uint32_t)r_idx * 0x54u), 32, "default");
		}
	}
	write_s32(out->data + coll_foff + 0x234u, 1);
	write_u32(out->data + coll_foff + 0x238u, CE_TAG_BASE_ADDR + (mat_foff - tag_data_offset));
	write_s32(out->data + coll_foff + 0x240u, 8);
	write_u32(out->data + coll_foff + 0x244u, CE_TAG_BASE_ADDR + (reg_foff - tag_data_offset));
	memcpy(out->data + coll_foff + 0x268u, pf_box, 24);
	write_s32(out->data + coll_foff + 0x28Cu, cnode_count);
	write_u32(out->data + coll_foff + 0x290u, CE_TAG_BASE_ADDR + (cnodes_foff - tag_data_offset));
	{
		struct tag_schema_group const *coll_schema = forge_schema_group_get('coll');
		if (coll_schema && coll_schema->definition)
			out_map_init_schema_null_refs(out, tag_data_offset, coll_schema->definition, coll_foff, 1);
	}

	if (out_mnode_count) *out_mnode_count = mnode_count;
	if (out_mnodes_foff) *out_mnodes_foff = mnodes_foff;

	snprintf(coll_name, sizeof(coll_name), "%s_baked_coll", tag_path);
	return out_map_alloc_tag_instance(
		out, tag_data_offset, scnr_off,
		'coll', 0xFFFFFFFFu, 0xFFFFFFFFu,
		coll_name, coll_foff, 0u,
		NULL, NULL);
}

static void out_map_clone_schema_blocks(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	struct tag_schema_definition const *def,
	uint32_t elem_foff,
	int32_t elem_count)
{
	int32_t elem;
	struct tag_schema_field const *field;

	if (!out || !out->data || !def || !def->fields || elem_count <= 0)
		return;

	for (elem = 0; elem < elem_count; elem++)
	{
		uint32_t base_foff = elem_foff + (uint32_t)elem * (uint32_t)def->size;
		if (base_foff + (uint32_t)def->size > out->size)
			break;

		for (field = def->fields; field->type != _tag_schema_terminator; field++)
		{
			int16_t k;
			int16_t fcount = field->count > 0 ? field->count : 1;
			uint32_t fstride = field->size > 0 ? (uint32_t)field->size : 0;

			for (k = 0; k < fcount; k++)
			{
				uint32_t f_foff = base_foff + (uint32_t)field->offset + (uint32_t)k * fstride;

				if (field->type == _tag_schema_struct)
				{
					struct tag_schema_definition const *sub_def =
						(struct tag_schema_definition const *)field->definition;
					if (sub_def)
						out_map_clone_schema_blocks(out, tag_data_offset, sub_def, f_foff, 1);
				}
				else if (field->type == _tag_schema_block)
				{
					struct tag_schema_definition const *child_def =
						(struct tag_schema_definition const *)field->definition;
					int32_t cnt = read_s32(out->data + f_foff + 0u);
					uint32_t addr = read_u32(out->data + f_foff + 4u);
					write_u32(out->data + f_foff + 8u, 0u);

					if (cnt <= 0 || !child_def || child_def->size <= 0 || addr < CE_TAG_BASE_ADDR)
					{
						write_s32(out->data + f_foff + 0u, 0);
						write_u32(out->data + f_foff + 4u, 0u);
					}
					else
					{
						uint32_t child_src_foff = tag_data_offset + (addr - CE_TAG_BASE_ADDR);
						uint64_t total_bytes;
						if (field->maximum > 0 && !(field->flags & (1 << _tag_schema_tool_maximum_bit)) &&
							cnt > field->maximum)
						{
							cnt = field->maximum;
						}
						total_bytes = (uint64_t)(uint32_t)cnt * (uint64_t)(uint32_t)child_def->size;
						if ((uint64_t)child_src_foff + total_bytes > out->size)
						{
							write_s32(out->data + f_foff + 0u, 0);
							write_u32(out->data + f_foff + 4u, 0u);
						}
						else
						{
							uint32_t child_dst_foff = buf_alloc(out, (uint32_t)total_bytes, 4);
							if (child_dst_foff && out->data)
							{
								memcpy(out->data + child_dst_foff, out->data + child_src_foff, (size_t)total_bytes);
								write_s32(out->data + f_foff + 0u, cnt);
								write_u32(out->data + f_foff + 4u, CE_TAG_BASE_ADDR + (child_dst_foff - tag_data_offset));
								out_map_clone_schema_blocks(out, tag_data_offset, child_def, child_dst_foff, cnt);
							}
							else
							{
								write_s32(out->data + f_foff + 0u, 0);
								write_u32(out->data + f_foff + 4u, 0u);
							}
						}
					}
				}
				else if (field->type == _tag_schema_data)
				{
					int32_t sz = read_s32(out->data + f_foff + 0u);
					uint32_t addr = read_u32(out->data + f_foff + 12u);
					write_u32(out->data + f_foff + 16u, 0u);

					if (sz <= 0 || addr < CE_TAG_BASE_ADDR)
					{
						memset(out->data + f_foff, 0, 20u);
					}
					else
					{
						uint32_t td_src_foff = tag_data_offset + (addr - CE_TAG_BASE_ADDR);
						if ((uint64_t)td_src_foff + (uint32_t)sz > out->size)
						{
							memset(out->data + f_foff, 0, 20u);
						}
						else
						{
							uint32_t td_dst_foff = buf_alloc(out, (uint32_t)sz, 4);
							if (td_dst_foff && out->data)
							{
								memcpy(out->data + td_dst_foff, out->data + td_src_foff, (size_t)sz);
								write_s32(out->data + f_foff + 0u, sz);
								write_u32(out->data + f_foff + 12u, CE_TAG_BASE_ADDR + (td_dst_foff - tag_data_offset));
							}
							else
							{
								memset(out->data + f_foff, 0, 20u);
							}
						}
					}
				}
			}
		}
	}
}

static int cb_ocetags_find_key_range(char const *buf, size_t start, size_t end, char const *key, size_t *out_val_pos)
{
	char pat[128];
	size_t pat_len, p;
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	pat_len = strlen(pat);
	if (end <= start || end - start < pat_len)
		return 0;
	for (p = start; p + pat_len <= end; p++)
	{
		if (memcmp(buf + p, pat, pat_len) == 0)
		{
			size_t q = p + pat_len;
			while (q < end && (buf[q] == ' ' || buf[q] == '\t' || buf[q] == '\r' || buf[q] == '\n'))
				q++;
			if (q < end && buf[q] == ':')
			{
				q++;
				while (q < end && (buf[q] == ' ' || buf[q] == '\t' || buf[q] == '\r' || buf[q] == '\n'))
					q++;
				if (out_val_pos)
					*out_val_pos = q;
				return 1;
			}
		}
	}
	return 0;
}

static int cb_ocetags_skip_value(char const *buf, size_t pos, size_t end, size_t *out_next)
{
	char open_c, close_c;
	int depth = 0, in_str = 0;
	size_t p = pos;
	while (p < end && (buf[p] == ' ' || buf[p] == '\t' || buf[p] == '\r' || buf[p] == '\n'))
		p++;
	if (p >= end)
		return 0;
	if (buf[p] == '"')
	{
		p++;
		while (p < end)
		{
			if (buf[p] == '\\' && p + 1 < end) p += 2;
			else if (buf[p] == '"') { p++; break; }
			else p++;
		}
		if (out_next) *out_next = p;
		return 1;
	}
	if (buf[p] == '{' || buf[p] == '[')
	{
		open_c = buf[p];
		close_c = (open_c == '{') ? '}' : ']';
		while (p < end)
		{
			char c = buf[p];
			if (in_str)
			{
				if (c == '\\' && p + 1 < end) { p += 2; continue; }
				if (c == '"') in_str = 0;
			}
			else
			{
				if (c == '"') in_str = 1;
				else if (c == open_c) depth++;
				else if (c == close_c)
				{
					depth--;
					if (depth == 0) { p++; break; }
				}
			}
			p++;
		}
		if (out_next) *out_next = p;
		return (depth == 0);
	}
	while (p < end && buf[p] != ',' && buf[p] != '}' && buf[p] != ']' &&
		   buf[p] != ' ' && buf[p] != '\t' && buf[p] != '\r' && buf[p] != '\n')
		p++;
	if (out_next) *out_next = p;
	return 1;
}

static int cb_ocetags_get_str(char const *buf, size_t start, size_t end, char const *key, char *out_str, size_t out_sz)
{
	size_t vpos = 0, o = 0;
	if (!out_str || out_sz == 0)
		return 0;
	out_str[0] = 0;
	if (!cb_ocetags_find_key_range(buf, start, end, key, &vpos))
		return 0;
	if (vpos >= end || buf[vpos] != '"')
		return 0;
	vpos++;
	while (vpos < end && buf[vpos] != '"' && o + 1 < out_sz)
	{
		if (buf[vpos] == '\\' && vpos + 1 < end)
		{
			vpos++;
			out_str[o++] = buf[vpos++];
		}
		else
		{
			out_str[o++] = buf[vpos++];
		}
	}
	out_str[o] = 0;
	return 1;
}

static int cb_ocetags_get_u32(char const *buf, size_t start, size_t end, char const *key, uint32_t *out_val)
{
	size_t vpos = 0;
	if (!cb_ocetags_find_key_range(buf, start, end, key, &vpos) || vpos >= end)
		return 0;
	if ((buf[vpos] < '0' || buf[vpos] > '9') && buf[vpos] != '-')
		return 0;
	if (out_val)
		*out_val = (uint32_t)strtoul(buf + vpos, NULL, 10);
	return 1;
}

static uint8_t *cb_ocetags_decode_base64_key(char const *buf, size_t start, size_t end, char const *key, size_t *out_len)
{
	size_t vpos = 0, b64_start, b64_len, cap, o = 0, p;
	uint8_t *out;
	uint32_t val = 0;
	int valb = -8;

	if (out_len) *out_len = 0;
	if (!cb_ocetags_find_key_range(buf, start, end, key, &vpos) || vpos >= end || buf[vpos] != '"')
		return NULL;
	vpos++;
	b64_start = vpos;
	while (vpos < end && buf[vpos] != '"')
	{
		if (buf[vpos] == '\\' && vpos + 1 < end) vpos += 2;
		else vpos++;
	}
	b64_len = vpos - b64_start;
	cap = (b64_len * 3u) / 4u + 16u;
	out = (uint8_t *)malloc(cap);
	if (!out)
		return NULL;
	for (p = b64_start; p < vpos; p++)
	{
		unsigned char c = (unsigned char)buf[p];
		int d = -1;
		if (c >= 'A' && c <= 'Z') d = c - 'A';
		else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
		else if (c >= '0' && c <= '9') d = c - '0' + 52;
		else if (c == '+') d = 62;
		else if (c == '/') d = 63;
		else if (c == '=') break;
		else continue;
		val = (val << 6) | (uint32_t)d;
		valb += 6;
		if (valb >= 0)
		{
			if (o < cap)
				out[o++] = (uint8_t)((val >> valb) & 0xFFu);
			valb -= 8;
		}
	}
	if (out_len) *out_len = o;
	return out;
}

static uint32_t cb_ocetags_group_str_to_fourcc(char const *grp)
{
	static struct { char const *name; uint32_t fourcc; } const s_map[] = {
		{ "actor", 'actr' },
		{ "actor_variant", 'actv' },
		{ "antenna", 'ant!' },
		{ "model_animations", 'antr' },
		{ "biped", 'bipd' },
		{ "bitmap", 'bitm' },
		{ "continuous_damage_effect", 'cdmg' },
		{ "model_collision_geometry", 'coll' },
		{ "color_table", 'colo' },
		{ "contrail", 'cont' },
		{ "device_control", 'ctrl' },
		{ "decal", 'deca' },
		{ "ui_widget_definition", 'DeLa' },
		{ "input_device_defaults", 'devc' },
		{ "device", 'devi' },
		{ "detail_object_collection", 'dobc' },
		{ "effect", 'effe' },
		{ "equipment", 'eqip' },
		{ "flag", 'flag' },
		{ "fog", 'fog ' },
		{ "font", 'font' },
		{ "material_effects", 'foot' },
		{ "garbage", 'garb' },
		{ "glow", 'glw!' },
		{ "grenade_hud_interface", 'grhi' },
		{ "hud_message_text", 'hmt ' },
		{ "hud_number", 'hud#' },
		{ "hud_globals", 'hudg' },
		{ "item", 'item' },
		{ "item_collection", 'itmc' },
		{ "damage_effect", 'jpt!' },
		{ "lens_flare", 'lens' },
		{ "lightning", 'elec' },
		{ "device_light_fixture", 'lifi' },
		{ "light", 'ligh' },
		{ "sound_looping", 'lsnd' },
		{ "device_machine", 'mach' },
		{ "globals", 'matg' },
		{ "meter", 'metr' },
		{ "light_volume", 'mgs2' },
		{ "gbxmodel", 'mod2' },
		{ "model", 'mode' },
		{ "multiplayer_scenario_description", 'mply' },
		{ "preferences_network_game", 'ngpr' },
		{ "object", 'obje' },
		{ "particle", 'part' },
		{ "particle_system", 'pctl' },
		{ "physics", 'phys' },
		{ "placeholder", 'plac' },
		{ "point_physics", 'pphy' },
		{ "projectile", 'proj' },
		{ "weather_particle_system", 'rain' },
		{ "scenario_structure_bsp", 'sbsp' },
		{ "scenery", 'scen' },
		{ "shader_transparent_chicago_extended", 'scex' },
		{ "shader_transparent_chicago", 'schi' },
		{ "scenario", 'scnr' },
		{ "shader_environment", 'senv' },
		{ "shader_transparent_glass", 'sgla' },
		{ "shader", 'shdr' },
		{ "sky", 'sky ' },
		{ "shader_transparent_meter", 'smet' },
		{ "sound", 'snd!' },
		{ "sound_environment", 'snde' },
		{ "shader_model", 'soso' },
		{ "shader_transparent_generic", 'sotr' },
		{ "ui_widget_collection", 'Soul' },
		{ "shader_transparent_plasma", 'spla' },
		{ "sound_scenery", 'ssce' },
		{ "string_list", 'str#' },
		{ "shader_transparent_water", 'swat' },
		{ "tag_collection", 'tagc' },
		{ "camera_track", 'trak' },
		{ "dialogue", 'udlg' },
		{ "unit_hud_interface", 'unhi' },
		{ "unit", 'unit' },
		{ "unicode_string_list", 'ustr' },
		{ "virtual_keyboard", 'vcky' },
		{ "vehicle", 'vehi' },
		{ "weapon", 'weap' },
		{ "wind", 'wind' },
		{ "weapon_hud_interface", 'wphi' }
	};
	size_t i;

	if (!grp || !grp[0])
		return 0;
	for (i = 0; i < sizeof(s_map) / sizeof(s_map[0]); i++)
	{
		if (_stricmp(grp, s_map[i].name) == 0)
			return s_map[i].fourcc;
	}
	if (strlen(grp) == 4)
	{
		return ((uint32_t)(uint8_t)grp[0] << 24) |
		       ((uint32_t)(uint8_t)grp[1] << 16) |
		       ((uint32_t)(uint8_t)grp[2] << 8) |
		       (uint32_t)(uint8_t)grp[3];
	}
	return 0;
}

static int cb_load_matching_ocetags_json(
	uint32_t req_group,
	char const *req_path,
	char **out_json,
	size_t *out_json_len)
{
#ifdef _WIN32
	static char const *const s_dirs[] = {
		"OpenCE Forge/custom_tags",
		"custom_tags",
		"OpenCE Forge/ocetags",
		"OpenCE Forge/cached_tags",
		"OpenCE Forge",
		"ocetags",
		"custom_maps",
		"maps",
		"mods",
		"sample tags",
		"../sample tags"
	};
	int d;

	if (out_json) *out_json = NULL;
	if (out_json_len) *out_json_len = 0;
	if (!req_path || !req_path[0])
		return 0;

	for (d = 0; d < (int)(sizeof(s_dirs) / sizeof(s_dirs[0])); d++)
	{
		char pattern[512];
		struct _finddata_t fd;
		intptr_t h;
		snprintf(pattern, sizeof(pattern), "%s/*.ocetags", s_dirs[d]);
		h = _findfirst(pattern, &fd);
		if (h != -1)
		{
			do
			{
				if (!(fd.attrib & _A_SUBDIR))
				{
					char full_path[512];
					FILE *fp;
					snprintf(full_path, sizeof(full_path), "%s/%s", s_dirs[d], fd.name);
					fp = fopen(full_path, "rb");
					if (fp)
					{
						long fsz;
						fseek(fp, 0, SEEK_END);
						fsz = ftell(fp);
						fseek(fp, 0, SEEK_SET);
						if (fsz > 32 && fsz <= 128 * 1024 * 1024)
						{
							char peek[8193];
							size_t pn = fread(peek, 1, 8192, fp);
							peek[pn] = 0;
							if (strstr(peek, "\"formatVersion\"") != NULL)
							{
								size_t t_pos = 0;
								if (cb_ocetags_find_key_range(peek, 0, pn, "tags", &t_pos))
								{
									size_t p = t_pos + 1;
									while (p < pn && peek[p] != '{') p++;
									if (p < pn)
									{
										char gstr[64], pstr[256];
										gstr[0] = 0;
										pstr[0] = 0;
										cb_ocetags_get_str(peek, p, pn, "group", gstr, sizeof(gstr));
										cb_ocetags_get_str(peek, p, pn, "path", pstr, sizeof(pstr));
										if (cb_ocetags_group_str_to_fourcc(gstr) == req_group &&
											_stricmp(pstr, req_path) == 0)
										{
											char *jbuf = (char *)malloc((size_t)fsz + 1u);
											fseek(fp, 0, SEEK_SET);
											if (jbuf && fread(jbuf, 1, (size_t)fsz, fp) == (size_t)fsz)
											{
												jbuf[fsz] = 0;
												fclose(fp);
												_findclose(h);
												if (out_json) *out_json = jbuf;
												if (out_json_len) *out_json_len = (size_t)fsz;
												return 1;
											}
											free(jbuf);
										}
									}
								}
							}
						}
						fclose(fp);
					}
				}
			} while (_findnext(h, &fd) == 0);
			_findclose(h);
		}
	}
#else
	(void)req_group;
	(void)req_path;
	if (out_json) *out_json = NULL;
	if (out_json_len) *out_json_len = 0;
#endif
	return 0;
}

struct cb_ocetags_bake_tag
{
	uint32_t fourcc;
	uint32_t orig_index;
	uint16_t orig_low;
	char path[256];
	size_t obj_start;
	size_t obj_end;
	uint32_t def_foff;
	size_t raw_len;
	int32_t baked_tag_idx;
	uint32_t baked_handle;
	uint32_t baked_name_addr;
	int is_reused_existing;
};

static void cb_ocetags_rebase_and_link_schema(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	struct tag_schema_definition const *def,
	uint32_t elem_foff,
	int32_t elem_count,
	uint32_t tag_def_foff,
	size_t raw_len,
	uint32_t root_size,
	struct cb_ocetags_bake_tag const *pkg_tags,
	int pkg_tag_count,
	int depth)
{
	int32_t e;
	struct tag_schema_field const *field;
	uint32_t elem_size;

	if (!out || !out->data || !def || !def->fields || elem_count <= 0 || def->size <= 0 || depth > 10)
		return;

	elem_size = (uint32_t)def->size;
	for (e = 0; e < elem_count; e++)
	{
		uint32_t base_foff = elem_foff + (uint32_t)e * elem_size;
		if (base_foff + elem_size > out->size)
			break;

		for (field = def->fields; field->type != _tag_schema_terminator; field++)
		{
			int16_t k;
			int16_t fcount = (field->count > 0) ? field->count : 1;
			uint32_t fstride = (field->size > 0) ? (uint32_t)field->size : 0;

			for (k = 0; k < fcount; k++)
			{
				uint32_t f_foff = base_foff + (uint32_t)field->offset + (uint32_t)k * fstride;

				if (field->type == _tag_schema_struct)
				{
					struct tag_schema_definition const *sub_def =
						(struct tag_schema_definition const *)field->definition;
					if (sub_def && (uint32_t)field->offset + (uint32_t)k * fstride + (uint32_t)sub_def->size <= elem_size)
					{
						cb_ocetags_rebase_and_link_schema(
							out, tag_data_offset, sub_def, f_foff, 1,
							tag_def_foff, raw_len, root_size, pkg_tags, pkg_tag_count, depth + 1);
					}
				}
				else if (field->type == _tag_schema_block)
				{
					struct tag_schema_definition const *child_def =
						(struct tag_schema_definition const *)field->definition;
					if (child_def && child_def->size > 0 && f_foff + 12u <= out->size)
					{
						int32_t cnt = read_s32(out->data + f_foff + 0u);
						uint32_t rel = read_u32(out->data + f_foff + 4u);
						uint32_t child_sz = (uint32_t)child_def->size;
						if (cnt > 0 && cnt <= 65536 &&
							rel >= root_size &&
							(uint64_t)rel + (uint64_t)cnt * child_sz <= (uint64_t)raw_len)
						{
							uint32_t child_foff = tag_def_foff + rel;
							write_s32(out->data + f_foff + 0u, cnt);
							write_u32(out->data + f_foff + 4u, CE_TAG_BASE_ADDR + (child_foff - tag_data_offset));
							write_u32(out->data + f_foff + 8u, 0u);
							cb_ocetags_rebase_and_link_schema(
								out, tag_data_offset, child_def, child_foff, cnt,
								tag_def_foff, raw_len, root_size, pkg_tags, pkg_tag_count, depth + 1);
						}
						else
						{
							memset(out->data + f_foff, 0, 12u);
						}
					}
				}
				else if (field->type == _tag_schema_data)
				{
					if (f_foff + 20u <= out->size)
					{
						int32_t sz = read_s32(out->data + f_foff + 0u);
						uint32_t rel = read_u32(out->data + f_foff + 12u);
						if (sz > 0 && sz <= 16 * 1024 * 1024 &&
							rel >= root_size &&
							(uint64_t)rel + (uint64_t)sz <= (uint64_t)raw_len)
						{
							uint32_t td_foff = tag_def_foff + rel;
							write_s32(out->data + f_foff + 0u, sz);
							write_u32(out->data + f_foff + 4u, 0u);
							write_u32(out->data + f_foff + 8u, 0u);
							write_u32(out->data + f_foff + 12u, CE_TAG_BASE_ADDR + (td_foff - tag_data_offset));
							write_u32(out->data + f_foff + 16u, 0u);
						}
						else
						{
							memset(out->data + f_foff, 0, 20u);
						}
					}
				}
				else if (field->type == _tag_schema_reference)
				{
					if (f_foff + 16u <= out->size)
					{
						int32_t ref_idx = read_s32(out->data + f_foff + 12u);
						uint32_t resolved_handle = 0xFFFFFFFFu;
						uint32_t resolved_name_addr = 0u;
						uint32_t resolved_grp = read_u32(out->data + f_foff + 0u);
						if (ref_idx != -1 && ref_idx != 0)
						{
							uint16_t orig_low = (uint16_t)((uint32_t)ref_idx & 0xFFFFu);
							int m;
							for (m = 0; m < pkg_tag_count; m++)
							{
								if ((uint32_t)ref_idx == pkg_tags[m].orig_index || orig_low == pkg_tags[m].orig_low)
								{
									if (pkg_tags[m].baked_tag_idx >= 0)
									{
										resolved_handle = pkg_tags[m].baked_handle;
										resolved_name_addr = pkg_tags[m].baked_name_addr;
										resolved_grp = pkg_tags[m].fourcc;
									}
									break;
								}
							}
						}
						write_u32(out->data + f_foff + 0u, (resolved_handle != 0xFFFFFFFFu) ? resolved_grp : 0xFFFFFFFFu);
						write_u32(out->data + f_foff + 4u, resolved_name_addr);
						write_s32(out->data + f_foff + 8u, 0);
						write_u32(out->data + f_foff + 12u, resolved_handle);
					}
				}
				else if (field->type == _tag_schema_tag_index)
				{
					if (f_foff + 4u <= out->size)
					{
						int32_t ref_idx = read_s32(out->data + f_foff);
						uint32_t resolved_handle = 0xFFFFFFFFu;
						if (ref_idx != -1 && ref_idx != 0)
						{
							uint16_t orig_low = (uint16_t)((uint32_t)ref_idx & 0xFFFFu);
							int m;
							for (m = 0; m < pkg_tag_count; m++)
							{
								if ((uint32_t)ref_idx == pkg_tags[m].orig_index || orig_low == pkg_tags[m].orig_low)
								{
									if (pkg_tags[m].baked_tag_idx >= 0)
										resolved_handle = pkg_tags[m].baked_handle;
									break;
								}
							}
						}
						write_u32(out->data + f_foff, resolved_handle);
					}
				}
			}
		}
	}
}

static int32_t out_map_bake_ocetags_package(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	uint32_t scnr_off,
	uint32_t orig_vtx_size,
	uint32_t orig_idx_size,
	struct forge_pending_bake_obj const *pobj)
{
	char const *json = pobj->ocetags_json;
	size_t json_len = pobj->ocetags_json_len;
	size_t tags_pos = 0, tags_end = 0;
	struct cb_ocetags_bake_tag *pkg_tags = NULL;
	int pkg_tag_count = 0;
	uint8_t **raw_assets = NULL;
	size_t *raw_asset_lens = NULL;
	int raw_asset_count = 0;
	int i;
	int32_t root_baked_idx = -1;

	if (!json || json_len < 32)
		return -1;
	if (!cb_ocetags_find_key_range(json, 0, json_len, "tags", &tags_pos) ||
		!cb_ocetags_skip_value(json, tags_pos, json_len, &tags_end))
		return -1;

	pkg_tags = (struct cb_ocetags_bake_tag *)calloc(512, sizeof(*pkg_tags));
	if (!pkg_tags)
		return -1;

	/* 1. Parse tags[] */
	{
		size_t p = tags_pos + 1;
		while (p < tags_end && pkg_tag_count < 512)
		{
			size_t obj_start, obj_end;
			while (p < tags_end && json[p] != '{' && json[p] != ']') p++;
			if (p >= tags_end || json[p] != '{') break;
			obj_start = p;
			if (!cb_ocetags_skip_value(json, obj_start, tags_end, &obj_end)) break;
			{
				struct cb_ocetags_bake_tag *pt = &pkg_tags[pkg_tag_count++];
				char gstr[64];
				gstr[0] = 0;
				cb_ocetags_get_str(json, obj_start, obj_end, "group", gstr, sizeof(gstr));
				cb_ocetags_get_str(json, obj_start, obj_end, "path", pt->path, sizeof(pt->path));
				cb_ocetags_get_u32(json, obj_start, obj_end, "originalIndex", &pt->orig_index);
				pt->orig_low = (uint16_t)(pt->orig_index & 0xFFFFu);
				pt->fourcc = cb_ocetags_group_str_to_fourcc(gstr);
				pt->obj_start = obj_start;
				pt->obj_end = obj_end;
				pt->baked_tag_idx = -1;
				pt->baked_handle = 0xFFFFFFFFu;
			}
			p = obj_end;
		}
	}

	if (pkg_tag_count <= 0)
	{
		free(pkg_tags);
		return -1;
	}

	/* 2. Decode embeddedRawDataAssets[] */
	{
		size_t assets_pos = 0, assets_end = 0;
		if (cb_ocetags_find_key_range(json, 0, json_len, "embeddedRawDataAssets", &assets_pos) &&
			cb_ocetags_skip_value(json, assets_pos, json_len, &assets_end))
		{
			size_t p = assets_pos + 1;
			raw_assets = (uint8_t **)calloc(256, sizeof(uint8_t *));
			raw_asset_lens = (size_t *)calloc(256, sizeof(size_t));
			while (raw_assets && raw_asset_lens && p < assets_end && raw_asset_count < 256)
			{
				size_t a_start, a_end;
				while (p < assets_end && json[p] != '{' && json[p] != ']') p++;
				if (p >= assets_end || json[p] != '{') break;
				a_start = p;
				if (!cb_ocetags_skip_value(json, a_start, assets_end, &a_end)) break;
				raw_assets[raw_asset_count] = cb_ocetags_decode_base64_key(
					json, a_start, a_end, "data", &raw_asset_lens[raw_asset_count]);
				raw_asset_count++;
				p = a_end;
			}
		}
	}

	/* 3. Allocate each tag in out */
	for (i = 0; i < pkg_tag_count; i++)
	{
		struct cb_ocetags_bake_tag *pt = &pkg_tags[i];
		struct tag_schema_group const *schema_grp = forge_schema_group_get(pt->fourcc);
		uint32_t p0 = schema_grp ? (uint32_t)schema_grp->parent_group_tags[0] : 0xFFFFFFFFu;
		uint32_t p1 = schema_grp ? (uint32_t)schema_grp->parent_group_tags[1] : 0xFFFFFFFFu;
		uint8_t *decoded = NULL;
		size_t decoded_len = 0;

		if (pt->fourcc == 'scex')
		{
			p0 = 'shdr';
			p1 = 0xFFFFFFFFu;
		}

		/* Reuse existing tag in out if non-root and already baked/native (for 'snd!' or already-baked shared dependencies) */
		if (i > 0)
		{
			int32_t ex = out_map_find_tag(out, tag_data_offset, pt->fourcc, pt->path);
			if (ex >= 0 && (pt->fourcc == 'snd!' || read_u32(out->data + tag_data_offset + 0x28u + (uint32_t)ex * 32u + 0x18u) == 0u))
			{
				uint32_t io = tag_data_offset + 0x28u + (uint32_t)ex * 32u;
				pt->baked_tag_idx = ex;
				pt->baked_handle = read_u32(out->data + io + 0x0Cu);
				pt->baked_name_addr = read_u32(out->data + io + 0x10u);
				pt->is_reused_existing = 1;
				continue;
			}
		}

		decoded = cb_ocetags_decode_base64_key(json, pt->obj_start, pt->obj_end, "data", &decoded_len);
		if (!decoded || decoded_len == 0)
		{
			free(decoded);
			continue;
		}

		pt->def_foff = buf_alloc(out, (uint32_t)decoded_len, 4);
		if (!pt->def_foff || !out->data)
		{
			free(decoded);
			continue;
		}
		memcpy(out->data + pt->def_foff, decoded, decoded_len);
		pt->raw_len = decoded_len;
		free(decoded);

		if (pt->fourcc == 'snd!' && decoded_len >= 0xA4u)
		{
			memset(out->data + pt->def_foff + 0x98u, 0, 12u);
		}

		pt->baked_tag_idx = out_map_alloc_tag_instance(
			out, tag_data_offset, scnr_off,
			pt->fourcc, p0, p1,
			pt->path, pt->def_foff, 0u,
			&pt->baked_handle, &pt->baked_name_addr);
	}

	/* 4. Rebase internal tag_blocks/tag_data and link tag_references */
	for (i = 0; i < pkg_tag_count; i++)
	{
		struct cb_ocetags_bake_tag *pt = &pkg_tags[i];
		struct tag_schema_group const *schema_grp;
		uint32_t root_sz;

		if (!pt->def_foff || pt->is_reused_existing || pt->baked_tag_idx < 0)
			continue;
		schema_grp = forge_schema_group_get(pt->fourcc);
		if (!schema_grp || !schema_grp->definition || schema_grp->definition->size <= 0)
			continue;
		root_sz = (uint32_t)schema_grp->definition->size;
		if (pt->raw_len < root_sz)
			continue;

		cb_ocetags_rebase_and_link_schema(
			out, tag_data_offset, schema_grp->definition,
			pt->def_foff, 1, pt->def_foff, pt->raw_len, root_sz,
			pkg_tags, pkg_tag_count, 0);
	}

	/* 5. Embed raw bitmap pixel data from top-level and per-tag embeddedRawDataPatches[] */
	{
		size_t patches_pos = 0, patches_end = 0;
		if (raw_asset_count > 0 &&
			cb_ocetags_find_key_range(json, 0, json_len, "embeddedRawDataPatches", &patches_pos) &&
			cb_ocetags_skip_value(json, patches_pos, json_len, &patches_end))
		{
			size_t p = patches_pos + 1;
			while (p < patches_end)
			{
				size_t p_start, p_end;
				uint32_t t_idx = 0xFFFFFFFFu, a_idx = 0xFFFFFFFFu, off_field = 0, sz_field = 0;
				while (p < patches_end && json[p] != '{' && json[p] != ']') p++;
				if (p >= patches_end || json[p] != '{') break;
				p_start = p;
				if (!cb_ocetags_skip_value(json, p_start, patches_end, &p_end)) break;
				if (cb_ocetags_get_u32(json, p_start, p_end, "tagIndex", &t_idx) &&
					cb_ocetags_get_u32(json, p_start, p_end, "assetIndex", &a_idx) &&
					cb_ocetags_get_u32(json, p_start, p_end, "offsetFieldOffset", &off_field) &&
					t_idx < (uint32_t)pkg_tag_count &&
					a_idx < (uint32_t)raw_asset_count &&
					raw_assets[a_idx] && raw_asset_lens[a_idx] > 0)
				{
					struct cb_ocetags_bake_tag *pt = &pkg_tags[t_idx];
					cb_ocetags_get_u32(json, p_start, p_end, "sizeFieldOffset", &sz_field);
					if (pt->def_foff && !pt->is_reused_existing && pt->fourcc == 'bitm' &&
						off_field >= 24u && off_field + 24u <= pt->raw_len)
					{
						uint32_t asset_len = (uint32_t)raw_asset_lens[a_idx];
						uint32_t bdata_foff = pt->def_foff + (off_field - 24u);
						uint16_t bflags = read_u16(out->data + bdata_foff + 0x0Eu);
						bflags &= (uint16_t)~0x0100u; /* clear _bitmap_in_resource_map_bit */
						bflags |= 0x0080u;            /* set _bitmap_cached_bit */
						write_u16(out->data + bdata_foff + 0x0Eu, bflags);
						write_u32(out->data + bdata_foff + 0x18u, 0u);
						write_u32(out->data + bdata_foff + 0x1Cu, asset_len);
						write_u32(out->data + bdata_foff + 0x20u, pt->baked_handle);
						write_u32(out->data + bdata_foff + 0x28u, 0xFFFFFFFFu);
						cb_alloc_deferred_media(raw_assets[a_idx], asset_len, 32, bdata_foff + 0x18u);
					}
				}
				p = p_end;
			}
		}

		/* Also support per-tag embeddedRawDataAssets[] + embeddedRawDataPatches[] inside tags[i] */
		if (cb_ocetags_find_key_range(json, 0, json_len, "tags", &tags_pos) &&
			cb_ocetags_skip_value(json, tags_pos, json_len, &tags_end))
		{
			size_t p = tags_pos + 1;
			i = 0;
			while (p < tags_end && i < pkg_tag_count)
			{
				size_t obj_start, obj_end;
				struct cb_ocetags_bake_tag *pt = &pkg_tags[i];
				size_t ta_pos = 0, ta_end = 0, tp_pos = 0, tp_end = 0;
				while (p < tags_end && json[p] != '{' && json[p] != ']') p++;
				if (p >= tags_end || json[p] != '{') break;
				obj_start = p;
				if (!cb_ocetags_skip_value(json, obj_start, tags_end, &obj_end)) break;

				if (pt->def_foff && !pt->is_reused_existing && pt->fourcc == 'bitm' &&
					cb_ocetags_find_key_range(json, obj_start, obj_end, "embeddedRawDataAssets", &ta_pos) &&
					cb_ocetags_skip_value(json, ta_pos, obj_end, &ta_end) &&
					cb_ocetags_find_key_range(json, obj_start, obj_end, "embeddedRawDataPatches", &tp_pos) &&
					cb_ocetags_skip_value(json, tp_pos, obj_end, &tp_end))
				{
					uint8_t *t_assets[32];
					size_t t_lens[32];
					int t_acount = 0, k;
					size_t ap = ta_pos + 1;
					memset(t_assets, 0, sizeof(t_assets));
					memset(t_lens, 0, sizeof(t_lens));
					while (ap < ta_end && t_acount < 32)
					{
						size_t a_start, a_end;
						while (ap < ta_end && json[ap] != '{' && json[ap] != ']') ap++;
						if (ap >= ta_end || json[ap] != '{') break;
						a_start = ap;
						if (!cb_ocetags_skip_value(json, a_start, ta_end, &a_end)) break;
						t_assets[t_acount] = cb_ocetags_decode_base64_key(json, a_start, a_end, "data", &t_lens[t_acount]);
						if (!t_assets[t_acount])
							t_assets[t_acount] = cb_ocetags_decode_base64_key(json, a_start, a_end, "rawData", &t_lens[t_acount]);
						t_acount++;
						ap = a_end;
					}
					ap = tp_pos + 1;
					while (ap < tp_end)
					{
						size_t p_start, p_end;
						uint32_t a_idx = 0xFFFFFFFFu, off_field = 0;
						while (ap < tp_end && json[ap] != '{' && json[ap] != ']') ap++;
						if (ap >= tp_end || json[ap] != '{') break;
						p_start = ap;
						if (!cb_ocetags_skip_value(json, p_start, tp_end, &p_end)) break;
						if (cb_ocetags_get_u32(json, p_start, p_end, "assetIndex", &a_idx) &&
							(cb_ocetags_get_u32(json, p_start, p_end, "patchOffset", &off_field) ||
							 cb_ocetags_get_u32(json, p_start, p_end, "offsetFieldOffset", &off_field)) &&
							a_idx < (uint32_t)t_acount && t_assets[a_idx] && t_lens[a_idx] > 0 &&
							off_field >= 24u && off_field + 24u <= pt->raw_len)
						{
							uint32_t asset_len = (uint32_t)t_lens[a_idx];
							uint32_t bdata_foff = pt->def_foff + (off_field - 24u);
							uint16_t bflags = read_u16(out->data + bdata_foff + 0x0Eu);
							bflags &= (uint16_t)~0x0100u;
							bflags |= 0x0080u;
							write_u16(out->data + bdata_foff + 0x0Eu, bflags);
							write_u32(out->data + bdata_foff + 0x18u, 0u);
							write_u32(out->data + bdata_foff + 0x1Cu, asset_len);
							write_u32(out->data + bdata_foff + 0x20u, pt->baked_handle);
							write_u32(out->data + bdata_foff + 0x28u, 0xFFFFFFFFu);
							cb_alloc_deferred_media(t_assets[a_idx], asset_len, 32, bdata_foff + 0x18u);
						}
						ap = p_end;
					}
					for (k = 0; k < t_acount; k++)
						free(t_assets[k]);
				}
				i++;
				p = obj_end;
			}
		}
	}

	/* 6. Patch gbxmodel ('mod2') part vertex/strip offsets and sanitize coll/antr/vehi */
	for (i = 0; i < pkg_tag_count; i++)
	{
		struct cb_ocetags_bake_tag *pt = &pkg_tags[i];
		if (!pt->def_foff || pt->is_reused_existing || pt->baked_tag_idx < 0)
			continue;

		if (pt->fourcc == 'mod2' && pt->raw_len >= 0xE8u)
		{
			int32_t geom_cnt = read_s32(out->data + pt->def_foff + 0xD0u);
			uint32_t geom_addr = read_u32(out->data + pt->def_foff + 0xD4u);
			int32_t shdr_cnt = read_s32(out->data + pt->def_foff + 0xDCu);
			int32_t node_cnt = read_s32(out->data + pt->def_foff + 0xB8u);
			int g_idx;

			if (!(read_f32(out->data + pt->def_foff + 0x30u) > 0.0001f))
				write_f32(out->data + pt->def_foff + 0x30u, 1.0f);
			if (!(read_f32(out->data + pt->def_foff + 0x34u) > 0.0001f))
				write_f32(out->data + pt->def_foff + 0x34u, 1.0f);

			if (geom_cnt > 0 && geom_addr >= CE_TAG_BASE_ADDR)
			{
				uint32_t geom_base_foff = tag_data_offset + (geom_addr - CE_TAG_BASE_ADDR);
				for (g_idx = 0; g_idx < geom_cnt; g_idx++)
				{
					uint32_t g_foff = geom_base_foff + (uint32_t)g_idx * 48u;
					int32_t part_cnt = read_s32(out->data + g_foff + 36u);
					uint32_t part_addr = read_u32(out->data + g_foff + 40u);
					int p_idx;

					if (part_cnt <= 0 || part_addr < CE_TAG_BASE_ADDR)
						continue;
					for (p_idx = 0; p_idx < part_cnt; p_idx++)
					{
						uint32_t p_foff = tag_data_offset + (part_addr - CE_TAG_BASE_ADDR) + (uint32_t)p_idx * 132u;
						int gp;
						int16_t s_idx = read_s16(out->data + p_foff + 0x04u);
						int16_t c_pri = read_s16(out->data + p_foff + 0x08u);
						int16_t c_sec = read_s16(out->data + p_foff + 0x0Au);
						if (s_idx < 0 || s_idx >= shdr_cnt)
							write_s16(out->data + p_foff + 0x04u, (shdr_cnt > 0) ? 0 : -1);
						out->data[p_foff + 0x06u] = 0xFFu; /* previous_part_index = -1 */
						out->data[p_foff + 0x07u] = 0xFFu; /* next_part_index = -1 */
						if (c_pri < 0 || c_pri >= node_cnt)
							write_s16(out->data + p_foff + 0x08u, 0);
						if (c_sec < 0 || c_sec >= node_cnt)
							write_s16(out->data + p_foff + 0x0Au, 0);
						memset(out->data + p_foff + 0x20u, 0, 36u); /* zero uncompressed_vertices, compressed_vertices, triangles tag_blocks */
						write_u32(out->data + p_foff + 0x00u, read_u32(out->data + p_foff + 0x00u) & ~0x02u); /* clear _model_geometry_part_local_nodes_bit */
						out->data[p_foff + 0x6Bu] = 0u; /* local_node_count = 0 (resolved to model nodes) */
						for (gp = 0; gp < pobj->ocetags_gpart_count; gp++)
						{
							struct forge_pending_ocetags_gpart const *ogp = &pobj->ocetags_gparts[gp];
							if (ogp->tag_idx == (uint32_t)i &&
								ogp->geom_idx == (uint32_t)g_idx &&
								ogp->part_idx == (uint32_t)p_idx)
							{
								int32_t tri_cnt = (ogp->idx_count >= 2u) ? (int32_t)(ogp->idx_count - 2u) : 1;
								write_s16(out->data + p_foff + 0x44u, 1); /* TRIANGLE_BUFFER_TYPE_PRECOMPILED_STRIP */
								write_s32(out->data + p_foff + 0x48u, tri_cnt);
								write_u32(out->data + p_foff + 0x4Cu, orig_idx_size + ogp->extra_idx_rel_off);
								write_u32(out->data + p_foff + 0x50u, orig_idx_size + ogp->extra_idx_rel_off);
								write_s16(out->data + p_foff + 0x54u, 4);
								write_s32(out->data + p_foff + 0x58u, (int32_t)ogp->vtx_count);
								write_u32(out->data + p_foff + 0x60u, orig_vtx_size + ogp->extra_vtx_rel_off);
								write_u32(out->data + p_foff + 0x64u, orig_vtx_size + ogp->extra_vtx_rel_off);
								break;
							}
						}
					}
				}
			}
			write_u32(out->data + pt->def_foff + 0x00u, read_u32(out->data + pt->def_foff + 0x00u) & ~0x02u); /* clear _gbxmodel_parts_have_local_nodes_bit */
		}
		else if (pt->fourcc == 'coll' && pt->raw_len >= 0x298u)
		{
			int pf_i, n;
			int32_t reg_cnt = read_s32(out->data + pt->def_foff + 0x240u);
			int32_t cn_cnt = read_s32(out->data + pt->def_foff + 0x28Cu);
			uint32_t cn_addr = read_u32(out->data + pt->def_foff + 0x290u);
			for (pf_i = 0; pf_i < 6; pf_i++)
			{
				float pfv = read_f32(out->data + pt->def_foff + 0x268u + (uint32_t)pf_i * 4u);
				if (!(pfv >= -500.0f && pfv <= 500.0f))
					write_f32(out->data + pt->def_foff + 0x268u + (uint32_t)pf_i * 4u, (pf_i & 1) ? 2.5f : -2.5f);
			}
			if (cn_cnt > 0 && cn_addr >= CE_TAG_BASE_ADDR)
			{
				uint32_t cn_base = tag_data_offset + (cn_addr - CE_TAG_BASE_ADDR);
				for (n = 0; n < cn_cnt; n++)
				{
					uint32_t cn_foff = cn_base + (uint32_t)n * 64u;
					int16_t r_idx = read_s16(out->data + cn_foff + 0x20u);
					int32_t bsp_cnt = read_s32(out->data + cn_foff + 0x34u);
					uint32_t bsp_addr = read_u32(out->data + cn_foff + 0x38u);
					int b;
					if (r_idx != -1 && (r_idx < 0 || r_idx >= reg_cnt))
						write_s16(out->data + cn_foff + 0x20u, (reg_cnt > 0) ? 0 : -1);
					if (bsp_cnt > 0 && bsp_addr >= CE_TAG_BASE_ADDR)
					{
						uint32_t bsp_base = tag_data_offset + (bsp_addr - CE_TAG_BASE_ADDR);
						for (b = 0; b < bsp_cnt; b++)
						{
							uint32_t bsp_foff = bsp_base + (uint32_t)b * 96u;
							int32_t v_cnt = read_s32(out->data + bsp_foff + 7u * 12u + 0u);
							uint32_t v_addr = read_u32(out->data + bsp_foff + 7u * 12u + 4u);
							if (v_cnt > 0 && v_addr >= CE_TAG_BASE_ADDR)
							{
								uint32_t v_base = tag_data_offset + (v_addr - CE_TAG_BASE_ADDR);
								int v_i, c_i;
								for (v_i = 0; v_i < v_cnt; v_i++)
								{
									uint32_t cv = v_base + (uint32_t)v_i * 16u;
									for (c_i = 0; c_i < 3; c_i++)
									{
										float val = read_f32(out->data + cv + (uint32_t)c_i * 4u);
										if (!(val >= -500.0f && val <= 500.0f))
										{
											uint32_t raw_bits = read_u32(out->data + cv + (uint32_t)c_i * 4u);
											uint32_t fixed_bits = raw_bits - 0x0BDEA5CEu;
											float fixed_val = 0.0f;
											if (fixed_bits >= 0x40440000u && fixed_bits <= 0x41B40000u)
												memcpy(&fixed_val, &fixed_bits, 4);
											write_f32(out->data + cv + (uint32_t)c_i * 4u, fixed_val);
										}
									}
								}
							}
						}
					}
				}
			}
		}
		else if (pt->fourcc == 'vehi' && pt->raw_len >= 0x3F0u)
		{
			int32_t seat_cnt = read_s32(out->data + pt->def_foff + 0x2DCu);
			uint32_t seat_addr = read_u32(out->data + pt->def_foff + 0x2E0u);
			write_u32(out->data + pt->def_foff + 0x17Cu,
				read_u32(out->data + pt->def_foff + 0x17Cu) | (1u << 29) | (1u << 4));
			if (seat_cnt > 0 && seat_addr >= CE_TAG_BASE_ADDR)
			{
				uint32_t s0_foff = tag_data_offset + (seat_addr - CE_TAG_BASE_ADDR);
				if (s0_foff + 284u <= out->size)
				{
					uint32_t sflags = read_u32(out->data + s0_foff + 0x00u);
					char const *slabel = (char const *)(out->data + s0_foff + 0x04u);
					sflags &= ~0x01u; /* clear _unit_seat_invisible_bit */
					sflags |= (1u << 2) | (1u << 4) | (1u << 5); /* driver | third_person_camera | third_person_on_enter */
					write_u32(out->data + s0_foff + 0x00u, sflags);
					if (_stricmp(slabel, "W-driver") != 0 &&
						_stricmp(slabel, "G-driver") != 0 &&
						_stricmp(slabel, "B-driver") != 0 &&
						_stricmp(slabel, "S-driver") != 0 &&
						_stricmp(slabel, "R-driver") != 0 &&
						_stricmp(slabel, "scorpion-driver") != 0 &&
						_stricmp(slabel, "CT-driver") != 0)
					{
						memset(out->data + s0_foff + 0x04u, 0, 32u);
						memcpy(out->data + s0_foff + 0x04u, "W-driver", 8u);
					}
				}
			}
		}
	}

	root_baked_idx = pkg_tags[0].baked_tag_idx;

	if (raw_assets)
	{
		for (i = 0; i < raw_asset_count; i++)
			free(raw_assets[i]);
		free(raw_assets);
	}
	free(raw_asset_lens);
	free(pkg_tags);
	return root_baked_idx;
}

/* =========================================================================
 * Native Xbox (.map v5) Tag Closure Baking into Custom Edition (.map v609)
 * ========================================================================= */

struct cb_xbox_bake_tag
{
	int32_t src_tag_idx;
	uint32_t fourcc;
	char name[256];
	uint32_t src_base_addr;
	uint32_t def_foff;
	int32_t baked_tag_idx;
	uint32_t baked_handle;
	uint32_t baked_name_addr;
	int is_reused_existing;
};

static int cb_find_xbox_donor_map_for_tag(
	uint32_t group_tag,
	char const *tag_path,
	char *out_map_path,
	size_t out_map_path_sz)
{
	static char const *const map_files[] = {
		"a10.map", "a30.map", "a50.map", "b30.map", "b40.map",
		"c10.map", "c20.map", "c40.map", "d20.map", "d40.map",
		"beavercreek.map", "boardingaction.map", "bloodgulch.map",
		"carousel.map", "chillout.map", "damnation.map", "hangemhigh.map",
		"longest.map", "prisoner.map", "putput.map", "ratrace.map",
		"sidewinder.map", "wizard.map"
	};
	static char const *const map_dirs[] = {
		"OpenCE Forge/cached_tags/xbox_cache",
		"maps",
		"assets/maps",
		"../assets/maps",
		"../FORGE CODE/GAME MAPS/XBOX DEFAULT MAPS",
		"FORGE CODE/GAME MAPS/XBOX DEFAULT MAPS"
	};
	size_t m, d;

	if (!tag_path || !tag_path[0] || !out_map_path || out_map_path_sz == 0)
		return 0;
	out_map_path[0] = '\0';

	for (m = 0; m < sizeof(map_files) / sizeof(map_files[0]); m++)
	{
		for (d = 0; d < sizeof(map_dirs) / sizeof(map_dirs[0]); d++)
		{
			char cand[512];
			struct forge_source_map xmap;

			snprintf(cand, sizeof(cand), "%s/%s", map_dirs[d], map_files[m]);
			if (!forge_source_map_open(cand, &xmap))
				continue;
			if (xmap.version == CACHE_VERSION_XBOX &&
				src_map_find_tag_or_variant(&xmap, group_tag, tag_path) >= 0)
			{
				snprintf(out_map_path, out_map_path_sz, "%s", cand);
				forge_source_map_close(&xmap);
				return 1;
			}
			forge_source_map_close(&xmap);
		}
	}
	return 0;
}

static real_vector3d cb_uncompress_int32_to_real_vector3d(uint32_t compressed)
{
	real_vector3d v;
	float value = (float)(int32_t)(compressed << 21);

	v.i = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	compressed >>= 11;
	value = (float)(int32_t)(compressed << 21);
	v.j = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	compressed >>= 11;
	value = (float)(int32_t)(compressed << 22);
	v.k = (value * (1.0f / 2097152.0f) + 1.0f) * (1.0f / 1023.0f);
	return v;
}

static void cb_uncompress_xbox_model_vertex(uint8_t const *src32, uint8_t *dst68, int32_t node_limit)
{
	real_vector3d v;
	int16_t u_s16, v_s16, w_s16;
	int16_t n0, n1;
	float w0, w1;

	memset(dst68, 0, 68u);
	memcpy(dst68 + 0x00u, src32 + 0x00u, 12u); /* position (3 floats) */

	v = cb_uncompress_int32_to_real_vector3d(read_u32(src32 + 0x0Cu));
	write_f32(dst68 + 0x0Cu, PIN(v.i, -1.0f, 1.0f));
	write_f32(dst68 + 0x10u, PIN(v.j, -1.0f, 1.0f));
	write_f32(dst68 + 0x14u, PIN(v.k, -1.0f, 1.0f));

	v = cb_uncompress_int32_to_real_vector3d(read_u32(src32 + 0x10u));
	write_f32(dst68 + 0x18u, PIN(v.i, -1.0f, 1.0f));
	write_f32(dst68 + 0x1Cu, PIN(v.j, -1.0f, 1.0f));
	write_f32(dst68 + 0x20u, PIN(v.k, -1.0f, 1.0f));

	v = cb_uncompress_int32_to_real_vector3d(read_u32(src32 + 0x14u));
	write_f32(dst68 + 0x24u, PIN(v.i, -1.0f, 1.0f));
	write_f32(dst68 + 0x28u, PIN(v.j, -1.0f, 1.0f));
	write_f32(dst68 + 0x2Cu, PIN(v.k, -1.0f, 1.0f));

	u_s16 = read_s16(src32 + 0x18u);
	v_s16 = read_s16(src32 + 0x1Au);
	write_f32(dst68 + 0x30u, ((float)u_s16 * 2.0f + 1.0f) / 65535.0f);
	write_f32(dst68 + 0x34u, ((float)v_s16 * 2.0f + 1.0f) / 65535.0f);

	if (node_limit <= 0) node_limit = 1;
	n0 = (int16_t)(src32[0x1Cu] / 3u);
	n1 = (int16_t)(src32[0x1Du] / 3u);
	if (n0 < 0 || n0 >= node_limit) n0 = 0;
	if (n1 < 0 || n1 >= node_limit) n1 = 0;
	write_s16(dst68 + 0x38u, n0);
	write_s16(dst68 + 0x3Au, n1);

	w_s16 = read_s16(src32 + 0x1Eu);
	w0 = ((float)w_s16 * 2.0f + 1.0f) / 65535.0f;
	w0 = PIN(w0, 0.0f, 1.0f);
	w1 = 1.0f - w0;
	write_f32(dst68 + 0x3Cu, w0);
	write_f32(dst68 + 0x40u, w1);
}

static void cb_prescan_xbox_models(
	struct forge_source_map const *xmap,
	int32_t root_tag_idx,
	struct forge_pending_bake_obj *pobj,
	struct forge_byte_buf *extra_verts,
	struct forge_byte_buf *extra_strips)
{
	uint8_t visited[8192];
	int32_t queue[2048];
	int q_head = 0, q_tail = 0;

	if (!xmap || root_tag_idx < 0 || root_tag_idx >= xmap->tag_count || root_tag_idx >= 8192)
		return;
	memset(visited, 0, sizeof(visited));
	visited[root_tag_idx] = 1;
	queue[q_tail++] = root_tag_idx;

	while (q_head < q_tail)
	{
		int32_t tidx = queue[q_head++];
		uint8_t const *inst = src_tag_instance(xmap, tidx);
		uint32_t fourcc, daddr;
		struct tag_schema_group const *sgrp;

		if (!inst) continue;
		fourcc = read_u32(inst + 0x00u);
		daddr = read_u32(inst + 0x14u);
		if (daddr < xmap->tag_base_addr || daddr >= xmap->tag_base_addr + xmap->tag_data_size)
			continue;

		if (fourcc == 'mode' || fourcc == 'mod2')
		{
			uint32_t m_foff = xmap->tag_data_offset + (daddr - xmap->tag_base_addr);
			if (m_foff + 0xE8u <= xmap->file_size)
			{
				int32_t node_cnt = read_s32(xmap->data + m_foff + 0xB8u);
				int32_t geom_cnt = read_s32(xmap->data + m_foff + 0xD0u);
				uint32_t geom_addr = read_u32(xmap->data + m_foff + 0xD4u);
				int g_idx;

				if (node_cnt <= 0) node_cnt = 1;
				if (geom_cnt > 0 && geom_cnt <= 64 && geom_addr >= xmap->tag_base_addr &&
					(geom_addr - xmap->tag_base_addr) + (uint32_t)geom_cnt * 48u <= xmap->tag_data_size)
				{
					uint32_t g_base = xmap->tag_data_offset + (geom_addr - xmap->tag_base_addr);
					for (g_idx = 0; g_idx < geom_cnt; g_idx++)
					{
						uint32_t g_foff = g_base + (uint32_t)g_idx * 48u;
						int32_t part_cnt = read_s32(xmap->data + g_foff + 36u);
						uint32_t part_addr = read_u32(xmap->data + g_foff + 40u);
						int p_idx;
						if (part_cnt <= 0 || part_cnt > 64 || part_addr < xmap->tag_base_addr ||
							(part_addr - xmap->tag_base_addr) + (uint32_t)part_cnt * 104u > xmap->tag_data_size)
							continue;
						for (p_idx = 0; p_idx < part_cnt && pobj->ocetags_gpart_count < 256; p_idx++)
						{
							uint32_t p_foff = xmap->tag_data_offset + (part_addr - xmap->tag_base_addr) + (uint32_t)p_idx * 104u;
							int16_t tri_type = read_s16(xmap->data + p_foff + 68u);
							int32_t tri_cnt = read_s32(xmap->data + p_foff + 72u);
							uint32_t tb_hw_addr = read_u32(xmap->data + p_foff + 80u);
							int16_t vtx_type = read_s16(xmap->data + p_foff + 84u);
							int32_t vtx_cnt = read_s32(xmap->data + p_foff + 88u);
							uint32_t vb_hw_addr = read_u32(xmap->data + p_foff + 100u);
							uint32_t vtx_stride = (vtx_type == 5) ? 32u : 68u;

							if (tri_type == 1 && tri_cnt > 0 && vtx_cnt >= 3 &&
								tb_hw_addr >= xmap->tag_base_addr && vb_hw_addr >= xmap->tag_base_addr &&
								(tb_hw_addr - xmap->tag_base_addr) + 12u <= xmap->tag_data_size &&
								(vb_hw_addr - xmap->tag_base_addr) + 12u <= xmap->tag_data_size)
							{
								uint32_t tb_hdr_foff = xmap->tag_data_offset + (tb_hw_addr - xmap->tag_base_addr);
								uint32_t vb_hdr_foff = xmap->tag_data_offset + (vb_hw_addr - xmap->tag_base_addr);
								uint32_t ib_data_addr = read_u32(xmap->data + tb_hdr_foff + 4u);
								uint32_t vb_data_addr = read_u32(xmap->data + vb_hdr_foff + 4u);
								if (ib_data_addr >= xmap->tag_base_addr && vb_data_addr >= xmap->tag_base_addr)
								{
									uint32_t i_file_off = xmap->tag_data_offset + (ib_data_addr - xmap->tag_base_addr);
									uint32_t v_file_off = xmap->tag_data_offset + (vb_data_addr - xmap->tag_base_addr);
									uint32_t ic = (uint32_t)tri_cnt + 2u;

									if ((uint64_t)v_file_off + (uint64_t)vtx_cnt * vtx_stride <= xmap->file_size &&
										(uint64_t)i_file_off + (uint64_t)ic * 2u <= xmap->file_size)
									{
										uint32_t v_rel = buf_alloc(extra_verts, (uint32_t)vtx_cnt * 68u, 4);
										uint32_t i_rel = buf_alloc(extra_strips, (ic + 2u) * 2u, 4);
										if (extra_verts->data && extra_strips->data)
										{
											int32_t vi;
											uint32_t k;
											struct forge_pending_ocetags_gpart *ogp = &pobj->ocetags_gparts[pobj->ocetags_gpart_count++];
											for (vi = 0; vi < vtx_cnt; vi++)
											{
												uint8_t const *vsrc = xmap->data + v_file_off + (uint32_t)vi * vtx_stride;
												uint8_t *vdst = extra_verts->data + v_rel + (uint32_t)vi * 68u;
												if (vtx_stride == 32u)
													cb_uncompress_xbox_model_vertex(vsrc, vdst, node_cnt);
												else
												{
													int16_t n0, n1;
													memcpy(vdst, vsrc, 68u);
													n0 = read_s16(vdst + 0x38u);
													n1 = read_s16(vdst + 0x3Au);
													if (n0 < 0 || n0 >= node_cnt) write_s16(vdst + 0x38u, 0);
													if (n1 < 0 || n1 >= node_cnt) write_s16(vdst + 0x3Au, 0);
												}
											}
											memcpy(extra_strips->data + i_rel, xmap->data + i_file_off, ic * 2u);
											while (ic > 3u && read_u16(extra_strips->data + i_rel + (ic - 1u) * 2u) >= (uint32_t)vtx_cnt)
												ic--;
											for (k = 0; k < ic; k++)
											{
												uint16_t idx_val = read_u16(extra_strips->data + i_rel + k * 2u);
												if (idx_val >= (uint32_t)vtx_cnt)
												{
													uint16_t safe_val = (k > 0) ? read_u16(extra_strips->data + i_rel + (k - 1u) * 2u) : 0u;
													if (safe_val >= (uint32_t)vtx_cnt) safe_val = 0u;
													write_u16(extra_strips->data + i_rel + k * 2u, safe_val);
												}
											}
											write_u16(extra_strips->data + i_rel + ic * 2u, 0xFFFFu);
											write_u16(extra_strips->data + i_rel + (ic + 1u) * 2u, 0xFFFFu);
											ogp->tag_idx = (uint32_t)tidx;
											ogp->geom_idx = (uint32_t)g_idx;
											ogp->part_idx = (uint32_t)p_idx;
											ogp->vtx_count = (uint32_t)vtx_cnt;
											ogp->idx_count = ic;
											ogp->extra_vtx_rel_off = v_rel;
											ogp->extra_idx_rel_off = i_rel;
										}
									}
								}
							}
						}
					}
				}
			}
		}

		/* Walk schema references in this tag to discover all dependent tags */
		sgrp = forge_schema_group_get(fourcc);
		if (sgrp && sgrp->definition)
		{
			struct
			{
				struct tag_schema_definition const *def;
				uint32_t foff;
				int32_t count;
			} stack[64];
			int sp = 0;
			stack[sp].def = sgrp->definition;
			stack[sp].foff = xmap->tag_data_offset + (daddr - xmap->tag_base_addr);
			stack[sp].count = 1;
			sp++;

			while (sp > 0)
			{
				struct tag_schema_definition const *def = stack[sp - 1].def;
				uint32_t base_foff = stack[sp - 1].foff;
				int32_t count = stack[sp - 1].count;
				uint32_t elem_sz = (def && def->size > 0) ? (uint32_t)def->size : 0u;
				int32_t e;
				sp--;
				if (!def || !def->fields || elem_sz == 0) continue;

				for (e = 0; e < count; e++)
				{
					uint32_t e_foff = base_foff + (uint32_t)e * elem_sz;
					struct tag_schema_field const *f;
					if (e_foff + elem_sz > xmap->file_size) break;
					for (f = def->fields; f->type != _tag_schema_terminator; f++)
					{
						uint32_t f_off = e_foff + (uint32_t)f->offset;
						struct tag_schema_definition const *fdef = (struct tag_schema_definition const *)f->definition;
						if (f->type == _tag_schema_struct && fdef)
						{
							if (sp < 64)
							{
								stack[sp].def = fdef;
								stack[sp].foff = f_off;
								stack[sp].count = 1;
								sp++;
							}
						}
						else if (f->type == _tag_schema_block && fdef && fdef->size > 0)
						{
							if (f_off + 12u <= xmap->file_size)
							{
								int32_t cnt = read_s32(xmap->data + f_off + 0u);
								uint32_t addr = read_u32(xmap->data + f_off + 4u);
								uint32_t c_sz = (uint32_t)fdef->size;
								if (cnt > 0 && cnt <= 32768 && addr >= xmap->tag_base_addr &&
									(uint64_t)(addr - xmap->tag_base_addr) + (uint64_t)cnt * c_sz <= xmap->tag_data_size &&
									sp < 64)
								{
									stack[sp].def = fdef;
									stack[sp].foff = xmap->tag_data_offset + (addr - xmap->tag_base_addr);
									stack[sp].count = cnt;
									sp++;
								}
							}
						}
						else if (f->type == _tag_schema_reference && f_off + 16u <= xmap->file_size)
						{
							uint32_t ref_h = read_u32(xmap->data + f_off + 12u);
							int32_t ref_idx = (ref_h != 0xFFFFFFFFu) ? (int32_t)(ref_h & 0xFFFFu) : -1;
							if (ref_idx >= 0 && ref_idx < xmap->tag_count && ref_idx < 8192 &&
								!visited[ref_idx] && q_tail < 2048)
							{
								uint8_t const *rinst = src_tag_instance(xmap, ref_idx);
								uint32_t rgrp = rinst ? read_u32(rinst) : 0u;
								if (rgrp != 'udlg' && rgrp != 'sbsp' && rgrp != 'scnr')
								{
									visited[ref_idx] = 1;
									queue[q_tail++] = ref_idx;
								}
							}
						}
						else if (f->type == _tag_schema_tag_index && f_off + 4u <= xmap->file_size)
						{
							uint32_t ref_h = read_u32(xmap->data + f_off);
							int32_t ref_idx = (ref_h != 0xFFFFFFFFu) ? (int32_t)(ref_h & 0xFFFFu) : -1;
							if (ref_idx >= 0 && ref_idx < xmap->tag_count && ref_idx < 8192 &&
								!visited[ref_idx] && q_tail < 2048)
							{
								uint8_t const *rinst = src_tag_instance(xmap, ref_idx);
								uint32_t rgrp = rinst ? read_u32(rinst) : 0u;
								if (rgrp != 'udlg' && rgrp != 'sbsp' && rgrp != 'scnr')
								{
									visited[ref_idx] = 1;
									queue[q_tail++] = ref_idx;
								}
							}
						}
					}
				}
			}
		}
	}
}

static void cb_xbox_rebase_and_link_schema(
	struct forge_byte_buf *out,
	struct forge_source_map const *xmap,
	uint32_t tag_data_offset,
	struct tag_schema_definition const *def,
	uint32_t elem_base_foff,
	int32_t count,
	struct cb_xbox_bake_tag const *xtags,
	int xtag_count,
	int depth)
{
	int32_t i;
	uint32_t elem_sz;

	if (!out || !out->data || !xmap || !def || !def->fields || count <= 0 || depth > 28)
		return;
	elem_sz = (uint32_t)def->size;
	if (elem_sz == 0)
		return;

	for (i = 0; i < count; i++)
	{
		uint32_t e_foff = elem_base_foff + (uint32_t)i * elem_sz;
		struct tag_schema_field const *f;
		if (e_foff + elem_sz > out->size)
			break;

		for (f = def->fields; f->type != _tag_schema_terminator; f++)
		{
			uint32_t f_off = e_foff + (uint32_t)f->offset;
			struct tag_schema_definition const *fdef = (struct tag_schema_definition const *)f->definition;

			if (f->type == _tag_schema_struct && fdef)
			{
				cb_xbox_rebase_and_link_schema(
					out, xmap, tag_data_offset, fdef, f_off, 1,
					xtags, xtag_count, depth + 1);
			}
			else if (f->type == _tag_schema_block && fdef)
			{
				int32_t cnt;
				uint32_t addr;
				uint32_t child_elem_sz = (uint32_t)fdef->size;
				if (f_off + 12u > out->size)
					continue;
				cnt = read_s32(out->data + f_off + 0u);
				addr = read_u32(out->data + f_off + 4u);
				if (cnt <= 0 || child_elem_sz == 0)
				{
					write_s32(out->data + f_off + 0u, 0);
					write_u32(out->data + f_off + 4u, 0u);
					write_u32(out->data + f_off + 8u, 0u);
					continue;
				}
				if (f->maximum > 0 && cnt > f->maximum)
					cnt = f->maximum;

				if (addr >= xmap->tag_base_addr &&
					(uint64_t)(addr - xmap->tag_base_addr) + (uint64_t)cnt * child_elem_sz <= xmap->tag_data_size)
				{
					uint32_t total_bytes = (uint32_t)cnt * child_elem_sz;
					uint32_t child_foff = buf_alloc(out, total_bytes, 4);
					if (child_foff && out->data)
					{
						memcpy(out->data + child_foff,
							xmap->data + xmap->tag_data_offset + (addr - xmap->tag_base_addr),
							total_bytes);
						write_s32(out->data + f_off + 0u, cnt);
						write_u32(out->data + f_off + 4u, CE_TAG_BASE_ADDR + (child_foff - tag_data_offset));
						write_u32(out->data + f_off + 8u, 0u);
						cb_xbox_rebase_and_link_schema(
							out, xmap, tag_data_offset, fdef, child_foff, cnt,
							xtags, xtag_count, depth + 1);
					}
					else
					{
						write_s32(out->data + f_off + 0u, 0);
						write_u32(out->data + f_off + 4u, 0u);
						write_u32(out->data + f_off + 8u, 0u);
					}
				}
				else
				{
					write_s32(out->data + f_off + 0u, 0);
					write_u32(out->data + f_off + 4u, 0u);
					write_u32(out->data + f_off + 8u, 0u);
				}
			}
			else if (f->type == _tag_schema_data)
			{
				int32_t sz;
				uint32_t addr;
				if (f_off + 20u > out->size)
					continue;
				sz = read_s32(out->data + f_off + 0u);
				addr = read_u32(out->data + f_off + 12u);
				if (sz <= 0 || (f->maximum > 0 && sz > f->maximum))
				{
					memset(out->data + f_off, 0, 20u);
					continue;
				}
				if (addr >= xmap->tag_base_addr &&
					(uint64_t)(addr - xmap->tag_base_addr) + (uint64_t)sz <= xmap->tag_data_size)
				{
					uint32_t data_foff = buf_alloc(out, (uint32_t)sz, 4);
					if (data_foff && out->data)
					{
						memcpy(out->data + data_foff,
							xmap->data + xmap->tag_data_offset + (addr - xmap->tag_base_addr),
							(size_t)sz);
						write_u32(out->data + f_off + 4u, 0u);
						write_u32(out->data + f_off + 8u, 0u);
						write_u32(out->data + f_off + 12u, CE_TAG_BASE_ADDR + (data_foff - tag_data_offset));
						write_u32(out->data + f_off + 16u, 0u);
					}
					else
					{
						memset(out->data + f_off, 0, 20u);
					}
				}
				else
				{
					memset(out->data + f_off, 0, 20u);
				}
			}
			else if (f->type == _tag_schema_reference)
			{
				uint32_t ref_h;
				int32_t ref_src_idx;
				int k, linked = 0;
				if (f_off + 16u > out->size)
					continue;
				ref_h = read_u32(out->data + f_off + 12u);
				ref_src_idx = (ref_h != 0xFFFFFFFFu) ? (int32_t)(ref_h & 0xFFFFu) : -1;
				if (ref_src_idx >= 0)
				{
					for (k = 0; k < xtag_count; k++)
					{
						if (xtags[k].src_tag_idx == ref_src_idx && xtags[k].baked_tag_idx >= 0)
						{
							uint32_t dst_grp = (xtags[k].fourcc == 'mode') ? 'mod2' : xtags[k].fourcc;
							write_u32(out->data + f_off + 0u, dst_grp);
							write_u32(out->data + f_off + 4u, xtags[k].baked_name_addr);
							write_u32(out->data + f_off + 8u, 0u);
							write_u32(out->data + f_off + 12u, xtags[k].baked_handle);
							linked = 1;
							break;
						}
					}
				}
				if (!linked)
				{
					if (read_u32(out->data + f_off + 0u) == 0u)
						write_u32(out->data + f_off + 0u, 0xFFFFFFFFu);
					write_u32(out->data + f_off + 4u, 0u);
					write_u32(out->data + f_off + 8u, 0u);
					write_u32(out->data + f_off + 12u, 0xFFFFFFFFu);
				}
			}
			else if (f->type == _tag_schema_tag_index)
			{
				uint32_t ref_h;
				int32_t ref_src_idx;
				int k, linked = 0;
				if (f_off + 4u > out->size)
					continue;
				ref_h = read_u32(out->data + f_off);
				ref_src_idx = (ref_h != 0xFFFFFFFFu) ? (int32_t)(ref_h & 0xFFFFu) : -1;
				if (ref_src_idx >= 0)
				{
					for (k = 0; k < xtag_count; k++)
					{
						if (xtags[k].src_tag_idx == ref_src_idx && xtags[k].baked_tag_idx >= 0)
						{
							write_u32(out->data + f_off, xtags[k].baked_handle);
							linked = 1;
							break;
						}
					}
				}
				if (!linked)
					write_u32(out->data + f_off, 0xFFFFFFFFu);
			}
		}
	}
}

static void cb_compute_swizzle_masks(
	int16_t width,
	int16_t height,
	int16_t depth,
	uint32_t *out_x_mask,
	uint32_t *out_y_mask,
	uint32_t *out_z_mask)
{
	uint32_t x_mask = 0, y_mask = 0, z_mask = 0;
	uint32_t bit = 1, swiz_bit = 1;
	int i;

	for (i = 0; i < 16; i++)
	{
		if (bit < (uint32_t)width)  { x_mask |= swiz_bit; swiz_bit <<= 1; }
		if (bit < (uint32_t)height) { y_mask |= swiz_bit; swiz_bit <<= 1; }
		if (bit < (uint32_t)depth)  { z_mask |= swiz_bit; swiz_bit <<= 1; }
		bit <<= 1;
	}
	*out_x_mask = x_mask;
	*out_y_mask = y_mask;
	*out_z_mask = z_mask;
}

static uint8_t *cb_unpack_xbox_bitmap_to_ce(
	uint8_t const *xbox_pixels,
	uint32_t xbox_pixel_bytes,
	int16_t width,
	int16_t height,
	int16_t depth,
	int16_t type,
	int16_t format,
	uint16_t flags,
	int16_t mipmap_count,
	uint32_t *out_ce_size)
{
	uint32_t ce_total_bytes = 0;
	uint8_t *ce_buf;
	uint8_t *unpacked_buf = NULL;
	uint8_t const *src_pixels = xbox_pixels;
	boolean is_compressed;
	boolean is_swizzled;
	uint32_t bits_per_pix;
	int16_t mips;
	int m_idx;

	if (out_ce_size) *out_ce_size = 0;
	if (!xbox_pixels || xbox_pixel_bytes == 0 || width <= 0 || height <= 0 || depth <= 0)
		return NULL;
	if ((uint16_t)type >= 4u || (uint16_t)format >= 18u)
		return NULL;

	/* Formats: 0=a8, 1=y8, 2=ay8, 3=a8y8, 6=r5g6b5, 8=a1r5g5b5, 9=a4r4g4b4,
	 * 10=x8r8g8b8, 11=a8r8g8b8, 14=dxt1, 15=dxt3, 16=dxt5, 17=p8-bump */
	is_compressed = (format >= 14 && format <= 16) ? TRUE : FALSE;
	is_swizzled = ((flags & 0x0008u) != 0 && !is_compressed) ? TRUE : FALSE;
	bits_per_pix = (format == 14) ? 4u :
		((format == 15 || format == 16) ? 8u :
		((format == 0 || format == 1 || format == 2 || format == 17) ? 8u :
		((format == 10 || format == 11) ? 32u : 16u)));
	mips = (mipmap_count >= 0) ? mipmap_count : 0;

	for (m_idx = 0; m_idx <= (int)mips; m_idx++)
	{
		uint32_t mw = (uint32_t)width >> m_idx;
		uint32_t mh = (uint32_t)height >> m_idx;
		uint32_t md = (uint32_t)depth >> m_idx;
		uint32_t mpix;
		if (mw == 0) mw = 1;
		if (mh == 0) mh = 1;
		if (md == 0) md = 1;
		if (is_compressed)
		{
			mw = (mw + 3u) & ~3u;
			mh = (mh + 3u) & ~3u;
		}
		mpix = mw * mh * md;
		if (type == 2) /* _bitmap_type_cube_map */
			mpix *= 6u;
		ce_total_bytes += (mpix * bits_per_pix) >> 3;
	}
	if (ce_total_bytes == 0 || ce_total_bytes > 16u * 1024u * 1024u)
		return NULL;

	ce_buf = (uint8_t *)calloc(1, ce_total_bytes);
	if (!ce_buf)
		return NULL;

	if (type == 2) /* _bitmap_type_cube_map */
	{
		static int const face_inv[6] = { 0, 2, 1, 3, 4, 5 };
		uint32_t src_off = 0;
		int face, mip;
		int mip_total = (int)mips + 1;

		unpacked_buf = (uint8_t *)calloc(1, ce_total_bytes);
		if (!unpacked_buf)
		{
			free(ce_buf);
			return NULL;
		}
		for (face = 0; face < 6; face++)
		{
			uint32_t dst_off = 0;
			int ce_face = face_inv[face];
			for (mip = 0; mip < mip_total; mip++)
			{
				uint32_t mw = (uint32_t)width >> mip;
				uint32_t mh = (uint32_t)height >> mip;
				uint32_t face_bytes;
				if (mw == 0) mw = 1;
				if (mh == 0) mh = 1;
				if (is_compressed)
				{
					uint32_t bw = (mw + 3u) & ~3u;
					uint32_t bh = (mh + 3u) & ~3u;
					face_bytes = (bw * bh * bits_per_pix) >> 3;
				}
				else
				{
					face_bytes = (mw * mh * bits_per_pix) >> 3;
				}
				if (src_off + face_bytes <= xbox_pixel_bytes &&
					dst_off + (uint32_t)(ce_face + 1) * face_bytes <= ce_total_bytes)
				{
					memcpy(unpacked_buf + dst_off + (uint32_t)ce_face * face_bytes,
						xbox_pixels + src_off, face_bytes);
				}
				src_off += face_bytes;
				dst_off += face_bytes * 6u;
			}
			src_off = (src_off + 127u) & ~127u;
		}
		src_pixels = unpacked_buf;
	}
	else if (!is_compressed && !is_swizzled && type == 0) /* _bitmap_type_2d_textures */
	{
		uint32_t row_bytes = ((uint32_t)width * bits_per_pix) >> 3;
		uint32_t pitch = (row_bytes + 63u) & ~63u;
		if (pitch != row_bytes && pitch * (uint32_t)height <= xbox_pixel_bytes)
		{
			int y;
			for (y = 0; y < (int)height; y++)
			{
				if ((uint32_t)(y + 1) * row_bytes <= ce_total_bytes)
					memcpy(ce_buf + (uint32_t)y * row_bytes, xbox_pixels + (uint32_t)y * pitch, row_bytes);
			}
			if (out_ce_size) *out_ce_size = ce_total_bytes;
			return ce_buf;
		}
	}

	if (is_swizzled)
	{
		uint32_t bytes_per_pix = bits_per_pix >> 3;
		int mip, mip_total = (int)mips + 1;
		int faces = (type == 2) ? 6 : 1;
		uint32_t mip_off = 0;

		for (mip = 0; mip < mip_total; mip++)
		{
			int16_t mw = (int16_t)(width >> mip);
			int16_t mh = (int16_t)(height >> mip);
			int16_t md = (int16_t)(depth >> mip);
			uint32_t face_bytes;
			int f_idx;
			if (mw <= 0) mw = 1;
			if (mh <= 0) mh = 1;
			if (md <= 0) md = 1;
			face_bytes = (uint32_t)mw * (uint32_t)mh * (uint32_t)md * bytes_per_pix;

			for (f_idx = 0; f_idx < faces; f_idx++)
			{
				uint32_t sub_off = mip_off + (uint32_t)f_idx * face_bytes;
				uint32_t src_limit = (src_pixels == unpacked_buf) ? ce_total_bytes : xbox_pixel_bytes;
				if (sub_off + face_bytes <= ce_total_bytes && sub_off + face_bytes <= src_limit)
				{
					uint32_t x_mask = 0, y_mask = 0, z_mask = 0;
					int x, y, z;
					uint32_t z_swiz, y_swiz, x_swiz, src_idx = 0;
					uint8_t const *s_base = src_pixels + sub_off;
					uint8_t *d_base = ce_buf + sub_off;

					cb_compute_swizzle_masks(mw, mh, md, &x_mask, &y_mask, &z_mask);
					for (z = 0, z_swiz = 0; z < (int)md; z++, z_swiz = (z_swiz - z_mask) & z_mask)
					{
						for (y = 0, y_swiz = 0; y < (int)mh; y++, y_swiz = (y_swiz - y_mask) & y_mask)
						{
							for (x = 0, x_swiz = 0; x < (int)mw; x++, x_swiz = (x_swiz - x_mask) & x_mask)
							{
								uint32_t sw_i = z_swiz | y_swiz | x_swiz;
								if (bytes_per_pix == 1u)
									d_base[src_idx] = s_base[sw_i];
								else if (bytes_per_pix == 2u)
									((uint16_t *)d_base)[src_idx] = ((uint16_t const *)s_base)[sw_i];
								else if (bytes_per_pix == 4u)
									((uint32_t *)d_base)[src_idx] = ((uint32_t const *)s_base)[sw_i];
								src_idx++;
							}
						}
					}
				}
			}
			mip_off += face_bytes * (uint32_t)faces;
		}
	}
	else
	{
		uint32_t src_limit = (src_pixels == unpacked_buf) ? ce_total_bytes : xbox_pixel_bytes;
		uint32_t copy_n = (src_limit < ce_total_bytes) ? src_limit : ce_total_bytes;
		memcpy(ce_buf, src_pixels, copy_n);
	}

	free(unpacked_buf);
	if (out_ce_size) *out_ce_size = ce_total_bytes;
	return ce_buf;
}

static void cb_tint_dxt1_blue_to_red(uint8_t *pix, uint32_t byte_count)
{
	uint32_t off;
	if (!pix)
		return;
	for (off = 0; off + 8u <= byte_count; off += 8u)
	{
		uint16_t c0 = read_u16(pix + off + 0u);
		uint16_t c1 = read_u16(pix + off + 2u);
		uint32_t lut = read_u32(pix + off + 4u);
		uint16_t r0 = (uint16_t)((c0 >> 11) & 0x1Fu);
		uint16_t g0 = (uint16_t)((c0 >> 5) & 0x3Fu);
		uint16_t b0 = (uint16_t)(c0 & 0x1Fu);
		uint16_t r1 = (uint16_t)((c1 >> 11) & 0x1Fu);
		uint16_t g1 = (uint16_t)((c1 >> 5) & 0x3Fu);
		uint16_t b1 = (uint16_t)(c1 & 0x1Fu);
		uint16_t nr0 = (b0 > r0) ? b0 : r0;
		uint16_t nb0 = (r0 < b0) ? (uint16_t)(r0 >> 1) : (uint16_t)(b0 >> 1);
		uint16_t ng0 = (uint16_t)(g0 >> 2);
		uint16_t nr1 = (b1 > r1) ? b1 : r1;
		uint16_t nb1 = (r1 < b1) ? (uint16_t)(r1 >> 1) : (uint16_t)(b1 >> 1);
		uint16_t ng1 = (uint16_t)(g1 >> 2);
		uint16_t nc0 = (uint16_t)((nr0 << 11) | (ng0 << 5) | nb0);
		uint16_t nc1 = (uint16_t)((nr1 << 11) | (ng1 << 5) | nb1);

		if (c0 > c1)
		{
			if (nc0 < nc1)
			{
				uint16_t tmp = nc0;
				nc0 = nc1;
				nc1 = tmp;
				lut ^= 0x55555555u;
			}
			else if (nc0 == nc1)
			{
				if (nc0 < 0xFFFFu) nc0++;
				else nc1--;
			}
		}
		else
		{
			if (nc0 > nc1)
			{
				uint16_t tmp = nc0;
				uint32_t new_lut = 0;
				int p;
				nc0 = nc1;
				nc1 = tmp;
				for (p = 0; p < 16; p++)
				{
					uint32_t idx = (lut >> (p * 2)) & 3u;
					if (idx == 0u) idx = 1u;
					else if (idx == 1u) idx = 0u;
					new_lut |= (idx << (p * 2));
				}
				lut = new_lut;
			}
		}
		write_u16(pix + off + 0u, nc0);
		write_u16(pix + off + 2u, nc1);
		write_u32(pix + off + 4u, lut);
	}
}

static int32_t out_map_bake_xbox_tag_closure(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	uint32_t scnr_off,
	uint32_t orig_vtx_size,
	uint32_t orig_idx_size,
	struct forge_pending_bake_obj const *pobj)
{
	struct forge_source_map xmap;
	int32_t root_src_idx;
	struct cb_xbox_bake_tag *xtags = NULL;
	int xtag_count = 0;
	uint8_t visited[8192];
	int32_t queue[2048];
	int q_head = 0, q_tail = 0;
	int i;
	int32_t root_baked_idx = -1;
	int is_red_beam_variant = 0;

	if (!out || !out->data || !pobj || !pobj->xbox_map_path[0])
		return -1;
	is_red_beam_variant = (pobj->group_tag == 'mach' &&
		_stricmp(pobj->tag_path, "levels\\a30\\devices\\beam emitter\\beam emitter red") == 0) ? 1 : 0;
	if (!forge_source_map_open(pobj->xbox_map_path, &xmap) || xmap.version != CACHE_VERSION_XBOX)
	{
		if (xmap.data) forge_source_map_close(&xmap);
		return -1;
	}

	root_src_idx = src_map_find_tag_or_variant(&xmap, pobj->group_tag, pobj->tag_path);
	if (root_src_idx < 0 || root_src_idx >= 8192)
	{
		forge_source_map_close(&xmap);
		return -1;
	}

	xtags = (struct cb_xbox_bake_tag *)calloc(2048, sizeof(struct cb_xbox_bake_tag));
	if (!xtags)
	{
		forge_source_map_close(&xmap);
		return -1;
	}

	/* 1. Discover transitive closure of tags starting at root_src_idx */
	memset(visited, 0, sizeof(visited));
	visited[root_src_idx] = 1;
	queue[q_tail++] = root_src_idx;

	while (q_head < q_tail && xtag_count < 2048)
	{
		int32_t tidx = queue[q_head++];
		uint8_t const *inst = src_tag_instance(&xmap, tidx);
		uint32_t fourcc, daddr;
		char const *nm;
		struct cb_xbox_bake_tag *xt;
		struct tag_schema_group const *sgrp;

		if (!inst) continue;
		fourcc = read_u32(inst + 0x00u);
		daddr = read_u32(inst + 0x14u);
		if (daddr < xmap.tag_base_addr || daddr >= xmap.tag_base_addr + xmap.tag_data_size)
			continue;
		nm = src_tag_name(&xmap, tidx);

		xt = &xtags[xtag_count++];
		memset(xt, 0, sizeof(*xt));
		xt->src_tag_idx = tidx;
		xt->fourcc = fourcc;
		snprintf(xt->name, sizeof(xt->name), "%s", nm ? nm : "");
		xt->src_base_addr = daddr;
		xt->baked_tag_idx = -1;
		xt->baked_handle = 0xFFFFFFFFu;

		sgrp = forge_schema_group_get(fourcc);
		if (sgrp && sgrp->definition)
		{
			struct
			{
				struct tag_schema_definition const *def;
				uint32_t foff;
				int32_t count;
			} stack[64];
			int sp = 0;
			stack[sp].def = sgrp->definition;
			stack[sp].foff = xmap.tag_data_offset + (daddr - xmap.tag_base_addr);
			stack[sp].count = 1;
			sp++;

			while (sp > 0)
			{
				struct tag_schema_definition const *def = stack[sp - 1].def;
				uint32_t base_foff = stack[sp - 1].foff;
				int32_t count = stack[sp - 1].count;
				uint32_t elem_sz = (def && def->size > 0) ? (uint32_t)def->size : 0u;
				int32_t e;
				sp--;
				if (!def || !def->fields || elem_sz == 0) continue;

				for (e = 0; e < count; e++)
				{
					uint32_t e_foff = base_foff + (uint32_t)e * elem_sz;
					struct tag_schema_field const *f;
					if (e_foff + elem_sz > xmap.file_size) break;
					for (f = def->fields; f->type != _tag_schema_terminator; f++)
					{
						uint32_t f_off = e_foff + (uint32_t)f->offset;
						struct tag_schema_definition const *fdef = (struct tag_schema_definition const *)f->definition;
						if (f->type == _tag_schema_struct && fdef)
						{
							if (sp < 64)
							{
								stack[sp].def = fdef;
								stack[sp].foff = f_off;
								stack[sp].count = 1;
								sp++;
							}
						}
						else if (f->type == _tag_schema_block && fdef && fdef->size > 0)
						{
							if (f_off + 12u <= xmap.file_size)
							{
								int32_t cnt = read_s32(xmap.data + f_off + 0u);
								uint32_t addr = read_u32(xmap.data + f_off + 4u);
								uint32_t c_sz = (uint32_t)fdef->size;
								if (cnt > 0 && cnt <= 32768 && addr >= xmap.tag_base_addr &&
									(uint64_t)(addr - xmap.tag_base_addr) + (uint64_t)cnt * c_sz <= xmap.tag_data_size &&
									sp < 64)
								{
									stack[sp].def = fdef;
									stack[sp].foff = xmap.tag_data_offset + (addr - xmap.tag_base_addr);
									stack[sp].count = cnt;
									sp++;
								}
							}
						}
						else if (f->type == _tag_schema_reference && f_off + 16u <= xmap.file_size)
						{
							uint32_t ref_h = read_u32(xmap.data + f_off + 12u);
							int32_t ref_idx = (ref_h != 0xFFFFFFFFu) ? (int32_t)(ref_h & 0xFFFFu) : -1;
							if (ref_idx >= 0 && ref_idx < xmap.tag_count && ref_idx < 8192 &&
								!visited[ref_idx] && q_tail < 2048)
							{
								uint8_t const *rinst = src_tag_instance(&xmap, ref_idx);
								uint32_t rgrp = rinst ? read_u32(rinst) : 0u;
								if (rgrp != 'udlg' && rgrp != 'sbsp' && rgrp != 'scnr')
								{
									visited[ref_idx] = 1;
									queue[q_tail++] = ref_idx;
								}
							}
						}
						else if (f->type == _tag_schema_tag_index && f_off + 4u <= xmap.file_size)
						{
							uint32_t ref_h = read_u32(xmap.data + f_off);
							int32_t ref_idx = (ref_h != 0xFFFFFFFFu) ? (int32_t)(ref_h & 0xFFFFu) : -1;
							if (ref_idx >= 0 && ref_idx < xmap.tag_count && ref_idx < 8192 &&
								!visited[ref_idx] && q_tail < 2048)
							{
								uint8_t const *rinst = src_tag_instance(&xmap, ref_idx);
								uint32_t rgrp = rinst ? read_u32(rinst) : 0u;
								if (rgrp != 'udlg' && rgrp != 'sbsp' && rgrp != 'scnr')
								{
									visited[ref_idx] = 1;
									queue[q_tail++] = ref_idx;
								}
							}
						}
					}
				}
			}
		}
	}

	/* 2. Allocate root tag definitions (or reuse existing tags in out) */
	for (i = 0; i < xtag_count; i++)
	{
		struct cb_xbox_bake_tag *xt = &xtags[i];
		uint32_t dst_grp = (xt->fourcc == 'mode') ? 'mod2' : xt->fourcc;
		uint32_t p1 = 0xFFFFFFFFu, p2 = 0xFFFFFFFFu;
		int32_t existing;
		struct tag_schema_group const *sgrp;
		uint32_t root_sz;
		char alloc_name[256];

		if (i == 0 && is_red_beam_variant)
			snprintf(xt->name, sizeof(xt->name), "%s", pobj->tag_path);

		existing = (i > 0 && dst_grp != 'bitm' && dst_grp != 'snd!')
			? out_map_find_tag(out, tag_data_offset, dst_grp, xt->name) : -1;
		if (is_red_beam_variant &&
			(dst_grp == 'mod2' || dst_grp == 'sotr' || dst_grp == 'ligh' || dst_grp == 'lens' || dst_grp == 'bitm'))
		{
			existing = -1;
		}

		if (existing >= 0)
		{
			uint32_t eio = tag_data_offset + 0x28u + (uint32_t)existing * 32u;
			xt->is_reused_existing = 1;
			xt->baked_tag_idx = existing;
			xt->baked_handle = read_u32(out->data + eio + 0x0Cu);
			xt->baked_name_addr = read_u32(out->data + eio + 0x10u);
			continue;
		}

		sgrp = forge_schema_group_get(dst_grp);
		if (!sgrp || !sgrp->definition || sgrp->definition->size <= 0)
			continue;
		root_sz = (uint32_t)sgrp->definition->size;
		if ((uint64_t)(xt->src_base_addr - xmap.tag_base_addr) + root_sz > xmap.tag_data_size)
			continue;

		xt->def_foff = buf_alloc(out, root_sz, 4);
		if (!xt->def_foff || !out->data)
			continue;
		memcpy(out->data + xt->def_foff,
			xmap.data + xmap.tag_data_offset + (xt->src_base_addr - xmap.tag_base_addr),
			root_sz);

		if (dst_grp == 'vehi' || dst_grp == 'bipd') { p1 = 'unit'; p2 = 'obje'; }
		else if (dst_grp == 'weap' || dst_grp == 'eqip' || dst_grp == 'garb') { p1 = 'item'; p2 = 'obje'; }
		else if (dst_grp == 'scen' || dst_grp == 'proj') { p1 = 'obje'; p2 = 0xFFFFFFFFu; }
		else if (dst_grp == 'mach' || dst_grp == 'ctrl' || dst_grp == 'lifi') { p1 = 'devi'; p2 = 'obje'; }
		else if (dst_grp == 'soso' || dst_grp == 'senv' || dst_grp == 'sgla' || dst_grp == 'schi' ||
				 dst_grp == 'scex' || dst_grp == 'swat' || dst_grp == 'smet' || dst_grp == 'spla' || dst_grp == 'strn' || dst_grp == 'sotr')
		{
			p1 = 'shdr'; p2 = 0xFFFFFFFFu;
		}

		if (is_red_beam_variant && i > 0 &&
			(dst_grp == 'mod2' || dst_grp == 'sotr' || dst_grp == 'ligh' || dst_grp == 'lens' || dst_grp == 'bitm'))
		{
			snprintf(alloc_name, sizeof(alloc_name), "%s__red", xt->name);
		}
		else if (dst_grp == 'bitm' || dst_grp == 'snd!')
			snprintf(alloc_name, sizeof(alloc_name), "%s__xbox", xt->name);
		else
			snprintf(alloc_name, sizeof(alloc_name), "%s", xt->name);

		xt->baked_tag_idx = out_map_alloc_tag_instance(
			out, tag_data_offset, scnr_off,
			dst_grp, p1, p2,
			alloc_name, xt->def_foff, 0u,
			&xt->baked_handle, &xt->baked_name_addr);
	}

	/* 3. Rebase & clone all nested tag_blocks and tag_data, and link tag_references */
	for (i = 0; i < xtag_count; i++)
	{
		struct cb_xbox_bake_tag *xt = &xtags[i];
		struct tag_schema_group const *sgrp;
		if (!xt->def_foff || xt->is_reused_existing || xt->baked_tag_idx < 0)
			continue;
		/* Note: for 'mode', walk the Xbox 'mode' schema first (where parts are 104 bytes),
		 * then step 4 converts each 104-byte Xbox part block into a 132-byte CE 'mod2' part block! */
		sgrp = forge_schema_group_get(xt->fourcc);
		if (!sgrp || !sgrp->definition || sgrp->definition->size <= 0)
			continue;
		cb_xbox_rebase_and_link_schema(
			out, &xmap, tag_data_offset, sgrp->definition,
			xt->def_foff, 1, xtags, xtag_count, 0);
	}

	/* 4. Convert Xbox 'mode' (104-byte parts) to Custom Edition 'mod2' (132-byte parts) */
	for (i = 0; i < xtag_count; i++)
	{
		struct cb_xbox_bake_tag *xt = &xtags[i];
		int32_t geom_cnt, shdr_cnt, node_cnt;
		uint32_t geom_addr;
		int g_idx;

		if (!xt->def_foff || xt->is_reused_existing || xt->baked_tag_idx < 0 || xt->fourcc != 'mode')
			continue;

		/* Clear local_nodes_bit (bit 2) and ensure non-zero UV scales */
		write_u32(out->data + xt->def_foff + 0x00u, read_u32(out->data + xt->def_foff + 0x00u) & ~0x04u);
		if (!(read_f32(out->data + xt->def_foff + 0x30u) > 0.0001f))
			write_f32(out->data + xt->def_foff + 0x30u, 1.0f);
		if (!(read_f32(out->data + xt->def_foff + 0x34u) > 0.0001f))
			write_f32(out->data + xt->def_foff + 0x34u, 1.0f);

		node_cnt = read_s32(out->data + xt->def_foff + 0xB8u);
		geom_cnt = read_s32(out->data + xt->def_foff + 0xD0u);
		geom_addr = read_u32(out->data + xt->def_foff + 0xD4u);
		shdr_cnt = read_s32(out->data + xt->def_foff + 0xDCu);

		/* Shift beam emitter root node down by 6.5 world units (~10m in Forge HUD) so its flare collar sits at the placement origin */
		if (is_red_beam_variant || strstr(xt->name, "beam emitter") != NULL)
		{
			uint32_t node_addr = read_u32(out->data + xt->def_foff + 0xBCu);
			if (node_cnt > 0 && node_addr >= CE_TAG_BASE_ADDR)
			{
				uint32_t node0_foff = tag_data_offset + (node_addr - CE_TAG_BASE_ADDR);
				if (node0_foff + 156u <= out->size)
					write_f32(out->data + node0_foff + 48u, -6.5f);
			}
		}

		if (geom_cnt > 0 && geom_addr >= CE_TAG_BASE_ADDR)
		{
			uint32_t geom_base_foff = tag_data_offset + (geom_addr - CE_TAG_BASE_ADDR);
			for (g_idx = 0; g_idx < geom_cnt; g_idx++)
			{
				uint32_t g_foff = geom_base_foff + (uint32_t)g_idx * 48u;
				int32_t part_cnt = read_s32(out->data + g_foff + 36u);
				uint32_t old_part_addr = read_u32(out->data + g_foff + 40u);
				uint32_t new_parts_foff;
				int p_idx;

				if (part_cnt <= 0 || old_part_addr < CE_TAG_BASE_ADDR)
				{
					write_s32(out->data + g_foff + 36u, 0);
					write_u32(out->data + g_foff + 40u, 0u);
					continue;
				}
				new_parts_foff = buf_alloc(out, (uint32_t)part_cnt * 132u, 4);
				if (!new_parts_foff || !out->data)
					continue;
				memset(out->data + new_parts_foff, 0, (size_t)part_cnt * 132u);
				geom_base_foff = tag_data_offset + (geom_addr - CE_TAG_BASE_ADDR);
				g_foff = geom_base_foff + (uint32_t)g_idx * 48u;

				for (p_idx = 0; p_idx < part_cnt; p_idx++)
				{
					uint32_t old_p = tag_data_offset + (old_part_addr - CE_TAG_BASE_ADDR) + (uint32_t)p_idx * 104u;
					uint32_t new_p = new_parts_foff + (uint32_t)p_idx * 132u;
					int16_t s_idx = read_s16(out->data + old_p + 0x04u);
					int16_t c_pri = read_s16(out->data + old_p + 0x08u);
					int16_t c_sec = read_s16(out->data + old_p + 0x0Au);
					int gp, matched = 0;

					/* Copy first 0x48 bytes (flags, shader_index, prev/next, centroid, bounds) */
					memcpy(out->data + new_p, out->data + old_p, 0x48u);
					write_u32(out->data + new_p + 0x00u, read_u32(out->data + new_p + 0x00u) & ~0x04u);
					if (s_idx < 0 || s_idx >= shdr_cnt)
						write_s16(out->data + new_p + 0x04u, (shdr_cnt > 0) ? 0 : -1);
					out->data[new_p + 0x06u] = 0xFFu;
					out->data[new_p + 0x07u] = 0xFFu;
					if (c_pri < 0 || c_pri >= node_cnt)
						write_s16(out->data + new_p + 0x08u, 0);
					if (c_sec < 0 || c_sec >= node_cnt)
						write_s16(out->data + new_p + 0x0Au, 0);

					for (gp = 0; gp < pobj->ocetags_gpart_count; gp++)
					{
						struct forge_pending_ocetags_gpart const *ogp = &pobj->ocetags_gparts[gp];
						if (ogp->tag_idx == (uint32_t)xt->src_tag_idx &&
							ogp->geom_idx == (uint32_t)g_idx &&
							ogp->part_idx == (uint32_t)p_idx)
						{
							int32_t tri_cnt = (ogp->idx_count >= 2u) ? (int32_t)(ogp->idx_count - 2u) : 1;
							write_s16(out->data + new_p + 0x44u, 1); /* TRIANGLE_BUFFER_TYPE_PRECOMPILED_STRIP */
							write_s32(out->data + new_p + 0x48u, tri_cnt);
							write_u32(out->data + new_p + 0x4Cu, orig_idx_size + ogp->extra_idx_rel_off);
							write_u32(out->data + new_p + 0x50u, orig_idx_size + ogp->extra_idx_rel_off);
							write_s16(out->data + new_p + 0x54u, 4); /* VERTEX_TYPE_MODEL_UNCOMPRESSED */
							write_s32(out->data + new_p + 0x58u, (int32_t)ogp->vtx_count);
							write_u32(out->data + new_p + 0x60u, orig_vtx_size + ogp->extra_vtx_rel_off);
							write_u32(out->data + new_p + 0x64u, orig_vtx_size + ogp->extra_vtx_rel_off);
							matched = 1;
							break;
						}
					}
					if (!matched)
					{
						write_s16(out->data + new_p + 0x44u, 1);
						write_s32(out->data + new_p + 0x48u, 0);
						write_s16(out->data + new_p + 0x54u, 4);
						write_s32(out->data + new_p + 0x58u, 0);
					}
				}
				write_u32(out->data + g_foff + 40u, CE_TAG_BASE_ADDR + (new_parts_foff - tag_data_offset));
			}
		}
	}

	/* 5. Convert Xbox 'bitm' raw pixel data into CE format and embed in out */
	for (i = 0; i < xtag_count; i++)
	{
		struct cb_xbox_bake_tag *xt = &xtags[i];
		int32_t b_cnt;
		uint32_t b_addr;
		int b_idx;

		if (!xt->def_foff || xt->is_reused_existing || xt->baked_tag_idx < 0 || xt->fourcc != 'bitm')
			continue;

		b_cnt = read_s32(out->data + xt->def_foff + 0x60u);
		b_addr = read_u32(out->data + xt->def_foff + 0x64u);
		if (b_cnt <= 0 || b_addr < CE_TAG_BASE_ADDR)
			continue;

		for (b_idx = 0; b_idx < b_cnt; b_idx++)
		{
			uint32_t bd_foff = tag_data_offset + (b_addr - CE_TAG_BASE_ADDR) + (uint32_t)b_idx * 48u;
			int16_t w = read_s16(out->data + bd_foff + 0x04u);
			int16_t h = read_s16(out->data + bd_foff + 0x06u);
			int16_t d = read_s16(out->data + bd_foff + 0x08u);
			int16_t type = read_s16(out->data + bd_foff + 0x0Au);
			int16_t fmt = read_s16(out->data + bd_foff + 0x0Cu);
			uint16_t flg = read_u16(out->data + bd_foff + 0x0Eu);
			int16_t mips = read_s16(out->data + bd_foff + 0x14u);
			uint32_t raw_off = read_u32(out->data + bd_foff + 0x18u) & 0x7FFFFFFFu;
			uint32_t ce_sz = 0;
			uint8_t *ce_pix = NULL;

			if (raw_off < xmap.file_size)
			{
				ce_pix = cb_unpack_xbox_bitmap_to_ce(
					xmap.data + raw_off, xmap.file_size - raw_off,
					w, h, d, type, fmt, flg, mips, &ce_sz);
			}
			if (ce_pix && ce_sz > 0)
			{
				uint16_t new_flg = (uint16_t)((flg & 0x00FFu) & ~0x0008u) | 0x0080u;
				if (is_red_beam_variant && fmt == 14 &&
					(strstr(xt->name, "jackal shield") != NULL || strstr(xt->name, "static blue") != NULL))
				{
					cb_tint_dxt1_blue_to_red(ce_pix, ce_sz);
				}
				bd_foff = tag_data_offset + (b_addr - CE_TAG_BASE_ADDR) + (uint32_t)b_idx * 48u;
				write_u16(out->data + bd_foff + 0x0Eu, new_flg);
				write_u32(out->data + bd_foff + 0x18u, 0u);
				write_u32(out->data + bd_foff + 0x1Cu, ce_sz);
				write_u32(out->data + bd_foff + 0x20u, xt->baked_handle);
				write_u32(out->data + bd_foff + 0x24u, 0u);
				write_u32(out->data + bd_foff + 0x28u, 0u);
				cb_alloc_deferred_media(ce_pix, ce_sz, 32, bd_foff + 0x18u);
			}
			free(ce_pix);
		}

		/* Ensure OpenCE's reordered_bitmaps_find sees this Xbox bitmap as drawn_otherwise
		 * so its Xbox multipurpose/meter RGBA channels are never reordered as Custom Edition! */
		{
			uint32_t guard_foff = buf_alloc(out, 440u, 4);
			if (guard_foff && out->data)
			{
				struct tag_schema_group const *soso_grp = forge_schema_group_get('soso');
				char guard_name[256];
				memset(out->data + guard_foff, 0, 440u);
				if (soso_grp && soso_grp->definition)
					out_map_init_schema_null_refs(out, tag_data_offset, soso_grp->definition, guard_foff, 1);
				write_s16(out->data + guard_foff + 0x24u, 4); /* _shader_type_model */
				write_u32(out->data + guard_foff + 0xA4u + 0u, 'bitm');
				write_u32(out->data + guard_foff + 0xA4u + 4u, xt->baked_name_addr);
				write_u32(out->data + guard_foff + 0xA4u + 8u, 0u);
				write_u32(out->data + guard_foff + 0xA4u + 12u, xt->baked_handle);
				snprintf(guard_name, sizeof(guard_name), "%s__xbox_ch_guard", xt->name);
				out_map_alloc_tag_instance(
					out, tag_data_offset, scnr_off,
					'soso', 'shdr', 0xFFFFFFFFu,
					guard_name, guard_foff, 0u,
					NULL, NULL);
			}
		}
	}

	/* 6. Copy Xbox 'snd!' raw audio samples (already Xbox ADPCM) and embed in out */
	for (i = 0; i < xtag_count; i++)
	{
		struct cb_xbox_bake_tag *xt = &xtags[i];
		int32_t pr_cnt;
		uint32_t pr_addr;
		int pr_i;

		if (!xt->def_foff || xt->is_reused_existing || xt->baked_tag_idx < 0 || xt->fourcc != 'snd!')
			continue;

		pr_cnt = read_s32(out->data + xt->def_foff + 0x98u);
		pr_addr = read_u32(out->data + xt->def_foff + 0x9Cu);
		if (pr_cnt <= 0 || pr_addr < CE_TAG_BASE_ADDR)
			continue;

		for (pr_i = 0; pr_i < pr_cnt; pr_i++)
		{
			uint32_t pr_foff = tag_data_offset + (pr_addr - CE_TAG_BASE_ADDR) + (uint32_t)pr_i * 72u;
			int32_t pm_cnt = read_s32(out->data + pr_foff + 0x3Cu);
			uint32_t pm_addr = read_u32(out->data + pr_foff + 0x40u);
			int pm_i;

			if (pm_cnt <= 0 || pm_addr < CE_TAG_BASE_ADDR)
				continue;
			for (pm_i = 0; pm_i < pm_cnt; pm_i++)
			{
				uint32_t pm_foff = tag_data_offset + (pm_addr - CE_TAG_BASE_ADDR) + (uint32_t)pm_i * 124u;
				int32_t smp_sz = read_s32(out->data + pm_foff + 0x40u);
				uint32_t smp_off = read_u32(out->data + pm_foff + 0x48u) & 0x7FFFFFFFu;

				/* Set tag_index0 (+0x34) and tag_index1 (+0x3C) to baked_handle */
				write_u32(out->data + pm_foff + 0x34u, xt->baked_handle);
				write_u32(out->data + pm_foff + 0x3Cu, xt->baked_handle);
				/* Clear mouth_data (+0x54) and subtitle_data (+0x68) */
				memset(out->data + pm_foff + 0x54u, 0, 40u);

				if (smp_sz > 0 && (uint64_t)smp_off + (uint64_t)smp_sz <= xmap.file_size)
				{
					uint32_t use_sz = (uint32_t)smp_sz;
					write_s32(out->data + pm_foff + 0x40u, (int32_t)use_sz);
					/* Bit 0 (1u) is SOUND_SAMPLES_IN_RESOURCE_MAP_BIT; 0u means embedded in this .map! */
					write_u32(out->data + pm_foff + 0x44u, 0u);
					write_u32(out->data + pm_foff + 0x48u, 0u);
					write_u32(out->data + pm_foff + 0x4Cu, 0u);
					write_u32(out->data + pm_foff + 0x50u, 0u);
					cb_alloc_deferred_media(xmap.data + smp_off, use_sz, 16, pm_foff + 0x48u);
				}
				else
				{
					memset(out->data + pm_foff + 0x40u, 0, 20u);
				}
			}
		}
	}

	/* 7. Apply light / door / vehicle patches so baked tags work identically in halo.exe */
	for (i = 0; i < xtag_count; i++)
	{
		struct cb_xbox_bake_tag *xt = &xtags[i];
		if (!xt->def_foff || xt->is_reused_existing || xt->baked_tag_idx < 0)
			continue;

		if (xt->fourcc == 'ligh')
		{
			uint32_t flags = read_u32(out->data + xt->def_foff + 0x00u);
			float rad = read_f32(out->data + xt->def_foff + 0x04u);
			flags |= 0x01u; /* _light_definition_dynamic_bit */
			write_u32(out->data + xt->def_foff + 0x00u, flags);
			if (rad < 4.5f)
				write_f32(out->data + xt->def_foff + 0x04u, 4.5f);
			write_f32(out->data + xt->def_foff + 0x08u, 1.0f);
			write_f32(out->data + xt->def_foff + 0x0Cu, 1.0f);
			if (is_red_beam_variant)
			{
				write_f32(out->data + xt->def_foff + 0x38u, 1.0f);
				write_f32(out->data + xt->def_foff + 0x3Cu, 1.0f);
				write_f32(out->data + xt->def_foff + 0x40u, 0.12f);
				write_f32(out->data + xt->def_foff + 0x44u, 0.08f);
				write_f32(out->data + xt->def_foff + 0x48u, 1.0f);
				write_f32(out->data + xt->def_foff + 0x4Cu, 1.0f);
				write_f32(out->data + xt->def_foff + 0x50u, 0.18f);
				write_f32(out->data + xt->def_foff + 0x54u, 0.10f);
			}
			else if (read_f32(out->data + xt->def_foff + 0x3Cu) <= 0.01f &&
				read_f32(out->data + xt->def_foff + 0x40u) <= 0.01f &&
				read_f32(out->data + xt->def_foff + 0x44u) <= 0.01f)
			{
				write_f32(out->data + xt->def_foff + 0x38u, 1.0f);
				write_f32(out->data + xt->def_foff + 0x3Cu, 1.0f);
				write_f32(out->data + xt->def_foff + 0x40u, 0.96f);
				write_f32(out->data + xt->def_foff + 0x44u, 0.88f);
				write_f32(out->data + xt->def_foff + 0x48u, 1.0f);
				write_f32(out->data + xt->def_foff + 0x4Cu, 1.0f);
				write_f32(out->data + xt->def_foff + 0x50u, 0.96f);
				write_f32(out->data + xt->def_foff + 0x54u, 0.88f);
			}
		}
		else if (xt->fourcc == 'lens' && is_red_beam_variant)
		{
			int32_t refl_cnt = read_s32(out->data + xt->def_foff + 0xC4u);
			uint32_t refl_addr = read_u32(out->data + xt->def_foff + 0xC8u);
			if (refl_cnt > 0 && refl_addr >= CE_TAG_BASE_ADDR)
			{
				uint32_t refl_base = tag_data_offset + (refl_addr - CE_TAG_BASE_ADDR);
				int r_idx;
				for (r_idx = 0; r_idx < refl_cnt; r_idx++)
				{
					uint32_t rf = refl_base + (uint32_t)r_idx * 128u;
					if (rf + 128u <= out->size)
					{
						write_f32(out->data + rf + 0x40u, 1.0f);
						write_f32(out->data + rf + 0x44u, 1.0f);
						write_f32(out->data + rf + 0x48u, 0.12f);
						write_f32(out->data + rf + 0x4Cu, 0.08f);
						write_f32(out->data + rf + 0x50u, 1.0f);
						write_f32(out->data + rf + 0x54u, 1.0f);
						write_f32(out->data + rf + 0x58u, 0.18f);
						write_f32(out->data + rf + 0x5Cu, 0.10f);
						write_f32(out->data + rf + 0x60u, 1.0f);
						write_f32(out->data + rf + 0x64u, 1.0f);
						write_f32(out->data + rf + 0x68u, 0.12f);
						write_f32(out->data + rf + 0x6Cu, 0.08f);
					}
				}
			}
		}
		else if (xt->fourcc == 'scen' || xt->fourcc == 'lifi')
		{
			if (read_f32(out->data + xt->def_foff + 0x04u) < 0.45f)
				write_f32(out->data + xt->def_foff + 0x04u, 0.45f);
			if (read_f32(out->data + xt->def_foff + 0x104u) < 1.5f)
				write_f32(out->data + xt->def_foff + 0x104u, 1.5f);
		}
		else if (xt->fourcc == 'mach')
		{
			int16_t mtype = read_s16(out->data + xt->def_foff + 0x290u);
			if (read_f32(out->data + xt->def_foff + 0x04u) < 0.45f)
				write_f32(out->data + xt->def_foff + 0x04u, 0.45f);
			if (read_f32(out->data + xt->def_foff + 0x104u) < 1.5f)
				write_f32(out->data + xt->def_foff + 0x104u, 1.5f);
			if (is_red_beam_variant || strstr(xt->name, "beam emitter") != NULL)
			{
				write_f32(out->data + xt->def_foff + 0x04u, 65.0f);  /* bounding_radius */
				write_f32(out->data + xt->def_foff + 0x10u, 0.0f);   /* bounding_offset.z */
				write_f32(out->data + xt->def_foff + 0x104u, 65.0f); /* render_bounding_radius */
			}
			if (mtype == 0 || strstr(xt->name, "door") != NULL)
			{
				write_s16(out->data + xt->def_foff + 0x290u, 0);    /* _machine_type_door */
				write_f32(out->data + xt->def_foff + 0x21Cu, 2.5f); /* device.automatic_activation_radius */
				write_u16(out->data + xt->def_foff + 0x292u,
					(uint16_t)(read_u16(out->data + xt->def_foff + 0x292u) | 0x0003u));
				write_f32(out->data + xt->def_foff + 0x294u, 3.0f); /* machine.door_open_time */
				write_s32(out->data + xt->def_foff + 0x320u, 90);   /* machine.runtime_door_open_ticks */
			}
		}
		else if (xt->fourcc == 'antr')
		{
			/* Clear _animation_world_relative_bit (0x0002) on scenery/character animations (e.g. flood_captain in d20)
			 * so placed objects render at their placed world position rather than teleporting to campaign cutscene coordinates. */
			int32_t anim_cnt = read_s32(out->data + xt->def_foff + 0x74u);
			uint32_t anim_addr = read_u32(out->data + xt->def_foff + 0x78u);
			float n0_tx = 0.0f, n0_ty = 0.0f, n0_tz = 0.0f;
			int m_k, a_idx;

			for (m_k = 0; m_k < xtag_count; m_k++)
			{
				if (xtags[m_k].fourcc == 'mode' && xtags[m_k].def_foff)
				{
					int32_t n_cnt = read_s32(out->data + xtags[m_k].def_foff + 0xB8u);
					uint32_t n_addr = read_u32(out->data + xtags[m_k].def_foff + 0xBCu);
					if (n_cnt > 0 && n_addr >= CE_TAG_BASE_ADDR)
					{
						uint32_t n0_foff = tag_data_offset + (n_addr - CE_TAG_BASE_ADDR);
						if (n0_foff + 156u <= out->size)
						{
							float tx = read_f32(out->data + n0_foff + 40u);
							float ty = read_f32(out->data + n0_foff + 44u);
							float tz = read_f32(out->data + n0_foff + 48u);
							if (tx > -10.0f && tx < 10.0f && ty > -10.0f && ty < 10.0f && tz > -10.0f && tz < 10.0f)
							{
								n0_tx = tx;
								n0_ty = ty;
								n0_tz = tz;
							}
						}
					}
					break;
				}
			}

			if (anim_cnt > 0 && anim_addr >= CE_TAG_BASE_ADDR)
			{
				uint32_t anim_base = tag_data_offset + (anim_addr - CE_TAG_BASE_ADDR);
				for (a_idx = 0; a_idx < anim_cnt; a_idx++)
				{
					uint32_t af = anim_base + (uint32_t)a_idx * 180u;
					uint16_t aflags;
					if (af + 180u > out->size)
						break;
					aflags = read_u16(out->data + af + 0x3Au);
					if (aflags & 0x0002u)
					{
						write_u16(out->data + af + 0x3Au, (uint16_t)(aflags & ~0x0002u));
						if (!(aflags & 0x0001u))
						{
							uint32_t trans_flags0 = read_u32(out->data + af + 0x5Cu);
							uint32_t rot_flags0 = read_u32(out->data + af + 0x64u);
							uint32_t n0_trans_off = (rot_flags0 & 1u) ? 0u : 8u;
							if (!(trans_flags0 & 1u))
							{
								int32_t def_sz = read_s32(out->data + af + 0x80u);
								uint32_t def_addr = read_u32(out->data + af + 0x88u);
								if (def_sz >= (int32_t)(n0_trans_off + 12u) && def_addr >= CE_TAG_BASE_ADDR)
								{
									uint32_t d_foff = tag_data_offset + (def_addr - CE_TAG_BASE_ADDR) + n0_trans_off;
									if (d_foff + 12u <= out->size)
									{
										write_f32(out->data + d_foff + 0u, n0_tx);
										write_f32(out->data + d_foff + 4u, n0_ty);
										write_f32(out->data + d_foff + 8u, n0_tz);
									}
								}
							}
							else
							{
								int16_t frame_cnt = read_s16(out->data + af + 0x24u);
								int16_t frame_sz = read_s16(out->data + af + 0x26u);
								int32_t fdata_sz = read_s32(out->data + af + 0x94u);
								uint32_t fdata_addr = read_u32(out->data + af + 0x9Cu);
								if (frame_cnt > 0 && frame_sz >= (int16_t)(n0_trans_off + 12u) &&
									fdata_sz > 0 && fdata_addr >= CE_TAG_BASE_ADDR)
								{
									uint32_t fb_foff = tag_data_offset + (fdata_addr - CE_TAG_BASE_ADDR);
									int f_i;
									for (f_i = 0; f_i < frame_cnt; f_i++)
									{
										uint32_t f_off = (uint32_t)f_i * (uint32_t)frame_sz + n0_trans_off;
										if (f_off + 12u <= (uint32_t)fdata_sz && fb_foff + f_off + 12u <= out->size)
										{
											write_f32(out->data + fb_foff + f_off + 0u, n0_tx);
											write_f32(out->data + fb_foff + f_off + 4u, n0_ty);
											write_f32(out->data + fb_foff + f_off + 8u, n0_tz);
										}
									}
								}
							}
						}
					}
				}
			}
		}
		else if (xt->fourcc == 'vehi')
		{
			int32_t seat_cnt = read_s32(out->data + xt->def_foff + 0x2DCu);
			uint32_t seat_addr = read_u32(out->data + xt->def_foff + 0x2E0u);
			write_u32(out->data + xt->def_foff + 0x17Cu,
				read_u32(out->data + xt->def_foff + 0x17Cu) | (1u << 29) | (1u << 4));
			if (seat_cnt > 0 && seat_addr >= CE_TAG_BASE_ADDR)
			{
				uint32_t s0_foff = tag_data_offset + (seat_addr - CE_TAG_BASE_ADDR);
				if (s0_foff + 284u <= out->size)
				{
					uint32_t sflags = read_u32(out->data + s0_foff + 0x00u);
					char const *slabel = (char const *)(out->data + s0_foff + 0x04u);
					sflags &= ~0x01u; /* clear _unit_seat_invisible_bit */
					sflags |= (1u << 2) | (1u << 4) | (1u << 5); /* driver | third_person_camera | third_person_on_enter */
					write_u32(out->data + s0_foff + 0x00u, sflags);
					if (_stricmp(slabel, "W-driver") != 0 &&
						_stricmp(slabel, "G-driver") != 0 &&
						_stricmp(slabel, "B-driver") != 0 &&
						_stricmp(slabel, "S-driver") != 0 &&
						_stricmp(slabel, "R-driver") != 0 &&
						_stricmp(slabel, "scorpion-driver") != 0 &&
						_stricmp(slabel, "CT-driver") != 0)
					{
						memset(out->data + s0_foff + 0x04u, 0, 32u);
						memcpy(out->data + s0_foff + 0x04u, "W-driver", 8u);
					}
				}
			}
		}
	}

	root_baked_idx = xtags[0].baked_tag_idx;
	free(xtags);
	forge_source_map_close(&xmap);
	return root_baked_idx;
}

static int32_t out_map_clone_vehicle_tag_for_mp(
	struct forge_byte_buf *out,
	uint32_t tag_data_offset,
	uint32_t scnr_off,
	int32_t src_vehi_tag_idx,
	int32_t unique_seq)
{
	uint32_t io, daddr, new_foff;
	struct tag_schema_group const *sgrp = forge_schema_group_get('vehi');
	uint32_t root_sz = (sgrp && sgrp->definition && sgrp->definition->size > 0)
		? (uint32_t)sgrp->definition->size : 1008u;
	char neutral_name[128];

	if (!out || !out->data || src_vehi_tag_idx < 0)
		return src_vehi_tag_idx;
	io = tag_data_offset + 0x28u + (uint32_t)src_vehi_tag_idx * 32u;
	if (io + 32u > out->size)
		return src_vehi_tag_idx;
	daddr = read_u32(out->data + io + 0x14u);
	if (daddr < CE_TAG_BASE_ADDR || tag_data_offset + (daddr - CE_TAG_BASE_ADDR) + root_sz > out->size)
		return src_vehi_tag_idx;

	new_foff = buf_alloc(out, root_sz, 4);
	if (!new_foff || !out->data)
		return src_vehi_tag_idx;
	memcpy(out->data + new_foff, out->data + tag_data_offset + (daddr - CE_TAG_BASE_ADDR), root_sz);
	if (sgrp && sgrp->definition)
		out_map_clone_schema_blocks(out, tag_data_offset, sgrp->definition, new_foff, 1);

	/* Give the placed vehicle a neutral tag path that does not match any of the 6 substring checks
	 * in game_engine_variant_vehicle_type (warthog, ghost, scorpion, rwarthog, banshee, turret)
	 * so standard halo.exe spawns it unconditionally in all multiplayer game modes! */
	snprintf(neutral_name, sizeof(neutral_name), "forge\\vehicles\\placed_unit_%d", (int)unique_seq);
	return out_map_alloc_tag_instance(
		out, tag_data_offset, scnr_off,
		'vehi', 'unit', 'obje',
		neutral_name, new_foff, 0u,
		NULL, NULL);
}

static int32_t out_map_bake_non_native_object(
	struct forge_byte_buf *out,
	struct forge_source_map const *base_map,
	uint32_t tag_data_offset,
	uint32_t scnr_off,
	uint32_t orig_vtx_size,
	uint32_t orig_idx_size,
	struct forge_pending_bake_obj const *pobj)
{
	int32_t tmpl_idx = -1;
	uint32_t tmpl_grp = pobj ? pobj->group_tag : 0;
	int32_t i, p;
	uint32_t mod2_foff, nodes_foff = 0, reg_foff, perm_foff, geom_foff, parts_foff, shdrs_foff;
	int32_t mnode_count = 1;
	int32_t coll_tag_idx = -1;
	uint8_t coll_offsets_24[24];
	int32_t has_coll_offsets = 0;
	int32_t mod2_tag_idx;
	uint32_t mod2_handle = 0, mod2_name_addr = 0;
	char mod2_name[256];
	uint32_t obj_def_foff;
	uint32_t tmpl_io, tmpl_daddr;

	if (!pobj)
		return -1;
	if (pobj->ocetags_json && pobj->ocetags_json_len > 0)
	{
		return out_map_bake_ocetags_package(
			out, tag_data_offset, scnr_off, orig_vtx_size, orig_idx_size, pobj);
	}
	if (pobj->xbox_map_path[0])
	{
		return out_map_bake_xbox_tag_closure(
			out, tag_data_offset, scnr_off, orig_vtx_size, orig_idx_size, pobj);
	}
	if (pobj->part_count <= 0)
		return -1;

	/* 1. Choose a valid template object tag in base_map */
	if (pobj->group_tag == 'vehi')
	{
		if (strstr(pobj->tag_path, "wraith") || strstr(pobj->tag_path, "ghost") || strstr(pobj->tag_path, "turret"))
			tmpl_idx = src_map_find_tag_or_variant(base_map, 'vehi', "vehicles\\ghost\\ghost_mp");
		else if (strstr(pobj->tag_path, "banshee") || strstr(pobj->tag_path, "pelican") ||
				 strstr(pobj->tag_path, "dropship") || strstr(pobj->tag_path, "lifepod"))
			tmpl_idx = src_map_find_tag_or_variant(base_map, 'vehi', "vehicles\\banshee\\banshee_mp");
		else if (strstr(pobj->tag_path, "scorpion") || strstr(pobj->tag_path, "tank"))
			tmpl_idx = src_map_find_tag_or_variant(base_map, 'vehi', "vehicles\\scorpion\\scorpion_mp");
		if (tmpl_idx < 0)
			tmpl_idx = src_map_find_tag_or_variant(base_map, 'vehi', "vehicles\\warthog\\mp_warthog");
	}
	if (tmpl_idx < 0)
	{
		for (i = 0; i < base_map->tag_count; i++)
		{
			uint8_t const *inst = src_tag_instance(base_map, i);
			if (inst && read_u32(inst) == pobj->group_tag)
			{
				uint32_t daddr = read_u32(inst + 0x14u);
				if (daddr >= CE_TAG_BASE_ADDR && (daddr - CE_TAG_BASE_ADDR) + 0x17Cu <= base_map->tag_data_size)
				{
					tmpl_idx = i;
					if (read_s32(base_map->data + base_map->tag_data_offset + (daddr - CE_TAG_BASE_ADDR) + 0x7Cu) != -1)
						break;
				}
			}
		}
	}
	if (tmpl_idx < 0)
	{
		for (i = 0; i < base_map->tag_count; i++)
		{
			uint8_t const *inst = src_tag_instance(base_map, i);
			if (inst && (read_u32(inst) == 'scen' || read_u32(inst) == 'vehi' || read_u32(inst) == 'weap'))
			{
				tmpl_idx = i;
				tmpl_grp = read_u32(inst);
				break;
			}
		}
	}
	if (tmpl_idx < 0)
		return -1;

	tmpl_io = tag_data_offset + 0x28u + (uint32_t)tmpl_idx * 32u;
	tmpl_daddr = read_u32(out->data + tmpl_io + 0x14u);
	if (tmpl_daddr < CE_TAG_BASE_ADDR)
		return -1;

	/* 2. Bake collision (.fcol) if present (for scenery/weapons/objects or Wraith) */
	if (pobj->group_tag != 'vehi' || strstr(pobj->tag_path, "wraith") != NULL)
	{
		coll_tag_idx = out_map_bake_collision_from_fcol(
			out, tag_data_offset, scnr_off,
			pobj->group_tag, pobj->tag_path,
			coll_offsets_24, &mnode_count, &nodes_foff);
		if (coll_tag_idx >= 0)
			has_coll_offsets = 1;
	}

	/* If no .fcol nodes were loaded, copy the template object's model nodes into a fresh buffer (or create 1 default node) */
	if (nodes_foff == 0 || mnode_count <= 0)
	{
		int32_t tmpl_mod2_idx = read_s32(out->data + tag_data_offset + (tmpl_daddr - CE_TAG_BASE_ADDR) + 0x34u) & 0xFFFF;
		if (tmpl_mod2_idx >= 0 && tmpl_mod2_idx < base_map->tag_count)
		{
			uint32_t mio = tag_data_offset + 0x28u + (uint32_t)tmpl_mod2_idx * 32u;
			uint32_t mdaddr = read_u32(out->data + mio + 0x14u);
			if (mdaddr >= CE_TAG_BASE_ADDR && tag_data_offset + (mdaddr - CE_TAG_BASE_ADDR) + 0xE8u <= out->size)
			{
				uint32_t mfoff = tag_data_offset + (mdaddr - CE_TAG_BASE_ADDR);
				int32_t ncnt = read_s32(out->data + mfoff + 0xB8u);
				uint32_t naddr = read_u32(out->data + mfoff + 0xBCu);
				if (ncnt > 0 && ncnt <= 64 && naddr >= CE_TAG_BASE_ADDR &&
					tag_data_offset + (naddr - CE_TAG_BASE_ADDR) + (uint32_t)ncnt * 156u <= out->size)
				{
					uint32_t src_nodes_foff = tag_data_offset + (naddr - CE_TAG_BASE_ADDR);
					uint32_t cloned_nodes_foff = buf_alloc(out, (uint32_t)ncnt * 156u, 4);
					if (cloned_nodes_foff && out->data)
					{
						memcpy(out->data + cloned_nodes_foff, out->data + src_nodes_foff, (size_t)ncnt * 156u);
						mnode_count = ncnt;
						nodes_foff = cloned_nodes_foff;
					}
				}
			}
		}
		if (nodes_foff == 0)
		{
			mnode_count = 1;
			nodes_foff = buf_alloc(out, 156u, 4);
			if (!nodes_foff || !out->data)
				return -1;
			memset(out->data + nodes_foff, 0, 156u);
			snprintf((char *)(out->data + nodes_foff), 32, "frame");
			write_s16(out->data + nodes_foff + 0x20u, -1);
			write_s16(out->data + nodes_foff + 0x22u, -1);
			write_s16(out->data + nodes_foff + 0x24u, -1);
			write_f32(out->data + nodes_foff + 0x40u, 1.0f); /* default_rotation.w = 1.0f */
			write_f32(out->data + nodes_foff + 0x68u, 1.0f); /* runtime_default_inverse_matrix.scale = 1.0f */
			write_f32(out->data + nodes_foff + 0x6Cu, 1.0f); /* forward.i = 1.0f */
			write_f32(out->data + nodes_foff + 0x7Cu, 1.0f); /* left.j = 1.0f */
			write_f32(out->data + nodes_foff + 0x8Cu, 1.0f); /* up.k = 1.0f */
		}
	}

	/* 3. Synthesize a valid 'mod2' (gbxmodel, 0xE8 bytes) tag with pobj->part_count parts */
	mod2_foff = buf_alloc(out, 0xE8u, 4);
	reg_foff = buf_alloc(out, 76u, 4);
	perm_foff = buf_alloc(out, 88u, 4);
	geom_foff = buf_alloc(out, 48u, 4);
	parts_foff = buf_alloc(out, (uint32_t)pobj->part_count * 132u, 4);
	shdrs_foff = buf_alloc(out, (uint32_t)pobj->part_count * 32u, 4);
	if (!mod2_foff || !reg_foff || !perm_foff || !geom_foff || !parts_foff || !shdrs_foff || !out->data)
		return -1;

	memset(out->data + mod2_foff, 0, 0xE8u);
	memset(out->data + reg_foff, 0, 76u);
	memset(out->data + perm_foff, 0, 88u);
	memset(out->data + geom_foff, 0, 48u);
	memset(out->data + parts_foff, 0, (size_t)pobj->part_count * 132u);
	memset(out->data + shdrs_foff, 0, (size_t)pobj->part_count * 32u);

	/* Populate mod2 header (0xE8 bytes):
	 * +0x00: flags (0), +0x04: node_list_checksum (0),
	 * +0x08..+0x1B: detail_cutoff_pixels[5] (0.0f so render_model draws at all distances),
	 * +0x1C..+0x25: detail_node_counts[5],
	 * +0x30: base_map_u_scale, +0x34: base_map_v_scale, +0xAC: markers (count=0, address=0) */
	write_f32(out->data + mod2_foff + 0x08u, 0.0f);
	write_f32(out->data + mod2_foff + 0x0Cu, 0.0f);
	write_f32(out->data + mod2_foff + 0x10u, 0.0f);
	write_f32(out->data + mod2_foff + 0x14u, 0.0f);
	write_f32(out->data + mod2_foff + 0x18u, 0.0f);
	for (i = 0; i < 5; i++)
		write_s16(out->data + mod2_foff + 0x1Cu + (uint32_t)i * 2u, (int16_t)mnode_count);
	write_f32(out->data + mod2_foff + 0x30u, (pobj->u_scale > 0.0001f) ? pobj->u_scale : 1.0f);
	write_f32(out->data + mod2_foff + 0x34u, (pobj->v_scale > 0.0001f) ? pobj->v_scale : 1.0f);
	write_s32(out->data + mod2_foff + 0xB8u, mnode_count);
	write_u32(out->data + mod2_foff + 0xBCu, CE_TAG_BASE_ADDR + (nodes_foff - tag_data_offset));
	write_s32(out->data + mod2_foff + 0xC4u, 1); /* 1 region */
	write_u32(out->data + mod2_foff + 0xC8u, CE_TAG_BASE_ADDR + (reg_foff - tag_data_offset));
	write_s32(out->data + mod2_foff + 0xD0u, 1); /* 1 geometry */
	write_u32(out->data + mod2_foff + 0xD4u, CE_TAG_BASE_ADDR + (geom_foff - tag_data_offset));
	write_s32(out->data + mod2_foff + 0xDCu, pobj->part_count); /* shaders.count */
	write_u32(out->data + mod2_foff + 0xE0u, CE_TAG_BASE_ADDR + (shdrs_foff - tag_data_offset));

	/* Region ("default", 1 permutation) */
	snprintf((char *)(out->data + reg_foff), 32, "default");
	write_s32(out->data + reg_foff + 0x40u, 1);
	write_u32(out->data + reg_foff + 0x44u, CE_TAG_BASE_ADDR + (perm_foff - tag_data_offset));

	/* Permutation ("base", all 5 LODs -> geometry 0) */
	snprintf((char *)(out->data + perm_foff), 32, "base");
	for (i = 0; i < 5; i++)
		write_s16(out->data + perm_foff + 0x40u + (uint32_t)i * 2u, 0);

	/* Geometry (pobj->part_count parts) */
	write_s32(out->data + geom_foff + 0x24u, pobj->part_count);
	write_u32(out->data + geom_foff + 0x28u, CE_TAG_BASE_ADDR + (parts_foff - tag_data_offset));

	for (p = 0; p < pobj->part_count; p++)
	{
		struct forge_pending_bake_part const *pt = &pobj->parts[p];
		int32_t shdr_idx = out_map_ensure_part_shader_tag(out, tag_data_offset, scnr_off, pt->shdr_path, pt->bitm_path);
		uint32_t po_foff = parts_foff + (uint32_t)p * 132u;
		uint32_t so_foff = shdrs_foff + (uint32_t)p * 32u;

		/* Write shader reference entry (32 bytes) */
		if (shdr_idx >= 0)
		{
			uint32_t sio = tag_data_offset + 0x28u + (uint32_t)shdr_idx * 32u;
			write_u32(out->data + so_foff + 0x00u, read_u32(out->data + sio + 0x00u));
			write_u32(out->data + so_foff + 0x04u, read_u32(out->data + sio + 0x10u));
			write_u32(out->data + so_foff + 0x08u, 0u);
			write_u32(out->data + so_foff + 0x0Cu, read_u32(out->data + sio + 0x0Cu));
		}
		else
		{
			write_u32(out->data + so_foff + 0x00u, 'soso');
			write_u32(out->data + so_foff + 0x0Cu, 0xFFFFFFFFu);
		}

		/* Write custom_edition_model_part (132 bytes) matching custom_edition_model_part_verify */
		write_u32(out->data + po_foff + 0x00u, 0u);          /* flags */
		write_s16(out->data + po_foff + 0x04u, (int16_t)p);  /* shader_index */
		out->data[po_foff + 0x06u] = 0xFFu;                  /* previous_part_index = -1 */
		out->data[po_foff + 0x07u] = 0xFFu;                  /* next_part_index = -1 */
		write_s16(out->data + po_foff + 0x08u, 0);           /* centroid_primary_node_index = 0 */
		write_s16(out->data + po_foff + 0x0Au, 0);           /* centroid_secondary_node_index = 0 */
		write_f32(out->data + po_foff + 0x0Cu, 1.0f);        /* centroid_primary_node_weight = 1.0 */
		write_s16(out->data + po_foff + 0x44u, 1);           /* strip_type = TRIANGLE_BUFFER_TYPE_PRECOMPILED_STRIP (1) */
		write_s32(out->data + po_foff + 0x48u, (int32_t)(pt->idx_count >= 2u ? (pt->idx_count - 2u) : 1u)); /* triangle_count */
		write_u32(out->data + po_foff + 0x4Cu, orig_idx_size + pt->extra_idx_rel_off); /* strip_offset */
		write_u32(out->data + po_foff + 0x50u, orig_idx_size + pt->extra_idx_rel_off); /* strip_offset_again */
		write_s16(out->data + po_foff + 0x54u, 4);           /* vertex_type = VERTEX_TYPE_MODEL_UNCOMPRESSED (4) */
		write_s32(out->data + po_foff + 0x58u, (int32_t)pt->vtx_count); /* vertex_count */
		write_u32(out->data + po_foff + 0x60u, orig_vtx_size + pt->extra_vtx_rel_off); /* vertex_offset */
		write_u32(out->data + po_foff + 0x64u, orig_vtx_size + pt->extra_vtx_rel_off); /* vertex_offset_again */
	}

	snprintf(mod2_name, sizeof(mod2_name), "%s_baked_mod2", pobj->tag_path);
	mod2_tag_idx = out_map_alloc_tag_instance(
		out, tag_data_offset, scnr_off,
		'mod2', 0xFFFFFFFFu, 0xFFFFFFFFu,
		mod2_name, mod2_foff, 0u,
		&mod2_handle, &mod2_name_addr);
	if (mod2_tag_idx < 0)
		return -1;

	/* 4. Clone the template object definition and all of its nested tag_blocks/tag_data using OpenCE's official tag_schema */
	{
		struct tag_schema_group const *obj_schema = forge_schema_group_get(tmpl_grp);
		uint32_t obj_def_bytes = (obj_schema && obj_schema->definition && obj_schema->definition->size > 0)
			? (uint32_t)obj_schema->definition->size : 1280u;
		obj_def_foff = buf_alloc(out, obj_def_bytes, 4);
		if (!obj_def_foff || !out->data)
			return -1;
		memcpy(out->data + obj_def_foff, out->data + tag_data_offset + (tmpl_daddr - CE_TAG_BASE_ADDR), obj_def_bytes);

		if (obj_schema && obj_schema->definition)
		{
			out_map_clone_schema_blocks(out, tag_data_offset, obj_schema->definition, obj_def_foff, 1);
		}
	}

	/* Ensure object.type (+0x00, int16_t) matches pobj->group_tag and set _object_does_not_cast_shadow_bit (+0x02, bit 0) */
	{
		int16_t expected_obj_type = 6; /* _object_type_scenery */
		if (pobj->group_tag == 'bipd') expected_obj_type = 0;
		else if (pobj->group_tag == 'vehi') expected_obj_type = 1;
		else if (pobj->group_tag == 'weap') expected_obj_type = 2;
		else if (pobj->group_tag == 'eqip') expected_obj_type = 3;
		else if (pobj->group_tag == 'garb') expected_obj_type = 4;
		else if (pobj->group_tag == 'proj') expected_obj_type = 5;
		else if (pobj->group_tag == 'scen') expected_obj_type = 6;
		else if (pobj->group_tag == 'mach') expected_obj_type = 7;
		else if (pobj->group_tag == 'ctrl') expected_obj_type = 8;
		else if (pobj->group_tag == 'lifi') expected_obj_type = 9;
		write_s16(out->data + obj_def_foff + 0x00u, expected_obj_type);
		write_u16(out->data + obj_def_foff + 0x02u, read_u16(out->data + obj_def_foff + 0x02u) | (1u << 0));
	}
	if (pobj->bounding_radius > 0.1f)
		write_f32(out->data + obj_def_foff + 0x04u, pobj->bounding_radius);
	if (has_coll_offsets)
		memcpy(out->data + obj_def_foff + 0x08u, coll_offsets_24, 24);

	/* Bind mod2 at +0x28 */
	write_u32(out->data + obj_def_foff + 0x28u, 'mod2');
	write_u32(out->data + obj_def_foff + 0x2Cu, mod2_name_addr);
	write_u32(out->data + obj_def_foff + 0x30u, 0u);
	write_u32(out->data + obj_def_foff + 0x34u, mod2_handle);

	/* Clear animation_graph (+0x38..+0x44) on non-vehicles so template animations don't distort custom node counts */
	if (pobj->group_tag != 'vehi' || strstr(pobj->tag_path, "wraith") != NULL)
	{
		write_u32(out->data + obj_def_foff + 0x38u, 0xFFFFFFFFu);
		write_u32(out->data + obj_def_foff + 0x3Cu, 0u);
		write_u32(out->data + obj_def_foff + 0x40u, 0u);
		write_u32(out->data + obj_def_foff + 0x44u, 0xFFFFFFFFu);
	}

	/* Zero out base object tag_blocks (+0x140..+0x17B: attachments, widgets, functions, change_colors, predicted_resources)
	 * so custom objects do not inherit template attachments/widgets */
	memset(out->data + obj_def_foff + 0x140u, 0, 60u);

	if (tmpl_grp == 'vehi' || tmpl_grp == 'bipd')
	{
		/* Zero dialogue_variants (+0x2A8) */
		memset(out->data + obj_def_foff + 0x2A8u, 0, 12u);
		if (!strstr(pobj->tag_path, "warthog"))
			memset(out->data + obj_def_foff + 0x2D8u, 0, 12u);

		/* Ensure baked vehicles have a powered seat (+0x2CC) and an enterable driver seat (+0x2E4) */
		if (tmpl_grp == 'vehi')
		{
			int32_t seat_cnt = read_s32(out->data + obj_def_foff + 0x2E4u);
			uint32_t seat_addr = read_u32(out->data + obj_def_foff + 0x2E8u);
			write_u32(out->data + obj_def_foff + 0x17Cu, read_u32(out->data + obj_def_foff + 0x17Cu) | (1u << 4));
			if (read_s32(out->data + obj_def_foff + 0x2CCu) <= 0)
			{
				uint32_t ps_foff = buf_alloc(out, 68u, 4);
				if (ps_foff && out->data)
				{
					memset(out->data + ps_foff, 0, 68u);
					write_f32(out->data + ps_foff + 0x04u, 0.5f);
					write_f32(out->data + ps_foff + 0x08u, 0.5f);
					write_s32(out->data + obj_def_foff + 0x2CCu, 1);
					write_u32(out->data + obj_def_foff + 0x2D0u, CE_TAG_BASE_ADDR + (ps_foff - tag_data_offset));
					write_u32(out->data + obj_def_foff + 0x2D4u, 0u);
				}
			}
			if (seat_cnt > 0 && seat_addr >= CE_TAG_BASE_ADDR)
			{
				uint32_t seat_foff = tag_data_offset + (seat_addr - CE_TAG_BASE_ADDR);
				if (seat_foff + 284u <= out->size)
				{
					uint32_t sflags = read_u32(out->data + seat_foff + 0x00u);
					sflags &= ~(1u << 0); /* clear _unit_seat_invisible_bit */
					sflags |= (1u << 2) | (1u << 3) | (1u << 4) | (1u << 9); /* driver | gunner | third_person_camera | third_person_on_enter */
					write_u32(out->data + seat_foff + 0x00u, sflags);
					if (!strstr(pobj->tag_path, "warthog") && !strstr(pobj->tag_path, "scorpion"))
						write_s32(out->data + obj_def_foff + 0x2E4u, 1);
				}
			}
		}
	}
	else if (tmpl_grp == 'weap')
	{
		/* Zero weapon predicted_resources (+0x4E0) */
		memset(out->data + obj_def_foff + 0x4E0u, 0, 12u);
	}

	/* Bind baked coll at +0x70 if present */
	if (coll_tag_idx >= 0)
	{
		uint32_t cio = tag_data_offset + 0x28u + (uint32_t)coll_tag_idx * 32u;
		write_u32(out->data + obj_def_foff + 0x70u, 'coll');
		write_u32(out->data + obj_def_foff + 0x74u, read_u32(out->data + cio + 0x10u));
		write_u32(out->data + obj_def_foff + 0x78u, 0u);
		write_u32(out->data + obj_def_foff + 0x7Cu, read_u32(out->data + cio + 0x0Cu));
	}

	/* Apply vehicle physics overrides for baked custom/SP vehicles (Pelican, Wraith, Mongoose, etc.) */
	if (tmpl_grp == 'vehi')
	{
		uint8_t *vd = out->data + obj_def_foff;
		/* Remove template vehicle's attached child weapons (e.g. Warthog chaingun) unless Warthog */
		if (!strstr(pobj->tag_path, "warthog"))
		{
			write_s32(vd + 0x2D8u, 0);
			write_u32(vd + 0x2DCu, 0u);
		}
		if (strstr(pobj->tag_path, "banshee") || strstr(pobj->tag_path, "pelican") ||
			strstr(pobj->tag_path, "dropship") || strstr(pobj->tag_path, "lifepod") ||
			strstr(pobj->tag_path, "fighter"))
		{
			int is_heavy = (strstr(pobj->tag_path, "pelican") || strstr(pobj->tag_path, "dropship") || strstr(pobj->tag_path, "lifepod")) ? 1 : 0;
			write_s16(vd + 0x2F4u, 5); /* _vehicle_type_alien_fighter */
			write_f32(vd + 0x2F8u, is_heavy ? 0.28f : 0.25f);
			write_f32(vd + 0x2FCu, is_heavy ? -0.10f : -0.05f);
			write_f32(vd + 0x300u, 0.009f);
			write_f32(vd + 0x304u, 0.009f);
			write_f32(vd + 0x308u, 45.0f);
			write_f32(vd + 0x30Cu, -45.0f);
			write_f32(vd + 0x314u, is_heavy ? 65.0f : 90.0f);
			write_f32(vd + 0x330u, 0.14f);
			write_f32(vd + 0x334u, 0.14f);
			write_f32(vd + 0x364u, -0.1745f);
			write_u32(vd + 0x3BCu, 0xFFFFFFFFu);
			write_u32(vd + 0x3DCu, 0xFFFFFFFFu);
			write_u32(vd + 0x3ECu, 0xFFFFFFFFu);

			/* Clone the template physics ('phys') tag via OpenCE's schema and set low flight gravity_scale */
			{
				uint32_t phys_ref_handle = read_u32(vd + 0x8Cu);
				int32_t phys_src_idx = (phys_ref_handle != 0xFFFFFFFFu) ? (int32_t)(phys_ref_handle & 0xFFFFu) : -1;
				int32_t cur_tag_cnt = read_s32(out->data + tag_data_offset + 0x0Cu);
				if (phys_src_idx >= 0 && phys_src_idx < cur_tag_cnt)
				{
					uint32_t pio = tag_data_offset + 0x28u + (uint32_t)phys_src_idx * 32u;
					uint32_t phys_daddr = read_u32(out->data + pio + 0x14u);
					struct tag_schema_group const *phys_schema = forge_schema_group_get('phys');
					uint32_t phys_def_bytes = (phys_schema && phys_schema->definition && phys_schema->definition->size > 0)
						? (uint32_t)phys_schema->definition->size : 128u;
					if (phys_daddr >= CE_TAG_BASE_ADDR && tag_data_offset + (phys_daddr - CE_TAG_BASE_ADDR) + phys_def_bytes <= out->size)
					{
						uint32_t new_phys_foff = buf_alloc(out, phys_def_bytes, 4);
						if (new_phys_foff && out->data)
						{
							char phys_name[256];
							uint32_t new_phys_handle = 0xFFFFFFFFu, new_phys_naddr = 0u;
							int32_t new_phys_idx;
							memcpy(out->data + new_phys_foff, out->data + tag_data_offset + (phys_daddr - CE_TAG_BASE_ADDR), phys_def_bytes);
							if (phys_schema && phys_schema->definition)
								out_map_clone_schema_blocks(out, tag_data_offset, phys_schema->definition, new_phys_foff, 1);
							write_f32(out->data + new_phys_foff + 0x1Cu, 0.12f);  /* gravity_scale for smooth flight */
							write_f32(out->data + new_phys_foff + 0x20u, 0.002f); /* ground_friction */
							snprintf(phys_name, sizeof(phys_name), "%s_baked_phys", pobj->tag_path);
							new_phys_idx = out_map_alloc_tag_instance(
								out, tag_data_offset, scnr_off,
								'phys', 0xFFFFFFFFu, 0xFFFFFFFFu,
								phys_name, new_phys_foff, 0u,
								&new_phys_handle, &new_phys_naddr);
							if (new_phys_idx >= 0 && out->data)
							{
								vd = out->data + obj_def_foff;
								write_u32(vd + 0x80u, 'phys');
								write_u32(vd + 0x84u, new_phys_naddr);
								write_u32(vd + 0x88u, 0u);
								write_u32(vd + 0x8Cu, new_phys_handle);
							}
						}
					}
				}
			}
		}
		else if (strstr(pobj->tag_path, "wraith") || strstr(pobj->tag_path, "ghost"))
		{
			write_s16(vd + 0x2F4u, 4); /* _vehicle_type_alien_scout */
			write_f32(vd + 0x2F8u, 0.18f);
			write_f32(vd + 0x2FCu, -0.08f);
			write_f32(vd + 0x300u, 0.005f);
			write_f32(vd + 0x304u, 0.005f);
			write_f32(vd + 0x330u, 0.10f);
			write_f32(vd + 0x334u, 0.10f);
			write_u32(vd + 0x3DCu, 0xFFFFFFFFu);
			write_u32(vd + 0x3ECu, 0xFFFFFFFFu);
		}
	}

	return out_map_alloc_tag_instance(
		out, tag_data_offset, scnr_off,
		pobj->group_tag,
		(pobj->group_tag == 'vehi' || pobj->group_tag == 'bipd') ? 'unit' :
		((pobj->group_tag == 'weap' || pobj->group_tag == 'eqip') ? 'item' : 'obje'),
		(pobj->group_tag == 'vehi' || pobj->group_tag == 'bipd' ||
		 pobj->group_tag == 'weap' || pobj->group_tag == 'eqip') ? 'obje' : 0xFFFFFFFFu,
		pobj->tag_path, obj_def_foff, 0u,
		NULL, NULL);
}

static uint32_t compute_ce_map_checksum(uint8_t const *map_bytes, uint32_t map_size, uint32_t tag_data_offset, uint32_t scnr_foff)
{
	uint32_t crc = 0xFFFFFFFFu;
	uint32_t tag_data_size;
	uint32_t vtx_off, model_sz;
	int32_t bsp_count, i;
	uint32_t bsp_addr;

	if (!map_bytes || tag_data_offset + 0x28u > map_size)
		return 0;

	/* 1. Structure BSPs */
	if (scnr_foff + 0x5B0u <= map_size)
	{
		bsp_count = read_s32(map_bytes + scnr_foff + 0x5A4u);
		bsp_addr = read_u32(map_bytes + scnr_foff + 0x5A8u);
		if (bsp_count > 0 && bsp_count <= 32 && bsp_addr >= CE_TAG_BASE_ADDR)
		{
			uint32_t barr_foff = tag_data_offset + (bsp_addr - CE_TAG_BASE_ADDR);
			for (i = 0; i < bsp_count; i++)
			{
				uint32_t beo = barr_foff + (uint32_t)i * 32u;
				if (beo + 32u <= map_size)
				{
					uint32_t boff = read_u32(map_bytes + beo + 0x00u);
					uint32_t bsz = read_u32(map_bytes + beo + 0x04u);
					if (boff >= 0x800u && (uint64_t)boff + bsz <= map_size)
						crc = forge_crc32_update(crc, map_bytes + boff, bsz);
				}
			}
		}
	}

	/* 2. Model data (vertices + triangles) */
	vtx_off = read_u32(map_bytes + tag_data_offset + 0x14u);
	model_sz = read_u32(map_bytes + tag_data_offset + 0x20u);
	if (vtx_off >= 0x800u && (uint64_t)vtx_off + model_sz <= map_size)
		crc = forge_crc32_update(crc, map_bytes + vtx_off, model_sz);

	/* 3. Tag data */
	tag_data_size = read_u32(map_bytes + 0x14u);
	if ((uint64_t)tag_data_offset + tag_data_size <= map_size)
		crc = forge_crc32_update(crc, map_bytes + tag_data_offset, tag_data_size);

	return crc;
}

static int forge_mem_source_read(void *context, uint32_t offset, uint32_t size, void *buffer)
{
	struct forge_byte_buf const *buf = (struct forge_byte_buf const *)context;
	if (!buf || !buf->data || (uint64_t)offset + size > buf->size)
		return 0;
	if (size > 0)
		memcpy(buffer, buf->data + offset, size);
	return 1;
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
	struct forge_byte_buf extra_verts;
	struct forge_byte_buf extra_strips;
	struct forge_pending_bake_obj *pending_objs = NULL;
	int pending_count = 0;
	struct forge_map_blueprint_data *bp = NULL;
	char map_path[512];
	uint8_t const *scnr_inst;
	uint32_t scnr_addr, scnr_off, scnr_foff;
	uint32_t orig_vtx_off, orig_vtx_size, orig_model_size, orig_idx_off, orig_idx_size;
	uint32_t tag_data_offset;
	int32_t i, j;
	FILE *f;

	(void)ce_res_dir;
	(void)pc_res_dir;
	(void)donor_map_paths;
	(void)donor_map_count;
	(void)output_dir;

	if (!base_map_path || !output_map_name)
		return 0;

#undef _mkdir
#undef mkdir
	FORGE_MKDIR("custom_maps");

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

	orig_vtx_off = read_u32(base_map.data + base_map.tag_data_offset + 0x14u);
	orig_vtx_size = read_u32(base_map.data + base_map.tag_data_offset + 0x1Cu);
	orig_model_size = read_u32(base_map.data + base_map.tag_data_offset + 0x20u);
	orig_idx_off = orig_vtx_off + orig_vtx_size;
	orig_idx_size = (orig_model_size >= orig_vtx_size) ? (orig_model_size - orig_vtx_size) : 0u;

	buf_init(&extra_verts, 65536u);
	buf_init(&extra_strips, 32768u);
	cb_reset_deferred_media();

	/* Pre-scan non-native placed objects and collect their .ocetags or Xbox model vertices & triangle strips */
	if (edits && edits->placed_object_count > 0 && orig_vtx_off >= 0x800u && orig_idx_off <= base_map.tag_data_offset)
	{
		pending_objs = (struct forge_pending_bake_obj *)calloc(128, sizeof(struct forge_pending_bake_obj));
		if (pending_objs)
		{
			for (i = 0; i < edits->placed_object_count && pending_count < 128; i++)
			{
				struct forge_placed_object_entry const *po = &edits->placed_objects[i];
				uint32_t g = po->group_tag;
				int already = 0;

				if (!po->tag_path[0] || po->x < -90000.0f)
					continue;
				if (g == 'mach' || g == 'lifi' || g == 'ctrl')
					;
				else if (g == 'scen' || po->category == _forge_object_scenery) g = 'scen';
				else if (g == 'bipd' || po->category == _forge_object_biped) g = 'bipd';
				else if (g == 'vehi' || po->category == _forge_object_vehicle) g = 'vehi';
				else if (g == 'weap' || po->category == _forge_object_weapon) g = 'weap';
				else if (g == 'eqip' || po->category == _forge_object_equipment) g = 'eqip';
				else continue;

				for (j = 0; j < pending_count; j++)
				{
					if (pending_objs[j].group_tag == g && _stricmp(pending_objs[j].tag_path, po->tag_path) == 0)
					{
						already = 1;
						break;
					}
				}
				if (already)
					continue;

				/* 1. Try baking from a Custom Edition .ocetags package first (so .ocetags overrides any prior donor tag) */
				{
					char *oce_json = NULL;
					size_t oce_json_len = 0;
					if (cb_load_matching_ocetags_json(g, po->tag_path, &oce_json, &oce_json_len) && oce_json)
					{
						struct forge_pending_bake_obj *pobj = &pending_objs[pending_count];
						size_t t_pos = 0, t_end = 0;
						memset(pobj, 0, sizeof(*pobj));
						pobj->group_tag = g;
						snprintf(pobj->tag_path, sizeof(pobj->tag_path), "%s", po->tag_path);
						pobj->bounding_radius = 1.5f;
						pobj->u_scale = 1.0f;
						pobj->v_scale = 1.0f;
						pobj->ocetags_json = oce_json;
						pobj->ocetags_json_len = oce_json_len;
						pobj->baked_tag_idx = -1;

						if (cb_ocetags_find_key_range(oce_json, 0, oce_json_len, "tags", &t_pos) &&
							cb_ocetags_skip_value(oce_json, t_pos, oce_json_len, &t_end))
						{
							size_t p = t_pos + 1;
							uint32_t t_idx = 0;
							while (p < t_end)
							{
								size_t obj_start, obj_end;
								char gstr[64];
								while (p < t_end && oce_json[p] != '{' && oce_json[p] != ']') p++;
								if (p >= t_end || oce_json[p] != '{') break;
								obj_start = p;
								if (!cb_ocetags_skip_value(oce_json, obj_start, t_end, &obj_end)) break;
								gstr[0] = 0;
								cb_ocetags_get_str(oce_json, obj_start, obj_end, "group", gstr, sizeof(gstr));
								if (cb_ocetags_group_str_to_fourcc(gstr) == 'mod2')
								{
									size_t gp_arr = 0, gp_end = 0;
									if (cb_ocetags_find_key_range(oce_json, obj_start, obj_end, "gbxmodelGeometryParts", &gp_arr) &&
										cb_ocetags_skip_value(oce_json, gp_arr, obj_end, &gp_end))
									{
										size_t gp = gp_arr + 1;
										while (gp < gp_end && pobj->ocetags_gpart_count < 256)
										{
											size_t gp_start, gp_end_obj;
											uint32_t j_geom = 0, j_part = 0;
											uint8_t *v_raw = NULL, *i_raw = NULL;
											size_t v_bytes = 0, i_bytes = 0;
											while (gp < gp_end && oce_json[gp] != '{' && oce_json[gp] != ']') gp++;
											if (gp >= gp_end || oce_json[gp] != '{') break;
											gp_start = gp;
											if (!cb_ocetags_skip_value(oce_json, gp_start, gp_end, &gp_end_obj)) break;
											cb_ocetags_get_u32(oce_json, gp_start, gp_end_obj, "geometryIndex", &j_geom);
											cb_ocetags_get_u32(oce_json, gp_start, gp_end_obj, "partIndex", &j_part);
											v_raw = cb_ocetags_decode_base64_key(oce_json, gp_start, gp_end_obj, "vertexData", &v_bytes);
											if (!v_raw)
												v_raw = cb_ocetags_decode_base64_key(oce_json, gp_start, gp_end_obj, "vertices", &v_bytes);
											i_raw = cb_ocetags_decode_base64_key(oce_json, gp_start, gp_end_obj, "triangleData", &i_bytes);
											if (!i_raw)
												i_raw = cb_ocetags_decode_base64_key(oce_json, gp_start, gp_end_obj, "triangles", &i_bytes);
											if (v_raw && i_raw && v_bytes >= 3u * 68u && i_bytes >= 6u)
											{
												uint32_t vc = (uint32_t)(v_bytes / 68u);
												uint32_t ic = (uint32_t)(i_bytes / 2u);
												uint32_t v_rel = buf_alloc(&extra_verts, vc * 68u, 4);
												uint32_t i_rel = buf_alloc(&extra_strips, (ic + 2u) * 2u, 4);
												if (extra_verts.data && extra_strips.data)
												{
													uint32_t k;
													struct forge_pending_ocetags_gpart *ogp = &pobj->ocetags_gparts[pobj->ocetags_gpart_count++];
													memcpy(extra_verts.data + v_rel, v_raw, vc * 68u);
													for (k = 0; k < vc; k++)
														cb_sanitize_vertex_68(extra_verts.data + v_rel + k * 68u);
													memcpy(extra_strips.data + i_rel, i_raw, ic * 2u);
													while (ic > 3u && read_u16(extra_strips.data + i_rel + (ic - 1u) * 2u) >= vc)
														ic--;
													for (k = 0; k < ic; k++)
													{
														uint16_t idx_val = read_u16(extra_strips.data + i_rel + k * 2u);
														if (idx_val >= vc)
														{
															uint16_t safe_val = (k > 0) ? read_u16(extra_strips.data + i_rel + (k - 1u) * 2u) : 0u;
															if (safe_val >= vc) safe_val = 0u;
															write_u16(extra_strips.data + i_rel + k * 2u, safe_val);
														}
													}
													write_u16(extra_strips.data + i_rel + ic * 2u, 0xFFFFu);
													write_u16(extra_strips.data + i_rel + (ic + 1u) * 2u, 0xFFFFu);
													ogp->tag_idx = t_idx;
													ogp->geom_idx = j_geom;
													ogp->part_idx = j_part;
													ogp->vtx_count = vc;
													ogp->idx_count = ic;
													ogp->extra_vtx_rel_off = v_rel;
													ogp->extra_idx_rel_off = i_rel;
												}
											}
											free(v_raw);
											free(i_raw);
											gp = gp_end_obj;
										}
									}
								}
								t_idx++;
								p = obj_end;
							}
						}
						pending_count++;
						continue;
					}
				}

				if (src_map_find_tag_or_variant(&base_map, g, po->tag_path) >= 0)
					continue;

				/* 2. Otherwise locate the tag in an Xbox donor map (campaign or other MP map)
				 * and pre-scan its 'mode' geometry so the full Xbox tag closure is baked into the saved .map! */
				{
					char xbox_path[512];
					if (cb_find_xbox_donor_map_for_tag(g, po->tag_path, xbox_path, sizeof(xbox_path)))
					{
						struct forge_source_map xmap;
						if (forge_source_map_open(xbox_path, &xmap))
						{
							int32_t root_idx = src_map_find_tag_or_variant(&xmap, g, po->tag_path);
							if (root_idx >= 0)
							{
								struct forge_pending_bake_obj *pobj = &pending_objs[pending_count];
								memset(pobj, 0, sizeof(*pobj));
								pobj->group_tag = g;
								snprintf(pobj->tag_path, sizeof(pobj->tag_path), "%s", po->tag_path);
								snprintf(pobj->xbox_map_path, sizeof(pobj->xbox_map_path), "%s", xbox_path);
								pobj->bounding_radius = 1.5f;
								pobj->u_scale = 1.0f;
								pobj->v_scale = 1.0f;
								pobj->baked_tag_idx = -1;
								cb_prescan_xbox_models(&xmap, root_idx, pobj, &extra_verts, &extra_strips);
								pending_count++;
							}
							forge_source_map_close(&xmap);
						}
					}
				}
			}
		}
	}

	buf_alloc(&extra_verts, 0, 4);
	buf_alloc(&extra_strips, 0, 4);

	/* Assemble out buffer with extra model vertices and triangle strips spliced before tag_data */
	buf_init(&out, base_map.file_size + extra_verts.size + extra_strips.size + 524288u);
	if (!out.data)
	{
		buf_free(&extra_verts);
		buf_free(&extra_strips);
		free(pending_objs);
		forge_source_map_close(&base_map);
		return 0;
	}

	if (pending_count > 0 && (extra_verts.size > 0 || extra_strips.size > 0))
	{
		uint32_t pos = 0;
		uint32_t orig_model_end = orig_idx_off + orig_idx_size;
		memcpy(out.data + pos, base_map.data, orig_idx_off);
		pos += orig_idx_off;
		if (extra_verts.size > 0)
		{
			memcpy(out.data + pos, extra_verts.data, extra_verts.size);
			pos += extra_verts.size;
		}
		if (orig_idx_size > 0)
		{
			memcpy(out.data + pos, base_map.data + orig_idx_off, orig_idx_size);
			pos += orig_idx_size;
		}
		if (extra_strips.size > 0)
		{
			memcpy(out.data + pos, extra_strips.data, extra_strips.size);
			pos += extra_strips.size;
		}
		if (base_map.tag_data_offset > orig_model_end)
		{
			uint32_t pad_bytes = base_map.tag_data_offset - orig_model_end;
			memcpy(out.data + pos, base_map.data + orig_model_end, pad_bytes);
			pos += pad_bytes;
		}
		tag_data_offset = pos;
		memcpy(out.data + pos, base_map.data + base_map.tag_data_offset, base_map.file_size - base_map.tag_data_offset);
		pos += (base_map.file_size - base_map.tag_data_offset);
		out.size = pos;

		write_u32(out.data + tag_data_offset + 0x1Cu, orig_vtx_size + extra_verts.size);
		write_u32(out.data + tag_data_offset + 0x20u, orig_model_size + extra_verts.size + extra_strips.size);
	}
	else
	{
		memcpy(out.data, base_map.data, base_map.file_size);
		out.size = base_map.file_size;
		tag_data_offset = base_map.tag_data_offset;
	}

	buf_free(&extra_verts);
	buf_free(&extra_strips);
	scnr_foff = tag_data_offset + scnr_off;

	/* Relocate existing tag_instance name strings to the end of tag_data
	 * to free contiguous 32-byte tag_instance slots after base_map.tag_count, then bake each object! */
	if (pending_count > 0 || (edits && edits->placed_object_count > 0))
	{
		for (i = 0; i < base_map.tag_count; i++)
		{
			uint32_t io = tag_data_offset + 0x28u + (uint32_t)i * 32u;
			uint32_t naddr = read_u32(out.data + io + 0x10u);
			if (naddr >= CE_TAG_BASE_ADDR && tag_data_offset + (naddr - CE_TAG_BASE_ADDR) < out.size)
			{
				char const *nm = (char const *)(out.data + tag_data_offset + (naddr - CE_TAG_BASE_ADDR));
				size_t nlen = strlen(nm);
				uint32_t new_nfoff = buf_alloc(&out, (uint32_t)nlen + 1u, 1);
				if (new_nfoff && out.data)
				{
					memcpy(out.data + new_nfoff, out.data + tag_data_offset + (naddr - CE_TAG_BASE_ADDR), nlen + 1u);
					write_u32(out.data + io + 0x10u, CE_TAG_BASE_ADDR + (new_nfoff - tag_data_offset));
				}
			}
		}

		for (j = 0; j < pending_count; j++)
		{
			pending_objs[j].baked_tag_idx = out_map_bake_non_native_object(
				&out, &base_map, tag_data_offset, scnr_off,
				orig_vtx_size, orig_idx_size, &pending_objs[j]);
			free(pending_objs[j].ocetags_json);
			pending_objs[j].ocetags_json = NULL;
		}
	}

	if (edits)
	{
		/* 1. Apply edits/deletions to default scenario objects */
		for (i = 0; i < edits->default_edit_count; i++)
		{
			inplace_apply_default_object_edit(&out, tag_data_offset, scnr_foff, &edits->default_edits[i]);
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
					write_u32(out.data + scnr_foff + pl_field + 4u, CE_TAG_BASE_ADDR + (arr_foff - tag_data_offset));
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
					&out, tag_data_offset, scnr_foff, (uint32_t)offsetof(struct scenario, players), 52u);
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
					write_u32(out.data + scnr_foff + nf_field + 4u, CE_TAG_BASE_ADDR + (arr_foff - tag_data_offset));
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
					&out, tag_data_offset, scnr_foff, (uint32_t)offsetof(struct scenario, netgame_flags), 148u);
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

		/* 3. Append all placed objects (both native base_map tags and newly baked non-native tags) */
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

			if (g == 'mach')
			{
				blk_off = (uint32_t)offsetof(struct scenario, machines);
				pal_off = (uint32_t)offsetof(struct scenario, machine_palette);
				elem_sz = 64u;
			}
			else if (g == 'lifi')
			{
				blk_off = (uint32_t)offsetof(struct scenario, light_fixtures);
				pal_off = (uint32_t)offsetof(struct scenario, light_fixtures_palette);
				elem_sz = 88u;
			}
			else if (g == 'ctrl')
			{
				blk_off = (uint32_t)offsetof(struct scenario, controls);
				pal_off = (uint32_t)offsetof(struct scenario, control_palette);
				elem_sz = 64u;
			}
			else if (g == 'scen' || po->category == _forge_object_scenery)
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
				tag_idx = -1;
				for (j = 0; j < pending_count; j++)
				{
					if (pending_objs[j].group_tag == g && _stricmp(pending_objs[j].tag_path, po->tag_path) == 0)
					{
						tag_idx = pending_objs[j].baked_tag_idx;
						break;
					}
				}
				if (tag_idx < 0)
					tag_idx = src_map_find_tag_or_variant(&base_map, g, po->tag_path);
				if (tag_idx >= 0)
				{
					int16_t pal;
					if (g == 'vehi')
						tag_idx = out_map_clone_vehicle_tag_for_mp(&out, tag_data_offset, scnr_off, tag_idx, i);
					pal = inplace_ensure_scenario_palette_entry(
						&out, &base_map, tag_data_offset, scnr_foff, pal_off, 48u, tag_idx);
					if (pal >= 0)
					{
						uint32_t eo = inplace_append_scenario_element(
							&out, tag_data_offset, scnr_foff, blk_off, elem_sz);
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
							{
								out.data[eo + 0x5Au] = 0x3Fu;
							}
							else if (g == 'weap')
							{
								write_s16(out.data + eo + 0x28u, 9999);
								write_s16(out.data + eo + 0x2Au, 9999);
							}
							else if (g == 'mach' || g == 'ctrl')
							{
								write_s16(out.data + eo + 0x28u, -1);
								write_s16(out.data + eo + 0x2Au, -1);
							}
							else if (g == 'lifi')
							{
								write_s16(out.data + eo + 0x28u, -1);
								write_s16(out.data + eo + 0x2Au, -1);
								write_f32(out.data + eo + 0x40u, 1.0f);
								write_f32(out.data + eo + 0x44u, 1.0f);
								write_f32(out.data + eo + 0x48u, 1.0f);
								write_f32(out.data + eo + 0x4Cu, 1.0f);
								write_f32(out.data + eo + 0x50u, 1.0f);
								write_f32(out.data + eo + 0x54u, 1.2f);
							}
						}
					}
				}
			}
		}
	}

	/* Build internal [OpenCE Forge Map] + [ForgeState] text, menu description text, and 140x114 BMP preview in memory,
	 * and embed them inside custom_maps/<output_map_name>.map (recording their offsets at 0x1E4..0x1F8 in the header)
	 * so no external .bmp or .txt files are ever created! */
	{
		uint32_t tag_data_end_foff = buf_alloc(&out, 0, 32);
		struct forge_byte_buf state_buf;
		char desc_buf[512];
		uint32_t desc_len = 0;
		uint8_t *bmp_mem = NULL;
		uint32_t bmp_size = 0;
		uint32_t state_foff = 0, state_size = 0;
		uint32_t bmp_foff = 0, desc_foff = 0;
		char custom_bmp_hint[512];
		char line[512];
		int line_len;

		/* Append deferred raw bitmap pixels and sound samples AFTER tag_data_end_foff so
		 * tag_data_size never exceeds lowest_structure_bsp_address - CE_TAG_BASE_ADDR */
		if (s_cb_raw_media.data && s_cb_raw_media.size > 0)
		{
			uint32_t media_base_foff = buf_alloc(&out, s_cb_raw_media.size, 32);
			if (media_base_foff && out.data)
			{
				int mp_i;
				memcpy(out.data + media_base_foff, s_cb_raw_media.data, s_cb_raw_media.size);
				for (mp_i = 0; mp_i < s_cb_media_patch_count; mp_i++)
				{
					uint32_t f_foff = s_cb_media_patches[mp_i].field_foff;
					if (f_foff + 4u <= out.size)
						write_u32(out.data + f_foff, media_base_foff + s_cb_media_patches[mp_i].media_rel_off);
				}
			}
		}
		buf_free(&s_cb_raw_media);
		s_cb_media_patch_count = 0;

		buf_init(&state_buf, 16384u);
		line_len = snprintf(line, sizeof(line),
			"[OpenCE Forge Map]\r\n"
			"ForgeSource=UserCreated\r\n"
			"Map Name: %s\r\n"
			"Base Map: %s\r\n"
			"Author: OpenCE Forge Creator\r\n"
			"Objects Placed: %d\r\n"
			"AI Squads: %d\r\n",
			output_map_name,
			base_map.map_name,
			edits ? (int)edits->placed_object_count : 0,
			edits ? (int)edits->ai_spawn_count : 0);
		if (line_len > 0 && state_buf.data)
		{
			uint32_t o = buf_alloc(&state_buf, (uint32_t)line_len, 1);
			if (state_buf.data) memcpy(state_buf.data + o, line, (size_t)line_len);
		}
		if (edits)
		{
			line_len = snprintf(line, sizeof(line), "[ForgeState]\r\n");
			if (line_len > 0 && state_buf.data)
			{
				uint32_t o = buf_alloc(&state_buf, (uint32_t)line_len, 1);
				if (state_buf.data) memcpy(state_buf.data + o, line, (size_t)line_len);
			}
			for (i = 0; i < edits->default_edit_count; i++)
			{
				struct forge_default_object_edit const *de = &edits->default_edits[i];
				if (!de->active)
					continue;
				line_len = snprintf(line, sizeof(line),
					"DEFAULT_EDIT|%u|%d|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%s\r\n",
					(unsigned)de->group_tag, (int)de->deleted,
					de->orig_x, de->orig_y, de->orig_z,
					de->new_x, de->new_y, de->new_z,
					de->new_yaw, de->new_pitch, de->new_roll,
					de->tag_path);
				if (line_len > 0 && state_buf.data)
				{
					uint32_t o = buf_alloc(&state_buf, (uint32_t)line_len, 1);
					if (state_buf.data) memcpy(state_buf.data + o, line, (size_t)line_len);
				}
			}
			for (i = 0; i < edits->placed_object_count; i++)
			{
				struct forge_placed_object_entry const *po = &edits->placed_objects[i];
				uint32_t g = po->group_tag;
				int baked = 0;
				if (!po->tag_path[0] || po->x < -90000.0f)
					continue;
				if (g == 'mach' || g == 'lifi' || g == 'ctrl')
					;
				else if (g == 'scen' || po->category == _forge_object_scenery) g = 'scen';
				else if (g == 'bipd' || po->category == _forge_object_biped) g = 'bipd';
				else if (g == 'vehi' || po->category == _forge_object_vehicle) g = 'vehi';
				else if (g == 'weap' || po->category == _forge_object_weapon) g = 'weap';
				else if (g == 'eqip' || po->category == _forge_object_equipment) g = 'eqip';
				if (src_map_find_tag_or_variant(&base_map, g, po->tag_path) >= 0)
					baked = 1;
				else
				{
					for (j = 0; j < pending_count; j++)
					{
						if (pending_objs[j].group_tag == g &&
							_stricmp(pending_objs[j].tag_path, po->tag_path) == 0 &&
							pending_objs[j].baked_tag_idx >= 0)
						{
							baked = 1;
							break;
						}
					}
				}
				line_len = snprintf(line, sizeof(line),
					"OBJECT|%u|%d|%d|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%d|%s\r\n",
					(unsigned)po->group_tag,
					(int)po->source_cache_id,
					baked,
					po->x, po->y, po->z,
					po->yaw, po->pitch, po->roll,
					(int)po->team_index,
					po->tag_path);
				if (line_len > 0 && state_buf.data)
				{
					uint32_t o = buf_alloc(&state_buf, (uint32_t)line_len, 1);
					if (state_buf.data) memcpy(state_buf.data + o, line, (size_t)line_len);
				}
			}
			for (i = 0; i < edits->ai_spawn_count; i++)
			{
				struct forge_ai_spawn_entry const *ai = &edits->ai_spawns[i];
				if (!ai->actor_variant_path[0])
					continue;
				line_len = snprintf(line, sizeof(line),
					"AI|%.4f|%.4f|%.4f|%.4f|%d|%s\r\n",
					ai->x, ai->y, ai->z, ai->yaw,
					(int)ai->team_designator,
					ai->actor_variant_path);
				if (line_len > 0 && state_buf.data)
				{
					uint32_t o = buf_alloc(&state_buf, (uint32_t)line_len, 1);
					if (state_buf.data) memcpy(state_buf.data + o, line, (size_t)line_len);
				}
			}
		}

		desc_len = (uint32_t)snprintf(desc_buf, sizeof(desc_buf),
			"OpenCE Forge Map: %s\r\n"
			"Base Map: %s\r\n"
			"Author: OpenCE Forge Creator\r\n"
			"Objects Placed: %d\r\n",
			output_map_name,
			base_map.map_name,
			edits ? (int)edits->placed_object_count : 0);

		/* Build 140x114 BMP blueprint in memory (using base_map's BSP wireframe + placed objects, or user custom .bmp if present) */
		snprintf(custom_bmp_hint, sizeof(custom_bmp_hint), "OpenCE Forge/custom_bmp/%s.bmp", output_map_name);
		bp = (struct forge_map_blueprint_data *)calloc(1, sizeof(*bp));
		if (bp)
		{
			forge_load_map_blueprint(base_map_path, bp);
			bmp_mem = build_blueprint_bmp_buffer(custom_bmp_hint, bp, edits, &bmp_size);
			forge_free_map_blueprint(bp);
			free(bp);
			bp = NULL;
		}

		if (state_buf.data && state_buf.size > 0)
		{
			state_size = state_buf.size;
			state_foff = buf_alloc(&out, state_size + 1u, 4);
			if (state_foff && out.data)
			{
				memcpy(out.data + state_foff, state_buf.data, state_size);
				out.data[state_foff + state_size] = 0;
			}
		}
		buf_free(&state_buf);

		if (bmp_mem && bmp_size > 0)
		{
			bmp_foff = buf_alloc(&out, bmp_size, 4);
			if (bmp_foff && out.data)
				memcpy(out.data + bmp_foff, bmp_mem, bmp_size);
			free(bmp_mem);
		}

		if (desc_len > 0)
		{
			desc_foff = buf_alloc(&out, desc_len + 1u, 4);
			if (desc_foff && out.data)
			{
				memcpy(out.data + desc_foff, desc_buf, desc_len);
				out.data[desc_foff + desc_len] = 0;
			}
		}

		/* Align file end to 32 bytes and update cache header fields */
		buf_alloc(&out, 0, 32);
		if (!out.data)
		{
			free(pending_objs);
			forge_source_map_close(&base_map);
			return 0;
		}

		write_s32(out.data + 0x04u, CACHE_VERSION_CE);
		write_u32(out.data + 0x08u, out.size);
		write_u32(out.data + 0x0Cu, 0);
		write_u32(out.data + 0x10u, tag_data_offset);
		write_u32(out.data + 0x14u, tag_data_end_foff - tag_data_offset);
		memset(out.data + 0x20u, 0, 32);
		snprintf((char *)(out.data + 0x20u), 32, "%s", output_map_name);
		write_s16(out.data + 0x60u, 1); /* _scenario_type_multiplayer */
		if (scnr_foff + 4u <= out.size)
			write_s16(out.data + scnr_foff + 0x00u, 1);

		/* Record embedded Forge state, BMP preview, and menu description offsets in header 0x1E4..0x1FC */
		write_u32(out.data + 0x1E4u, state_foff);
		write_u32(out.data + 0x1E8u, state_size);
		write_u32(out.data + 0x1ECu, bmp_foff);
		write_u32(out.data + 0x1F0u, bmp_size);
		write_u32(out.data + 0x1F4u, desc_foff);
		write_u32(out.data + 0x1F8u, desc_len);
		write_u32(out.data + 0x1FCu, 0x464C4731u); /* 'FLG1' OpenCE Forge map signature */
		write_u32(out.data + 0x7FCu, CACHE_FOOTER_SIGNATURE);

		/* Update Custom Edition CRC-32 checksum (+0x64) across BSPs + model data + tag data */
		write_u32(out.data + 0x64u, compute_ce_map_checksum(out.data, out.size, tag_data_offset, scnr_foff));

		/* Verify baked Custom Edition cache container via OpenCE's built-in cache_file_identify */
		{
			struct cache_file_source mem_src;
			struct cache_file_identity verify_id;
			mem_src.context = &out;
			mem_src.read = forge_mem_source_read;
			mem_src.size = out.size;
			memset(&verify_id, 0, sizeof(verify_id));
			if (cache_file_identify(&mem_src, &verify_id) != _cache_file_status_ok)
			{
				free(pending_objs);
				buf_free(&out);
				forge_source_map_close(&base_map);
				return 0;
			}
		}
	}

	free(pending_objs);

	snprintf(map_path, sizeof(map_path), "custom_maps/%s.map", output_map_name);
	/* Remove any legacy external .bmp / .txt / sidecar files for this map */
	{
		char legacy_path[512];
		snprintf(legacy_path, sizeof(legacy_path), "custom_maps/%s.bmp", output_map_name);
		remove(legacy_path);
		snprintf(legacy_path, sizeof(legacy_path), "custom_maps/%s.txt", output_map_name);
		remove(legacy_path);
		snprintf(legacy_path, sizeof(legacy_path), "forge_maps/%s/%s.map", output_map_name, output_map_name);
		remove(legacy_path);
		snprintf(legacy_path, sizeof(legacy_path), "forge_maps/%s/%s.bmp", output_map_name, output_map_name);
		remove(legacy_path);
		snprintf(legacy_path, sizeof(legacy_path), "forge_maps/%s/%s.txt", output_map_name, output_map_name);
		remove(legacy_path);
	}

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
	forge_source_map_close(&base_map);
	return 1;
}

