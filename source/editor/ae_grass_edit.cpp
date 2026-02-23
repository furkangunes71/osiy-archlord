#include "editor/ae_grass_edit.h"

#include "core/core.h"
#include "core/file_system.h"
#include "core/log.h"
#include "core/malloc.h"
#include "core/string.h"
#include "core/vector.h"

#include "public/ap_config.h"
#include "public/ap_define.h"
#include "public/ap_sector.h"

#include "client/ac_camera.h"
#include "client/ac_dat.h"
#include "client/ac_imgui.h"
#include "client/ac_render.h"
#include "client/ac_terrain.h"
#include "client/ac_texture.h"

#include "editor/ae_editor_action.h"
#include "editor/ae_transform_tool.h"

#include "vendor/cglm/cglm.h"
#include "vendor/imgui/imgui.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Defined in ae_terrain.cpp */
extern bool tool_button(
	bgfx_texture_handle_t tex,
	const bgfx_texture_info_t * info,
	float box_size,
	const char * hint,
	boolean * btn_state,
	boolean is_disabled);

#define GRASS_FILE_VERSION 2
#define GRASS_SEGMENTS_PER_SECTOR 256
#define GRASS_SEGMENT_STRIDE 16
#define GRASS_INDEX_ENTRY_SIZE 8
#define GRASS_TYPE_HEADER_SIZE 36
#define GRASS_ENTRY_SIZE 28
#define GRASS_MAX_TEXTURES 128
#define GRASS_MAX_TEMPLATES 256
#define GRASS_PICK_RADIUS_SCALE 0.6f

/* ------------------------------------------------------------------ */
/*  Data structures                                                   */
/* ------------------------------------------------------------------ */

struct grass_entry {
	uint32_t flags;
	float x, y, z;
	float rotation_y;
	float scale;
	float density;
};

struct grass_type {
	uint32_t template_id;
	uint32_t segment_x, segment_z;
	float bound_x, bound_y, bound_z;
	float bound_radius, max_height;
	struct grass_entry * entries;
	uint32_t entry_count;
	uint32_t entry_capacity;
};

struct grass_segment {
	struct grass_type * types;
	uint32_t type_count;
};

struct grass_file {
	char name[64];
	uint32_t div_index;
	uint32_t sector_x, sector_z;
	struct grass_segment segments[GRASS_SEGMENTS_PER_SECTOR];
	bool loaded;
	bool dirty;
};

struct grass_texture_def {
	uint32_t id;
	char name[128];
	bgfx_texture_handle_t tex;
	bgfx_texture_info_t info;
};

struct grass_template_def {
	uint32_t grass_id;
	uint32_t texture_id;
	uint32_t shape_type;
	float width, height;
	float u0, v0, u1, v1;
};

struct billboard_vertex {
	float x, y, z;
	float u, v;
	uint32_t abgr;
};

struct ae_grass_edit_module {
	struct ap_module_instance instance;
	struct ap_config_module * ap_config;
	struct ac_camera_module * ac_camera;
	struct ac_render_module * ac_render;
	struct ac_terrain_module * ac_terrain;
	struct ac_texture_module * ac_texture;
	struct ae_editor_action_module * ae_editor_action;
	struct ae_transform_tool_module * ae_transform_tool;
	/* State */
	bool active;
	/* File management */
	struct grass_file * files;
	uint32_t file_count;
	/* Streaming */
	vec2 last_sync_pos;
	uint32_t * visible_divs;
	uint32_t * prev_visible_divs;
	/* Template data */
	struct grass_texture_def textures[GRASS_MAX_TEXTURES];
	uint32_t texture_count;
	struct grass_template_def templates[GRASS_MAX_TEMPLATES];
	uint32_t template_count;
	bool templates_loaded;
	/* Editing */
	uint32_t paint_template_id;
	float paint_scale;
	float paint_density;
	bool show_markers;
	bool paint_mode;
	/* Selection */
	int sel_file;
	int sel_segment;
	int sel_type;
	int sel_entry;
	struct au_pos sel_pos;
	/* Billboard rendering */
	bgfx_program_handle_t bb_program;
	bgfx_vertex_layout_t bb_layout;
	bgfx_uniform_handle_t bb_sampler;
	/* Debug line rendering (fallback) */
	bgfx_program_handle_t line_program;
	bgfx_vertex_layout_t line_layout;
	/* Toolbox */
	bgfx_texture_handle_t icon;
};

/* ------------------------------------------------------------------ */
/*  Grass DAT file I/O                                                */
/* ------------------------------------------------------------------ */

static void free_segment(struct grass_segment * seg)
{
	uint32_t i;
	for (i = 0; i < seg->type_count; i++) {
		if (seg->types[i].entries)
			dealloc(seg->types[i].entries);
	}
	if (seg->types)
		dealloc(seg->types);
	seg->types = NULL;
	seg->type_count = 0;
}

static void free_file_data(struct grass_file * f)
{
	uint32_t i;
	for (i = 0; i < GRASS_SEGMENTS_PER_SECTOR; i++)
		free_segment(&f->segments[i]);
	f->loaded = false;
	f->dirty = false;
}

static bool load_grass_file(
	struct ae_grass_edit_module * mod,
	struct grass_file * f)
{
	char path[512];
	const char * clientdir;
	FILE * fp;
	uint32_t version;
	uint32_t index_table[GRASS_SEGMENTS_PER_SECTOR][2];
	uint8_t * file_data = NULL;
	long file_size;
	uint32_t i;
	if (f->loaded)
		free_file_data(f);
	clientdir = ap_config_get(mod->ap_config, "ClientDir");
	if (!clientdir)
		return false;
	make_path(path, sizeof(path), "%s/world/grass/%s",
		clientdir, f->name);
	fp = fopen(path, "rb");
	if (!fp)
		return false;
	fseek(fp, 0, SEEK_END);
	file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (file_size < (long)(4 + GRASS_SEGMENTS_PER_SECTOR *
			GRASS_INDEX_ENTRY_SIZE)) {
		fclose(fp);
		return false;
	}
	file_data = (uint8_t *)alloc(file_size);
	if (fread(file_data, 1, file_size, fp) != (size_t)file_size) {
		dealloc(file_data);
		fclose(fp);
		return false;
	}
	fclose(fp);
	memcpy(&version, file_data, 4);
	if (version != GRASS_FILE_VERSION) {
		dealloc(file_data);
		return false;
	}
	memcpy(index_table, file_data + 4,
		GRASS_SEGMENTS_PER_SECTOR * GRASS_INDEX_ENTRY_SIZE);
	memset(f->segments, 0, sizeof(f->segments));
	for (i = 0; i < GRASS_SEGMENTS_PER_SECTOR; i++) {
		uint32_t seg_offset = index_table[i][0];
		uint32_t seg_size = index_table[i][1];
		uint32_t type_count;
		uint32_t t;
		uint32_t pos;
		if (seg_size <= 4)
			continue;
		if (seg_offset + seg_size > (uint32_t)file_size)
			continue;
		memcpy(&type_count, file_data + seg_offset, 4);
		if (type_count == 0 || type_count > 100)
			continue;
		f->segments[i].type_count = type_count;
		f->segments[i].types = (struct grass_type *)alloc(
			type_count * sizeof(struct grass_type));
		memset(f->segments[i].types, 0,
			type_count * sizeof(struct grass_type));
		pos = 4;
		for (t = 0; t < type_count; t++) {
			struct grass_type * gt = &f->segments[i].types[t];
			uint32_t e;
			if (seg_offset + pos + GRASS_TYPE_HEADER_SIZE >
					(uint32_t)file_size)
				break;
			{
				uint8_t * hdr = file_data + seg_offset + pos;
				memcpy(&gt->entry_count, hdr, 4);
				memcpy(&gt->segment_x, hdr + 4, 4);
				memcpy(&gt->segment_z, hdr + 8, 4);
				memcpy(&gt->template_id, hdr + 12, 4);
				memcpy(&gt->bound_x, hdr + 16, 4);
				memcpy(&gt->bound_y, hdr + 20, 4);
				memcpy(&gt->bound_z, hdr + 24, 4);
				memcpy(&gt->bound_radius, hdr + 28, 4);
				memcpy(&gt->max_height, hdr + 32, 4);
			}
			pos += GRASS_TYPE_HEADER_SIZE;
			if (gt->entry_count > 0 &&
					gt->entry_count < 100000) {
				gt->entry_capacity = gt->entry_count;
				gt->entries = (struct grass_entry *)alloc(
					gt->entry_count *
					sizeof(struct grass_entry));
				for (e = 0; e < gt->entry_count; e++) {
					uint8_t * ep =
						file_data + seg_offset + pos;
					if (seg_offset + pos +
							GRASS_ENTRY_SIZE >
							(uint32_t)file_size)
						break;
					memcpy(&gt->entries[e].flags, ep, 4);
					memcpy(&gt->entries[e].x, ep + 4, 4);
					memcpy(&gt->entries[e].y, ep + 8, 4);
					memcpy(&gt->entries[e].z, ep + 12, 4);
					memcpy(&gt->entries[e].rotation_y,
						ep + 16, 4);
					memcpy(&gt->entries[e].scale,
						ep + 20, 4);
					memcpy(&gt->entries[e].density,
						ep + 24, 4);
					pos += GRASS_ENTRY_SIZE;
				}
			} else {
				/* Skip entries we can't/won't load */
				pos += gt->entry_count * GRASS_ENTRY_SIZE;
				gt->entry_count = 0;
				gt->entries = NULL;
				gt->entry_capacity = 0;
			}
		}
	}
	dealloc(file_data);
	f->loaded = true;
	f->dirty = false;
	return true;
}

static bool save_grass_file(
	struct ae_grass_edit_module * mod,
	struct grass_file * f)
{
	char path[512];
	const char * clientdir;
	FILE * fp;
	uint32_t version = GRASS_FILE_VERSION;
	uint32_t index_table[GRASS_SEGMENTS_PER_SECTOR][2];
	uint8_t * buffer;
	uint32_t buffer_size;
	uint32_t data_offset;
	uint32_t i;
	if (!f->loaded)
		return false;
	clientdir = ap_config_get(mod->ap_config, "ClientDir");
	if (!clientdir)
		return false;
	buffer_size = 4 + GRASS_SEGMENTS_PER_SECTOR *
		GRASS_INDEX_ENTRY_SIZE;
	for (i = 0; i < GRASS_SEGMENTS_PER_SECTOR; i++) {
		struct grass_segment * seg = &f->segments[i];
		if (seg->type_count == 0) {
			buffer_size += 4;
		} else {
			uint32_t t;
			buffer_size += 4;
			for (t = 0; t < seg->type_count; t++) {
				buffer_size += GRASS_TYPE_HEADER_SIZE;
				buffer_size +=
					seg->types[t].entry_count *
					GRASS_ENTRY_SIZE;
			}
		}
	}
	buffer = (uint8_t *)alloc(buffer_size);
	memset(buffer, 0, buffer_size);
	memcpy(buffer, &version, 4);
	data_offset = 4 + GRASS_SEGMENTS_PER_SECTOR *
		GRASS_INDEX_ENTRY_SIZE;
	for (i = 0; i < GRASS_SEGMENTS_PER_SECTOR; i++) {
		struct grass_segment * seg = &f->segments[i];
		uint32_t seg_start = data_offset;
		if (seg->type_count == 0) {
			uint32_t zero = 0;
			memcpy(buffer + data_offset, &zero, 4);
			index_table[i][0] = seg_start;
			index_table[i][1] = 4;
			data_offset += 4;
		} else {
			uint32_t t;
			memcpy(buffer + data_offset,
				&seg->type_count, 4);
			data_offset += 4;
			for (t = 0; t < seg->type_count; t++) {
				struct grass_type * gt = &seg->types[t];
				uint32_t e;
				memcpy(buffer + data_offset,
					&gt->entry_count, 4);
				memcpy(buffer + data_offset + 4,
					&gt->segment_x, 4);
				memcpy(buffer + data_offset + 8,
					&gt->segment_z, 4);
				memcpy(buffer + data_offset + 12,
					&gt->template_id, 4);
				memcpy(buffer + data_offset + 16,
					&gt->bound_x, 4);
				memcpy(buffer + data_offset + 20,
					&gt->bound_y, 4);
				memcpy(buffer + data_offset + 24,
					&gt->bound_z, 4);
				memcpy(buffer + data_offset + 28,
					&gt->bound_radius, 4);
				memcpy(buffer + data_offset + 32,
					&gt->max_height, 4);
				data_offset += GRASS_TYPE_HEADER_SIZE;
				for (e = 0; e < gt->entry_count; e++) {
					struct grass_entry * ge =
						&gt->entries[e];
					memcpy(buffer + data_offset,
						&ge->flags, 4);
					memcpy(buffer + data_offset + 4,
						&ge->x, 4);
					memcpy(buffer + data_offset + 8,
						&ge->y, 4);
					memcpy(buffer + data_offset + 12,
						&ge->z, 4);
					memcpy(buffer + data_offset + 16,
						&ge->rotation_y, 4);
					memcpy(buffer + data_offset + 20,
						&ge->scale, 4);
					memcpy(buffer + data_offset + 24,
						&ge->density, 4);
					data_offset += GRASS_ENTRY_SIZE;
				}
			}
			index_table[i][0] = seg_start;
			index_table[i][1] = data_offset - seg_start;
		}
	}
	memcpy(buffer + 4, index_table,
		GRASS_SEGMENTS_PER_SECTOR * GRASS_INDEX_ENTRY_SIZE);
	make_path(path, sizeof(path), "%s/world/grass/%s",
		clientdir, f->name);
	fp = fopen(path, "wb");
	if (!fp) {
		WARN("Failed to write grass file: %s", path);
		dealloc(buffer);
		return false;
	}
	fwrite(buffer, 1, data_offset, fp);
	fclose(fp);
	dealloc(buffer);
	f->dirty = false;
	INFO("Saved grass file: %s (%u bytes)", f->name, data_offset);
	return true;
}

/* ------------------------------------------------------------------ */
/*  Grasstemplate.ini parser                                          */
/* ------------------------------------------------------------------ */

static void parse_grass_templates(struct ae_grass_edit_module * mod)
{
	char path[512];
	const char * clientdir;
	FILE * fp = NULL;
	char line[512];
	bool in_textures = false;
	bool in_grassinfo = false;
	struct grass_template_def * cur_tmpl = NULL;
	mod->texture_count = 0;
	mod->template_count = 0;
	mod->templates_loaded = false;
	/* Try plaintext INI first */
	clientdir = ap_config_get(mod->ap_config, "ClientDir");
	if (clientdir) {
		make_path(path, sizeof(path),
			"%s/ini/grasstemplate_text.ini", clientdir);
		fp = fopen(path, "r");
	}
	if (!fp) {
		/* Try Language/ini_ja path */
		fp = fopen("Language/ini_ja/grasstemplate.ini", "r");
	}
	if (!fp) {
		fp = fopen(
			"C:/Archonia-INI/Language/ini_ja/grasstemplate.ini",
			"r");
	}
	if (!fp) {
		WARN("Could not open grasstemplate.ini");
		return;
	}
	while (fgets(line, sizeof(line), fp)) {
		char * p = line;
		char * nl;
		/* Strip newline */
		nl = strchr(p, '\n');
		if (nl) *nl = '\0';
		nl = strchr(p, '\r');
		if (nl) *nl = '\0';
		/* Skip comments and empty lines */
		while (*p == ' ' || *p == '\t') p++;
		if (*p == ';' || *p == '\0')
			continue;
		/* Section headers */
		if (strncmp(p, "[Textures]", 10) == 0) {
			in_textures = true;
			in_grassinfo = false;
			continue;
		}
		if (strncmp(p, "[GRASSINFO]", 11) == 0) {
			in_textures = false;
			in_grassinfo = true;
			continue;
		}
		if (*p == '[') {
			in_textures = false;
			in_grassinfo = false;
			continue;
		}
		if (in_textures) {
			/* TEXTURE_NAME=ID:FILENAME */
			if (strncmp(p, "TEXTURE_NAME=", 13) == 0) {
				uint32_t id;
				char name[128];
				if (sscanf(p + 13, "%u:%127s",
						&id, name) == 2) {
					if (mod->texture_count <
							GRASS_MAX_TEXTURES) {
						struct grass_texture_def * td =
							&mod->textures[
							mod->texture_count++];
						td->id = id;
						strlcpy(td->name, name,
							sizeof(td->name));
						BGFX_INVALIDATE_HANDLE(
							td->tex);
						memset(&td->info, 0,
							sizeof(td->info));
					}
				}
			}
		}
		if (in_grassinfo) {
			if (strncmp(p, "GRASS_GROUP=", 12) == 0)
				continue;
			if (strncmp(p, "GRASS_ID=", 9) == 0) {
				uint32_t gid;
				if (sscanf(p + 9, "%u", &gid) == 1 &&
						mod->template_count <
						GRASS_MAX_TEMPLATES) {
					cur_tmpl = &mod->templates[
						mod->template_count++];
					memset(cur_tmpl, 0,
						sizeof(*cur_tmpl));
					cur_tmpl->grass_id = gid;
					cur_tmpl->width = 100.0f;
					cur_tmpl->height = 100.0f;
				}
				continue;
			}
			if (!cur_tmpl)
				continue;
			if (strncmp(p, "TEXTURE_ID=", 11) == 0)
				sscanf(p + 11, "%u",
					&cur_tmpl->texture_id);
			else if (strncmp(p, "SHAPE_TYPE=", 11) == 0)
				sscanf(p + 11, "%u",
					&cur_tmpl->shape_type);
			else if (strncmp(p, "GRASS_WIDTH=", 12) == 0)
				sscanf(p + 12, "%f",
					&cur_tmpl->width);
			else if (strncmp(p, "GRASS_HEIGHT=", 13) == 0)
				sscanf(p + 13, "%f",
					&cur_tmpl->height);
			else if (strncmp(p, "IMAGE_START_X=", 14) == 0) {
				float v;
				if (sscanf(p + 14, "%f", &v) == 1)
					cur_tmpl->u0 = v;
			}
			else if (strncmp(p, "IMAGE_START_Y=", 14) == 0) {
				float v;
				if (sscanf(p + 14, "%f", &v) == 1)
					cur_tmpl->v0 = v;
			}
			else if (strncmp(p, "IMAGE_WIDTH=", 12) == 0) {
				float v;
				if (sscanf(p + 12, "%f", &v) == 1)
					cur_tmpl->u1 = v;
			}
			else if (strncmp(p, "IMAGE_HEIGHT=", 13) == 0) {
				float v;
				if (sscanf(p + 13, "%f", &v) == 1)
					cur_tmpl->v1 = v;
			}
		}
	}
	fclose(fp);
	INFO("Parsed %u grass textures, %u templates.",
		mod->texture_count, mod->template_count);
	mod->templates_loaded = true;
}

static void load_grass_textures(struct ae_grass_edit_module * mod)
{
	uint32_t i;
	uint32_t loaded = 0;
	const char * clientdir =
		ap_config_get(mod->ap_config, "ClientDir");
	for (i = 0; i < mod->texture_count; i++) {
		struct grass_texture_def * td = &mod->textures[i];
		if (BGFX_HANDLE_IS_VALID(td->tex))
			continue;
		/* Try loose PNG file first (TIF pre-converted to PNG).
		 * This is preferred because DAT textures may have
		 * corrupted color channels. */
		if (clientdir) {
			char base[128];
			char path[512];
			char * dot;
			strlcpy(base, td->name, sizeof(base));
			dot = strrchr(base, '.');
			if (dot)
				*dot = '\0';
			make_path(path, sizeof(path),
				"%s/texture/etc/%s.png", clientdir,
				base);
			td->tex = ac_texture_load(mod->ac_texture,
				path, &td->info);
			if (BGFX_HANDLE_IS_VALID(td->tex)) {
				INFO("Loaded grass texture from disk: %s "
					"(%ux%u)", base,
					td->info.width, td->info.height);
			}
		}
		/* Fallback: try DAT archive */
		if (!BGFX_HANDLE_IS_VALID(td->tex)) {
			td->tex = ac_texture_load_packed(
				mod->ac_texture,
				AC_DAT_DIR_TEX_ETC, td->name, TRUE,
				&td->info);
		}
		if (!BGFX_HANDLE_IS_VALID(td->tex)) {
			td->tex = ac_texture_load_packed(
				mod->ac_texture,
				AC_DAT_DIR_TEX_WORLD, td->name,
				TRUE, &td->info);
		}
		if (BGFX_HANDLE_IS_VALID(td->tex))
			loaded++;
		else
			WARN("Failed to load grass texture: %s",
				td->name);
	}
	/* Compute normalized UVs for templates */
	for (i = 0; i < mod->template_count; i++) {
		struct grass_template_def * tmpl = &mod->templates[i];
		struct grass_texture_def * td = NULL;
		uint32_t j;
		float tw, th;
		for (j = 0; j < mod->texture_count; j++) {
			if (mod->textures[j].id == tmpl->texture_id) {
				td = &mod->textures[j];
				break;
			}
		}
		if (!td || !BGFX_HANDLE_IS_VALID(td->tex) ||
				td->info.width == 0 ||
				td->info.height == 0) {
			/* Fallback: assume 256x256 texture */
			tw = 256.0f;
			th = 256.0f;
		} else {
			tw = (float)td->info.width;
			th = (float)td->info.height;
		}
		{
			float sx = tmpl->u0;
			float sy = tmpl->v0;
			float sw = tmpl->u1;
			float sh = tmpl->v1;
			tmpl->u0 = sx / tw;
			tmpl->v0 = sy / th;
			tmpl->u1 = (sx + sw) / tw;
			tmpl->v1 = (sy + sh) / th;
		}
	}
	INFO("Loaded %u / %u grass textures.", loaded,
		mod->texture_count);
}

static const struct grass_template_def * find_template(
	struct ae_grass_edit_module * mod,
	uint32_t template_id)
{
	/* Grass DAT files store encoded template IDs:
	 *   raw_id = grass_id * 64 + sub_type
	 * Decode to get actual GRASS_ID for INI lookup. */
	uint32_t grass_id = template_id / 64;
	uint32_t i;
	for (i = 0; i < mod->template_count; i++) {
		if (mod->templates[i].grass_id == grass_id)
			return &mod->templates[i];
	}
	return NULL;
}

static const struct grass_texture_def * find_texture_for_template(
	struct ae_grass_edit_module * mod,
	const struct grass_template_def * tmpl)
{
	uint32_t i;
	for (i = 0; i < mod->texture_count; i++) {
		if (mod->textures[i].id == tmpl->texture_id)
			return &mod->textures[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/*  File scanning                                                     */
/* ------------------------------------------------------------------ */

static int compare_grass_files(const void * a, const void * b)
{
	const struct grass_file * fa = (const struct grass_file *)a;
	const struct grass_file * fb = (const struct grass_file *)b;
	return (int)fa->div_index - (int)fb->div_index;
}

static boolean scan_grass_cb(
	char * current_dir,
	size_t maxcount,
	const char * name,
	size_t size,
	void * user_data)
{
	struct ae_grass_edit_module * mod =
		(struct ae_grass_edit_module *)user_data;
	uint32_t div;
	uint32_t sx, sz;
	size_t len = strlen(name);
	if (len < 10 ||
		(name[0] != 'G' && name[0] != 'g') ||
		(name[1] != 'R' && name[1] != 'r'))
		return TRUE;
	if (sscanf(name + 2, "%u.dat", &div) != 1 &&
		sscanf(name + 2, "%u.DAT", &div) != 1)
		return TRUE;
	if (!ap_scr_from_division_index(div, &sx, &sz))
		return TRUE;
	mod->file_count++;
	mod->files = (struct grass_file *)reallocate(
		mod->files,
		mod->file_count * sizeof(*mod->files));
	{
		struct grass_file * f =
			&mod->files[mod->file_count - 1];
		memset(f, 0, sizeof(*f));
		strlcpy(f->name, name, sizeof(f->name));
		f->div_index = div;
		f->sector_x = sx;
		f->sector_z = sz;
		f->loaded = false;
		f->dirty = false;
	}
	return TRUE;
}

static void scan_grass_files(struct ae_grass_edit_module * mod)
{
	char dir[512];
	const char * clientdir =
		ap_config_get(mod->ap_config, "ClientDir");
	if (mod->files) {
		uint32_t i;
		for (i = 0; i < mod->file_count; i++) {
			if (mod->files[i].loaded)
				free_file_data(&mod->files[i]);
		}
		dealloc(mod->files);
		mod->files = NULL;
	}
	mod->file_count = 0;
	if (!clientdir)
		return;
	make_path(dir, sizeof(dir), "%s/world/grass", clientdir);
	enum_dir(dir, sizeof(dir), FALSE, scan_grass_cb, mod);
	if (mod->file_count > 1) {
		qsort(mod->files, mod->file_count,
			sizeof(*mod->files), compare_grass_files);
	}
	INFO("Found %u grass files.", mod->file_count);
}

/* ------------------------------------------------------------------ */
/*  Division lookup                                                   */
/* ------------------------------------------------------------------ */

static struct grass_file * find_file_by_div(
	struct ae_grass_edit_module * mod,
	uint32_t div_index)
{
	int lo = 0;
	int hi = (int)mod->file_count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (mod->files[mid].div_index == div_index)
			return &mod->files[mid];
		else if (mod->files[mid].div_index < div_index)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

static struct grass_file * find_file_for_position(
	struct ae_grass_edit_module * mod,
	float x, float z)
{
	uint32_t sx, sz, div;
	float pos[3] = { x, 0.0f, z };
	if (!ap_scr_pos_to_index(pos, &sx, &sz))
		return NULL;
	if (!ap_scr_div_index_from_sector_index(sx, sz, &div))
		return NULL;
	return find_file_by_div(mod, div);
}

static bool is_div_in_list(
	const uint32_t * divs,
	uint32_t div)
{
	uint32_t i;
	uint32_t count = vec_count(divs);
	for (i = 0; i < count; i++) {
		if (divs[i] == div)
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/*  Streaming (sync)                                                  */
/* ------------------------------------------------------------------ */

void ae_grass_edit_sync(
	struct ae_grass_edit_module * mod,
	const float * pos,
	boolean force)
{
	float view_distance;
	float x, z;
	uint32_t i;
	if (!force) {
		vec2 vp = { pos[0], pos[2] };
		float dd = glm_vec2_distance(mod->last_sync_pos, vp);
		if (dd < AP_SECTOR_WIDTH)
			return;
	}
	view_distance = ac_terrain_get_view_distance(mod->ac_terrain);
	vec_copy((void **)&mod->prev_visible_divs,
		mod->visible_divs);
	vec_clear(mod->visible_divs);
	for (x = pos[0] - view_distance;
		x < pos[0] + view_distance;
		x += AP_SECTOR_WIDTH) {
		for (z = pos[2] - view_distance;
			z < pos[2] + view_distance;
			z += AP_SECTOR_HEIGHT) {
			uint32_t sx, sz, div;
			float p[3] = { x, pos[1], z };
			if (!ap_scr_pos_to_index(p, &sx, &sz))
				continue;
			if (ap_scr_distance(pos, sx, sz) > view_distance)
				continue;
			if (!ap_scr_div_index_from_sector_index(
					sx, sz, &div))
				continue;
			if (!is_div_in_list(mod->visible_divs, div))
				vec_push_back(
					(void **)&mod->visible_divs,
					&div);
		}
	}
	for (i = 0; i < vec_count(mod->prev_visible_divs); i++) {
		uint32_t div = mod->prev_visible_divs[i];
		if (!is_div_in_list(mod->visible_divs, div)) {
			struct grass_file * f =
				find_file_by_div(mod, div);
			if (f && f->loaded && !f->dirty)
				free_file_data(f);
		}
	}
	for (i = 0; i < vec_count(mod->visible_divs); i++) {
		uint32_t div = mod->visible_divs[i];
		struct grass_file * f = find_file_by_div(mod, div);
		if (f && !f->loaded)
			load_grass_file(mod, f);
	}
	mod->last_sync_pos[0] = pos[0];
	mod->last_sync_pos[1] = pos[2];
}

/* ------------------------------------------------------------------ */
/*  Terrain raycast helper                                            */
/* ------------------------------------------------------------------ */

static boolean do_terrain_raycast(
	struct ae_grass_edit_module * mod,
	int mouse_x,
	int mouse_y,
	vec3 point)
{
	struct ac_camera * cam =
		ac_camera_get_main(mod->ac_camera);
	int w, h;
	vec3 dir;
	SDL_GetWindowSize(
		ac_render_get_window(mod->ac_render), &w, &h);
	if (mouse_x < 0 || mouse_y < 0 ||
			mouse_x > w || mouse_y > h)
		return FALSE;
	ac_camera_ray(cam,
		(mouse_x / (w * 0.5f)) - 1.f,
		1.f - (mouse_y / (h * 0.5f)), dir);
	return ac_terrain_raycast(mod->ac_terrain,
		cam->eye, dir, point);
}

/* ------------------------------------------------------------------ */
/*  Grass placement                                                   */
/* ------------------------------------------------------------------ */

static int find_segment_for_position(
	struct grass_file * f,
	float x, float z)
{
	float start_x = ap_scr_get_start_x(f->sector_x);
	float start_z = ap_scr_get_start_z(f->sector_z);
	int seg_lx = (int)((x - start_x) / AP_SECTOR_STEPSIZE);
	int seg_lz = (int)((z - start_z) / AP_SECTOR_STEPSIZE);
	if (seg_lx < 0 || seg_lx >= 16 ||
		seg_lz < 0 || seg_lz >= 16)
		return -1;
	return seg_lz * 16 + seg_lx;
}

static void add_grass_entry(
	struct ae_grass_edit_module * mod,
	float x, float y, float z)
{
	struct grass_file * f;
	int seg_idx;
	struct grass_segment * seg;
	struct grass_type * gt = NULL;
	struct grass_entry * ge;
	uint32_t t;
	bool found_type = false;
	f = find_file_for_position(mod, x, z);
	if (!f || !f->loaded) {
		WARN("No grass file loaded for this position.");
		return;
	}
	seg_idx = find_segment_for_position(f, x, z);
	if (seg_idx < 0) {
		WARN("Position outside sector bounds.");
		return;
	}
	seg = &f->segments[seg_idx];
	for (t = 0; t < seg->type_count; t++) {
		if (seg->types[t].template_id ==
				mod->paint_template_id) {
			gt = &seg->types[t];
			found_type = true;
			break;
		}
	}
	if (!found_type) {
		seg->type_count++;
		seg->types = (struct grass_type *)reallocate(
			seg->types,
			seg->type_count * sizeof(struct grass_type));
		gt = &seg->types[seg->type_count - 1];
		memset(gt, 0, sizeof(*gt));
		gt->template_id = mod->paint_template_id;
		gt->segment_x = f->sector_x + (seg_idx % 16);
		gt->segment_z = f->sector_z + (seg_idx / 16);
		gt->bound_x = ap_scr_get_start_x(f->sector_x) +
			(seg_idx % 16) * AP_SECTOR_STEPSIZE +
			AP_SECTOR_STEPSIZE * 0.5f;
		gt->bound_y = y;
		gt->bound_z = ap_scr_get_start_z(f->sector_z) +
			(seg_idx / 16) * AP_SECTOR_STEPSIZE +
			AP_SECTOR_STEPSIZE * 0.5f;
		gt->bound_radius = 1400.0f;
		gt->max_height = y;
		gt->entries = NULL;
		gt->entry_count = 0;
		gt->entry_capacity = 0;
	}
	if (gt->entry_count >= gt->entry_capacity) {
		gt->entry_capacity = gt->entry_capacity < 16 ?
			16 : gt->entry_capacity * 2;
		gt->entries = (struct grass_entry *)reallocate(
			gt->entries,
			gt->entry_capacity *
			sizeof(struct grass_entry));
	}
	ge = &gt->entries[gt->entry_count++];
	ge->flags = 0;
	ge->x = x;
	ge->y = y;
	ge->z = z;
	ge->rotation_y =
		((float)(rand() % 628) - 314.0f) / 100.0f;
	ge->scale = mod->paint_scale;
	ge->density = mod->paint_density;
	if (y > gt->max_height)
		gt->max_height = y;
	f->dirty = true;
}

/* ------------------------------------------------------------------ */
/*  Selection helpers                                                 */
/* ------------------------------------------------------------------ */

static void clear_selection(struct ae_grass_edit_module * mod)
{
	mod->sel_file = -1;
	mod->sel_segment = -1;
	mod->sel_type = -1;
	mod->sel_entry = -1;
}

static bool has_selection(struct ae_grass_edit_module * mod)
{
	return mod->sel_file >= 0 && mod->sel_segment >= 0 &&
		mod->sel_type >= 0 && mod->sel_entry >= 0;
}

static struct grass_entry * get_selected_entry(
	struct ae_grass_edit_module * mod)
{
	struct grass_file * f;
	struct grass_segment * seg;
	struct grass_type * gt;
	if (!has_selection(mod))
		return NULL;
	if ((uint32_t)mod->sel_file >= mod->file_count)
		return NULL;
	f = &mod->files[mod->sel_file];
	if (!f->loaded)
		return NULL;
	if (mod->sel_segment >= (int)GRASS_SEGMENTS_PER_SECTOR)
		return NULL;
	seg = &f->segments[mod->sel_segment];
	if (!seg->types ||
			(uint32_t)mod->sel_type >= seg->type_count)
		return NULL;
	gt = &seg->types[mod->sel_type];
	if (!gt->entries ||
			(uint32_t)mod->sel_entry >= gt->entry_count)
		return NULL;
	return &gt->entries[mod->sel_entry];
}

static void delete_selected_entry(struct ae_grass_edit_module * mod)
{
	struct grass_file * f;
	struct grass_segment * seg;
	struct grass_type * gt;
	if (!has_selection(mod))
		return;
	f = &mod->files[mod->sel_file];
	seg = &f->segments[mod->sel_segment];
	gt = &seg->types[mod->sel_type];
	if ((uint32_t)mod->sel_entry < gt->entry_count) {
		uint32_t last = gt->entry_count - 1;
		if ((uint32_t)mod->sel_entry < last)
			gt->entries[mod->sel_entry] =
				gt->entries[last];
		gt->entry_count--;
		f->dirty = true;
	}
	clear_selection(mod);
}

/* ------------------------------------------------------------------ */
/*  Picking (ray vs sphere)                                           */
/* ------------------------------------------------------------------ */

static bool ray_sphere_test(
	const float * origin,
	const float * dir,
	float cx, float cy, float cz,
	float radius,
	float * out_dist)
{
	float dx = origin[0] - cx;
	float dy = origin[1] - cy;
	float dz = origin[2] - cz;
	float b = dx * dir[0] + dy * dir[1] + dz * dir[2];
	float c = dx * dx + dy * dy + dz * dz - radius * radius;
	float disc = b * b - c;
	float t;
	if (disc < 0.0f)
		return false;
	t = -b - sqrtf(disc);
	if (t < 0.0f)
		t = -b + sqrtf(disc);
	if (t < 0.0f)
		return false;
	*out_dist = t;
	return true;
}

static bool pick_grass_entry(
	struct ae_grass_edit_module * mod,
	struct ac_camera * cam,
	int mouse_x, int mouse_y)
{
	int w, h;
	vec3 dir;
	float best_dist = FLT_MAX;
	int best_fi = -1, best_seg = -1, best_type = -1;
	int best_entry = -1;
	uint32_t fi;
	SDL_GetWindowSize(
		ac_render_get_window(mod->ac_render), &w, &h);
	if (mouse_x < 0 || mouse_y < 0 ||
			mouse_x > w || mouse_y > h)
		return false;
	ac_camera_ray(cam,
		(mouse_x / (w * 0.5f)) - 1.f,
		1.f - (mouse_y / (h * 0.5f)), dir);
	for (fi = 0; fi < mod->file_count; fi++) {
		struct grass_file * f = &mod->files[fi];
		uint32_t i;
		if (!f->loaded)
			continue;
		if (!is_div_in_list(mod->visible_divs, f->div_index))
			continue;
		for (i = 0; i < GRASS_SEGMENTS_PER_SECTOR; i++) {
			struct grass_segment * seg = &f->segments[i];
			uint32_t t;
			if (seg->type_count == 0 || !seg->types)
				continue;
			for (t = 0; t < seg->type_count; t++) {
				struct grass_type * gt =
					&seg->types[t];
				uint32_t e;
				float pick_radius = 50.0f;
				const struct grass_template_def *
					tmpl;
				if (gt->entry_count == 0 ||
						!gt->entries)
					continue;
				tmpl = find_template(mod,
					gt->template_id);
				if (tmpl) {
					pick_radius =
						tmpl->width *
						GRASS_PICK_RADIUS_SCALE;
				}
				for (e = 0; e < gt->entry_count;
						e++) {
					struct grass_entry * ge =
						&gt->entries[e];
					float dist;
					float r = pick_radius *
						(ge->scale / 100.0f);
					if (r < 20.0f)
						r = 20.0f;
					if (ray_sphere_test(
							cam->eye, dir,
							ge->x,
							ge->y +
							r * 0.5f,
							ge->z,
							r,
							&dist)) {
						if (dist <
								best_dist) {
							best_dist =
								dist;
							best_fi = fi;
							best_seg = i;
							best_type = t;
							best_entry =
								e;
						}
					}
				}
			}
		}
	}
	if (best_fi >= 0) {
		mod->sel_file = best_fi;
		mod->sel_segment = best_seg;
		mod->sel_type = best_type;
		mod->sel_entry = best_entry;
		return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/*  Transform tool callback                                           */
/* ------------------------------------------------------------------ */

static void grass_translate_cb(
	struct ae_grass_edit_module * mod,
	const struct au_pos * dest)
{
	struct grass_entry * ge = get_selected_entry(mod);
	if (!ge)
		return;
	ge->x = dest->x;
	ge->y = dest->y;
	ge->z = dest->z;
	mod->sel_pos = *dest;
	if (mod->sel_file >= 0 &&
			(uint32_t)mod->sel_file < mod->file_count)
		mod->files[mod->sel_file].dirty = true;
}

/* ------------------------------------------------------------------ */
/*  Billboard rendering                                               */
/* ------------------------------------------------------------------ */

struct render_batch {
	bgfx_texture_handle_t tex;
	struct billboard_vertex * verts;
	uint16_t * indices;
	uint32_t vert_count;
	uint32_t index_count;
	uint32_t vert_capacity;
	uint32_t index_capacity;
};

static void batch_ensure(
	struct render_batch * b,
	uint32_t extra_verts,
	uint32_t extra_indices)
{
	if (b->vert_count + extra_verts > b->vert_capacity) {
		b->vert_capacity = (b->vert_capacity < 256) ?
			256 : b->vert_capacity * 2;
		while (b->vert_count + extra_verts > b->vert_capacity)
			b->vert_capacity *= 2;
		b->verts = (struct billboard_vertex *)reallocate(
			b->verts,
			b->vert_capacity * sizeof(*b->verts));
	}
	if (b->index_count + extra_indices > b->index_capacity) {
		b->index_capacity = (b->index_capacity < 256) ?
			256 : b->index_capacity * 2;
		while (b->index_count + extra_indices >
				b->index_capacity)
			b->index_capacity *= 2;
		b->indices = (uint16_t *)reallocate(
			b->indices,
			b->index_capacity * sizeof(*b->indices));
	}
}

static void batch_add_quad(
	struct render_batch * b,
	float x, float y, float z,
	float hw, float h,
	float rot_y,
	float u0, float v0, float u1, float v1,
	uint32_t color,
	bool billboard,
	const float * cam_right,
	const float * cam_up)
{
	uint16_t base;
	struct billboard_vertex * v;
	float rx, rz;
	batch_ensure(b, 4, 6);
	base = (uint16_t)b->vert_count;
	v = &b->verts[b->vert_count];
	if (billboard) {
		/* Camera-facing billboard: use cam_right for width */
		float crx = cam_right[0] * hw;
		float crz = cam_right[2] * hw;
		v[0].x = x - crx; v[0].y = y;
		v[0].z = z - crz;
		v[0].u = u0; v[0].v = v1; v[0].abgr = color;
		v[1].x = x + crx; v[1].y = y;
		v[1].z = z + crz;
		v[1].u = u1; v[1].v = v1; v[1].abgr = color;
		v[2].x = x + crx; v[2].y = y + h;
		v[2].z = z + crz;
		v[2].u = u1; v[2].v = v0; v[2].abgr = color;
		v[3].x = x - crx; v[3].y = y + h;
		v[3].z = z - crz;
		v[3].u = u0; v[3].v = v0; v[3].abgr = color;
	} else {
		/* Fixed rotation quad */
		rx = cosf(rot_y) * hw;
		rz = sinf(rot_y) * hw;
		v[0].x = x - rx; v[0].y = y;
		v[0].z = z - rz;
		v[0].u = u0; v[0].v = v1; v[0].abgr = color;
		v[1].x = x + rx; v[1].y = y;
		v[1].z = z + rz;
		v[1].u = u1; v[1].v = v1; v[1].abgr = color;
		v[2].x = x + rx; v[2].y = y + h;
		v[2].z = z + rz;
		v[2].u = u1; v[2].v = v0; v[2].abgr = color;
		v[3].x = x - rx; v[3].y = y + h;
		v[3].z = z - rz;
		v[3].u = u0; v[3].v = v0; v[3].abgr = color;
	}
	b->vert_count += 4;
	b->indices[b->index_count++] = base;
	b->indices[b->index_count++] = base + 1;
	b->indices[b->index_count++] = base + 2;
	b->indices[b->index_count++] = base;
	b->indices[b->index_count++] = base + 2;
	b->indices[b->index_count++] = base + 3;
}

static void submit_batch(
	struct ae_grass_edit_module * mod,
	struct render_batch * b)
{
	bgfx_transient_vertex_buffer_t tvb;
	bgfx_transient_index_buffer_t tib;
	mat4 model;
	uint64_t state;
	if (b->vert_count == 0 || b->index_count == 0)
		return;
	if (!BGFX_HANDLE_IS_VALID(mod->bb_program))
		return;
	if (b->vert_count > 65535)
		return;
	if (bgfx_get_avail_transient_vertex_buffer(
			b->vert_count, &mod->bb_layout) <
			b->vert_count)
		return;
	if (bgfx_get_avail_transient_index_buffer(
			b->index_count, false) < b->index_count)
		return;
	bgfx_alloc_transient_vertex_buffer(
		&tvb, b->vert_count, &mod->bb_layout);
	bgfx_alloc_transient_index_buffer(
		&tib, b->index_count, false);
	memcpy(tvb.data, b->verts,
		b->vert_count * sizeof(struct billboard_vertex));
	memcpy(tib.data, b->indices,
		b->index_count * sizeof(uint16_t));
	glm_mat4_identity(model);
	bgfx_set_transform(&model, 1);
	bgfx_set_transient_vertex_buffer(0, &tvb, 0,
		b->vert_count);
	bgfx_set_transient_index_buffer(&tib, 0, b->index_count);
	bgfx_set_texture(0, mod->bb_sampler, b->tex, UINT32_MAX);
	/* Alpha test in shader (discard < 0.3), no blending needed.
	 * No backface culling so both sides are visible. */
	state = BGFX_STATE_WRITE_RGB |
		BGFX_STATE_WRITE_Z |
		BGFX_STATE_DEPTH_TEST_LESS |
		BGFX_STATE_MSAA;
	bgfx_set_state(state, 0);
	bgfx_submit(0, mod->bb_program, 0, BGFX_DISCARD_ALL);
}

/* ------------------------------------------------------------------ */
/*  Module callbacks                                                  */
/* ------------------------------------------------------------------ */

static boolean create_shaders(struct ae_grass_edit_module * mod)
{
	bgfx_shader_handle_t vsh, fsh;
	/* Billboard shader */
	if (ac_render_load_shader("ae_grass_billboard.vs", &vsh) &&
			ac_render_load_shader(
				"ae_grass_billboard.fs", &fsh)) {
		mod->bb_program = bgfx_create_program(vsh, fsh, true);
		INFO("Grass billboard shader loaded.");
	} else {
		WARN("Failed to load grass billboard shader.");
	}
	/* Line shader (fallback) */
	if (ac_render_load_shader("basic.vs", &vsh) &&
			ac_render_load_shader("basic.fs", &fsh)) {
		mod->line_program =
			bgfx_create_program(vsh, fsh, true);
		INFO("Grass fallback line shader loaded.");
	} else {
		WARN("Failed to load grass fallback line shader.");
	}
	return BGFX_HANDLE_IS_VALID(mod->bb_program) ||
		BGFX_HANDLE_IS_VALID(mod->line_program);
}

static boolean onregister(
	struct ae_grass_edit_module * mod,
	struct ap_module_registry * registry)
{
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry,
		mod->ap_config, AP_CONFIG_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry,
		mod->ac_camera, AC_CAMERA_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry,
		mod->ac_render, AC_RENDER_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry,
		mod->ac_terrain, AC_TERRAIN_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry,
		mod->ac_texture, AC_TEXTURE_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry,
		mod->ae_editor_action, AE_EDITOR_ACTION_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry,
		mod->ae_transform_tool,
		AE_TRANSFORM_TOOL_MODULE_NAME);
	return TRUE;
}

static boolean oninitialize(struct ae_grass_edit_module * mod)
{
	/* Billboard vertex layout: pos(3f) + texcoord(2f) + color(4u8) */
	bgfx_vertex_layout_begin(&mod->bb_layout,
		bgfx_get_renderer_type());
	bgfx_vertex_layout_add(&mod->bb_layout,
		BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT,
		false, false);
	bgfx_vertex_layout_add(&mod->bb_layout,
		BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT,
		false, false);
	bgfx_vertex_layout_add(&mod->bb_layout,
		BGFX_ATTRIB_COLOR0, 4, BGFX_ATTRIB_TYPE_UINT8,
		true, false);
	bgfx_vertex_layout_end(&mod->bb_layout);
	/* Line vertex layout: pos(3f) + color(4u8) */
	bgfx_vertex_layout_begin(&mod->line_layout,
		bgfx_get_renderer_type());
	bgfx_vertex_layout_add(&mod->line_layout,
		BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT,
		false, false);
	bgfx_vertex_layout_add(&mod->line_layout,
		BGFX_ATTRIB_COLOR0, 4, BGFX_ATTRIB_TYPE_UINT8,
		true, false);
	bgfx_vertex_layout_end(&mod->line_layout);
	mod->bb_sampler = bgfx_create_uniform(
		"s_texColor", BGFX_UNIFORM_TYPE_SAMPLER, 1);
	create_shaders(mod);
	if (BGFX_HANDLE_IS_VALID(mod->bb_program))
		INFO("Grass billboard program: OK");
	else
		WARN("Grass billboard program: FAILED");
	if (BGFX_HANDLE_IS_VALID(mod->line_program))
		INFO("Grass line program: OK");
	else
		WARN("Grass line program: FAILED");
	mod->icon = ac_texture_load(mod->ac_texture,
		"content/textures/grass-edit.png", NULL);
	if (!BGFX_HANDLE_IS_VALID(mod->icon))
		WARN("Failed to load grass edit icon.");
	scan_grass_files(mod);
	parse_grass_templates(mod);
	if (mod->templates_loaded)
		INFO("Grass templates: %u templates, %u textures",
			mod->template_count, mod->texture_count);
	else
		WARN("Grass templates: NOT loaded");
	load_grass_textures(mod);
	return TRUE;
}

static void onshutdown(struct ae_grass_edit_module * mod)
{
	uint32_t i;
	if (mod->files) {
		for (i = 0; i < mod->file_count; i++) {
			if (mod->files[i].loaded)
				free_file_data(&mod->files[i]);
		}
		dealloc(mod->files);
	}
	if (mod->visible_divs)
		vec_free(mod->visible_divs);
	if (mod->prev_visible_divs)
		vec_free(mod->prev_visible_divs);
	if (BGFX_HANDLE_IS_VALID(mod->bb_program))
		bgfx_destroy_program(mod->bb_program);
	if (BGFX_HANDLE_IS_VALID(mod->line_program))
		bgfx_destroy_program(mod->line_program);
	if (BGFX_HANDLE_IS_VALID(mod->bb_sampler))
		bgfx_destroy_uniform(mod->bb_sampler);
	if (BGFX_HANDLE_IS_VALID(mod->icon))
		ac_texture_release(mod->ac_texture, mod->icon);
	for (i = 0; i < mod->texture_count; i++) {
		if (BGFX_HANDLE_IS_VALID(mod->textures[i].tex))
			ac_texture_release(mod->ac_texture,
				mod->textures[i].tex);
	}
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

struct ae_grass_edit_module * ae_grass_edit_create_module()
{
	struct ae_grass_edit_module * mod =
		(struct ae_grass_edit_module *)ap_module_instance_new(
			AE_GRASS_EDIT_MODULE_NAME, sizeof(*mod),
			(ap_module_instance_register_t)onregister,
			(ap_module_instance_initialize_t)oninitialize,
			NULL,
			(ap_module_instance_shutdown_t)onshutdown);
	mod->active = false;
	mod->files = NULL;
	mod->file_count = 0;
	mod->last_sync_pos[0] = FLT_MAX;
	mod->last_sync_pos[1] = FLT_MAX;
	mod->visible_divs = (uint32_t *)vec_new(sizeof(uint32_t));
	mod->prev_visible_divs =
		(uint32_t *)vec_new(sizeof(uint32_t));
	mod->texture_count = 0;
	mod->template_count = 0;
	mod->templates_loaded = false;
	mod->paint_template_id = 276;
	mod->paint_scale = 150.0f;
	mod->paint_density = 0.9f;
	mod->show_markers = true;
	mod->paint_mode = false;
	clear_selection(mod);
	BGFX_INVALIDATE_HANDLE(mod->bb_program);
	BGFX_INVALIDATE_HANDLE(mod->line_program);
	BGFX_INVALIDATE_HANDLE(mod->bb_sampler);
	BGFX_INVALIDATE_HANDLE(mod->icon);
	return mod;
}

/* ------------------------------------------------------------------ */
/*  Debug line fallback rendering                                     */
/* ------------------------------------------------------------------ */

struct line_vertex {
	float x, y, z;
	uint32_t abgr;
};

static void submit_debug_lines(
	struct ae_grass_edit_module * mod,
	struct line_vertex * verts,
	uint32_t vert_count)
{
	bgfx_transient_vertex_buffer_t tvb;
	mat4 model;
	uint64_t state;
	if (!vert_count || !BGFX_HANDLE_IS_VALID(mod->line_program))
		return;
	if (vert_count > 65535)
		vert_count = 65535;
	if (bgfx_get_avail_transient_vertex_buffer(vert_count,
			&mod->line_layout) < vert_count)
		return;
	bgfx_alloc_transient_vertex_buffer(&tvb,
		vert_count, &mod->line_layout);
	memcpy(tvb.data, verts,
		vert_count * sizeof(struct line_vertex));
	glm_mat4_identity(model);
	bgfx_set_transform(&model, 1);
	bgfx_set_transient_vertex_buffer(0, &tvb, 0, vert_count);
	state = BGFX_STATE_WRITE_RGB |
		BGFX_STATE_WRITE_A |
		BGFX_STATE_DEPTH_TEST_LESS |
		BGFX_STATE_PT_LINES;
	bgfx_set_state(state, 0);
	bgfx_submit(0, mod->line_program, 0, BGFX_DISCARD_ALL);
}

static void add_line_marker(
	struct line_vertex ** verts,
	uint32_t * count,
	uint32_t * capacity,
	float x, float y, float z,
	float h,
	uint32_t color)
{
	if (*count + 2 > *capacity) {
		*capacity = (*capacity < 256) ? 256 : *capacity * 2;
		*verts = (struct line_vertex *)reallocate(
			*verts, *capacity * sizeof(struct line_vertex));
	}
	(*verts)[*count].x = x;
	(*verts)[*count].y = y;
	(*verts)[*count].z = z;
	(*verts)[*count].abgr = color;
	(*count)++;
	(*verts)[*count].x = x;
	(*verts)[*count].y = y + h;
	(*verts)[*count].z = z;
	(*verts)[*count].abgr = color;
	(*count)++;
}

/* ------------------------------------------------------------------ */
/*  Main render function                                              */
/* ------------------------------------------------------------------ */

void ae_grass_edit_render(
	struct ae_grass_edit_module * mod,
	struct ac_camera * cam)
{
	struct render_batch * batches = NULL;
	uint32_t batch_count = 0;
	uint32_t fi;
	vec3 cam_right, cam_up_v;
	bool is_sel;
	uint32_t i;
	bool can_billboard;
	/* Fallback line rendering */
	struct line_vertex * line_verts = NULL;
	uint32_t line_count = 0;
	uint32_t line_capacity = 0;
	if (!mod->show_markers)
		return;
	ac_camera_right(cam, cam_right);
	ac_camera_up(cam, cam_up_v);
	can_billboard = BGFX_HANDLE_IS_VALID(mod->bb_program) &&
		mod->templates_loaded && mod->texture_count > 0;
	/* Allocate batch array (one per texture) */
	if (can_billboard && mod->texture_count > 0) {
		batches = (struct render_batch *)alloc(
			mod->texture_count *
			sizeof(struct render_batch));
		memset(batches, 0,
			mod->texture_count *
			sizeof(struct render_batch));
		for (i = 0; i < mod->texture_count; i++)
			batches[i].tex = mod->textures[i].tex;
		batch_count = mod->texture_count;
	}
	for (fi = 0; fi < mod->file_count; fi++) {
		struct grass_file * f = &mod->files[fi];
		if (!f->loaded)
			continue;
		if (!is_div_in_list(mod->visible_divs, f->div_index))
			continue;
		for (i = 0; i < GRASS_SEGMENTS_PER_SECTOR; i++) {
			struct grass_segment * seg = &f->segments[i];
			uint32_t t;
			if (seg->type_count == 0 || !seg->types)
				continue;
			for (t = 0; t < seg->type_count; t++) {
				struct grass_type * gt =
					&seg->types[t];
				const struct grass_template_def * tmpl;
				const struct grass_texture_def * td;
				uint32_t batch_idx = UINT32_MAX;
				uint32_t e, bi;
				bool use_billboard = false;
				if (gt->entry_count == 0 ||
						!gt->entries)
					continue;
				tmpl = find_template(mod,
					gt->template_id);
				if (tmpl && batches) {
					td = find_texture_for_template(
						mod, tmpl);
					if (td && BGFX_HANDLE_IS_VALID(
							td->tex)) {
						for (bi = 0;
							bi < batch_count;
							bi++) {
							if (batches[bi].
								tex.idx ==
								td->tex.idx) {
								batch_idx =
									bi;
								break;
							}
						}
						if (batch_idx != UINT32_MAX)
							use_billboard = true;
					}
				}
				for (e = 0; e < gt->entry_count;
						e++) {
					struct grass_entry * ge =
						&gt->entries[e];
					float s =
						ge->scale / 100.0f;
					float h = 100.0f * s;
					uint32_t color =
						0xFFFFFFFF;
					is_sel =
						has_selection(mod) &&
						(int)fi ==
						mod->sel_file &&
						(int)i ==
						mod->sel_segment &&
						(int)t ==
						mod->sel_type &&
						(int)e ==
						mod->sel_entry;
					if (is_sel)
						color = 0xFF00FFFF;
					if (use_billboard) {
						float hw =
							tmpl->width * s *
							0.5f;
						float bh =
							tmpl->height * s;
						if (tmpl->shape_type == 1) {
							batch_add_quad(
								&batches[
								batch_idx],
								ge->x,
								ge->y,
								ge->z,
								hw, bh,
								ge->rotation_y,
								tmpl->u0,
								tmpl->v0,
								tmpl->u1,
								tmpl->v1,
								color,
								false,
								cam_right,
								cam_up_v);
							batch_add_quad(
								&batches[
								batch_idx],
								ge->x,
								ge->y,
								ge->z,
								hw, bh,
								ge->rotation_y
								+ 1.5708f,
								tmpl->u0,
								tmpl->v0,
								tmpl->u1,
								tmpl->v1,
								color,
								false,
								cam_right,
								cam_up_v);
						} else {
							batch_add_quad(
								&batches[
								batch_idx],
								ge->x,
								ge->y,
								ge->z,
								hw, bh,
								ge->rotation_y,
								tmpl->u0,
								tmpl->v0,
								tmpl->u1,
								tmpl->v1,
								color,
								true,
								cam_right,
								cam_up_v);
						}
					} else {
						/* Fallback: debug line */
						if (!is_sel)
							color = 0xFF00FF00;
						add_line_marker(
							&line_verts,
							&line_count,
							&line_capacity,
							ge->x, ge->y,
							ge->z, h,
							color);
					}
				}
			}
		}
	}
	/* Submit billboard batches */
	if (batches) {
		for (i = 0; i < batch_count; i++) {
			if (batches[i].vert_count > 0)
				submit_batch(mod, &batches[i]);
			if (batches[i].verts)
				dealloc(batches[i].verts);
			if (batches[i].indices)
				dealloc(batches[i].indices);
		}
		dealloc(batches);
	}
	/* Submit fallback debug lines */
	if (line_count > 0) {
		submit_debug_lines(mod, line_verts, line_count);
	}
	if (line_verts)
		dealloc(line_verts);
}

void ae_grass_edit_imgui(struct ae_grass_edit_module * mod)
{
	uint32_t loaded_count = 0;
	uint32_t total_entries = 0;
	uint32_t dirty_count = 0;
	uint32_t fi;
	if (!mod->active)
		return;
	/* Count stats */
	for (fi = 0; fi < mod->file_count; fi++) {
		struct grass_file * f = &mod->files[fi];
		if (f->loaded) {
			uint32_t i;
			loaded_count++;
			if (f->dirty)
				dirty_count++;
			for (i = 0; i < GRASS_SEGMENTS_PER_SECTOR; i++) {
				uint32_t t;
				if (!f->segments[i].types)
					continue;
				for (t = 0; t < f->segments[i].type_count;
						t++) {
					total_entries +=
						f->segments[i].types[t].
						entry_count;
				}
			}
		}
	}
	ImGui::SetNextWindowSize(ImVec2(320, 400),
		ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Grass Editor", &mod->active)) {
		ImGui::End();
		return;
	}
	ImGui::Text("Loaded: %u divisions, %u entries",
		loaded_count, total_entries);
	if (dirty_count > 0) {
		ImGui::SameLine();
		ImGui::TextColored(
			ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
			"(%u unsaved)", dirty_count);
	}
	if (dirty_count > 0) {
		if (ImGui::Button("Save All")) {
			for (fi = 0; fi < mod->file_count; fi++) {
				struct grass_file * f = &mod->files[fi];
				if (f->loaded && f->dirty)
					save_grass_file(mod, f);
			}
		}
	}
	ImGui::Text("Templates: %u, Textures: %u",
		mod->template_count, mod->texture_count);
	/* Diagnostic info */
	{
		uint32_t valid_tex = 0;
		uint32_t di;
		for (di = 0; di < mod->texture_count; di++) {
			if (BGFX_HANDLE_IS_VALID(
					mod->textures[di].tex))
				valid_tex++;
		}
		if (BGFX_HANDLE_IS_VALID(mod->bb_program))
			ImGui::TextColored(
				ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
				"Billboard shader: OK");
		else
			ImGui::TextColored(
				ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
				"Billboard shader: FAILED");
		ImGui::Text("Valid textures: %u / %u",
			valid_tex, mod->texture_count);
		ImGui::Text("Templates loaded: %s",
			mod->templates_loaded ? "YES" : "NO");
	}
	ImGui::Separator();
	/* Selected entry properties */
	{
		struct grass_entry * ge = get_selected_entry(mod);
		if (ge) {
			struct grass_file * f =
				&mod->files[mod->sel_file];
			struct grass_type * gt =
				&f->segments[mod->sel_segment].types[
				mod->sel_type];
			ImGui::TextColored(
				ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
				"Selected: %s [GrassID=%u]",
				f->name, gt->template_id / 64);
			bool changed = false;
			float pos[3] = { ge->x, ge->y, ge->z };
			if (ImGui::DragFloat3("Position", pos, 1.0f)) {
				ge->x = pos[0];
				ge->y = pos[1];
				ge->z = pos[2];
				mod->sel_pos.x = ge->x;
				mod->sel_pos.y = ge->y;
				mod->sel_pos.z = ge->z;
				changed = true;
			}
			if (ImGui::DragFloat("Rotation Y",
					&ge->rotation_y, 0.01f,
					-3.15f, 3.15f))
				changed = true;
			if (ImGui::DragFloat("Scale", &ge->scale,
					1.0f, 10.0f, 500.0f, "%.0f"))
				changed = true;
			if (ImGui::DragFloat("Density", &ge->density,
					0.01f, 0.5f, 1.0f, "%.2f"))
				changed = true;
			if (changed)
				f->dirty = true;
			if (ImGui::Button("Delete")) {
				ae_transform_tool_cancel_target(
					mod->ae_transform_tool);
				delete_selected_entry(mod);
			}
			ImGui::SameLine();
			if (ImGui::Button("Focus")) {
				vec3 c = { ge->x, ge->y, ge->z };
				ac_camera_focus_on(
					ac_camera_get_main(
						mod->ac_camera),
					c, 500.0f);
			}
		} else {
			ImGui::TextDisabled("No grass selected");
			ImGui::TextDisabled(
				"Click on grass to select");
		}
	}
	ImGui::Separator();
	/* Paint controls */
	ImGui::Text("Add Grass:");
	{
		/* Display and edit as GRASS_ID (0-125).
		 * Internally stored as raw encoded value:
		 * raw = grass_id * 64 + 20. */
		int gid = (int)(mod->paint_template_id / 64);
		if (ImGui::InputInt("Grass ID", &gid)) {
			if (gid >= 0 && gid <= 125)
				mod->paint_template_id =
					(uint32_t)(gid * 64 + 20);
		}
	}
	ImGui::SliderFloat("Scale##paint", &mod->paint_scale,
		10.0f, 500.0f, "%.0f");
	ImGui::SliderFloat("Density##paint", &mod->paint_density,
		0.5f, 1.0f, "%.2f");
	ImGui::Checkbox("Paint Mode", &mod->paint_mode);
	if (mod->paint_mode) {
		ImGui::TextColored(
			ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
			"Click on terrain to place grass");
	}
	ImGui::Separator();
	ImGui::Checkbox("Show Grass", &mod->show_markers);
	ImGui::End();
}

void ae_grass_edit_toolbox(struct ae_grass_edit_module * mod)
{
	ImVec2 avail_box = ImGui::GetContentRegionAvail();
	float avail = avail_box.x < avail_box.y ?
		avail_box.x : avail_box.y;
	if (BGFX_HANDLE_IS_VALID(mod->icon)) {
		boolean state = mod->active ? TRUE : FALSE;
		if (tool_button(mod->icon, NULL, avail,
				"Grass Edit", &state, FALSE)) {
			mod->active = !mod->active;
		}
		ImGui::Separator();
	}
}

void ae_grass_edit_toolbar(struct ae_grass_edit_module * mod)
{
	uint32_t loaded = 0;
	uint32_t dirty = 0;
	uint32_t fi;
	if (mod->files) {
		for (fi = 0; fi < mod->file_count; fi++) {
			if (mod->files[fi].loaded) {
				loaded++;
				if (mod->files[fi].dirty)
					dirty++;
			}
		}
	}
	if (dirty > 0)
		ImGui::Text("Grass: %u divisions (%u unsaved)",
			loaded, dirty);
	else
		ImGui::Text("Grass: %u divisions", loaded);
}

boolean ae_grass_edit_is_active(
	struct ae_grass_edit_module * mod)
{
	return mod->active ? TRUE : FALSE;
}

void ae_grass_edit_on_mdown(
	struct ae_grass_edit_module * mod,
	struct ac_camera * cam,
	int mouse_x,
	int mouse_y)
{
	vec3 hit;
	if (!mod->active)
		return;
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
		return;
	if (mod->paint_mode) {
		if (do_terrain_raycast(mod, mouse_x, mouse_y, hit))
			add_grass_entry(mod, hit[0], hit[1], hit[2]);
		return;
	}
	/* Selection mode: try to pick a grass entry */
	ae_transform_tool_complete_transform(mod->ae_transform_tool);
	ae_transform_tool_cancel_target(mod->ae_transform_tool);
	if (pick_grass_entry(mod, cam, mouse_x, mouse_y)) {
		struct grass_entry * ge = get_selected_entry(mod);
		if (ge) {
			mod->sel_pos.x = ge->x;
			mod->sel_pos.y = ge->y;
			mod->sel_pos.z = ge->z;
			ae_transform_tool_set_target(
				mod->ae_transform_tool,
				(ap_module_t)mod,
				(ae_transform_tool_translate_callback_t)
					grass_translate_cb,
				ge,
				&mod->sel_pos,
				0.0f);
		}
	} else {
		clear_selection(mod);
	}
}

boolean ae_grass_edit_on_key_down(
	struct ae_grass_edit_module * mod,
	uint32_t keycode)
{
	if (!mod->active)
		return FALSE;
	switch (keycode) {
	case SDLK_DELETE:
		if (has_selection(mod)) {
			ae_transform_tool_cancel_target(
				mod->ae_transform_tool);
			delete_selected_entry(mod);
			return TRUE;
		}
		break;
	case SDLK_KP_PERIOD: {
		struct grass_entry * ge = get_selected_entry(mod);
		if (ge) {
			vec3 c = { ge->x, ge->y, ge->z };
			ac_camera_focus_on(
				ac_camera_get_main(mod->ac_camera),
				c, 500.0f);
			return TRUE;
		}
		break;
	}
	}
	return FALSE;
}
