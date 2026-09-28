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
static patch_handle_t g_item_type_field = PATCH_NULL;
static patch_handle_t g_player_inventory_field = PATCH_NULL;
static patch_handle_t g_player_selected_item_field = PATCH_NULL;
static patch_handle_t g_new_npc_method = PATCH_NULL;
static patch_handle_t g_spawn_on_player_method = PATCH_NULL;
static patch_handle_t g_summon_item_check_method = PATCH_NULL;
static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_new_npc_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_spawn_npc_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_spawn_on_player_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_boss_summon_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_boss_can_use_hook = PATCH_HOOK_INVALID_ID;
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
    .version_code = 202609287,
    .api_version = 1,
    .version = "1.1.3"
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
    /* A new key prevents a persisted test-era disable from silently
     * turning off the redesigned Boss summon route after an upgrade. */
    g_enable_boss = read_bool_setting(text, "enable_boss_summon", g_enable_boss);
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

/* Terraria 1.4.5.x boss summon item IDs, checked against the held Item.type. */
static bool is_boss_summon_item(int item_type) {
    switch (item_type) {
        case 43:   /* Suspicious Looking Eye */
        case 70:   /* Worm Food */
        case 544:  /* Mechanical Eye */
        case 556:  /* Mechanical Worm */
        case 557:  /* Mechanical Skull */
        case 560:  /* Slime Crown */
        case 1133: /* Abeemination */
        case 1293: /* Lihzahrd Power Cell */
        case 1331: /* Bloody Spine */
        case 3601: /* Celestial Sigil */
        case 4988: /* Gelatin Crystal */
        case 5120: /* Deer Thing */
            return true;
        default:
            return false;
    }
}

static bool is_current_boss_summon_item(patch_handle_t instance, void** args,
                                        const patch_method_signature_t* signature,
                                        int* item_type_out) {
    if (!g_item_type_field) return false;

    patch_handle_t item = PATCH_NULL;
    if (signature && signature->arg_types.size > 0 && args && args[0]) {
        item = (patch_handle_t)args[0];
    } else if (instance && g_player_inventory_field &&
               g_player_selected_item_field) {
        patch_handle_t inventory = PATCH_NULL;
        int selected_item = -1;
        patchlib_field_get_value(g_player_inventory_field, instance, &inventory);
        patchlib_field_get_value(g_player_selected_item_field, instance,
                                 &selected_item);
        if (!inventory || selected_item < 0 ||
            (size_t)selected_item >= patchlib_array_length(inventory))
            return false;
        if (!patchlib_array_at(inventory, (size_t)selected_item, &item) || !item)
            return false;
    }
    if (!item) return false;

    int item_type = 0;
    patchlib_field_get_value(g_item_type_field, item, &item_type);
    if (item_type_out) *item_type_out = item_type;
    return is_boss_summon_item(item_type);
}

/* The reference mod reads Item.type from args[0] in this callback. */
static bool boss_summon_can_use_prefix(patch_handle_t instance, void** args,
                                       const patch_method_signature_t* signature,
                                       void* result) {
    (void)signature;
    if (!g_enable_boss || !result)
        return true;

    int item_type = 0;
    if (!is_current_boss_summon_item(instance, args, signature, &item_type))
        return true;

    *(bool*)result = true;
    log_msg(MOD_LOG_LEVEL_INFO,
            "Boss召唤限制已放行: item_type=%d", item_type);
    return false;
}

/* After the original summon check, replay it the requested number of times. */
static void boss_summon_postfix(patch_handle_t instance, void** args,
                                void* result,
                                const patch_method_signature_t* signature) {
    (void)result;
    int item_type = 0;
    if (!g_enable_boss || g_in_boss_duplicate || !instance ||
        !g_summon_item_check_method ||
        !is_current_boss_summon_item(instance, args, signature, &item_type))
        return;

    int multiplier = g_boss_multiplier;
    if (multiplier < 1) multiplier = 1;
    if (multiplier > 5) multiplier = 5;
    if (multiplier <= 1) return;

    g_in_boss_duplicate = true;
    int invoked = 0;
    for (int i = 1; i < multiplier; ++i) {
        if (!patchlib_method_invoke_args(g_summon_item_check_method,
                                         instance, NULL, args)) {
            log_msg(MOD_LOG_LEVEL_WARNING,
                    "Boss召唤物重复触发失败: copy=%d", i + 1);
            break;
        }
        ++invoked;
    }
    g_in_boss_duplicate = false;
    log_msg(MOD_LOG_LEVEL_INFO,
            "Boss召唤重复调用完成: item_type=%d, requested=%d, invoked=%d",
            item_type, multiplier, invoked);
}

static void init_mod(kernel_mod_handle_t* handle) {
    if (!handle) return;
    load_config(handle->private_dir);

    if (g_enable_boss) {
        patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
        patch_handle_t item_class = patchlib_type_get_type("Terraria", "Item");
        if (item_class) g_item_type_field = patchlib_type_get_field(item_class, "type");
        if (player_type) {
            g_summon_item_check_method =
                patchlib_type_get_method(player_type, "SummonItemCheck");
            patch_handle_t can_use_method = patchlib_type_get_method(
                player_type, "ItemCheck_CheckCanUse_Inner");
            g_player_inventory_field =
                patchlib_type_get_field(player_type, "inventory");
            g_player_selected_item_field =
                patchlib_type_get_field(player_type, "selectedItem");
            if (g_summon_item_check_method &&
                patchlib_method_is_instance(g_summon_item_check_method)) {
                g_boss_summon_hook = patchlib_install_prepost_hook(
                    g_summon_item_check_method, NULL, boss_summon_postfix);
            }
            if (can_use_method && patchlib_method_is_instance(can_use_method)) {
                g_boss_can_use_hook = patchlib_install_prepost_hook(
                    can_use_method, boss_summon_can_use_prefix, NULL);
            }
            log_msg(MOD_LOG_LEVEL_INFO,
                    "Boss Hook 查找结果: SummonItemCheck=%s(params=%d, hook=%d), "
                    "ItemCheck_CheckCanUse_Inner=%s(params=%d, hook=%d), "
                    "Item.type=%s",
                    g_summon_item_check_method ? "found" : "missing",
                    g_summon_item_check_method
                        ? patchlib_method_get_param_count(g_summon_item_check_method) : -1,
                    (int)g_boss_summon_hook,
                    can_use_method ? "found" : "missing",
                    can_use_method ? patchlib_method_get_param_count(can_use_method) : -1,
                    (int)g_boss_can_use_hook,
                    g_item_type_field ? "found" : "missing");
            patchlib_free(can_use_method);
            patchlib_free(player_type);
        }
        patchlib_free(item_class);
        if (g_boss_can_use_hook != PATCH_HOOK_INVALID_ID &&
            g_boss_summon_hook != PATCH_HOOK_INVALID_ID) {
            log_msg(MOD_LOG_LEVEL_INFO,
                    "Boss召唤翻倍已启用: hooks installed, multiplier=%d",
                    g_boss_multiplier);
        } else {
            log_msg(MOD_LOG_LEVEL_WARNING,
                    "Boss召唤部分 Hook 缺失: can_use=%s summon=%s",
                    g_boss_can_use_hook != PATCH_HOOK_INVALID_ID ? "ok" : "missing",
                    g_boss_summon_hook != PATCH_HOOK_INVALID_ID ? "ok" : "missing");
        }
    } else {
        log_msg(MOD_LOG_LEVEL_INFO, "Boss召唤翻倍关闭");
    }

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
    if (g_boss_summon_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_boss_summon_hook);
    }
    if (g_boss_can_use_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_boss_can_use_hook);
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
    patchlib_free(g_player_inventory_field);
    patchlib_free(g_player_selected_item_field);
    patchlib_free(g_item_type_field);
    patchlib_free(g_summon_item_check_method);

    g_max_spawns_field = PATCH_NULL;
    g_spawn_rate_field = PATCH_NULL;
    g_update_hook = PATCH_HOOK_INVALID_ID;
    g_new_npc_hook = PATCH_HOOK_INVALID_ID;
    g_spawn_npc_hook = PATCH_HOOK_INVALID_ID;
    g_spawn_on_player_hook = PATCH_HOOK_INVALID_ID;
    g_boss_summon_hook = PATCH_HOOK_INVALID_ID;
    g_boss_can_use_hook = PATCH_HOOK_INVALID_ID;
    g_item_type_field = PATCH_NULL;
    g_player_inventory_field = PATCH_NULL;
    g_player_selected_item_field = PATCH_NULL;
    g_summon_item_check_method = PATCH_NULL;
    g_in_boss_duplicate = false;
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
