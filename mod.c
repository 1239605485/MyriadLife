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
#include "tefkernel/patchlib/struct/array.h"

__attribute__((visibility("default")))
void (*mod_logger_write)(mod_log_level_t, const char*, const char*, ...) = NULL;

static patch_handle_t g_max_spawns_field = PATCH_NULL;
static patch_handle_t g_spawn_rate_field = PATCH_NULL;
static patch_handle_t g_main_npc_array_field = PATCH_NULL;
static patch_handle_t g_npc_active_field = PATCH_NULL;
static patch_handle_t g_npc_boss_field = PATCH_NULL;
static patch_handle_t g_npc_friendly_field = PATCH_NULL;
static patch_handle_t g_npc_town_field = PATCH_NULL;
static patch_handle_t g_new_npc_method = PATCH_NULL;
static patch_handle_t g_spawn_on_player_method = PATCH_NULL;
static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_new_npc_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_spawn_npc_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_spawn_on_player_hook = PATCH_HOOK_INVALID_ID;
static bool g_in_natural_spawn = false;
static bool g_in_boss_spawn = false;

static int g_multiplier = 5;
static int g_boss_multiplier = 5;
static int g_event_multiplier = 5;
static int g_friendly_multiplier = 1;
static int g_total_npc_limit = 200;
static bool g_enable_normal = true;
static bool g_enable_boss = false;
static bool g_enable_event = false;
static bool g_enable_friendly = false;
static bool g_enable_total_limit = false;
static int g_base_max_spawns = 0;
static int g_base_spawn_rate = 0;
static bool g_applied = false;

static kernel_mod_info_t g_info = {
    .pkg_id = "liuxin.myriadlife",
    .version_code = 202609220,
    .api_version = 1,
    .version = "1.0.0"
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

static int active_npc_count(void) {
    if (!g_main_npc_array_field || !g_npc_active_field) return 0;
    patch_handle_t array = PATCH_NULL;
    patchlib_field_get_value(g_main_npc_array_field, PATCH_NULL, &array);
    if (!array) return 0;
    size_t length = patchlib_array_length(array);
    int count = 0;
    for (size_t i = 0; i < length; ++i) {
        patch_handle_t npc = PATCH_NULL;
        bool active = false;
        if (patchlib_array_at(array, i, &npc) && npc) {
            patchlib_field_get_value(g_npc_active_field, npc, &active);
            if (active) ++count;
        }
    }
    return count;
}

static bool new_npc_prefix(patch_handle_t instance, void** args,
                           const patch_method_signature_t* signature,
                           void* result) {
    (void)instance; (void)signature;
    if (!args || !g_enable_total_limit || g_total_npc_limit <= 0) return true;
    if (active_npc_count() >= g_total_npc_limit) {
        if (result) *(int*)result = -1;
        return false;
    }
    return true;
}

static bool spawn_npc_prefix(patch_handle_t instance, void** args,
                             const patch_method_signature_t* signature,
                             void* result) {
    (void)instance; (void)args; (void)signature; (void)result;
    g_in_natural_spawn = true;
    return true;
}

static void spawn_npc_postfix(patch_handle_t instance, void** args,
                              void* result,
                              const patch_method_signature_t* signature) {
    (void)instance; (void)args; (void)result; (void)signature;
    g_in_natural_spawn = false;
}

static bool is_summonable_boss_type(int npc_type) {
    switch (npc_type) {
        case 4:   /* Eye of Cthulhu */
        case 13:  /* Eater of Worlds */
        case 35:  /* Skeletron */
        case 50:  /* King Slime */
        case 113: /* Wall of Flesh */
        case 125: /* Retinazer */
        case 126: /* Spazmatism */
        case 127: /* Skeletron Prime */
        case 134: /* The Destroyer */
        case 222: /* Plantera */
        case 245: /* Golem */
        case 262: /* Queen Bee */
        case 266: /* Brain of Cthulhu */
        case 398: /* Duke Fishron */
        case 439: /* Lunatic Cultist */
        case 657: /* Empress of Light */
        case 668: /* Queen Slime */
        case 636: /* Deerclops */
            return true;
        default:
            return false;
    }
}

static bool spawn_on_player_prefix(patch_handle_t instance, void** args,
                                   const patch_method_signature_t* signature,
                                   void* result) {
    (void)instance; (void)signature; (void)result;
    if (g_in_boss_spawn || !g_enable_boss || !args || !args[1] ||
        !g_spawn_on_player_method) return true;

    int npc_type = *(int*)args[1];
    if (!is_summonable_boss_type(npc_type)) return true;

    int multiplier = g_boss_multiplier;
    if (multiplier < 1) multiplier = 1;
    if (multiplier > 5) multiplier = 5;
    if (multiplier == 1) return true;

    g_in_boss_spawn = true;
    for (int i = 1; i < multiplier; ++i) {
        patchlib_method_invoke_args(g_spawn_on_player_method, PATCH_NULL,
                                    NULL, args);
    }
    g_in_boss_spawn = false;
    return true;
}

static void new_npc_postfix(patch_handle_t instance, void** args,
                            void* result,
                            const patch_method_signature_t* signature) {
    (void)instance; (void)signature;
    static bool duplicating = false;
    if (duplicating || g_in_natural_spawn || !args || !result || !g_new_npc_method) return;
    int index = *(int*)result;
    if (index < 0) return;

    int multiplier = 1;
    patch_handle_t array = PATCH_NULL;
    patchlib_field_get_value(g_main_npc_array_field, PATCH_NULL, &array);
    patch_handle_t npc = PATCH_NULL;
    if (array) patchlib_array_at(array, (size_t)index, &npc);
    bool boss = false, friendly = false, town = false;
    if (npc) {
        patchlib_field_get_value(g_npc_boss_field, npc, &boss);
        patchlib_field_get_value(g_npc_friendly_field, npc, &friendly);
        patchlib_field_get_value(g_npc_town_field, npc, &town);
    }
    if (boss) {
        if (g_enable_boss) multiplier = g_boss_multiplier;
    } else if (friendly || town) {
        if (g_enable_friendly) multiplier = g_friendly_multiplier;
    } else {
        if (g_enable_event) multiplier = g_event_multiplier;
    }
    if (multiplier <= 1) return;

    duplicating = true;
    for (int i = 1; i < multiplier; ++i) {
        if (g_enable_total_limit && active_npc_count() >= g_total_npc_limit) break;
        int duplicate_result = -1;
        patchlib_method_invoke_args(g_new_npc_method, PATCH_NULL,
                                    &duplicate_result, args);
    }
    duplicating = false;
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
    g_npc_active_field = patchlib_type_get_field(npc_type, "active");
    g_npc_boss_field = patchlib_type_get_field(npc_type, "boss");
    g_npc_friendly_field = patchlib_type_get_field(npc_type, "friendly");
    g_npc_town_field = patchlib_type_get_field(npc_type, "townNPC");

    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    if (main_type) g_main_npc_array_field = patchlib_type_get_field(main_type, "npc");

    patch_handle_t update_method =
        patchlib_type_get_method_by_param_count(npc_type, "UpdateFoundActiveNPCs", 0);
    if (update_method) {
        g_update_hook = patchlib_install_prepost_hook(
            update_method, NULL, main_update_postfix);
    }
    patchlib_free(update_method);
    patchlib_free(npc_type);
    patchlib_free(main_type);

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
    if (g_new_npc_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_new_npc_hook);
    }
    if (g_spawn_npc_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_spawn_npc_hook);
    }
    if (g_spawn_on_player_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_spawn_on_player_hook);
    }

    patchlib_free(g_max_spawns_field);
    patchlib_free(g_spawn_rate_field);
    patchlib_free(g_main_npc_array_field);
    patchlib_free(g_npc_active_field);
    patchlib_free(g_npc_boss_field);
    patchlib_free(g_npc_friendly_field);
    patchlib_free(g_npc_town_field);
    patchlib_free(g_new_npc_method);
    patchlib_free(g_spawn_on_player_method);

    g_max_spawns_field = PATCH_NULL;
    g_spawn_rate_field = PATCH_NULL;
    g_update_hook = PATCH_HOOK_INVALID_ID;
    g_new_npc_hook = PATCH_HOOK_INVALID_ID;
    g_spawn_npc_hook = PATCH_HOOK_INVALID_ID;
    g_spawn_on_player_hook = PATCH_HOOK_INVALID_ID;
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
