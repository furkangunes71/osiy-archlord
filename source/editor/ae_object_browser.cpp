#include "editor/ae_object_browser.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "core/log.h"
#include "core/malloc.h"
#include "core/string.h"

#include "public/ap_config.h"
#include "public/ap_object.h"

#include "client/ac_camera.h"
#include "client/ac_imgui.h"
#include "client/ac_object.h"
#include "client/ac_render.h"
#include "client/ac_terrain.h"
#include "client/ac_texture.h"

#include "editor/ae_editor_action.h"
#include "editor/ae_object.h"

#include "vendor/cglm/cglm.h"
#include "vendor/imgui/imgui.h"

/* Defined in ae_terrain.cpp */
extern bool tool_button(
	bgfx_texture_handle_t tex,
	const bgfx_texture_info_t * info,
	float box_size,
	const char * hint,
	boolean * btn_state,
	boolean is_disabled);

#define PREVIEW_SIZE 512

struct ae_object_browser_module {
	struct ap_module_instance instance;
	struct ap_object_module * ap_object;
	struct ap_config_module * ap_config;
	struct ac_camera_module * ac_camera;
	struct ac_object_module * ac_object;
	struct ac_render_module * ac_render;
	struct ac_terrain_module * ac_terrain;
	struct ac_texture_module * ac_texture;
	struct ae_editor_action_module * ae_editor_action;
	struct ae_object_module * ae_object;
	/* State */
	bool active;
	char filter[128];
	struct ap_object_template * selected_temp;
	/* Drag and drop */
	struct ap_object_template * dragging_temp;
	bool is_dragging;
	/* Sorted template list */
	struct ap_object_template ** sorted_templates;
	uint32_t sorted_count;
	/* 3D Preview */
	bgfx_frame_buffer_handle_t preview_fb;
	bgfx_texture_handle_t preview_color_tex;
	int preview_view;
	float preview_yaw;
	float preview_pitch;
	float preview_distance;
	bool preview_needs_render;
	/* Toolbox */
	bgfx_texture_handle_t icon;
};

static void select_template(
	struct ae_object_browser_module * mod,
	struct ap_object_template * temp)
{
	if (mod->selected_temp == temp)
		return;
	if (mod->selected_temp) {
		struct ac_object_template * ct =
			ac_object_get_template(mod->selected_temp);
		ac_object_release_template(mod->ac_object, ct);
	}
	mod->selected_temp = temp;
	if (temp) {
		struct ac_object_template * ct =
			ac_object_get_template(temp);
		ac_object_reference_template(mod->ac_object, ct);
		mod->preview_yaw = 45.0f;
		mod->preview_pitch = 30.0f;
		mod->preview_distance = 0.0f;
		mod->preview_needs_render = true;
	}
}

static int compare_template_tid(const void * a, const void * b)
{
	const struct ap_object_template * ta =
		*(const struct ap_object_template * const *)a;
	const struct ap_object_template * tb =
		*(const struct ap_object_template * const *)b;
	if (ta->tid < tb->tid)
		return -1;
	if (ta->tid > tb->tid)
		return 1;
	return 0;
}

static void build_sorted_template_list(
	struct ae_object_browser_module * mod)
{
	size_t iter = 0;
	struct ap_object_template * temp;
	uint32_t capacity = 256;
	mod->sorted_count = 0;
	mod->sorted_templates =
		(struct ap_object_template **)alloc(
			capacity * sizeof(*mod->sorted_templates));
	while ((temp = ap_object_iterate_templates(
			mod->ap_object, &iter)) != NULL) {
		if (mod->sorted_count >= capacity) {
			capacity *= 2;
			mod->sorted_templates =
				(struct ap_object_template **)reallocate(
					mod->sorted_templates,
					capacity * sizeof(*mod->sorted_templates));
		}
		mod->sorted_templates[mod->sorted_count++] = temp;
	}
	qsort(mod->sorted_templates, mod->sorted_count,
		sizeof(*mod->sorted_templates), compare_template_tid);
}

static boolean do_terrain_raycast(
	struct ae_object_browser_module * mod,
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

static boolean onregister(
	struct ae_object_browser_module * mod,
	struct ap_module_registry * registry)
{
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ap_object,
		AP_OBJECT_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ap_config,
		AP_CONFIG_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ac_camera,
		AC_CAMERA_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ac_object,
		AC_OBJECT_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ac_render,
		AC_RENDER_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ac_terrain,
		AC_TERRAIN_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ac_texture,
		AC_TEXTURE_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ae_object,
		AE_OBJECT_MODULE_NAME);
	AP_MODULE_INSTANCE_FIND_IN_REGISTRY(registry, mod->ae_editor_action,
		AE_EDITOR_ACTION_MODULE_NAME);
	return TRUE;
}

static boolean oninitialize(struct ae_object_browser_module * mod)
{
	bgfx_texture_handle_t textures[2];
	textures[0] = bgfx_create_texture_2d(PREVIEW_SIZE, PREVIEW_SIZE,
		false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
		BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
		NULL);
	textures[1] = bgfx_create_texture_2d(PREVIEW_SIZE, PREVIEW_SIZE,
		false, 1, BGFX_TEXTURE_FORMAT_D24S8,
		BGFX_TEXTURE_RT_WRITE_ONLY, NULL);
	mod->preview_fb = bgfx_create_frame_buffer_from_handles(
		2, textures, true);
	if (!BGFX_HANDLE_IS_VALID(mod->preview_fb)) {
		WARN("Failed to create object preview framebuffer.");
	}
	mod->preview_color_tex = textures[0];
	mod->preview_view = ac_render_allocate_view(mod->ac_render);
	mod->icon = ac_texture_load(mod->ac_texture,
		"content/textures/object-browser.png", NULL);
	if (!BGFX_HANDLE_IS_VALID(mod->icon))
		WARN("Failed to load object browser icon.");
	return TRUE;
}

static void onshutdown(struct ae_object_browser_module * mod)
{
	if (mod->selected_temp) {
		struct ac_object_template * ct =
			ac_object_get_template(mod->selected_temp);
		ac_object_release_template(mod->ac_object, ct);
		mod->selected_temp = NULL;
	}
	if (mod->sorted_templates)
		dealloc(mod->sorted_templates);
	if (BGFX_HANDLE_IS_VALID(mod->preview_fb))
		bgfx_destroy_frame_buffer(mod->preview_fb);
	if (BGFX_HANDLE_IS_VALID(mod->icon))
		ac_texture_release(mod->ac_texture, mod->icon);
}

struct ae_object_browser_module * ae_object_browser_create_module()
{
	struct ae_object_browser_module * mod =
		(struct ae_object_browser_module *)ap_module_instance_new(
			AE_OBJECT_BROWSER_MODULE_NAME, sizeof(*mod),
			(ap_module_instance_register_t)onregister,
			(ap_module_instance_initialize_t)oninitialize,
			NULL,
			(ap_module_instance_shutdown_t)onshutdown);
	mod->active = false;
	mod->filter[0] = '\0';
	mod->selected_temp = NULL;
	mod->dragging_temp = NULL;
	mod->is_dragging = false;
	mod->sorted_templates = NULL;
	mod->sorted_count = 0;
	mod->preview_fb.idx = UINT16_MAX;
	mod->preview_color_tex.idx = UINT16_MAX;
	mod->preview_view = -1;
	mod->preview_yaw = 45.0f;
	mod->preview_pitch = 30.0f;
	mod->preview_distance = 0.0f;
	mod->preview_needs_render = false;
	mod->icon.idx = UINT16_MAX;
	return mod;
}

void ae_object_browser_render(struct ae_object_browser_module * mod)
{
	struct ac_object_template * ct;
	vec4 bsphere;
	float radius;
	float dist;
	float yaw_rad, pitch_rad;
	vec3 eye, center, up;
	mat4 view_mat, proj_mat;
	struct ac_object_render_data rd;
	struct au_pos pos;
	int prev_view;
	if (!mod->active || !mod->selected_temp)
		return;
	if (!BGFX_HANDLE_IS_VALID(mod->preview_fb))
		return;
	ct = ac_object_get_template(mod->selected_temp);
	if (!ct->group_list || !ct->group_list->clump)
		return;
	/* Compute bounding sphere for camera distance */
	ac_object_get_bounding_sphere(mod->ac_object, ct, bsphere);
	radius = bsphere[3];
	if (radius < 1.0f)
		radius = 100.0f;
	dist = mod->preview_distance;
	if (dist == 0.0f)
		dist = radius * 3.0f;
	/* Compute camera position from yaw/pitch/distance */
	yaw_rad = glm_rad(mod->preview_yaw);
	pitch_rad = glm_rad(mod->preview_pitch);
	center[0] = bsphere[0];
	center[1] = bsphere[1];
	center[2] = bsphere[2];
	eye[0] = center[0] + dist * cosf(pitch_rad) * sinf(yaw_rad);
	eye[1] = center[1] + dist * sinf(pitch_rad);
	eye[2] = center[2] + dist * cosf(pitch_rad) * cosf(yaw_rad);
	up[0] = 0.0f;
	up[1] = 1.0f;
	up[2] = 0.0f;
	glm_lookat(eye, center, up, view_mat);
	glm_perspective(glm_rad(45.0f),
		1.0f, /* aspect = 1:1 */
		radius * 0.01f,
		dist * 10.0f,
		proj_mat);
	/* Setup view */
	bgfx_set_view_frame_buffer(mod->preview_view,
		mod->preview_fb);
	bgfx_set_view_clear(mod->preview_view,
		BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
		0xff333333, 1.0f, 0);
	bgfx_set_view_rect(mod->preview_view,
		0, 0, PREVIEW_SIZE, PREVIEW_SIZE);
	bgfx_set_view_transform(mod->preview_view,
		view_mat, proj_mat);
	bgfx_touch(mod->preview_view);
	/* Render object template to framebuffer */
	prev_view = ac_render_set_view(mod->ac_render,
		mod->preview_view);
	memset(&rd, 0, sizeof(rd));
	rd.state =
		BGFX_STATE_WRITE_RGB |
		BGFX_STATE_WRITE_A |
		BGFX_STATE_WRITE_Z |
		BGFX_STATE_DEPTH_TEST_LESS |
		BGFX_STATE_CULL_CW |
		BGFX_STATE_MSAA;
	rd.program.idx = UINT16_MAX;
	rd.no_texture = FALSE;
	pos.x = 0.0f;
	pos.y = 0.0f;
	pos.z = 0.0f;
	ac_object_render_object_template(mod->ac_object,
		mod->selected_temp, &rd, &pos);
	ac_render_set_view(mod->ac_render, prev_view);
}

void ae_object_browser_imgui(struct ae_object_browser_module * mod)
{
	if (!mod->active)
		return;
	if (!mod->sorted_templates)
		build_sorted_template_list(mod);
	if (!mod->sorted_count)
		return;
	/* Object List Window */
	ImGui::SetNextWindowSize(ImVec2(300, 500),
		ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Object List", &mod->active)) {
		uint32_t i;
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##ObjFilter", "Search...",
			mod->filter, sizeof(mod->filter));
		ImGui::Separator();
		if (ImGui::BeginChild("##ObjList", ImVec2(0, 0),
				false)) {
			for (i = 0; i < mod->sorted_count; i++) {
				struct ap_object_template * temp =
					mod->sorted_templates[i];
				char label[128];
				bool is_selected =
					(mod->selected_temp == temp);
				if (mod->filter[0] &&
						!stristr(temp->name, mod->filter))
					continue;
				snprintf(label, sizeof(label), "[%u] %s",
					temp->tid, temp->name);
				if (ImGui::Selectable(label, is_selected)) {
					if (is_selected)
						select_template(mod, NULL);
					else
						select_template(mod, temp);
				}
				if (ImGui::BeginDragDropSource(
						ImGuiDragDropFlags_None)) {
					ImGui::SetDragDropPayload(
						"OBJ_TEMPLATE",
						&temp, sizeof(temp));
					mod->dragging_temp = temp;
					ImGui::Text("[%u] %s",
						temp->tid, temp->name);
					ImGui::EndDragDropSource();
				}
			}
		}
		ImGui::EndChild();
	}
	ImGui::End();
	/* 3D Preview Window */
	if (mod->selected_temp &&
			BGFX_HANDLE_IS_VALID(mod->preview_fb)) {
		ImGui::SetNextWindowSize(ImVec2(PREVIEW_SIZE + 16,
			PREVIEW_SIZE + 60), ImGuiCond_FirstUseEver);
		bool show_preview = true;
		if (ImGui::Begin("Object Preview", &show_preview)) {
			ImVec2 avail = ImGui::GetContentRegionAvail();
			float img_size = avail.x < avail.y ?
				avail.x : avail.y;
			if (img_size < 64.0f)
				img_size = 64.0f;
			ImGui::Text("%s (TID: %u)",
				mod->selected_temp->name,
				mod->selected_temp->tid);
			ImGui::Separator();
			bgfx_texture_handle_t tex =
				bgfx_get_texture(mod->preview_fb, 0);
			ImVec2 img_cursor = ImGui::GetCursorScreenPos();
			ImGui::Image((ImTextureID)(uintptr_t)tex.idx,
				ImVec2(img_size, img_size));
			/* Overlay an InvisibleButton to capture mouse
			 * input instead of window drag. */
			ImGui::SetCursorScreenPos(img_cursor);
			ImGui::InvisibleButton("##preview_interact",
				ImVec2(img_size, img_size));
			if (ImGui::IsItemHovered()) {
				/* Scroll wheel for zoom */
				float wheel = ImGui::GetIO().MouseWheel;
				if (wheel != 0.0f) {
					struct ac_object_template * ct =
						ac_object_get_template(
							mod->selected_temp);
					vec4 bs;
					float radius;
					ac_object_get_bounding_sphere(
						mod->ac_object, ct, bs);
					radius = bs[3];
					if (radius < 1.0f)
						radius = 100.0f;
					if (mod->preview_distance == 0.0f)
						mod->preview_distance =
							radius * 3.0f;
					mod->preview_distance -=
						wheel * radius * 0.3f;
					if (mod->preview_distance <
							radius * 0.5f)
						mod->preview_distance =
							radius * 0.5f;
					if (mod->preview_distance >
							radius * 20.0f)
						mod->preview_distance =
							radius * 20.0f;
				}
			}
			if (ImGui::IsItemActive() &&
					ImGui::IsMouseDragging(
						ImGuiMouseButton_Left)) {
				ImVec2 delta =
					ImGui::GetIO().MouseDelta;
				mod->preview_yaw -= delta.x * 0.5f;
				mod->preview_pitch += delta.y * 0.5f;
				if (mod->preview_pitch > 89.0f)
					mod->preview_pitch = 89.0f;
				if (mod->preview_pitch < -89.0f)
					mod->preview_pitch = -89.0f;
			}
		}
		ImGui::End();
		if (!show_preview)
			select_template(mod, NULL);
	}
	/* Detect drag-and-drop onto viewport */
	{
		const ImGuiPayload * payload =
			ImGui::GetDragDropPayload();
		bool currently_dragging = payload != NULL &&
			payload->IsDataType("OBJ_TEMPLATE");
		if (mod->is_dragging && !currently_dragging &&
				mod->dragging_temp) {
			/* Drag just ended - check if mouse is over
			 * the 3D viewport (not over any ImGui window) */
			if (!ImGui::IsWindowHovered(
					ImGuiHoveredFlags_AnyWindow)) {
				ImVec2 mouse = ImGui::GetMousePos();
				vec3 hit;
				if (do_terrain_raycast(mod,
						(int)mouse.x, (int)mouse.y,
						hit)) {
					struct au_pos pos;
					pos.x = hit[0];
					pos.y = hit[1];
					pos.z = hit[2];
					ae_object_place_from_template(
						mod->ae_object,
						mod->dragging_temp, &pos);
					INFO("Placed object [%u] %s",
						mod->dragging_temp->tid,
						mod->dragging_temp->name);
				}
			}
			mod->dragging_temp = NULL;
		}
		mod->is_dragging = currently_dragging;
	}
}

void ae_object_browser_toolbox(struct ae_object_browser_module * mod)
{
	ImVec2 avail_box = ImGui::GetContentRegionAvail();
	float avail = avail_box.x < avail_box.y ?
		avail_box.x : avail_box.y;
	if (BGFX_HANDLE_IS_VALID(mod->icon)) {
		boolean state = mod->active ? TRUE : FALSE;
		if (tool_button(mod->icon, NULL, avail,
				"Object Browser", &state, FALSE)) {
			mod->active = !mod->active;
			if (!mod->active)
				select_template(mod, NULL);
		}
		ImGui::Separator();
	}
}

void ae_object_browser_toolbar(struct ae_object_browser_module * mod)
{
	if (mod->selected_temp) {
		ImGui::Text("Selected: %s (TID: %u)",
			mod->selected_temp->name,
			mod->selected_temp->tid);
	}
	else {
		ImGui::Text("Object Browser - No selection");
	}
}

boolean ae_object_browser_is_active(
	struct ae_object_browser_module * mod)
{
	return mod->active ? TRUE : FALSE;
}
