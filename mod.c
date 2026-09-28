#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_core.h"
#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/type.h"

__attribute__((visibility("default")))
void (*mod_logger_write)(mod_log_level_t, const char*, const char*, ...) = NULL;

static patch_handle_t g_max_spawns_field = PATCH_NULL;
static patch_handle_t g_spawn_rate_field = PATCH_NULL;
static patch_handle_t g_summon_item_check_method = PATCH_NULL;
static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_summon_item_check_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_can_use_inner_hook = PATCH_HOOK_INVALID_ID;
static bool g_in_boss_spawn = false;
static bool g_in_boss_duplicate = false;

static int g_multiplier = 5;
static int g_boss_multiplier = 2;
static int g_event_multiplier = 5;
static int g_friendly_multiplier = 1;
static int g_total_npc_limit = 200;
static bool g_enable_normal = true;
static bool g_enable_boss = true;
static bool g_enable_event = false;
static bool g_enable_friendly = false;
static bool g_enable_total_limit = false;
static int g_base_max_spawns = 0;
static int g_base_spawn_rate = 0;
static bool g_applied = false;

static kernel_mod_info_t g_info = {
    .pkg_id = "liuxin.myriadlife",
    .version_code = 202609280,
    .api_version = 1,
    .version = "1.0.1"
};

static void log_msg(mod_log_level_t level, const char* fmt, ...) {
    if (!mod_logger_write) return;
    char message[320];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    mod_logger_write(level, "MyriadLife", "%s", message);
}

static int clamp_multiplier(int value) {
    if (value < 1) return 1;
    if (value > 20) return 20;
    return value;
}

static int read_setting(const char* text, const char* key, int fallback) {
    char needle[96];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* key_pos = strstr(text, needle);
    if (!key_pos) return fallback;

    const char* colon = strchr(key_pos + strlen(needle), ':');
    if (!colon) return fallback;

    char* end = NULL;
    long value = strtol(colon + 1, &end, 10);
    if (end == colon + 1) return fallback;
    if (value < 1) value = 1;
    if (value > 20) value = 20;
    return (int)value;
}

static bool read_bool_setting(const char* text, const char* key, bool fallback) {
    char needle[96];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* key_pos = strstr(text, needle);
    if (!key_pos) return fallback;
    const char* colon = strchr(key_pos + strlen(needle), ':');
    if (!colon) return fallback;
    while (*++colon == ' ' || *colon == '\t') {}
    if (strncmp(colon, "true", 4) == 0) return true;
    if (strncmp(colon, "false", 5) == 0) return false;
    return fallback;
}

static void load_config(const char* private_dir) {
    if (!private_dir || !*private_dir) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/config.json", private_dir);
    FILE* file = fopen(path, "rb");
    if (!file) return;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long size = ftell(file);
    if (size <= 0 || size > 65536 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return;
    }

    char* text = (char*)malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return;
    }

    size_t read_count = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[read_count] = '\0';

    g_multiplier = clamp_multiplier(
        read_setting(text, "spawn_multiplier", g_multiplier));
    g_boss_multiplier = clamp_multiplier(read_setting(text, "boss_multiplier", g_boss_multiplier));
    g_event_multiplier = clamp_multiplier(read_setting(text, "event_multiplier", g_event_multiplier));
    g_friendly_multiplier = clamp_multiplier(read_setting(text, "friendly_multiplier", g_friendly_multiplier));
    g_total_npc_limit = read_setting(text, "total_npc_limit", g_total_npc_limit);
    if (g_total_npc_limit < 20) g_total_npc_limit = 20;
    if (g_total_npc_limit > 200) g_total_npc_limit = 200;
    g_enable_normal = read_bool_setting(text, "enable_normal", g_enable_normal);
    g_enable_boss = read_bool_setting(text, "enable_boss", g_enable_boss);
    g_enable_event = read_bool_setting(text, "enable_event", g_enable_event);
    g_enable_friendly = read_bool_setting(text, "enable_friendly", g_enable_friendly);
    g_enable_total_limit = read_bool_setting(text, "enable_total_limit", g_enable_total_limit);
    free(text);

    log_msg(MOD_LOG_LEVEL_INFO, "配置已读取: spawn_multiplier=%d", g_multiplier);
}

static int read_static_int(patch_handle_t field, int fallback) {
    if (!field) return fallback;
    int value = fallback;
    patchlib_field_get_value(field, PATCH_NULL, &value);
    return value;
}

static void write_static_int(patch_handle_t field, int value) {
    if (!field) return;
    patchlib_field_set_value(field, PATCH_NULL, &value);
}

static void apply_spawn_multiplier(void) {
    if (!g_max_spawns_field || !g_spawn_rate_field) return;

    if (g_base_max_spawns <= 0) {
        g_base_max_spawns = read_static_int(g_max_spawns_field, 0);
    }
    if (g_base_spawn_rate <= 0) {
        g_base_spawn_rate = read_static_int(g_spawn_rate_field, 0);
    }
    if (g_base_max_spawns <= 0 || g_base_spawn_rate <= 0) return;

    int multiplier = g_enable_normal ? g_multiplier : 1;
    int target_max = g_base_max_spawns * multiplier;
    if (target_max < 1) target_max = 1;
    if (target_max > 999) target_max = 999;

    int target_rate = g_base_spawn_rate / multiplier;
    if (target_rate < 1) target_rate = 1;

    int current_max = read_static_int(g_max_spawns_field, 0);
    int current_rate = read_static_int(g_spawn_rate_field, 0);
    if (current_max != target_max) write_static_int(g_max_spawns_field, target_max);
    if (current_rate != target_rate) write_static_int(g_spawn_rate_field, target_rate);

    if (!g_applied) {
        log_msg(MOD_LOG_LEVEL_INFO,
                "刷怪设置已应用: max=%d -> %d, rate=%d -> %d, multiplier=%d",
                g_base_max_spawns, target_max,
                g_base_spawn_rate, target_rate, multiplier);
        g_applied = true;
    }
}

static void main_update_postfix(patch_handle_t instance, void** args,
                                void* result,
                                const patch_method_signature_t* signature) {
    (void)instance;
    (void)args;
    (void)result;
    (void)signature;
    apply_spawn_multiplier();
}

static bool can_use_inner_prefix(patch_handle_t instance, void** args,
                                const patch_method_signature_t* signature,
                                void* result) {
    (void)instance; (void)args; (void)signature;
    if (!g_in_boss_duplicate || !result) return true;
    *(bool*)result = true;
    return false;
}

static void summon_item_check_postfix(patch_handle_t instance, void** args,
                                      void* result,
                                      const patch_method_signature_t* signature) {
    (void)result; (void)signature;
    if (!g_enable_boss || g_in_boss_spawn || !g_summon_item_check_method) return;

    int multiplier = g_boss_multiplier;
    if (multiplier < 1) multiplier = 1;
    if (multiplier > 5) multiplier = 5;
    if (multiplier <= 1) return;

    g_in_boss_spawn = true;
    g_in_boss_duplicate = true;
    for (int i = 1; i < multiplier; ++i) {
        patchlib_method_invoke_args(g_summon_item_check_method, instance,
                                    NULL, args);
    }
    g_in_boss_duplicate = false;
    g_in_boss_spawn = false;
}

static void init_mod(kernel_mod_handle_t* handle) {
    if (!handle) return;
    load_config(handle->private_dir);

    patch_handle_t npc_type = patchlib_type_get_type("Terraria", "NPC");
    if (!npc_type) {
        log_msg(MOD_LOG_LEVEL_ERROR, "找不到 Terraria.NPC 类型");
        return;
    }

    g_max_spawns_field = patchlib_type_get_field(npc_type, "defaultMaxSpawns");
    g_spawn_rate_field = patchlib_type_get_field(npc_type, "defaultSpawnRate");

    patch_handle_t update_method =
        patchlib_type_get_method_by_param_count(npc_type, "UpdateFoundActiveNPCs", 0);
    if (update_method) {
        g_update_hook = patchlib_install_prepost_hook(
            update_method, NULL, main_update_postfix);
    }
    patchlib_free(update_method);

    /* Mirror the reference mod's approach: duplicate through the player's
       summon-item path and bypass the boss-present gate only during repeats. */
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    if (player_type) {
        g_summon_item_check_method =
            patchlib_type_get_method_by_param_count(player_type, "SummonItemCheck", 1);
        patch_handle_t can_use_method =
            patchlib_type_get_method_by_param_count(player_type, "ItemCheck_CheckCanUse_Inner", 1);
        if (g_summon_item_check_method) {
            g_summon_item_check_hook = patchlib_install_prepost_hook(
                g_summon_item_check_method, NULL, summon_item_check_postfix);
        }
        if (can_use_method) {
            g_can_use_inner_hook = patchlib_install_prepost_hook(
                can_use_method, can_use_inner_prefix, NULL);
        }
        patchlib_free(can_use_method);
        patchlib_free(player_type);
    }
    if (!g_summon_item_check_method || g_summon_item_check_hook == PATCH_HOOK_INVALID_ID) {
        log_msg(MOD_LOG_LEVEL_WARNING, "Boss召唤入口未找到，Boss召唤翻倍未启用");
    } else {
        log_msg(MOD_LOG_LEVEL_INFO, "Boss召唤翻倍钩子已安装，倍率=%d", g_boss_multiplier);
    }
    patchlib_free(npc_type);

    if (!g_max_spawns_field || !g_spawn_rate_field) {
        log_msg(MOD_LOG_LEVEL_ERROR,
                "找不到 Terraria.NPC.defaultMaxSpawns/defaultSpawnRate");
        return;
    }

    apply_spawn_multiplier();
    log_msg(MOD_LOG_LEVEL_INFO, "MyriadLife 初始化完成");
}

static void cleanup_mod(kernel_mod_handle_t* handle) {
    (void)handle;
    if (g_update_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_update_hook);
    }
    if (g_summon_item_check_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_summon_item_check_hook);
    }
    if (g_can_use_inner_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_can_use_inner_hook);
    }

    patchlib_free(g_max_spawns_field);
    patchlib_free(g_spawn_rate_field);
    patchlib_free(g_summon_item_check_method);

    g_max_spawns_field = PATCH_NULL;
    g_spawn_rate_field = PATCH_NULL;
    g_update_hook = PATCH_HOOK_INVALID_ID;
    g_summon_item_check_hook = PATCH_HOOK_INVALID_ID;
    g_can_use_inner_hook = PATCH_HOOK_INVALID_ID;
    g_base_max_spawns = 0;
    g_base_spawn_rate = 0;
    g_applied = false;
}

static kernel_mod_info_t* get_info(void) {
    return &g_info;
}

static kernel_mod_ops_t g_ops = {
    init_mod,
    cleanup_mod,
    get_info
};

kernel_mod_ops_t* create_kernel_mod(void) {
    return &g_ops;
}
