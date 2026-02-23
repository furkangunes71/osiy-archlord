#ifndef _AE_OBJECT_BROWSER_H_
#define _AE_OBJECT_BROWSER_H_

#include "core/macros.h"
#include "core/types.h"

#include "public/ap_module_instance.h"

#define AE_OBJECT_BROWSER_MODULE_NAME "AgemObjectBrowser"

BEGIN_DECLS

struct ae_object_browser_module * ae_object_browser_create_module();

void ae_object_browser_toolbox(struct ae_object_browser_module * mod);

void ae_object_browser_toolbar(struct ae_object_browser_module * mod);

void ae_object_browser_render(struct ae_object_browser_module * mod);

void ae_object_browser_imgui(struct ae_object_browser_module * mod);

boolean ae_object_browser_is_active(struct ae_object_browser_module * mod);

END_DECLS

#endif /* _AE_OBJECT_BROWSER_H_ */
