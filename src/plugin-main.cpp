#include <obs-module.h>
#include <obs-frontend-api.h>

#include "plugin-support.h"
#include "motioncraft-controller.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void open_dialog_cb(void *)
{
	MotionCraftController::instance().showDialog();
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "[MotionCraft] loaded (version %s)", PLUGIN_VERSION);

	MotionCraftController::instance().initialize();

	obs_frontend_add_tools_menu_item(obs_module_text("Menu.Tools.MotionCraft"), open_dialog_cb, nullptr);
	return true;
}

void obs_module_unload(void)
{
	MotionCraftController::instance().shutdown();
	obs_log(LOG_INFO, "[MotionCraft] unloaded");
}
