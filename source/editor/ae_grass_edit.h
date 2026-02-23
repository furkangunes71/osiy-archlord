#ifndef _AE_GRASS_EDIT_H_
#define _AE_GRASS_EDIT_H_

#include "core/macros.h"
#include "core/types.h"

#include "public/ap_module_instance.h"

#define AE_GRASS_EDIT_MODULE_NAME "AgemGrassEdit"

BEGIN_DECLS

struct ac_camera;

struct ae_grass_edit_module * ae_grass_edit_create_module();

void ae_grass_edit_sync(
	struct ae_grass_edit_module * mod,
	const float * pos,
	boolean force);

void ae_grass_edit_render(
	struct ae_grass_edit_module * mod,
	struct ac_camera * cam);

void ae_grass_edit_imgui(struct ae_grass_edit_module * mod);

void ae_grass_edit_toolbox(struct ae_grass_edit_module * mod);

void ae_grass_edit_toolbar(struct ae_grass_edit_module * mod);

boolean ae_grass_edit_is_active(struct ae_grass_edit_module * mod);

void ae_grass_edit_on_mdown(
	struct ae_grass_edit_module * mod,
	struct ac_camera * cam,
	int mouse_x,
	int mouse_y);

boolean ae_grass_edit_on_key_down(
	struct ae_grass_edit_module * mod,
	uint32_t keycode);

END_DECLS

#endif /* _AE_GRASS_EDIT_H_ */
