#ifndef _AE_OCTREE_H_
#define _AE_OCTREE_H_

#include "core/macros.h"
#include "core/types.h"

#include "public/ap_module_instance.h"

#define AE_OCTREE_MODULE_NAME "AgemOctree"

BEGIN_DECLS

struct ac_camera;

struct ae_octree_module * ae_octree_create_module();

void ae_octree_render(
	struct ae_octree_module * mod,
	struct ac_camera * cam);

void ae_octree_imgui(struct ae_octree_module * mod);

void ae_octree_toolbox(struct ae_octree_module * mod);

void ae_octree_toolbar(struct ae_octree_module * mod);

boolean ae_octree_is_active(struct ae_octree_module * mod);

END_DECLS

#endif /* _AE_OCTREE_H_ */
