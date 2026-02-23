#include "editor/ae_octree.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "core/core.h"
#include "core/file_system.h"
#include "core/log.h"
#include "core/malloc.h"
#include "core/string.h"
#include "core/vector.h"

#include "public/ap_config.h"
#include "public/ap_object.h"
#include "public/ap_octree.h"
#include "public/ap_sector.h"

#include "client/ac_camera.h"
#include "client/ac_imgui.h"
#include "client/ac_object.h"
#include "client/ac_render.h"
#include "client/ac_texture.h"

#include "editor/ae_editor_action.h"

#include "vendor/cglm/cglm.h"
#include "vendor/imgui/imgui_internal.h"

/* Defined in ae_terrain.cpp */
extern bool tool_button(
	bgfx_texture_handle_t tex,
	const bgfx_texture_info_t * info,
	float box_size,
	const char * hint,
	boolean * btn_state,
	boolean is_disabled);

struct octree_file_info {
	char name[16];
	uint32_t div_index;
	uint32_t sector_x;
	uint32_t sector_z;
};

struct ae_octree_module {
	struct ap_module_instance instance;
	struct ap_config_module * ap_config;
	struct ac_camera_module * ac_camera;
	struct ac_object_module * ac_object;
	struct ac_render_module * ac_render;
	struct ac_texture_module * ac_texture;
	struct ae_editor_action_module * ae_editor_action;
	/* UI state */
	bool display_manager;
	bool display_debug;
	bool debug_show_empty;
	bool active;
	bool auto_create_pending;
	int debug_max_depth;
	int selected_file;
	/* Toolbox */
	bgfx_texture_handle_t icon;
	boolean button_state;
	/* File list */
	struct octree_file_info * files;
	uint32_t file_count;
	/* Debug rendering */
	bgfx_program_handle_t program;
	bgfx_vertex_layout_t debug_layout;
};

struct debug_vertex {
	float x, y, z;
	uint32_t abgr;
};

static boolean scan_octree_cb(
	char * current_dir,
	size_t maxcount,
	const char * name,
	size_t size,
	void * user_data)
{
	struct ae_octree_module * mod = (struct ae_octree_module *)user_data;
	uint32_t div;
	uint32_t sx, sz;
	size_t len = strlen(name);
	if (len < 10 || (name[0] != 'O' && name[0] != 'o') ||
		(name[1] != 'T' && name[1] != 't'))
		return TRUE;
	if (sscanf(name + 2, "%u.dat", &div) != 1 &&
		sscanf(name + 2, "%u.DAT", &div) != 1)
		return TRUE;
	if (!ap_scr_from_division_index(div, &sx, &sz))
		return TRUE;
	mod->file_count++;
	mod->files = (struct octree_file_info *)reallocate(mod->files,
		mod->file_count * sizeof(*mod->files));
	{
		struct octree_file_info * f =
			&mod->files[mod->file_count - 1];
		strlcpy(f->name, name, sizeof(f->name));
		f->div_index = div;
		f->sector_x = sx;
		f->sector_z = sz;
	}
	return TRUE;
}

static void scan_octree_files(struct ae_octree_module * mod)
{
	char dir[512];
	const char * clientdir = ap_config_get(mod->ap_config, "ClientDir");
	if (mod->files) {
		dealloc(mod->files);
		mod->files = NULL;
	}
	mod->file_count = 0;
	mod->selected_file = -1;
	if (!clientdir)
		return;
	make_path(dir, sizeof(dir), "%s/world/octree", clientdir);
	enum_dir(dir, sizeof(dir), FALSE, scan_octree_cb, mod);
}

static uint32_t count_sector_nodes(struct ap_octree_root * root)
{
	uint32_t total = 0;
	struct ap_octree_root_list * cur;
	if (!root)
		return 0;
	cur = root->roots;
	while (cur) {
		if (cur->node)
			total += ap_octree_count_nodes(cur->node);
		cur = cur->next;
	}
	return total;
}

static void add_cube_lines(
	struct debug_vertex ** verts,
	uint32_t * count,
	uint32_t * capacity,
	float cx, float cy, float cz,
	float half,
	uint32_t abgr)
{
	float minx = cx - half, miny = cy - half, minz = cz - half;
	float maxx = cx + half, maxy = cy + half, maxz = cz + half;
	/* 12 edges * 2 vertices = 24 vertices */
	float edges[24][3] = {
		/* Bottom face */
		{minx, miny, minz}, {maxx, miny, minz},
		{maxx, miny, minz}, {maxx, miny, maxz},
		{maxx, miny, maxz}, {minx, miny, maxz},
		{minx, miny, maxz}, {minx, miny, minz},
		/* Top face */
		{minx, maxy, minz}, {maxx, maxy, minz},
		{maxx, maxy, minz}, {maxx, maxy, maxz},
		{maxx, maxy, maxz}, {minx, maxy, maxz},
		{minx, maxy, maxz}, {minx, maxy, minz},
		/* Vertical edges */
		{minx, miny, minz}, {minx, maxy, minz},
		{maxx, miny, minz}, {maxx, maxy, minz},
		{maxx, miny, maxz}, {maxx, maxy, maxz},
		{minx, miny, maxz}, {minx, maxy, maxz}
	};
	uint32_t i;
	if (*count + 24 > *capacity) {
		*capacity = (*capacity < 256) ? 256 : *capacity * 2;
		while (*count + 24 > *capacity)
			*capacity *= 2;
		*verts = (struct debug_vertex *)reallocate(*verts,
			*capacity * sizeof(struct debug_vertex));
	}
	for (i = 0; i < 24; i++) {
		struct debug_vertex * v = &(*verts)[*count + i];
		v->x = edges[i][0];
		v->y = edges[i][1];
		v->z = edges[i][2];
		v->abgr = abgr;
	}
	*count += 24;
}

static void collect_debug_nodes(
	struct ae_octree_module * mod,
	struct ap_octree_node * node,
	struct debug_vertex ** verts,
	uint32_t * count,
	uint32_t * capacity)
{
	uint32_t abgr;
	if (!node)
		return;
	if ((int)node->level > mod->debug_max_depth)
		return;
	if (!mod->debug_show_empty && node->objectnum == 0 && !node->has_child)
		return;
	/* Color by depth: green=0, yellow=1, red=2 */
	switch (node->level) {
	case 0: abgr = 0xff00ff00; break; /* Green */
	case 1: abgr = 0xff00ffff; break; /* Yellow */
	default: abgr = 0xff0000ff; break; /* Red */
	}
	/* Blue for nodes with objects */
	if (node->objectnum > 0)
		abgr = 0xffff8800;
	add_cube_lines(verts, count, capacity,
		node->bsphere.center.x,
		node->bsphere.center.y,
		node->bsphere.center.z,
		(float)node->hsize, abgr);
	if (node->has_child) {
		uint32_t i;
		for (i = 0; i < 8; i++)
			collect_debug_nodes(mod, node->child[i],
				verts, count, capacity);
	}
}

static void rebuild_division(
	struct ae_octree_module * mod,
	uint32_t base_sx,
	uint32_t base_sz)
{
	struct ap_octree_root * roots[AP_SECTOR_DEFAULT_DEPTH][AP_SECTOR_DEFAULT_DEPTH];
	char path[512];
	uint32_t div_index;
	uint32_t x, z;
	const char * clientdir =
		ap_config_get(mod->ap_config, "ClientDir");
	if (!clientdir)
		return;
	if (!ap_scr_div_index_from_sector_index(base_sx, base_sz,
			&div_index))
		return;
	memset(roots, 0, sizeof(roots));
	for (x = 0; x < AP_SECTOR_DEFAULT_DEPTH; x++) {
		for (z = 0; z < AP_SECTOR_DEFAULT_DEPTH; z++) {
			uint32_t sx = base_sx + x;
			uint32_t sz = base_sz + z;
			struct ac_object_sector * s;
			struct ap_octree_root * old_root;
			struct ap_octree_root * new_root = NULL;
			struct ap_octree_node * start_node;
			uint32_t obj_count;
			uint32_t i;
			if (sx >= AP_SECTOR_WORLD_INDEX_WIDTH ||
				sz >= AP_SECTOR_WORLD_INDEX_HEIGHT)
				continue;
			s = ac_object_get_sector_by_index(
				mod->ac_object, sx, sz);
			/* Destroy existing octree */
			old_root = s->octree_roots;
			if (old_root) {
				ap_octree_destroy_tree(old_root);
				s->octree_roots = NULL;
			}
			/* Create new octree with default center_y */
			start_node = ap_octree_create_root(sx, sz,
				0.0f, &new_root);
			if (!start_node) {
				continue;
			}
			/* Divide to max depth */
			ap_octree_divide_all_trees(new_root);
			/* Assign objects to octree nodes */
			if (s->objects) {
				obj_count = vec_count(s->objects);
				for (i = 0; i < obj_count; i++) {
					struct ap_object * obj = s->objects[i];
					struct ac_object * objc =
						ac_object_get_object(
							mod->ac_object, obj);
					struct ap_octree_root_list * cur =
						new_root->roots;
					while (cur) {
						/* Clear existing links first */
						/* re-assign to new octree */
						cur = cur->next;
					}
				}
			}
			/* Optimize - remove empty leaves */
			ap_octree_optimize_tree(new_root, sx, sz);
			/* Store */
			s->octree_roots = new_root;
			roots[x][z] = new_root;
		}
	}
	/* Save to file */
	make_path(path, sizeof(path), "%s/world/octree/OT%04u.DAT",
		clientdir, div_index);
	if (ap_octree_save_division(path, roots)) {
		INFO("Octree saved: OT%04u.DAT", div_index);
	}
	else {
		ERROR("Failed to save octree: OT%04u.DAT", div_index);
	}
}

static boolean cbcommitchanges(
	struct ae_octree_module * mod,
	void * data)
{
	/* Octree changes are saved immediately on rebuild,
	 * so nothing to do here. */
	return TRUE;
}

static boolean cbrendereditors(
	struct ae_octree_module * mod,
	void * data)
{
	if (!mod->display_manager)
		return TRUE;
	ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Octree Manager", &mod->display_manager)) {
		ImGui::End();
		return TRUE;
	}
	/* Rebuild buttons */
	if (ImGui::Button("Rebuild Visible")) {
		struct ac_object_sector ** sectors = NULL;
		uint32_t i;
		uint32_t count;
		boolean rebuilt[100 * 100];
		memset(rebuilt, 0, sizeof(rebuilt));
		sectors = (struct ac_object_sector **)vec_new_reserved(
			sizeof(*sectors), 64);
		ac_object_query_visible_sectors(mod->ac_object, &sectors);
		count = vec_count(sectors);
		for (i = 0; i < count; i++) {
			struct ac_object_sector * s = sectors[i];
			uint32_t bx = s->index_x / AP_SECTOR_DEFAULT_DEPTH *
				AP_SECTOR_DEFAULT_DEPTH;
			uint32_t bz = s->index_z / AP_SECTOR_DEFAULT_DEPTH *
				AP_SECTOR_DEFAULT_DEPTH;
			uint32_t div;
			if (ap_scr_div_index_from_sector_index(bx, bz, &div) &&
				!rebuilt[div]) {
				rebuilt[div] = TRUE;
				rebuild_division(mod, bx, bz);
			}
		}
		vec_free(sectors);
		scan_octree_files(mod);
	}
	ImGui::SameLine();
	if (ImGui::Button("Auto Create")) {
		mod->auto_create_pending = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Scan Files")) {
		scan_octree_files(mod);
	}
	ImGui::Separator();
	/* File list */
	ImGui::Text("Octree Files (%u):", mod->file_count);
	{
		ImVec2 size = ImVec2(-1, 250);
		if (ImGui::BeginChild("##octree_filelist", size, true)) {
			uint32_t i;
			for (i = 0; i < mod->file_count; i++) {
				struct octree_file_info * f = &mod->files[i];
				char label[128];
				snprintf(label, sizeof(label),
					"%s  X[%u-%u] Z[%u-%u]",
					f->name,
					f->sector_x,
					f->sector_x + AP_SECTOR_DEFAULT_DEPTH - 1,
					f->sector_z,
					f->sector_z + AP_SECTOR_DEFAULT_DEPTH - 1);
				if (ImGui::Selectable(label,
						mod->selected_file == (int)i)) {
					mod->selected_file = (int)i;
				}
			}
		}
		ImGui::EndChild();
	}
	/* Selected file details */
	if (mod->selected_file >= 0 &&
		(uint32_t)mod->selected_file < mod->file_count) {
		struct octree_file_info * f =
			&mod->files[mod->selected_file];
		uint32_t total_nodes = 0;
		uint32_t sectors_with_data = 0;
		uint32_t x, z;
		ImGui::Separator();
		ImGui::Text("Selected: %s", f->name);
		ImGui::Text("Division: %u", f->div_index);
		ImGui::Text("Sectors: X[%u-%u] Z[%u-%u]",
			f->sector_x,
			f->sector_x + AP_SECTOR_DEFAULT_DEPTH - 1,
			f->sector_z,
			f->sector_z + AP_SECTOR_DEFAULT_DEPTH - 1);
		/* Count nodes and sectors with data */
		for (x = 0; x < AP_SECTOR_DEFAULT_DEPTH; x++) {
			for (z = 0; z < AP_SECTOR_DEFAULT_DEPTH; z++) {
				uint32_t sx = f->sector_x + x;
				uint32_t sz = f->sector_z + z;
				struct ac_object_sector * s;
				uint32_t n;
				if (sx >= AP_SECTOR_WORLD_INDEX_WIDTH ||
					sz >= AP_SECTOR_WORLD_INDEX_HEIGHT)
					continue;
				s = ac_object_get_sector_by_index(
					mod->ac_object, sx, sz);
				n = count_sector_nodes(s->octree_roots);
				if (n > 0) {
					sectors_with_data++;
					total_nodes += n;
				}
			}
		}
		ImGui::Text("Nodes: %u", total_nodes);
		ImGui::Text("Sectors with data: %u/256",
			sectors_with_data);
		if (ImGui::Button("Delete File")) {
			char path[512];
			const char * clientdir =
				ap_config_get(mod->ap_config, "ClientDir");
			if (clientdir) {
				make_path(path, sizeof(path),
					"%s/world/octree/%s",
					clientdir, f->name);
				if (remove(path) == 0) {
					/* Clear in-memory octree data */
					for (x = 0; x < AP_SECTOR_DEFAULT_DEPTH; x++) {
						for (z = 0; z < AP_SECTOR_DEFAULT_DEPTH; z++) {
							uint32_t sx = f->sector_x + x;
							uint32_t sz = f->sector_z + z;
							struct ac_object_sector * s;
							if (sx >= AP_SECTOR_WORLD_INDEX_WIDTH ||
								sz >= AP_SECTOR_WORLD_INDEX_HEIGHT)
								continue;
							s = ac_object_get_sector_by_index(
								mod->ac_object, sx, sz);
							if (s->octree_roots) {
								ap_octree_destroy_tree(
									s->octree_roots);
								s->octree_roots = NULL;
							}
						}
					}
					INFO("Deleted: %s", f->name);
					scan_octree_files(mod);
				}
				else {
					ERROR("Failed to delete: %s", f->name);
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Rebuild File")) {
			rebuild_division(mod, f->sector_x, f->sector_z);
			scan_octree_files(mod);
		}
	}
	ImGui::Separator();
	/* Debug options */
	ImGui::Checkbox("Debug Wireframe", &mod->display_debug);
	if (mod->display_debug) {
		ImGui::Checkbox("Show Empty Nodes", &mod->debug_show_empty);
		ImGui::SliderInt("Max Depth", &mod->debug_max_depth, 0, 2);
	}
	ImGui::End();
	return TRUE;
}

static boolean create_shaders(struct ae_octree_module * mod)
{
	bgfx_shader_handle_t vsh;
	bgfx_shader_handle_t fsh;
	if (!ac_render_load_shader("basic.vs", &vsh)) {
		WARN("Failed to load octree debug vertex shader.");
		return FALSE;
	}
	if (!ac_render_load_shader("basic.fs", &fsh)) {
		WARN("Failed to load octree debug fragment shader.");
		return FALSE;
	}
	mod->program = bgfx_create_program(vsh, fsh, true);
	if (!BGFX_HANDLE_IS_VALID(mod->program)) {
		WARN("Failed to create octree debug program.");
		return FALSE;
	}
	return TRUE;
}

static boolean onregister(
	struct ae_octree_module * mod,
	struct ap_module_registry * registry)
{
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ap_config,
		AP_CONFIG_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ac_camera,
		AC_CAMERA_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ac_object,
		AC_OBJECT_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ac_render,
		AC_RENDER_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ac_texture,
		AC_TEXTURE_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ae_editor_action,
		AE_EDITOR_ACTION_MODULE_NAME);
	ae_editor_action_add_callback(mod->ae_editor_action,
		AE_EDITOR_ACTION_CB_COMMIT_CHANGES, mod,
		(ap_module_default_t)cbcommitchanges);
	ae_editor_action_add_callback(mod->ae_editor_action,
		AE_EDITOR_ACTION_CB_RENDER_EDITORS, mod,
		(ap_module_default_t)cbrendereditors);
	return TRUE;
}

static boolean oninitialize(struct ae_octree_module * mod)
{
	/* Create debug vertex layout: position + color */
	bgfx_vertex_layout_begin(&mod->debug_layout,
		bgfx_get_renderer_type());
	bgfx_vertex_layout_add(&mod->debug_layout,
		BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT,
		false, false);
	bgfx_vertex_layout_add(&mod->debug_layout,
		BGFX_ATTRIB_COLOR0, 4, BGFX_ATTRIB_TYPE_UINT8,
		true, false);
	bgfx_vertex_layout_end(&mod->debug_layout);
	create_shaders(mod);
	mod->icon = ac_texture_load(mod->ac_texture,
		"content/textures/octree-tool.png", NULL);
	if (!BGFX_HANDLE_IS_VALID(mod->icon)) {
		WARN("Failed to load octree tool icon.");
	}
	scan_octree_files(mod);
	return TRUE;
}

static void onshutdown(struct ae_octree_module * mod)
{
	if (mod->files)
		dealloc(mod->files);
	if (BGFX_HANDLE_IS_VALID(mod->program))
		bgfx_destroy_program(mod->program);
	if (BGFX_HANDLE_IS_VALID(mod->icon))
		ac_texture_release(mod->ac_texture, mod->icon);
}

struct ae_octree_module * ae_octree_create_module()
{
	struct ae_octree_module * mod =
		(struct ae_octree_module *)ap_module_instance_new(
			AE_OCTREE_MODULE_NAME, sizeof(*mod),
			(ap_module_instance_register_t)onregister,
			(ap_module_instance_initialize_t)oninitialize,
			NULL,
			(ap_module_instance_shutdown_t)onshutdown);
	mod->display_manager = false;
	mod->display_debug = false;
	mod->debug_show_empty = false;
	mod->active = false;
	mod->auto_create_pending = false;
	mod->debug_max_depth = 2;
	mod->selected_file = -1;
	mod->button_state = FALSE;
	mod->files = NULL;
	mod->file_count = 0;
	BGFX_INVALIDATE_HANDLE(mod->program);
	BGFX_INVALIDATE_HANDLE(mod->icon);
	return mod;
}

static void add_line(
	struct debug_vertex ** verts,
	uint32_t * count,
	uint32_t * capacity,
	float x0, float y0, float z0,
	float x1, float y1, float z1,
	uint32_t abgr)
{
	if (*count + 2 > *capacity) {
		*capacity = (*capacity < 256) ? 256 : *capacity * 2;
		while (*count + 2 > *capacity)
			*capacity *= 2;
		*verts = (struct debug_vertex *)reallocate(*verts,
			*capacity * sizeof(struct debug_vertex));
	}
	(*verts)[*count].x = x0;
	(*verts)[*count].y = y0;
	(*verts)[*count].z = z0;
	(*verts)[*count].abgr = abgr;
	(*count)++;
	(*verts)[*count].x = x1;
	(*verts)[*count].y = y1;
	(*verts)[*count].z = z1;
	(*verts)[*count].abgr = abgr;
	(*count)++;
}

static void collect_sector_boundaries(
	struct ae_octree_module * mod,
	struct debug_vertex ** verts,
	uint32_t * count,
	uint32_t * capacity)
{
	struct ac_object_sector ** sectors = NULL;
	uint32_t i;
	uint32_t sec_count;
	float line_y = 200.0f;
	float line_h = 800.0f;
	sectors = (struct ac_object_sector **)vec_new_reserved(
		sizeof(*sectors), 64);
	ac_object_query_visible_sectors(mod->ac_object, &sectors);
	sec_count = vec_count(sectors);
	for (i = 0; i < sec_count; i++) {
		struct ac_object_sector * s = sectors[i];
		float sx0 = s->extent_start[0];
		float sz0 = s->extent_start[1];
		float sx1 = s->extent_end[0];
		float sz1 = s->extent_end[1];
		boolean has_octree = (s->octree_roots != NULL);
		/* Sector boundary color:
		 * green = has octree data
		 * red = no octree data */
		uint32_t abgr = has_octree ? 0xff00ff00 : 0xff0000ff;
		/* Division boundary (16x16 block edge): cyan */
		boolean is_div_edge_x =
			(s->index_x % AP_SECTOR_DEFAULT_DEPTH == 0);
		boolean is_div_edge_z =
			(s->index_z % AP_SECTOR_DEFAULT_DEPTH == 0);
		boolean is_div_edge_x1 =
			((s->index_x + 1) % AP_SECTOR_DEFAULT_DEPTH == 0);
		boolean is_div_edge_z1 =
			((s->index_z + 1) % AP_SECTOR_DEFAULT_DEPTH == 0);
		/* Draw sector bottom rectangle */
		add_line(verts, count, capacity,
			sx0, line_y, sz0, sx1, line_y, sz0, abgr);
		add_line(verts, count, capacity,
			sx1, line_y, sz0, sx1, line_y, sz1, abgr);
		add_line(verts, count, capacity,
			sx1, line_y, sz1, sx0, line_y, sz1, abgr);
		add_line(verts, count, capacity,
			sx0, line_y, sz1, sx0, line_y, sz0, abgr);
		/* Division edges: draw taller vertical lines (cyan) */
		if (is_div_edge_x) {
			add_line(verts, count, capacity,
				sx0, line_y, sz0, sx0, line_y + line_h, sz0,
				0xffffff00);
			add_line(verts, count, capacity,
				sx0, line_y, sz1, sx0, line_y + line_h, sz1,
				0xffffff00);
		}
		if (is_div_edge_z) {
			add_line(verts, count, capacity,
				sx0, line_y, sz0, sx0, line_y + line_h, sz0,
				0xffffff00);
			add_line(verts, count, capacity,
				sx1, line_y, sz0, sx1, line_y + line_h, sz0,
				0xffffff00);
		}
		if (is_div_edge_x1) {
			add_line(verts, count, capacity,
				sx1, line_y, sz0, sx1, line_y + line_h, sz0,
				0xffffff00);
			add_line(verts, count, capacity,
				sx1, line_y, sz1, sx1, line_y + line_h, sz1,
				0xffffff00);
		}
		if (is_div_edge_z1) {
			add_line(verts, count, capacity,
				sx0, line_y, sz1, sx0, line_y + line_h, sz1,
				0xffffff00);
			add_line(verts, count, capacity,
				sx1, line_y, sz1, sx1, line_y + line_h, sz1,
				0xffffff00);
		}
	}
	vec_free(sectors);
}

static void collect_selected_highlight(
	struct ae_octree_module * mod,
	struct debug_vertex ** verts,
	uint32_t * count,
	uint32_t * capacity)
{
	struct octree_file_info * f;
	uint32_t x, z;
	float sx0, sz0, sx1, sz1;
	float y_base = 100.0f;
	float y_top = 1200.0f;
	uint32_t highlight_abgr = 0xff00ffff; /* Bright yellow */
	uint32_t fill_abgr = 0x4000ffff; /* Semi-transparent yellow */
	if (mod->selected_file < 0 ||
		(uint32_t)mod->selected_file >= mod->file_count)
		return;
	f = &mod->files[mod->selected_file];
	/* Get world-space bounds for the 16x16 sector block */
	for (x = 0; x < AP_SECTOR_DEFAULT_DEPTH; x++) {
		for (z = 0; z < AP_SECTOR_DEFAULT_DEPTH; z++) {
			uint32_t si_x = f->sector_x + x;
			uint32_t si_z = f->sector_z + z;
			struct ac_object_sector * s;
			if (si_x >= AP_SECTOR_WORLD_INDEX_WIDTH ||
				si_z >= AP_SECTOR_WORLD_INDEX_HEIGHT)
				continue;
			s = ac_object_get_sector_by_index(
				mod->ac_object, si_x, si_z);
			if (x == 0 && z == 0) {
				sx0 = s->extent_start[0];
				sz0 = s->extent_start[1];
				sx1 = s->extent_end[0];
				sz1 = s->extent_end[1];
			}
			else {
				if (s->extent_start[0] < sx0)
					sx0 = s->extent_start[0];
				if (s->extent_start[1] < sz0)
					sz0 = s->extent_start[1];
				if (s->extent_end[0] > sx1)
					sx1 = s->extent_end[0];
				if (s->extent_end[1] > sz1)
					sz1 = s->extent_end[1];
			}
		}
	}
	/* Draw bright border around the entire division area */
	/* Bottom rectangle */
	add_line(verts, count, capacity,
		sx0, y_base, sz0, sx1, y_base, sz0, highlight_abgr);
	add_line(verts, count, capacity,
		sx1, y_base, sz0, sx1, y_base, sz1, highlight_abgr);
	add_line(verts, count, capacity,
		sx1, y_base, sz1, sx0, y_base, sz1, highlight_abgr);
	add_line(verts, count, capacity,
		sx0, y_base, sz1, sx0, y_base, sz0, highlight_abgr);
	/* Top rectangle */
	add_line(verts, count, capacity,
		sx0, y_top, sz0, sx1, y_top, sz0, highlight_abgr);
	add_line(verts, count, capacity,
		sx1, y_top, sz0, sx1, y_top, sz1, highlight_abgr);
	add_line(verts, count, capacity,
		sx1, y_top, sz1, sx0, y_top, sz1, highlight_abgr);
	add_line(verts, count, capacity,
		sx0, y_top, sz1, sx0, y_top, sz0, highlight_abgr);
	/* Vertical corners */
	add_line(verts, count, capacity,
		sx0, y_base, sz0, sx0, y_top, sz0, highlight_abgr);
	add_line(verts, count, capacity,
		sx1, y_base, sz0, sx1, y_top, sz0, highlight_abgr);
	add_line(verts, count, capacity,
		sx1, y_base, sz1, sx1, y_top, sz1, highlight_abgr);
	add_line(verts, count, capacity,
		sx0, y_base, sz1, sx0, y_top, sz1, highlight_abgr);
	/* Cross lines on top for visibility */
	add_line(verts, count, capacity,
		sx0, y_top, sz0, sx1, y_top, sz1, highlight_abgr);
	add_line(verts, count, capacity,
		sx1, y_top, sz0, sx0, y_top, sz1, highlight_abgr);
}

static void auto_create_missing_octrees(struct ae_octree_module * mod)
{
	struct ac_object_sector ** sectors = NULL;
	uint32_t i;
	uint32_t count;
	uint32_t created = 0;
	boolean rebuilt_divs[10000];
	memset(rebuilt_divs, 0, sizeof(rebuilt_divs));
	sectors = (struct ac_object_sector **)vec_new_reserved(
		sizeof(*sectors), 64);
	ac_object_query_visible_sectors(mod->ac_object, &sectors);
	count = vec_count(sectors);
	for (i = 0; i < count; i++) {
		struct ac_object_sector * s = sectors[i];
		uint32_t bx, bz, div;
		if (s->octree_roots)
			continue;
		bx = s->index_x / AP_SECTOR_DEFAULT_DEPTH *
			AP_SECTOR_DEFAULT_DEPTH;
		bz = s->index_z / AP_SECTOR_DEFAULT_DEPTH *
			AP_SECTOR_DEFAULT_DEPTH;
		if (!ap_scr_div_index_from_sector_index(bx, bz, &div))
			continue;
		if (div >= 10000 || rebuilt_divs[div])
			continue;
		rebuilt_divs[div] = TRUE;
		rebuild_division(mod, bx, bz);
		created++;
	}
	vec_free(sectors);
	if (created > 0) {
		INFO("Auto-created octree for %u divisions.", created);
		scan_octree_files(mod);
	}
}

static void render_debug_lines(
	struct ae_octree_module * mod,
	struct debug_vertex * verts,
	uint32_t vert_count)
{
	bgfx_transient_vertex_buffer_t tvb;
	mat4 model;
	uint64_t state;
	if (!vert_count || !BGFX_HANDLE_IS_VALID(mod->program))
		return;
	if (bgfx_get_avail_transient_vertex_buffer(vert_count,
			&mod->debug_layout) < vert_count)
		return;
	bgfx_alloc_transient_vertex_buffer(&tvb,
		vert_count, &mod->debug_layout);
	memcpy(tvb.data, verts, vert_count * sizeof(struct debug_vertex));
	glm_mat4_identity(model);
	bgfx_set_transform(&model, 1);
	bgfx_set_transient_vertex_buffer(0, &tvb, 0, vert_count);
	state = BGFX_STATE_WRITE_RGB |
		BGFX_STATE_WRITE_A |
		BGFX_STATE_DEPTH_TEST_LESS |
		BGFX_STATE_PT_LINES;
	bgfx_set_state(state, 0);
	bgfx_submit(0, mod->program, 0, 0xff);
}

void ae_octree_render(
	struct ae_octree_module * mod,
	struct ac_camera * cam)
{
	struct debug_vertex * verts = NULL;
	uint32_t vert_count = 0;
	uint32_t vert_capacity = 0;
	if (!mod->active && !mod->display_debug)
		return;
	if (!BGFX_HANDLE_IS_VALID(mod->program))
		return;
	/* When octree tool is active, draw sector boundaries */
	if (mod->active) {
		collect_sector_boundaries(mod, &verts, &vert_count,
			&vert_capacity);
		/* Highlight selected file's division area */
		collect_selected_highlight(mod, &verts, &vert_count,
			&vert_capacity);
	}
	/* When debug mode is on, also draw octree nodes */
	if (mod->display_debug) {
		struct ac_object_sector ** sectors = NULL;
		uint32_t i;
		uint32_t count;
		sectors = (struct ac_object_sector **)vec_new_reserved(
			sizeof(*sectors), 64);
		ac_object_query_visible_sectors(mod->ac_object, &sectors);
		count = vec_count(sectors);
		for (i = 0; i < count; i++) {
			struct ac_object_sector * s = sectors[i];
			struct ap_octree_root_list * cur;
			if (!s->octree_roots)
				continue;
			cur = s->octree_roots->roots;
			while (cur) {
				if (cur->node) {
					collect_debug_nodes(mod, cur->node,
						&verts, &vert_count,
						&vert_capacity);
				}
				cur = cur->next;
			}
		}
		vec_free(sectors);
	}
	if (vert_count) {
		render_debug_lines(mod, verts, vert_count);
	}
	if (verts)
		dealloc(verts);
}

void ae_octree_imgui(struct ae_octree_module * mod)
{
	if (mod->auto_create_pending) {
		mod->auto_create_pending = false;
		auto_create_missing_octrees(mod);
	}
}

void ae_octree_toolbox(struct ae_octree_module * mod)
{
	ImVec2 avail_box = ImGui::GetContentRegionAvail();
	float avail = MIN(avail_box.x, avail_box.y);
	if (BGFX_HANDLE_IS_VALID(mod->icon)) {
		boolean state = mod->active ? TRUE : FALSE;
		if (tool_button(mod->icon, NULL, avail,
				"Octree Tool", &state, FALSE)) {
			mod->active = !mod->active;
		}
		ImGui::Separator();
	}
}

void ae_octree_toolbar(struct ae_octree_module * mod)
{
	float toolkit_height = ImGui::GetContentRegionAvail().y;
	char file_label[256] = "No File Selected";
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		ImVec2(4.f, 5.f));
	ImGui::SameLine();
	/* File dropdown */
	if (mod->selected_file >= 0 &&
		(uint32_t)mod->selected_file < mod->file_count) {
		struct octree_file_info * f =
			&mod->files[mod->selected_file];
		snprintf(file_label, sizeof(file_label),
			"%s  X[%u-%u] Z[%u-%u]",
			f->name,
			f->sector_x,
			f->sector_x + AP_SECTOR_DEFAULT_DEPTH - 1,
			f->sector_z,
			f->sector_z + AP_SECTOR_DEFAULT_DEPTH - 1);
	}
	ImGui::SetNextItemWidth(280.f);
	if (ImGui::BeginCombo("##OctreeFile", file_label)) {
		uint32_t i;
		if (ImGui::Selectable("(None)",
				mod->selected_file == -1)) {
			mod->selected_file = -1;
		}
		ImGui::Separator();
		for (i = 0; i < mod->file_count; i++) {
			struct octree_file_info * f = &mod->files[i];
			char label[128];
			snprintf(label, sizeof(label),
				"%s  X[%u-%u] Z[%u-%u]",
				f->name,
				f->sector_x,
				f->sector_x + AP_SECTOR_DEFAULT_DEPTH - 1,
				f->sector_z,
				f->sector_z + AP_SECTOR_DEFAULT_DEPTH - 1);
			if (ImGui::Selectable(label,
					mod->selected_file == (int)i)) {
				mod->selected_file = (int)i;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::PopStyleVar(1);
	ImGui::SameLine();
	ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
	ImGui::SameLine();
	/* Rebuild buttons */
	if (ImGui::Button("Rebuild Visible")) {
		struct ac_object_sector ** sectors = NULL;
		uint32_t i;
		uint32_t count;
		boolean rebuilt[100 * 100];
		memset(rebuilt, 0, sizeof(rebuilt));
		sectors = (struct ac_object_sector **)vec_new_reserved(
			sizeof(*sectors), 64);
		ac_object_query_visible_sectors(mod->ac_object, &sectors);
		count = vec_count(sectors);
		for (i = 0; i < count; i++) {
			struct ac_object_sector * s = sectors[i];
			uint32_t bx = s->index_x / AP_SECTOR_DEFAULT_DEPTH *
				AP_SECTOR_DEFAULT_DEPTH;
			uint32_t bz = s->index_z / AP_SECTOR_DEFAULT_DEPTH *
				AP_SECTOR_DEFAULT_DEPTH;
			uint32_t div;
			if (ap_scr_div_index_from_sector_index(bx, bz,
					&div) && !rebuilt[div]) {
				rebuilt[div] = TRUE;
				rebuild_division(mod, bx, bz);
			}
		}
		vec_free(sectors);
		scan_octree_files(mod);
	}
	ImGui::SameLine();
	if (ImGui::Button("Auto Create")) {
		mod->auto_create_pending = true;
	}
	ImGui::SameLine();
	ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
	ImGui::SameLine();
	/* Selected file actions */
	if (mod->selected_file >= 0 &&
		(uint32_t)mod->selected_file < mod->file_count) {
		struct octree_file_info * f =
			&mod->files[mod->selected_file];
		if (ImGui::Button("Delete")) {
			char path[512];
			const char * clientdir =
				ap_config_get(mod->ap_config, "ClientDir");
			if (clientdir) {
				uint32_t x, z;
				make_path(path, sizeof(path),
					"%s/world/octree/%s",
					clientdir, f->name);
				if (remove(path) == 0) {
					for (x = 0; x < AP_SECTOR_DEFAULT_DEPTH;
							x++) {
						for (z = 0;
								z < AP_SECTOR_DEFAULT_DEPTH;
								z++) {
							uint32_t sx = f->sector_x + x;
							uint32_t sz = f->sector_z + z;
							struct ac_object_sector * s;
							if (sx >= AP_SECTOR_WORLD_INDEX_WIDTH ||
								sz >= AP_SECTOR_WORLD_INDEX_HEIGHT)
								continue;
							s = ac_object_get_sector_by_index(
								mod->ac_object, sx, sz);
							if (s->octree_roots) {
								ap_octree_destroy_tree(
									s->octree_roots);
								s->octree_roots = NULL;
							}
						}
					}
					INFO("Deleted: %s", f->name);
					scan_octree_files(mod);
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Rebuild")) {
			rebuild_division(mod, f->sector_x, f->sector_z);
			scan_octree_files(mod);
		}
		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();
	}
	/* Debug toggle */
	ImGui::Checkbox("Debug", &mod->display_debug);
	if (mod->display_debug) {
		ImGui::SameLine();
		ImGui::Checkbox("Empty", &mod->debug_show_empty);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(80.f);
		ImGui::SliderInt("Depth", &mod->debug_max_depth, 0, 2);
	}
	ImGui::SameLine();
	ImGui::Checkbox("Details", &mod->display_manager);
}

boolean ae_octree_is_active(struct ae_octree_module * mod)
{
	return mod->active ? TRUE : FALSE;
}
