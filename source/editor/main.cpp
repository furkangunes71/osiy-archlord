#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define WIN32_EXTRA_LEAN
#define NOGDI
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "psapi.lib")
#endif
#undef near
#undef far

#include "core/core.h"
#include "core/file_system.h"
#include "core/log.h"
#include "core/malloc.h"
#include "core/os.h"

#include "task/task.h"

#include "public/ap_admin.h"
#include "public/ap_ai2.h"
#include "public/ap_auction.h"
#include "public/ap_bill_info.h"
#include "public/ap_character.h"
#include "public/ap_cash_mall.h"
#include "public/ap_chat.h"
#include "public/ap_config.h"
#include "public/ap_drop_item.h"
#include "public/ap_dungeon_wnd.h"
#include "public/ap_event_bank.h"
#include "public/ap_event_binding.h"
#include "public/ap_event_gacha.h"
#include "public/ap_event_guild.h"
#include "public/ap_event_item_convert.h"
#include "public/ap_event_manager.h"
#include "public/ap_event_nature.h"
#include "public/ap_event_npc_dialog.h"
#include "public/ap_event_npc_trade.h"
#include "public/ap_event_point_light.h"
#include "public/ap_event_product.h"
#include "public/ap_event_refinery.h"
#include "public/ap_event_skill_master.h"
#include "public/ap_event_quest.h"
#include "public/ap_event_teleport.h"
#include "public/ap_factors.h"
#include "public/ap_guild.h"
#include "public/ap_item.h"
#include "public/ap_item_convert.h"
#include "public/ap_login.h"
#include "public/ap_map.h"
#include "public/ap_sector.h"
#include "public/ap_object.h"
#include "public/ap_octree.h"
#include "public/ap_optimized_packet2.h"
#include "public/ap_packet.h"
#include "public/ap_party.h"
#include "public/ap_party_item.h"
#include "public/ap_plugin_boss_spawn.h"
#include "public/ap_private_trade.h"
#include "public/ap_pvp.h"
#include "public/ap_random.h"
#include "public/ap_refinery.h"
#include "public/ap_ride.h"
#include "public/ap_service_npc.h"
#include "public/ap_skill.h"
#include "public/ap_shrine.h"
#include "public/ap_spawn.h"
#include "public/ap_startup_encryption.h"
#include "public/ap_summons.h"
#include "public/ap_system_message.h"
#include "public/ap_tick.h"
#include "public/ap_ui_status.h"
#include "public/ap_world.h"

#include "client/ac_ambient_occlusion_map.h"
#include "client/ac_camera.h"
#include "client/ac_character.h"
#include "client/ac_console.h"
#include "client/ac_dat.h"
#include "client/ac_effect.h"
#include "client/ac_event_effect.h"
#include "client/ac_event_point_light.h"
#include "client/ac_imgui.h"
#include "client/ac_lod.h"
#include "client/ac_mesh.h"
#include "client/ac_object.h"
#include "client/ac_render.h"
#include "client/ac_terrain.h"
#include "client/ac_texture.h"

#include "editor/ae_editor_action.h"
#include "editor/ae_event_binding.h"
#include "editor/ae_event_refinery.h"
#include "editor/ae_event_teleport.h"
#include "editor/ae_grass_edit.h"
#include "editor/ae_item.h"
#include "editor/ae_map.h"
#include "editor/ae_npc.h"
#include "editor/ae_object.h"
#include "editor/ae_object_browser.h"
#include "editor/ae_octree.h"
#include "editor/ae_terrain.h"
#include "editor/ae_texture.h"
#include "editor/ae_transform_tool.h"

/* 20 FPS */
#define STEPTIME (1.0f / 50.0f)

struct camera_controls {
	boolean key_state[512];
};

static bool g_ShowFps = false;
static int g_ViewDistMultiplier = 6;

static void save_config_value(const char * key, const char * value)
{
	FILE * f;
	char lines[32][1024];
	int line_count = 0;
	boolean found = FALSE;
	size_t key_len = strlen(key);
	f = fopen("./config", "r");
	if (f) {
		while (line_count < 32 &&
				fgets(lines[line_count], sizeof(lines[0]), f)) {
			char * nl = strchr(lines[line_count], '\n');
			if (nl)
				*nl = '\0';
			nl = strchr(lines[line_count], '\r');
			if (nl)
				*nl = '\0';
			if (strncmp(lines[line_count], key, key_len) == 0 &&
					lines[line_count][key_len] == '=') {
				snprintf(lines[line_count],
					sizeof(lines[0]), "%s=%s", key, value);
				found = TRUE;
			}
			line_count++;
		}
		fclose(f);
	}
	if (!found && line_count < 32) {
		snprintf(lines[line_count], sizeof(lines[0]),
			"%s=%s", key, value);
		line_count++;
	}
	f = fopen("./config", "w");
	if (f) {
		int i;
		for (i = 0; i < line_count; i++)
			fprintf(f, "%s\n", lines[i]);
		fclose(f);
	}
}

static float g_StatsTimer = 0.0f;
static char g_StatsBuf[256] = "";
static float g_StatsBufWidth = 0.0f;
#ifdef _WIN32
static float g_CpuUsage = 0.0f;
static ULARGE_INTEGER g_LastCpuKernel = { 0 };
static ULARGE_INTEGER g_LastCpuUser = { 0 };
static ULARGE_INTEGER g_LastSysTime = { 0 };
/* NVML dynamic loading for accurate GPU utilization */
typedef int (*nvmlInit_v2_fn)(void);
typedef int (*nvmlDeviceGetHandleByIndex_v2_fn)(
	unsigned int index, void ** device);
typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
typedef int (*nvmlDeviceGetUtilizationRates_fn)(
	void * device, nvmlUtilization_t * utilization);
static HMODULE g_NvmlLib = NULL;
static void * g_NvmlDevice = NULL;
static nvmlDeviceGetUtilizationRates_fn g_NvmlGetUtil = NULL;
static float g_GpuUsage = 0.0f;
static void init_nvml(void)
{
	nvmlInit_v2_fn init_fn;
	nvmlDeviceGetHandleByIndex_v2_fn getdev_fn;
	g_NvmlLib = LoadLibraryA("nvml.dll");
	if (!g_NvmlLib)
		return;
	init_fn = (nvmlInit_v2_fn)GetProcAddress(
		g_NvmlLib, "nvmlInit_v2");
	getdev_fn = (nvmlDeviceGetHandleByIndex_v2_fn)GetProcAddress(
		g_NvmlLib, "nvmlDeviceGetHandleByIndex_v2");
	g_NvmlGetUtil = (nvmlDeviceGetUtilizationRates_fn)GetProcAddress(
		g_NvmlLib, "nvmlDeviceGetUtilizationRates");
	if (!init_fn || !getdev_fn || !g_NvmlGetUtil) {
		FreeLibrary(g_NvmlLib);
		g_NvmlLib = NULL;
		g_NvmlGetUtil = NULL;
		return;
	}
	if (init_fn() != 0) {
		FreeLibrary(g_NvmlLib);
		g_NvmlLib = NULL;
		g_NvmlGetUtil = NULL;
		return;
	}
	if (getdev_fn(0, &g_NvmlDevice) != 0)
		g_NvmlDevice = NULL;
}
#endif

typedef ap_module_t (*create_module_t)();

struct module_desc {
	const char * name;
	void * cb_create;
	ap_module_t module_;
	void * global_handle;
};

static struct ap_ai2_module * g_ApAi2;
static struct ap_auction_module * g_ApAuction;
static struct ap_base_module * g_ApBase;
static struct ap_bill_info_module * g_ApBillInfo;
static struct ap_cash_mall_module * g_ApCashMall;
static struct ap_character_module * g_ApCharacter;
static struct ap_chat_module * g_ApChat;
static struct ap_config_module * g_ApConfig;
static struct ap_drop_item_module * g_ApDropItem;
static struct ap_dungeon_wnd_module * g_ApDungeonWnd;
static struct ap_event_bank_module * g_ApEventBank;
static struct ap_event_binding_module * g_ApEventBinding;
static struct ap_event_gacha_module * g_ApEventGacha;
static struct ap_event_guild_module * g_ApEventGuild;
static struct ap_event_item_convert_module * g_ApEventItemConvert;
static struct ap_event_manager_module * g_ApEventManager;
static struct ap_event_nature_module * g_ApEventNature;
static struct ap_event_npc_dialog_module * g_ApEventNpcDialog;
static struct ap_event_npc_trade_module * g_ApEventNpcTrade;
static struct ap_event_point_light_module * g_ApEventPointLight;
static struct ap_event_product_module * g_ApEventProduct;
static struct ap_event_refinery_module * g_ApEventRefinery;
static struct ap_event_skill_master_module * g_ApEventSkillMaster;
static struct ap_event_quest_module * g_ApEventQuest;
static struct ap_event_teleport_module * g_ApEventTeleport;
static struct ap_factors_module * g_ApFactors;
static struct ap_guild_module * g_ApGuild;
static struct ap_item_module * g_ApItem;
static struct ap_item_convert_module * g_ApItemConvert;
static struct ap_login_module * g_ApLogin;
static struct ap_map_module * g_ApMap;
static struct ap_object_module * g_ApObject;
static struct ap_optimized_packet2_module * g_ApOptimizedPacket2;
static struct ap_packet_module * g_ApPacket;
static struct ap_party_module * g_ApParty;
static struct ap_party_item_module * g_ApPartyItem;
static struct ap_plugin_boss_spawn_module * g_ApPluginBossSpawn;
static struct ap_private_trade_module * g_ApPrivateTrade;
static struct ap_pvp_module * g_ApPvP;
static struct ap_random_module * g_ApRandom;
static struct ap_refinery_module * g_ApRefinery;
static struct ap_ride_module * g_ApRide;
static struct ap_service_npc_module * g_ApServiceNpc;
static struct ap_skill_module * g_ApSkill;
static struct ap_spawn_module * g_ApSpawn;
static struct ap_startup_encryption_module * g_ApStartupEncryption;
static struct ap_summons_module * g_ApSummons;
static struct ap_system_message_module * g_ApSystemMessage;
static struct ap_tick_module * g_ApTick;
static struct ap_ui_status_module * g_ApUiStatus;
static struct ap_world_module * g_ApWorld;

static struct ac_ambient_occlusion_map_module * g_AcAmbientOcclusionMap;
static struct ac_camera_module * g_AcCamera;
static struct ac_character_module * g_AcCharacter;
static struct ac_console_module * g_AcConsole;
static struct ac_dat_module * g_AcDat;
static struct ac_effect_module * g_AcEffect;
static struct ac_event_effect_module * g_AcEventEffect;
static struct ac_event_point_light_module * g_AcEventPointLight;
static struct ac_imgui_module * g_AcImgui;
static struct ac_lod_module * g_AcLOD;
static struct ac_mesh_module * g_AcMesh;
static struct ac_object_module * g_AcObject;
static struct ac_render_module * g_AcRender;
static struct ac_terrain_module * g_AcTerrain;
static struct ac_texture_module * g_AcTexture;

static struct ae_editor_action_module * g_AeEditorAction;
static struct ae_event_auction_module * g_AeEventAuction;
static struct ae_event_binding_module * g_AeEventBinding;
static struct ae_event_refinery_module * g_AeEventRefinery;
static struct ae_event_teleport_module * g_AeEventTeleport;
static struct ae_grass_edit_module * g_AeGrass;
static struct ae_item_module * g_AeItem;
static struct ae_map_module * g_AeMap;
static struct ae_npc_module * g_AeNpc;
static struct ae_object_module * g_AeObject;
static struct ae_object_browser_module * g_AeObjectBrowser;
static struct ae_octree_module * g_AeOctree;
static struct ae_terrain_module * g_AeTerrain;
static struct ae_texture_module * g_AeTexture;
static struct ae_transform_tool_module * g_AeTransformTool;

static struct module_desc g_Modules[] = {
	/* Public modules. */
	{ AP_PACKET_MODULE_NAME, ap_packet_create_module, NULL, &g_ApPacket },
	{ AP_TICK_MODULE_NAME, ap_tick_create_module, NULL, &g_ApTick },
	{ AP_RANDOM_MODULE_NAME, ap_random_create_module, NULL, &g_ApRandom },
	{ AP_CONFIG_MODULE_NAME, ap_config_create_module, NULL, &g_ApConfig },
	{ AP_BASE_MODULE_NAME, ap_base_create_module, NULL, &g_ApBase },
	{ AP_STARTUP_ENCRYPTION_MODULE_NAME, ap_startup_encryption_create_module, NULL, &g_ApStartupEncryption },
	{ AP_SYSTEM_MESSAGE_MODULE_NAME, ap_system_message_create_module, NULL, &g_ApSystemMessage },
	{ AP_FACTORS_MODULE_NAME, ap_factors_create_module, NULL, &g_ApFactors },
	{ AP_OBJECT_MODULE_NAME, ap_object_create_module, NULL, &g_ApObject },
	{ AP_DUNGEON_WND_MODULE_NAME, ap_dungeon_wnd_create_module, NULL, &g_ApDungeonWnd },
	{ AP_PLUGIN_BOSS_SPAWN_MODULE_NAME, ap_plugin_boss_spawn_create_module, NULL, &g_ApPluginBossSpawn },
	{ AP_CHARACTER_MODULE_NAME, ap_character_create_module, NULL, &g_ApCharacter },
	{ AP_SUMMONS_MODULE_NAME, ap_summons_create_module, NULL, &g_ApSummons },
	{ AP_PVP_MODULE_NAME, ap_pvp_create_module, NULL, &g_ApPvP },
	{ AP_PARTY_MODULE_NAME, ap_party_create_module, NULL, &g_ApParty },
	{ AP_SPAWN_MODULE_NAME, ap_spawn_create_module, NULL, &g_ApSpawn },
	{ AP_SKILL_MODULE_NAME, ap_skill_create_module, NULL, &g_ApSkill },
	{ AP_ITEM_MODULE_NAME, ap_item_create_module, NULL, &g_ApItem },
	{ AP_ITEM_CONVERT_MODULE_NAME, ap_item_convert_create_module, NULL, &g_ApItemConvert },
	{ AP_RIDE_MODULE_NAME, ap_ride_create_module, NULL, &g_ApRide },
	{ AP_REFINERY_MODULE_NAME, ap_refinery_create_module, NULL, &g_ApRefinery },
	{ AP_PRIVATE_TRADE_MODULE_NAME, ap_private_trade_create_module, NULL, &g_ApPrivateTrade },
	{ AP_AI2_MODULE_NAME, ap_ai2_create_module, NULL, &g_ApAi2 },
	{ AP_DROP_ITEM_MODULE_NAME, ap_drop_item_create_module, NULL, &g_ApDropItem },
	{ AP_BILL_INFO_MODULE_NAME, ap_bill_info_create_module, NULL, &g_ApBillInfo },
	{ AP_CASH_MALL_MODULE_NAME, ap_cash_mall_create_module, NULL, &g_ApCashMall },
	{ AP_PARTY_ITEM_MODULE_NAME, ap_party_item_create_module, NULL, &g_ApPartyItem },
	{ AP_UI_STATUS_MODULE_NAME, ap_ui_status_create_module, NULL, &g_ApUiStatus },
	{ AP_GUILD_MODULE_NAME, ap_guild_create_module, NULL, &g_ApGuild },
	{ AP_EVENT_MANAGER_MODULE_NAME, ap_event_manager_create_module, NULL, &g_ApEventManager },
	{ AP_EVENT_BANK_MODULE_NAME, ap_event_bank_create_module, NULL, &g_ApEventBank },
	{ AP_EVENT_BINDING_MODULE_NAME, ap_event_binding_create_module, NULL, &g_ApEventBinding },
	{ AP_EVENT_GACHA_MODULE_NAME, ap_event_gacha_create_module, NULL, &g_ApEventGacha },
	{ AP_EVENT_GUILD_MODULE_NAME, ap_event_guild_create_module, NULL, &g_ApEventGuild },
	{ AP_EVENT_ITEM_CONVERT_MODULE_NAME, ap_event_item_convert_create_module, NULL, &g_ApEventItemConvert },
	{ AP_EVENT_NATURE_MODULE_NAME, ap_event_nature_create_module, NULL, &g_ApEventNature },
	{ AP_EVENT_NPC_DIALOG_MODULE_NAME, ap_event_npc_dialog_create_module, NULL, &g_ApEventNpcDialog },
	{ AP_EVENT_NPC_TRADE_MODULE_NAME, ap_event_npc_trade_create_module, NULL, &g_ApEventNpcTrade },
	{ AP_EVENT_POINT_LIGHT_MODULE_NAME, ap_event_point_light_create_module, NULL, &g_ApEventPointLight },
	{ AP_EVENT_PRODUCT_MODULE_NAME, ap_event_product_create_module, NULL, &g_ApEventProduct },
	{ AP_EVENT_SKILL_MASTER_MODULE_NAME, ap_event_skill_master_create_module, NULL, &g_ApEventSkillMaster },
	{ AP_EVENT_QUEST_MODULE_NAME, ap_event_quest_create_module, NULL, &g_ApEventQuest },
	{ AP_EVENT_TELEPORT_MODULE_NAME, ap_event_teleport_create_module, NULL, &g_ApEventTeleport },
	{ AP_EVENT_REFINERY_MODULE_NAME, ap_event_refinery_create_module, NULL, &g_ApEventRefinery },
	{ AP_AUCTION_MODULE_NAME, ap_auction_create_module, NULL, &g_ApAuction },
	{ AP_SERVICE_NPC_MODULE_NAME, ap_service_npc_create_module, NULL, &g_ApServiceNpc },
	{ AP_OPTIMIZED_PACKET2_MODULE_NAME, ap_optimized_packet2_create_module, NULL, &g_ApOptimizedPacket2 },
	{ AP_CHAT_MODULE_NAME, ap_chat_create_module, NULL, &g_ApChat },
	{ AP_MAP_MODULE_NAME, ap_map_create_module, NULL, &g_ApMap },
	{ AP_LOGIN_MODULE_NAME, ap_login_create_module, NULL, &g_ApLogin },
	{ AP_WORLD_MODULE_NAME, ap_world_create_module, NULL, &g_ApWorld },
	/* Client modules. */
	{ AC_CAMERA_MODULE_NAME, ac_camera_create_module, NULL, &g_AcCamera },
	{ AC_AMBIENT_OCCLUSION_MAP_MODULE_NAME, ac_ambient_occlusion_map_create_module, NULL, &g_AcAmbientOcclusionMap },
	{ AC_EVENT_POINT_LIGHT_MODULE_NAME, ac_event_point_light_create_module, NULL, &g_AcEventPointLight },
	{ AC_DAT_MODULE_NAME, ac_dat_create_module, NULL, &g_AcDat },
	{ AC_RENDER_MODULE_NAME, ac_render_create_module, NULL, &g_AcRender },
	{ AC_TEXTURE_MODULE_NAME, ac_texture_create_module, NULL, &g_AcTexture },
	{ AC_IMGUI_MODULE_NAME, ac_imgui_create_module, NULL, &g_AcImgui },
	{ AC_CONSOLE_MODULE_NAME, ac_console_create_module, NULL, &g_AcConsole },
	{ AC_MESH_MODULE_NAME, ac_mesh_create_module, NULL, &g_AcMesh },
	{ AC_LOD_MODULE_NAME, ac_lod_create_module, NULL, &g_AcLOD },
	{ AC_TERRAIN_MODULE_NAME, ac_terrain_create_module, NULL, &g_AcTerrain },
	{ AC_EFFECT_MODULE_NAME, ac_effect_create_module, NULL, &g_AcEffect },
	{ AC_OBJECT_MODULE_NAME, ac_object_create_module, NULL, &g_AcObject },
	{ AC_EVENT_EFFECT_MODULE_NAME, ac_event_effect_create_module, NULL, &g_AcEventEffect },
	{ AC_CHARACTER_MODULE_NAME, ac_character_create_module, NULL, &g_AcCharacter },
	/* Editor modules. */
	{ AE_EDITOR_ACTION_MODULE_NAME, ae_editor_action_create_module, NULL, &g_AeEditorAction },
	{ AE_TEXTURE_MODULE_NAME, ae_texture_create_module, NULL, &g_AeTexture },
	{ AE_TERRAIN_MODULE_NAME, ae_terrain_create_module, NULL, &g_AeTerrain },
	{ AE_TRANSFORM_TOOL_MODULE_NAME, ae_transform_tool_create_module, NULL, &g_AeTransformTool },
	{ AE_MAP_MODULE_NAME, ae_map_create_module, NULL, &g_AeMap },
	{ AE_EVENT_BINDING_MODULE_NAME, ae_event_binding_create_module, NULL, &g_AeEventBinding },
	{ AE_EVENT_REFINERY_MODULE_NAME, ae_event_refinery_create_module, NULL, &g_AeEventRefinery },
	{ AE_EVENT_TELEPORT_MODULE_NAME, ae_event_teleport_create_module, NULL, &g_AeEventTeleport },
	{ AE_OBJECT_MODULE_NAME, ae_object_create_module, NULL, &g_AeObject },
	{ AE_OBJECT_BROWSER_MODULE_NAME, ae_object_browser_create_module, NULL, &g_AeObjectBrowser },
	{ AE_OCTREE_MODULE_NAME, ae_octree_create_module, NULL, &g_AeOctree },
	{ AE_GRASS_EDIT_MODULE_NAME, ae_grass_edit_create_module, NULL, &g_AeGrass },
	{ AE_ITEM_MODULE_NAME, ae_item_create_module, NULL, &g_AeItem },
	{ AE_NPC_MODULE_NAME, ae_npc_create_module, NULL, &g_AeNpc },
};

/* With this definition added, any module context 
 * without 'static' storage modifier 
 * will raise a linker error.  */
void * g_Ctx = NULL;

static boolean create_modules()
{
	uint32_t i;
	INFO("Creating modules..");
	for (i = 0; i < COUNT_OF(g_Modules); i++) {
		struct module_desc * m = &g_Modules[i];
		m->module_ = ((ap_module_t *(*)())m->cb_create)();
		if (!m->module_) {
			ERROR("Failed to create module (%s).", m->name);
			return FALSE;
		}
		if (m->global_handle)
			*(ap_module_t *)m->global_handle = m->module_;
	}
	INFO("Completed module creation.");
	return TRUE;
}

static boolean register_modules(struct ap_module_registry * registry)
{
	uint32_t i;
	INFO("Registering modules..");
	for (i = 0; i < COUNT_OF(g_Modules); i++) {
		struct module_desc * m = &g_Modules[i];
		struct ap_module_instance * instance = (struct ap_module_instance *)m->module_;
		if (!ap_module_registry_register(registry, m->module_)) {
			ERROR("Module registration failed (%s).", m->name);
			return FALSE;
		}
		if (instance->cb_register && !instance->cb_register(instance, registry)) {
			ERROR("Module registration callback failed (%s).", m->name);
			return FALSE;
		}
	}
	INFO("Completed module registration.");
	return TRUE;
}

/*
 * Here we need to free resources that 
 * depend on other modules still being partly functional.
 * 
 * An example is destroying module data.
 * Because module data can have attachments, and modules 
 * that have added these attachments are shutdown earlier 
 * than the base module, we can only destroy 
 * module data BEFORE shutting down modules.
 *
 * Another example is the server module.
 * Closing a server will trigger disconnect callbacks, 
 * and these callbacks will require higher level modules 
 * to be partly functional in order to free various data 
 * such as accounts, characters, etc.
 */
static void close()
{
	int32_t i;
	INFO("Closing modules..");
	/* Close modules in reverse. */
	for (i = COUNT_OF(g_Modules) - 1; i >=0; i--) {
		const struct module_desc * m = &g_Modules[i];
		const struct ap_module_instance * instance = (struct ap_module_instance *)m->module_;
		if (instance->cb_close)
			instance->cb_close(m->module_);
	}
	INFO("Closed all modules.");
}

static void shutdown()
{
	int32_t i;
	INFO("Shutting down modules..");
	/* Shutdown modules in reverse. */
	for (i = COUNT_OF(g_Modules) - 1; i >=0; i--) {
		struct module_desc * m = &g_Modules[i];
		struct ap_module_instance * instance = (struct ap_module_instance *)m->module_;
		if (instance->cb_shutdown)
			instance->cb_shutdown(m->module_);
		ap_module_instance_destroy(m->module_);
		m->module_ = NULL;
		*(ap_module_t *)m->global_handle = NULL;
	}
	INFO("All modules are shutdown.");
}

static boolean initialize()
{
	char path[1024];
	uint32_t i;
	const char * inidir = NULL;
	const char * clientdir = NULL;
	INFO("Initializing..");
	for (i = 0; i < COUNT_OF(g_Modules); i++) {
		const struct module_desc * m = &g_Modules[i];
		const struct ap_module_instance * instance = (struct ap_module_instance *)m->module_;
		if (instance->cb_initialize && !instance->cb_initialize(m->module_)) {
			ERROR("Module initialization failed (%s).", m->name);
			return FALSE;
		}
	}
	inidir = ap_config_get(g_ApConfig, "ServerIniDir");
	if (!inidir) {
		ERROR("Failed to retrieve ServerIniDir config.");
		return FALSE;
	}
	clientdir = ap_config_get(g_ApConfig, "ClientDir");
	if (!clientdir) {
		ERROR("Failed to retrieve ClientDir config.");
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/chartype.ini", inidir)) {
		ERROR("Failed to create path (chartype.ini).");
		return FALSE;
	}
	if (!ap_factors_read_char_type(g_ApFactors, path, FALSE)) {
		ERROR("Failed to load character type names.");
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/objecttemplate.ini", inidir)) {
		ERROR("Failed to create path (objecttemplate.ini).");
		return FALSE;
	}
	if (!ap_object_load_templates(g_ApObject, path, FALSE)) {
		ERROR("Failed to load object templates.");
		return FALSE;
	}
	if (!make_path(path, sizeof(path), 
			"%s/charactertemplatepublic.ini", inidir)) {
		ERROR("Failed to create path (charactertemplatepublic.ini).");
		return FALSE;
	}
	if (!ap_character_read_templates(g_ApCharacter, path, FALSE)) {
		ERROR("Failed to read character templates (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/characterdatatable.txt", inidir)) {
		ERROR("Failed to create path (characterdatatable.txt).");
		return FALSE;
	}
	if (!ap_character_read_import_data(g_ApCharacter, path, FALSE)) {
		ERROR("Failed to read character import data (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/growupfactor.ini", inidir)) {
		ERROR("Failed to create path (growupfactor.ini).");
		return FALSE;
	}
	if (!ap_character_read_grow_up_table(g_ApCharacter, path, FALSE)) {
		ERROR("Failed to read character grow up factor (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/levelupexp.ini", inidir)) {
		ERROR("Failed to create path (levelupexp.ini).");
		return FALSE;
	}
	if (!ap_character_read_level_up_exp(g_ApCharacter, path, FALSE)) {
		ERROR("Failed to read character level up exp (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/skilltemplate.ini", inidir)) {
		ERROR("Failed to create path (skilltemplate.ini).");
		return FALSE;
	}
	if (!ap_skill_read_templates(g_ApSkill, path, FALSE)) {
		ERROR("Failed to read skill templates (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/skillspec.txt", inidir)) {
		ERROR("Failed to create path (skillspec.txt).");
		return FALSE;
	}
	if (!ap_skill_read_spec(g_ApSkill, path, FALSE)) {
		ERROR("Failed to read skill specialization (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/skillconst.txt", inidir)) {
		ERROR("Failed to create path (skillconst.txt).");
		return FALSE;
	}
	if (!ap_skill_read_const(g_ApSkill, path, FALSE)) {
		ERROR("Failed to read skill const (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/skillconst2.txt", inidir)) {
		ERROR("Failed to create path (skillconst2.txt).");
		return FALSE;
	}
	if (!ap_skill_read_const2(g_ApSkill, path, FALSE)) {
		ERROR("Failed to read skill const2 (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/itemoptiontable.txt", inidir)) {
		ERROR("Failed to create path (itemoptiontable.txt).");
		return FALSE;
	}
	if (!ap_item_read_option_data(g_ApItem, path, FALSE)) {
		ERROR("Failed to read item option data (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/itemdatatable.txt", inidir)) {
		ERROR("Failed to create path (itemdatatable.txt).");
		return FALSE;
	}
	if (!ap_item_read_import_data(g_ApItem, path, FALSE)) {
		ERROR("Failed to read item import data (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/itemlotterybox.txt", inidir)) {
		ERROR("Failed to create path (%s/itemlotterybox.txt).", inidir);
		return FALSE;
	}
	if (!ap_item_read_lottery_box(g_ApItem, path, FALSE)) {
		ERROR("Failed to read item lottery box (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/avatarset.ini", inidir)) {
		ERROR("Failed to create path (%s/avatarset.ini).", inidir);
		return FALSE;
	}
	if (!ap_item_read_avatar_set(g_ApItem, path, FALSE)) {
		ERROR("Failed to read item avatar sets (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/itemconverttable.txt", inidir)) {
		ERROR("Failed to create path (%s/itemconverttable.txt).", inidir);
		return FALSE;
	}
	if (!ap_item_convert_read_convert_table(g_ApItemConvert, path)) {
		ERROR("Failed to read item convert table (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/itemruneattributetable.txt", inidir)) {
		ERROR("Failed to create path (%s/itemruneattributetable.txt).", inidir);
		return FALSE;
	}
	if (!ap_item_convert_read_rune_attribute_table(g_ApItemConvert, path, FALSE)) {
		ERROR("Failed to read item rune attribute table (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/refineryrecipetable.txt", inidir)) {
		ERROR("Failed to create path (%s/refineryrecipetable.txt).", inidir);
		return FALSE;
	}
	if (!ap_refinery_read_recipe_table(g_ApRefinery, path, FALSE)) {
		ERROR("Failed to read refinery recipe table (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/aidatatable.txt", inidir)) {
		ERROR("Failed to create path (%s/aidatatable.txt).", inidir);
		return FALSE;
	}
	if (!ap_ai2_read_data_table(g_ApAi2, path, FALSE)) {
		ERROR("Failed to read ai data table (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/optionnumdroprate.txt", inidir)) {
		ERROR("Failed to create path (%s/optionnumdroprate.txt).", inidir);
		return FALSE;
	}
	if (!ap_drop_item_read_option_num_drop_rate(g_ApDropItem, path, FALSE)) {
		ERROR("Failed to read option number drop rates (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/socketnumdroprate.txt", inidir)) {
		ERROR("Failed to create path (%s/socketnumdroprate.txt).", inidir);
		return FALSE;
	}
	if (!ap_drop_item_read_socket_num_drop_rate(g_ApDropItem, path, FALSE)) {
		ERROR("Failed to read socket number drop rates (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/droprankrate.txt", inidir)) {
		ERROR("Failed to create path (%s/droprankrate.txt).", inidir);
		return FALSE;
	}
	if (!ap_drop_item_read_drop_rank_rate(g_ApDropItem, path, FALSE)) {
		ERROR("Failed to read item drop rank rates (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/groupdroprate.txt", inidir)) {
		ERROR("Failed to create path (%s/groupdroprate.txt).", inidir);
		return FALSE;
	}
	if (!ap_drop_item_read_drop_group_rate(g_ApDropItem, path, FALSE)) {
		ERROR("Failed to read item drop group rates (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/itemdroptable.txt", inidir)) {
		ERROR("Failed to create path (%s/itemdroptable.txt).", inidir);
		return FALSE;
	}
	if (!ap_drop_item_read_drop_table(g_ApDropItem, path, FALSE)) {
		ERROR("Failed to read item drop table (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/leveluprewardtable.txt", inidir)) {
		ERROR("Failed to create path (%s/leveluprewardtable.txt).", inidir);
		return FALSE;
	}
	if (!ap_service_npc_read_level_up_reward_table(g_ApServiceNpc, path, FALSE)) {
		ERROR("Failed to read level up reward table (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/cashmall.txt", inidir)) {
		ERROR("Failed to create path (%s/cashmall.txt).", inidir);
		return FALSE;
	}
	if (!ap_cash_mall_read_import_data(g_ApCashMall, path, FALSE)) {
		ERROR("Failed to read cash mall data (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/teleportpoint.ini", inidir)) {
		ERROR("Failed to create path (%s/teleportpoint.ini).", inidir);
		return FALSE;
	}
	if (!ap_event_teleport_read_teleport_points(g_ApEventTeleport, path, FALSE)) {
		ERROR("Failed to read teleport points (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/npctradelist.txt", inidir)) {
		ERROR("Failed to create path (%s/npctradelist.txt).", inidir);
		return FALSE;
	}
	if (!ap_event_npc_trade_read_trade_lists(g_ApEventNpcTrade, path)) {
		ERROR("Failed to read npc trade list templates.");
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/spawndatatable.txt", inidir)) {
		ERROR("Failed to create path (spawndatatable.txt).");
		return FALSE;
	}
	if (!ap_spawn_read_data(g_ApSpawn, path, FALSE)) {
		ERROR("Failed to read spawn data (%s).", path);
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/spawninstancetable.txt", inidir)) {
		ERROR("Failed to create path (spawninstancetable.txt).");
		return FALSE;
	}
	if (!ap_spawn_read_instances(g_ApSpawn, path, FALSE)) {
		ERROR("Failed to read spawn instances (%s).", path);
		return FALSE;
	}
	if (!ac_imgui_init(g_AcImgui, 255, ac_render_get_window(g_AcRender))) {
		ERROR("Failed to initialize ImGui.");
		return FALSE;
	}
	if (!make_path(path, sizeof(path), "%s/ini/charactertemplateclient.ini", clientdir)) {
		ERROR("Failed to create path (charactertemplateclient.ini).");
		return FALSE;
	}
	if (!ac_character_read_templates(g_AcCharacter, path, TRUE)) {
		WARN("Failed to read character client templates (%s), "
			"character visuals may be unavailable.", path);
	}
	if (!make_path(path, sizeof(path), "%s/npc.ini", inidir)) {
		ERROR("Failed to create path (npc.ini).");
		return FALSE;
	}
	if (!ap_character_read_static(g_ApCharacter, path, FALSE)) {
		ERROR("Failed to read static characters (%s).", path);
		return FALSE;
	}
	INFO("Completed initialization.");
	return TRUE;
}

static void logcb(
	enum LogLevel level,
	const char * file,
	uint32_t line,
	const char * message)
{
	const float * color;
	const char * label;
#ifdef _WIN32
	char str[1024];
	snprintf(str, sizeof(str), "%s:%u| %s\n", file, line,
		message);
	OutputDebugStringA(str);
#endif
	if (!g_AcConsole)
		return;
	switch (level) {
	case LOG_LEVEL_TRACE: {
		static float c[4] = { 0.54f, 0.74f, 0.71f, 1.0f };
		color = c;
		label = "TRACE";
		break;
	}
	case LOG_LEVEL_INFO: {
		static float c[4] = { 0.80f, 0.80f, 0.80f, 1.0f };
		color = c;
		label = "INFO";
		break;
	}
	case LOG_LEVEL_WARN: {
		static float c[4] = { 0.94f, 0.77f, 0.45f, 1.0f };
		color = c;
		label = "WARNING";
		break;
	}
	case LOG_LEVEL_ERROR: {
		static float c[4] = { 0.80f, 0.40f, 0.40f, 1.0f };
		color = c;
		label = "ERROR";
		break;
	}
	default: {
		static float c[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		color = c;
		label = "UNKNOWN";
		break;
	}
	}
	ac_console_println_colored(g_AcConsole, "%-10s %s", color, label, message);
}

static void updatetick(uint64_t * last, float * dt)
{
	uint64_t t = ap_tick_get(g_ApTick);
	uint64_t l = *last;
	if (t < l) {
		*dt = 0.0f;
		return;
	}
	*dt = (t - l) / 1000.0f;
	*last = t;
}

static void render(struct ac_camera * cam, float dt)
{
	static bool b = true;
	if (!ac_render_begin_frame(g_AcRender, cam, dt))
		return;
	ac_terrain_render(g_AcTerrain);
	ac_terrain_render_rough_textures(g_AcTerrain);
	ae_terrain_render(g_AeTerrain, cam);
	ac_object_render(g_AcObject);
	ae_object_render_outline(g_AeObject, cam);
	ae_octree_render(g_AeOctree, cam);
	ae_grass_edit_render(g_AeGrass, cam);
	ae_object_browser_render(g_AeObjectBrowser);
	ae_npc_render(g_AeNpc);
	ac_imgui_new_frame(g_AcImgui);
	ImGui::BeginMainMenuBar();
	if (ImGui::BeginMenu("File", true)) {
		if (ImGui::Selectable("Save"))
			ae_editor_action_commit_changes(g_AeEditorAction);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("View", true)) {
		ae_editor_action_render_view_menu(g_AeEditorAction);
		ImGui::Separator();
		if (ImGui::Selectable("FPS Counter", &g_ShowFps))
			save_config_value("ShowFps",
				g_ShowFps ? "1" : "0");
		ImGui::Separator();
		ImGui::Text("View Distance");
		{
			int vd_options[] = { 4, 6, 8, 10 };
			uint32_t vi;
			for (vi = 0; vi < 4; vi++) {
				char vd_label[32];
				snprintf(vd_label, sizeof(vd_label), "x%d",
					vd_options[vi]);
				if (ImGui::RadioButton(vd_label,
						g_ViewDistMultiplier ==
							vd_options[vi])) {
					g_ViewDistMultiplier = vd_options[vi];
					{
						char vd_buf[8];
						snprintf(vd_buf, sizeof(vd_buf),
							"%d", g_ViewDistMultiplier);
						save_config_value("ViewDistance",
							vd_buf);
					}
					{
						float vd = AP_SECTOR_WIDTH *
							(float)g_ViewDistMultiplier;
						ac_terrain_set_view_distance(
							g_AcTerrain, vd);
						ac_object_set_view_distance(
							g_AcObject, vd);
						ac_terrain_sync(g_AcTerrain,
							cam->center, TRUE);
						ac_object_sync(g_AcObject,
							cam->center, TRUE);
						ae_grass_edit_sync(g_AeGrass,
							cam->center, TRUE);
					}
				}
				if (vi < 3)
					ImGui::SameLine();
			}
		}
		ImGui::EndMenu();
	}
	ac_console_render_icon(g_AcConsole);
	ImGui::EndMainMenuBar();
	ac_imgui_begin_toolbar(g_AcImgui);
	if (ae_object_browser_is_active(g_AeObjectBrowser))
		ae_object_browser_toolbar(g_AeObjectBrowser);
	else if (ae_grass_edit_is_active(g_AeGrass))
		ae_grass_edit_toolbar(g_AeGrass);
	else if (ae_octree_is_active(g_AeOctree))
		ae_octree_toolbar(g_AeOctree);
	else
		ae_terrain_toolbar(g_AeTerrain);
	if (g_ShowFps) {
		g_StatsTimer += dt;
		if (g_StatsTimer >= 0.5f || g_StatsBuf[0] == '\0') {
			const bgfx_stats_t * stats = bgfx_get_stats();
			float fps = (dt > 0.0001f) ? (1.0f / dt) : 0.0f;
			int64_t ram_mb = 0;
			int64_t ram_total = 0;
			int64_t vram_mb = 0;
			int64_t vram_max = 0;
			g_StatsTimer = 0.0f;
			vram_mb = stats->gpuMemoryUsed / (1024 * 1024);
			vram_max = stats->gpuMemoryMax / (1024 * 1024);
#ifdef _WIN32
			{
				PROCESS_MEMORY_COUNTERS pmc;
				if (GetProcessMemoryInfo(GetCurrentProcess(),
						&pmc, sizeof(pmc)))
					ram_mb = (int64_t)(pmc.WorkingSetSize /
						(1024 * 1024));
			}
			{
				MEMORYSTATUSEX memstat;
				memstat.dwLength = sizeof(memstat);
				if (GlobalMemoryStatusEx(&memstat))
					ram_total = (int64_t)(
						memstat.ullTotalPhys /
						(1024 * 1024));
			}
			{
				FILETIME ct, et, kt, ut;
				FILETIME now_ft;
				if (GetProcessTimes(GetCurrentProcess(),
						&ct, &et, &kt, &ut)) {
					ULARGE_INTEGER kernel, user, now_time;
					SYSTEM_INFO sysinfo;
					kernel.LowPart = kt.dwLowDateTime;
					kernel.HighPart = kt.dwHighDateTime;
					user.LowPart = ut.dwLowDateTime;
					user.HighPart = ut.dwHighDateTime;
					GetSystemTimeAsFileTime(&now_ft);
					now_time.LowPart = now_ft.dwLowDateTime;
					now_time.HighPart = now_ft.dwHighDateTime;
					GetSystemInfo(&sysinfo);
					if (g_LastSysTime.QuadPart > 0) {
						ULONGLONG cpu_delta =
							(kernel.QuadPart -
								g_LastCpuKernel.QuadPart) +
							(user.QuadPart -
								g_LastCpuUser.QuadPart);
						ULONGLONG sys_delta =
							now_time.QuadPart -
							g_LastSysTime.QuadPart;
						if (sys_delta > 0) {
							g_CpuUsage = (float)(100.0 *
								(double)cpu_delta /
								(double)sys_delta /
								(double)sysinfo
									.dwNumberOfProcessors);
						}
					}
					g_LastCpuKernel = kernel;
					g_LastCpuUser = user;
					g_LastSysTime = now_time;
				}
			}
			/* Query GPU utilization via NVML */
			if (g_NvmlGetUtil && g_NvmlDevice) {
				nvmlUtilization_t util;
				if (g_NvmlGetUtil(g_NvmlDevice, &util) == 0)
					g_GpuUsage = (float)util.gpu;
			}
#endif
			snprintf(g_StatsBuf, sizeof(g_StatsBuf),
				"FPS: %.0f | CPU: %.0f%% | GPU: %.0f%% | RAM: %lld/%lldMB | VRAM: %lld/%lldMB",
				fps, g_CpuUsage, g_GpuUsage,
				(long long)ram_mb, (long long)ram_total,
				(long long)vram_mb, (long long)vram_max);
			g_StatsBufWidth =
				ImGui::CalcTextSize(g_StatsBuf).x + 16.0f;
		}
		ImGui::SameLine(
			ImGui::GetWindowWidth() - g_StatsBufWidth);
		ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f),
			"%s", g_StatsBuf);
	}
	ac_imgui_end_toolbar(g_AcImgui);
	ac_imgui_viewport(g_AcImgui);
	ac_console_render(g_AcConsole);
	ae_terrain_imgui(g_AeTerrain);
	ae_object_imgui(g_AeObject);
	ae_octree_imgui(g_AeOctree);
	ae_grass_edit_imgui(g_AeGrass);
	ae_object_browser_imgui(g_AeObjectBrowser);
	ae_editor_action_render_editors(g_AeEditorAction);
	ae_editor_action_render_outliner(g_AeEditorAction);
	ae_editor_action_render_properties(g_AeEditorAction);
	ac_imgui_begin_toolbox(g_AcImgui);
	ae_terrain_toolbox(g_AeTerrain);
	ae_octree_toolbox(g_AeOctree);
	ae_grass_edit_toolbox(g_AeGrass);
	ae_object_browser_toolbox(g_AeObjectBrowser);
	ac_imgui_end_toolbox(g_AcImgui);
	ae_editor_action_render_add_menu(g_AeEditorAction);
	ac_imgui_render(g_AcImgui);
	//ar_dd_begin();
	//ar_dd_end();
	ac_render_end_frame(g_AcRender);
}

static void on_keydown_cam(
	struct camera_controls * ctrl,
	const boolean * key_state,
	const SDL_KeyboardEvent * e)
{
	if (e->repeat ||
		key_state[SDL_SCANCODE_LCTRL] ||
		key_state[SDL_SCANCODE_RCTRL])
		return;
	ctrl->key_state[e->keysym.scancode] = TRUE;
}

static void on_keyup_cam(
	struct camera_controls * ctrl,
	const SDL_KeyboardEvent * e)
{
	ctrl->key_state[e->keysym.scancode] = FALSE;
}

static void update_camera(
	struct ac_camera * cam,
	struct camera_controls * ctrl,
	float dt)
{
	const boolean * key_state = ctrl->key_state;
	float speed =
		key_state[SDL_SCANCODE_LSHIFT] ? 30000.0f : 10000.0f;
	uint32_t mb_state = SDL_GetMouseState(NULL, NULL);
	if (key_state[SDL_SCANCODE_W])
		ac_camera_translate(cam, dt * speed, 0.f, 0.f);
	else if (key_state[SDL_SCANCODE_S])
		ac_camera_translate(cam, -dt * speed, 0.f, 0.f);
	if (key_state[SDL_SCANCODE_D])
		ac_camera_translate(cam, 0.f, dt * speed, 0.f);
	else if (key_state[SDL_SCANCODE_A])
		ac_camera_translate(cam, 0.f, -dt * speed, 0.f);
	if (key_state[SDL_SCANCODE_E])
		ac_camera_translate(cam, 0.f, 0.f, dt * speed);
	else if (key_state[SDL_SCANCODE_Q])
		ac_camera_translate(cam, 0.f, 0.f, -dt * speed);
	if ((mb_state & SDL_BUTTON(SDL_BUTTON_LEFT)) &&
		(mb_state & SDL_BUTTON(SDL_BUTTON_RIGHT))) {
		ac_camera_translate(cam, 
			speed * dt, 0.0f, 0.0f);
	}
	ac_camera_update(cam, dt);
}

static void setspawnpos(struct ac_camera * cam)
{
	vec3 center = { -462922.5f, 3217.9f, -44894.8f };
	const char * pos_cfg = ap_config_get(g_ApConfig, "EditorStartPosition");
	if (pos_cfg) {
		vec3 c = { 0 };
		int parsed = sscanf(pos_cfg, "%f,%f,%f", &c[0], &c[1], &c[2]);
		if (parsed == 3)
			glm_vec3_copy(c, center);
	}
	ac_camera_init(cam, center, 5000.0f, 45.0f, 30.0f, 60.0f, 100.0f, 400000.0f);
	ac_terrain_sync(g_AcTerrain, cam->center, TRUE);
	ac_object_sync(g_AcObject, cam->center, TRUE);
	ae_grass_edit_sync(g_AeGrass, cam->center, TRUE);
}
/* REMOVED: find_region_sectors, apply_region_filter */
#if 0
	double sum_x = 0.0;
	double sum_z = 0.0;
	uint32_t match_count = 0;
	float cx, cz;
	uint32_t cx_sx, cx_sz;
	float p_centroid[3];
	const float max_radius = AP_SECTOR_WIDTH * AP_SECTOR_DEFAULT_DEPTH * 3.0f;
	uint32_t smin_x = UINT32_MAX, smin_z = UINT32_MAX;
	uint32_t smax_x = 0, smax_z = 0;
	uint32_t gw, gh;
	boolean * grid;
	uint32_t grid_count = 0;
	if (!node_path) {
		ERROR("find_region_sectors: ServerNodePath not set in config.");
		return FALSE;
	}
	if (!get_file_size(node_path, &size) || !size) {
		ERROR("find_region_sectors: Failed to get file size (%s).",
			node_path);
		return FALSE;
	}
	data = alloc(size);
	if (!load_file(node_path, data, size)) {
		ERROR("find_region_sectors: Failed to load (%s).", node_path);
		dealloc(data);
		return FALSE;
	}
	/* Pass 1: compute centroid from all matching segments. */
	cursor = (uint32_t *)data;
	{
		size_t remaining = size;
		while (remaining >= 1032) {
			uint32_t x = *cursor++;
			uint32_t z = *cursor++;
			uint32_t si;
			for (si = 0; si < 256; si++) {
				uint16_t region_id;
				memcpy(&region_id, (uint8_t *)cursor + 2, 2);
				if ((region_id & 0x00FF) == target_region_id) {
					uint32_t sx = si % 16;
					uint32_t sz = si / 16;
					sum_x += AP_SECTOR_WORLD_START_X +
						x * AP_SECTOR_WIDTH +
						sx * AP_SECTOR_STEPSIZE +
						AP_SECTOR_STEPSIZE * 0.5f;
					sum_z += AP_SECTOR_WORLD_START_Z +
						z * AP_SECTOR_HEIGHT +
						sz * AP_SECTOR_STEPSIZE +
						AP_SECTOR_STEPSIZE * 0.5f;
					match_count++;
				}
				cursor++;
			}
			remaining -= 1032;
		}
	}
	if (match_count == 0) {
		INFO("find_region_sectors: no segments for region_id=%u",
			target_region_id);
		dealloc(data);
		return FALSE;
	}
	cx = (float)(sum_x / match_count);
	cz = (float)(sum_z / match_count);
	p_centroid[0] = cx;
	p_centroid[1] = 0.0f;
	p_centroid[2] = cz;
	if (!ap_scr_pos_to_index(p_centroid, &cx_sx, &cx_sz)) {
		dealloc(data);
		return FALSE;
	}
	INFO("find_region_sectors: centroid=[%.0f,%.0f] sector=[%u,%u] "
		"(%u segments)", cx, cz, cx_sx, cx_sz, match_count);
	/* Pass 2: find sectors near centroid that have ANY
	 * matching segment, compute grid bounds. */
	cursor = (uint32_t *)data;
	{
		size_t remaining = size;
		while (remaining >= 1032) {
			uint32_t x = *cursor++;
			uint32_t z = *cursor++;
			uint32_t si;
			boolean has_match = FALSE;
			float dx, dz;
			for (si = 0; si < 256; si++) {
				uint16_t region_id;
				memcpy(&region_id, (uint8_t *)cursor + 2, 2);
				if ((region_id & 0x00FF) == target_region_id)
					has_match = TRUE;
				cursor++;
			}
			dx = (AP_SECTOR_WORLD_START_X +
				x * AP_SECTOR_WIDTH + AP_SECTOR_WIDTH * 0.5f) - cx;
			dz = (AP_SECTOR_WORLD_START_Z +
				z * AP_SECTOR_HEIGHT + AP_SECTOR_HEIGHT * 0.5f) - cz;
			if (has_match &&
				dx * dx + dz * dz <= max_radius * max_radius) {
				if (x < smin_x) smin_x = x;
				if (z < smin_z) smin_z = z;
				if (x > smax_x) smax_x = x;
				if (z > smax_z) smax_z = z;
			}
			remaining -= 1032;
		}
	}
	if (smin_x > smax_x) {
		dealloc(data);
		return FALSE;
	}
	gw = smax_x - smin_x + 1;
	gh = smax_z - smin_z + 1;
	grid = (boolean *)alloc(gw * gh * sizeof(boolean));
	memset(grid, 0, gw * gh * sizeof(boolean));
	/* Pass 3: fill grid. */
	cursor = (uint32_t *)data;
	{
		size_t remaining = size;
		while (remaining >= 1032) {
			uint32_t x = *cursor++;
			uint32_t z = *cursor++;
			uint32_t si;
			boolean has_match = FALSE;
			float dx, dz;
			for (si = 0; si < 256; si++) {
				uint16_t region_id;
				memcpy(&region_id, (uint8_t *)cursor + 2, 2);
				if ((region_id & 0x00FF) == target_region_id)
					has_match = TRUE;
				cursor++;
			}
			dx = (AP_SECTOR_WORLD_START_X +
				x * AP_SECTOR_WIDTH + AP_SECTOR_WIDTH * 0.5f) - cx;
			dz = (AP_SECTOR_WORLD_START_Z +
				z * AP_SECTOR_HEIGHT + AP_SECTOR_HEIGHT * 0.5f) - cz;
			if (has_match &&
				dx * dx + dz * dz <= max_radius * max_radius) {
				uint32_t gx = x - smin_x;
				uint32_t gz = z - smin_z;
				grid[gz * gw + gx] = TRUE;
				grid_count++;
			}
			remaining -= 1032;
		}
	}
	/* Pass 4: build per-segment mask texture.
	 * Texture must be SQUARE because UV mapping uses a
	 * single length = max(extent_x, extent_z) for both axes
	 * (same convention as ae_terrain.cpp rebuild_segment_textures). */
	{
		uint32_t tex_dim = (gw > gh ? gw : gh) * 16;
		uint8_t * mask = (uint8_t *)alloc(tex_dim * tex_dim);
		float begin_x = AP_SECTOR_WORLD_START_X +
			smin_x * AP_SECTOR_WIDTH;
		float begin_z = AP_SECTOR_WORLD_START_Z +
			smin_z * AP_SECTOR_HEIGHT;
		float end_x = AP_SECTOR_WORLD_START_X +
			(smax_x + 1) * AP_SECTOR_WIDTH;
		float end_z = AP_SECTOR_WORLD_START_Z +
			(smax_z + 1) * AP_SECTOR_HEIGHT;
		float len = end_x - begin_x;
		float len_z = end_z - begin_z;
		if (len_z > len)
			len = len_z;
		memset(mask, 0, tex_dim * tex_dim);
		cursor = (uint32_t *)data;
		{
			size_t remaining = size;
			while (remaining >= 1032) {
				uint32_t x = *cursor++;
				uint32_t z = *cursor++;
				if (x >= smin_x && x <= smax_x &&
					z >= smin_z && z <= smax_z &&
					grid[(z - smin_z) * gw + (x - smin_x)]) {
					uint32_t si;
					for (si = 0; si < 256; si++) {
						uint16_t region_id;
						memcpy(&region_id,
							(uint8_t *)cursor + 2, 2);
						if ((region_id & 0x00FF) ==
							target_region_id) {
							uint32_t sx = si % 16;
							uint32_t sz = si / 16;
							uint32_t mx = (x - smin_x) * 16 + sx;
							uint32_t mz = (z - smin_z) * 16 + sz;
							mask[mz * tex_dim + mx] = 255;
						}
						cursor++;
					}
				} else {
					cursor += 256;
				}
				remaining -= 1032;
			}
		}
		result->segment_mask = mask;
		result->mask_width = tex_dim;
		result->mask_height = tex_dim;
		result->mask_begin[0] = begin_x;
		result->mask_begin[1] = begin_z;
		result->mask_length = len;
	}
	dealloc(data);
	result->centroid[0] = cx;
	result->centroid[1] = cz;
	result->grid_min_x = smin_x;
	result->grid_min_z = smin_z;
	result->grid_width = gw;
	result->grid_height = gh;
	result->grid = grid;
	INFO("find_region_sectors: grid [%u,%u] size %ux%u, "
		"%u sectors marked, mask %ux%u",
		smin_x, smin_z, gw, gh, grid_count,
		result->mask_width, result->mask_height);
	return TRUE;
}

static void apply_region_filter(
	uint32_t region_id,
	struct ac_camera * cam)
{
	if (region_id == UINT32_MAX) {
		/* Clear all filters. */
		ac_terrain_clear_sector_grid(g_AcTerrain);
		ac_terrain_clear_segment_mask(g_AcTerrain);
		ac_object_clear_sector_grid(g_AcObject);
		ac_object_clear_segment_mask(g_AcObject);
		g_RegionLock = FALSE;
		ac_terrain_set_view_distance(g_AcTerrain,
			AP_SECTOR_WIDTH * (float)g_ViewDistMultiplier);
		ac_object_set_view_distance(g_AcObject,
			AP_SECTOR_WIDTH * (float)g_ViewDistMultiplier);
		ac_terrain_sync(g_AcTerrain, cam->center, TRUE);
		ac_object_sync(g_AcObject, cam->center, TRUE);
		ae_grass_edit_sync(g_AeGrass, cam->center, TRUE);
		return;
	}
	{
		struct region_scan_result scan = { 0 };
		if (find_region_sectors(region_id, &scan)) {
			float vd;
			g_RegionLock = TRUE;
			cam->center[0] = scan.centroid[0];
			cam->center[2] = scan.centroid[1];
			cam->center_dst[0] = scan.centroid[0];
			cam->center_dst[2] = scan.centroid[1];
			g_RegionMin[0] = ap_scr_get_start_x(scan.grid_min_x);
			g_RegionMin[1] = ap_scr_get_start_z(scan.grid_min_z);
			g_RegionMax[0] = ap_scr_get_end_x(
				scan.grid_min_x + scan.grid_width - 1);
			g_RegionMax[1] = ap_scr_get_end_z(
				scan.grid_min_z + scan.grid_height - 1);
			ac_terrain_set_sector_grid(g_AcTerrain,
				scan.grid_min_x, scan.grid_min_z,
				scan.grid_width, scan.grid_height, scan.grid);
			ac_object_set_sector_grid(g_AcObject,
				scan.grid_min_x, scan.grid_min_z,
				scan.grid_width, scan.grid_height, scan.grid);
			ac_terrain_set_segment_mask(g_AcTerrain,
				scan.segment_mask,
				scan.mask_width, scan.mask_height,
				scan.mask_begin[0], scan.mask_begin[1],
				scan.mask_length);
			ac_object_set_segment_mask(g_AcObject,
				scan.segment_mask,
				scan.mask_width, scan.mask_height,
				scan.mask_begin[0], scan.mask_begin[1],
				scan.mask_length);
			vd = ((scan.grid_width > scan.grid_height ?
				scan.grid_width : scan.grid_height) *
				AP_SECTOR_WIDTH) * 0.5f + AP_SECTOR_WIDTH;
			ac_terrain_set_view_distance(g_AcTerrain, vd);
			ac_object_set_view_distance(g_AcObject, vd);
			INFO("apply_region_filter: region=%u grid [%u,%u] %ux%u "
				"view_distance=%.0f",
				region_id, scan.grid_min_x, scan.grid_min_z,
				scan.grid_width, scan.grid_height, vd);
			ac_terrain_sync(g_AcTerrain, cam->center, TRUE);
			ac_object_sync(g_AcObject, cam->center, TRUE);
			ae_grass_edit_sync(g_AeGrass, cam->center, TRUE);
			dealloc(scan.grid);
			dealloc(scan.segment_mask);
		} else {
			WARN("apply_region_filter: region %u not found in node_data.",
				region_id);
		}
	}
}
#endif

static void handleinput(
	SDL_Event * e, 
	struct ac_camera * cam,
	struct camera_controls * ctrl)
{
	const boolean * state = ac_render_get_key_state(g_AcRender);
	if (ac_imgui_process_event(g_AcImgui, e))
		return;
	if (ac_render_process_window_event(g_AcRender, e))
		return;
	switch (e->type) {
	case SDL_MOUSEMOTION:
		if (ac_render_button_down(g_AcRender, AC_RENDER_BUTTON_RIGHT)) {
			ac_camera_rotate(cam, e->motion.yrel * .3f, e->motion.xrel * .3f);
		}
		else if (ac_render_button_down(g_AcRender, AC_RENDER_BUTTON_MIDDLE)) {
			ac_camera_slide(cam, -(float)e->motion.xrel * 10.f,
				(float)e->motion.yrel * 10.f);
		}
		else {
			if (ae_terrain_on_mmove(g_AeTerrain, cam, e->motion.x, e->motion.y))
				break;
		}
		break;
	case SDL_MOUSEWHEEL:
		if (!ae_terrain_on_mwheel(g_AeTerrain, e->wheel.preciseY))
			ac_camera_zoom(cam, e->wheel.preciseY * 1000.f);
		break;
	case SDL_MOUSEBUTTONDOWN:
		if (e->button.button == SDL_BUTTON_LEFT) {
			uint32_t mb_state = SDL_GetMouseState(NULL, NULL);
			if (mb_state & SDL_BUTTON(SDL_BUTTON_RIGHT))
				break;
			ae_grass_edit_on_mdown(g_AeGrass, cam, e->button.x, e->button.y);
			ae_terrain_on_mdown(g_AeTerrain, cam, e->button.x, e->button.y);
			ae_editor_action_pick(g_AeEditorAction, cam, e->button.x, e->button.y);
		}
		break;
	case SDL_KEYDOWN:
		if (ae_grass_edit_on_key_down(g_AeGrass, e->key.keysym.sym))
			break;
		if (ae_terrain_on_key_down(g_AeTerrain, e->key.keysym.sym))
			break;
		if (ae_object_on_key_down(g_AeObject, cam, e->key.keysym.sym))
			break;
		on_keydown_cam(ctrl, ac_render_get_key_state(g_AcRender), &e->key);
		break;
	case SDL_KEYUP:
		on_keyup_cam(ctrl, &e->key);
		break;
	}
	ae_editor_action_handle_input(g_AeEditorAction, e);
}

int main(int argc, char * argv[])
{
	uint64_t last = 0;
	float dt = 0.0f;
	float accum = 0.0f;
	struct ap_module_registry * registry = NULL;
	struct ac_camera * cam;
	struct camera_controls cam_ctrl = { 0 };
	if (!log_init()) {
		fprintf(stderr, "log_init() failed.\n");
		return -1;
	}
	log_set_callback(logcb);
	if (!core_startup()) {
		ERROR("Failed to startup core module.");
		return -1;
	}
	if (!task_startup()) {
		ERROR("Failed to startup task module.");
		return -1;
	}
	if (!create_modules()) {
		ERROR("Module creation failed.");
		return -1;
	}
	registry = ap_module_registry_new();
	if (!register_modules(registry)) {
		ERROR("Failed to register modules.");
		return -1;
	}
	if (!initialize()) {
		ERROR("Failed to initialize.");
		return -1;
	}
	cam = ac_camera_get_main(g_AcCamera);
	setspawnpos(cam);
	{
		const char * vd_str = ap_config_get(g_ApConfig,
			"ViewDistance");
		if (vd_str) {
			int vd = atoi(vd_str);
			if (vd == 4 || vd == 6 || vd == 8 || vd == 10) {
				g_ViewDistMultiplier = vd;
				ac_terrain_set_view_distance(g_AcTerrain,
					AP_SECTOR_WIDTH * (float)vd);
				ac_object_set_view_distance(g_AcObject,
					AP_SECTOR_WIDTH * (float)vd);
			}
		}
	}
	{
		const char * fps_str = ap_config_get(g_ApConfig,
			"ShowFps");
		if (fps_str && atoi(fps_str))
			g_ShowFps = true;
	}
#ifdef _WIN32
	init_nvml();
#endif
	last = ap_tick_get(g_ApTick);
	INFO("Entering main loop..");
	while (!core_should_shutdown()) {
		uint64_t tick = ap_tick_get(g_ApTick);
		SDL_Event e;
		updatetick(&last, &dt);
		while (ac_render_poll_window_event(g_AcRender, &e))
			handleinput(&e, cam, &cam_ctrl);
		accum += dt;
		while (accum >= STEPTIME) {
			accum -= STEPTIME;
		}
		update_camera(cam, &cam_ctrl, dt);
		ac_terrain_sync(g_AcTerrain, cam->center, FALSE);
		ac_terrain_update(g_AcTerrain, dt);
		ac_object_sync(g_AcObject, cam->center, FALSE);
		ac_object_update(g_AcObject, dt);
		ae_grass_edit_sync(g_AeGrass, cam->center, FALSE);
		ae_terrain_update(g_AeTerrain, dt);
		ae_texture_process_loading_queue(g_AeTexture, 5);
		ae_object_update(g_AeObject, dt);
		render(cam, dt);
		task_do_post_cb();
		sleep(1);
	}
	INFO("Exited main loop.");
	close();
	shutdown();
	return 0;
}