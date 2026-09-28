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
#include "tefkernel/tefstd/vector.h"

__attribute__((visibility("default")))
void (*mod_logger_write)(mod_log_level_t, const char*, const char*, ...) = NULL;

static patch_handle_t g_max_spawns_field = PATCH_NULL;
static patch_handle_t g_spawn_rate_field = PATCH_NULL;
static patch_handle_t g_main_npc_array_field = PATCH_NULL;
static patch_handle_t g_npc_active_field = PATCH_NULL;
static patch_handle_t g_npc_type_field = PATCH_NULL;
static patch_handle_t g_npc_boss_field = PATCH_NULL;
static patch_handle_t g_npc_friendly_field = PATCH_NULL;
static patch_handle_t g_npc_town_field = PATCH_NULL;
static patch_handle_t g_new_npc_method = PATCH_NULL;
static patch_handle_t g_spawn_boss_method = PATCH_NULL;
static patch_handle_t g_item_type_field = PATCH_NULL;
static patch_handle_t g_summon_item_check_method = PATCH_NULL;
static patch_handle_t g_item_check_method = PATCH_NULL;
static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_new_npc_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_spawn_npc_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_spawn_boss_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_summon_item_check_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_item_check_hook = PATCH_HOOK_INVALID_ID;
static bool g_in_boss_spawn = false;
static char g_diag_path[1024] = {0};

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
    .version = "1.0.12"
};

static void log_msg(mod_log_level_t level, const char* fmt, ...) {
    char message[320];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (mod_logger_write) mod_logger_write(level, "MyriadLife", "%s", message);
    if (g_diag_path[0]) {
        FILE* file = fopen(g_diag_path, "ab");
        if (file) {
            fprintf(file, "[%d] %s\n", (int)level, message);
            fclose(file);
        }
    }
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
    /* 新键名避免旧版默认 false 的持久化配置覆盖本版默认开启状态。 */
    g_enable_boss = read_bool_setting(text, "boss_double", g_enable_boss);
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

/* 探测指定名称的重载，并记录实际参数数量，便于适配不同游戏小版本。 */
static void log_new_npc_signatures(patch_handle_t npc_type) {
    tefstd_vector_t methods = {0};
    if (!npc_type || !tefstd_vector_init(&methods, sizeof(patch_handle_t)) ||
        !patchlib_type_get_methods(npc_type, true, &methods)) {
        log_msg(MOD_LOG_LEVEL_WARNING, "无法枚举 Terraria.NPC 方法签名");
        return;
    }
    size_t count = tefstd_vector_size(&methods);
    int matched = 0;
    for (size_t i = 0; i < count; ++i) {
        patch_handle_t* method = (patch_handle_t*)tefstd_vector_at(&methods, i);
        if (!method || !*method) continue;
        const char* name = patchlib_method_get_name(*method);
        if (!name || strstr(name, "NewNPC") == NULL) continue;
        patch_method_signature_t sig = {0};
        bool got_sig = patchlib_method_get_signature(*method, &sig);
        size_t argc = got_sig ? tefstd_vector_size(&sig.arg_types) : 0;
        log_msg(MOD_LOG_LEVEL_INFO,
                "NewNPC 签名[%d]: name=%s args=%zu return_type=%d token=%d",
                matched++, name, argc,
                got_sig ? (int)sig.return_type : -1,
                got_sig ? sig.token : -1);
        if (got_sig) patchlib_method_signature_free(&sig);
    }
    log_msg(MOD_LOG_LEVEL_INFO, "NewNPC 签名探测完成: 匹配=%d, 方法总数=%zu",
            matched, count);
    tefstd_vector_destroy(&methods);
}

static int active_npc_type_count(int npc_type) {
    if (!g_main_npc_array_field || !g_npc_active_field || !g_npc_type_field)
        return -1;
    patch_handle_t array = PATCH_NULL;
    patchlib_field_get_value(g_main_npc_array_field, PATCH_NULL, &array);
    if (!array) return -1;
    size_t length = patchlib_array_length(array);
    int count = 0;
    for (size_t i = 0; i < length; ++i) {
        patch_handle_t npc = PATCH_NULL;
        if (!patchlib_array_at(array, i, &npc) || !npc) continue;
        bool active = false;
        patchlib_field_get_value(g_npc_active_field, npc, &active);
        if (!active) continue;
        int type = -1;
        patchlib_field_get_value(g_npc_type_field, npc, &type);
        if (type == npc_type) ++count;
    }
    return count;
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
        case 370: /* Duke Fishron */
        case 398: /* Moon Lord Core */
        case 439: /* Lunatic Cultist */
        case 636: /* Empress of Light */
        case 657: /* Queen Slime */
        case 668: /* Deerclops */
            return true;
        default:
            return false;
    }
}

static void spawn_boss_postfix(patch_handle_t instance, void** args,
                               void* result,
                               const patch_method_signature_t* signature) {
    (void)instance; (void)signature; (void)result;
    if (g_in_boss_spawn || !g_enable_boss || !args || !args[0] || !args[2] ||
        !g_spawn_boss_method) return;

    int npc_type = *(int*)args[2];
    if (!is_summonable_boss_type(npc_type)) return;

    int multiplier = g_boss_multiplier;
    if (multiplier < 1) multiplier = 1;
    if (multiplier > 10) multiplier = 10;
    if (multiplier == 1) return;

    int before = active_npc_type_count(npc_type);
    log_msg(MOD_LOG_LEVEL_INFO,
            "SpawnBoss 原版调用完成: npc_type=%d, active_before_extra=%d",
            npc_type, before);
    g_in_boss_spawn = true;
    for (int i = 1; i < multiplier; ++i) {
        int extra_x = *(int*)args[0] + 320 * i;
        void* extra_args[8];
        for (int j = 0; j < 8; ++j) extra_args[j] = args[j];
        extra_args[0] = &extra_x;
        bool invoked = patchlib_method_invoke_args(
            g_spawn_boss_method, PATCH_NULL, NULL, extra_args);
        if (!invoked) {
            log_msg(MOD_LOG_LEVEL_WARNING,
                    "Boss 额外召唤调用失败: npc_type=%d, extra_x=%d",
                    npc_type, extra_x);
            break;
        }
    }
    g_in_boss_spawn = false;
    log_msg(MOD_LOG_LEVEL_INFO,
            "SpawnBoss 额外调用完成: npc_type=%d, active_after_extra=%d",
            npc_type, active_npc_type_count(npc_type));
}

/*
 * 参考“强制召唤”模组：原版会在已有 Boss 时拒绝部分 Boss 召唤物。
 * 这里仅对已知的 Boss 召唤物放行，不修改普通物品的使用判定。
 */
static bool is_boss_summon_item(int item_type) {
    switch (item_type) {
        case 43:    /* Suspicious Looking Eye */
        case 70:    /* Worm Food */
        case 544:   /* Slime Crown */
        case 556:   /* Abeemination */
        case 557:   /* Suspicious Looking Egg */
        case 560:   /* Guide Voodoo Doll */
        case 1133:  /* Mechanical Eye */
        case 1331:  /* Mechanical Worm */
        case 4988:  /* Celestial Sigil */
        case 5120:  /* Deer Thing */
        case 5334:  /* Gelatin Crystal */
            return true;
        default:
            return false;
    }
}

static void summon_item_check_postfix(patch_handle_t instance, void** args,
                                      void* result,
                                      const patch_method_signature_t* signature) {
    (void)instance;
    (void)args;
    (void)signature;
    if (result) *(bool*)result = true;
}

static bool item_check_can_use_prefix(patch_handle_t instance, void** args,
                                      const patch_method_signature_t* signature,
                                      void* result) {
    (void)instance;
    (void)signature;
    if (!args || !args[0] || !g_item_type_field || !result) return true;
    int item_type = 0;
    patchlib_field_get_value(g_item_type_field, (patch_handle_t)args[0], &item_type);
    if (!is_boss_summon_item(item_type)) return true;
    *(bool*)result = true;
    log_msg(MOD_LOG_LEVEL_INFO,
            "强制召唤放行 Boss 召唤物: item_type=%d", item_type);
    return true;
}

static void init_mod(kernel_mod_handle_t* handle) {
    if (!handle) return;
    if (handle->private_dir && *handle->private_dir) {
        snprintf(g_diag_path, sizeof(g_diag_path), "%s/myriadlife_diagnostics.log",
                 handle->private_dir);
    }
    load_config(handle->private_dir);
    log_msg(MOD_LOG_LEVEL_INFO, "v1.0.12 启动: boss_double=%s, boss_multiplier=%d",
            g_enable_boss ? "true" : "false", g_boss_multiplier);

    patch_handle_t npc_type = patchlib_type_get_type("Terraria", "NPC");
    if (!npc_type) {
        log_msg(MOD_LOG_LEVEL_ERROR, "找不到 Terraria.NPC 类型");
        return;
    }

    g_max_spawns_field = patchlib_type_get_field(npc_type, "defaultMaxSpawns");
    g_spawn_rate_field = patchlib_type_get_field(npc_type, "defaultSpawnRate");
    g_npc_active_field = patchlib_type_get_field(npc_type, "active");
    g_npc_type_field = patchlib_type_get_field(npc_type, "type");
    g_npc_boss_field = patchlib_type_get_field(npc_type, "boss");
    g_npc_friendly_field = patchlib_type_get_field(npc_type, "friendly");
    g_npc_town_field = patchlib_type_get_field(npc_type, "townNPC");

    patch_handle_t item_type = patchlib_type_get_type("Terraria", "Item");
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    if (item_type) {
        g_item_type_field = patchlib_type_get_field(item_type, "type");
    }
    if (player_type) {
        g_summon_item_check_method =
            patchlib_type_get_method_by_param_count(player_type,
                                                    "SummonItemCheck", 1);
        g_item_check_method =
            patchlib_type_get_method_by_param_count(player_type,
                                                    "ItemCheck_CheckCanUse_Inner", 0);
        if (g_summon_item_check_method) {
            g_summon_item_check_hook = patchlib_install_prepost_hook(
                g_summon_item_check_method, NULL, summon_item_check_postfix);
        }
        if (g_item_check_method) {
            g_item_check_hook = patchlib_install_prepost_hook(
                g_item_check_method, item_check_can_use_prefix, NULL);
        }
        log_msg(MOD_LOG_LEVEL_INFO,
                "强制召唤 Hook: field=%s summon=%s can_use=%s",
                g_item_type_field ? "成功" : "失败",
                g_summon_item_check_hook != PATCH_HOOK_INVALID_ID ? "成功" : "失败",
                g_item_check_hook != PATCH_HOOK_INVALID_ID ? "成功" : "失败");
    } else {
        log_msg(MOD_LOG_LEVEL_WARNING, "找不到 Terraria.Item 或 Terraria.Player 类型");
    }
    patchlib_free(item_type);
    patchlib_free(player_type);

    log_msg(MOD_LOG_LEVEL_INFO,
            "字段探测: maxSpawns=%s spawnRate=%s active=%s boss=%s friendly=%s townNPC=%s",
            g_max_spawns_field ? "命中" : "缺失",
            g_spawn_rate_field ? "命中" : "缺失",
            g_npc_active_field ? "命中" : "缺失",
            g_npc_boss_field ? "命中" : "缺失",
            g_npc_friendly_field ? "命中" : "缺失",
            g_npc_town_field ? "命中" : "缺失");
    log_new_npc_signatures(npc_type);

    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    if (main_type) g_main_npc_array_field = patchlib_type_get_field(main_type, "npc");

    patch_handle_t update_method =
        patchlib_type_get_method_by_param_count(npc_type, "UpdateFoundActiveNPCs", 0);
    if (update_method) {
        g_update_hook = patchlib_install_prepost_hook(
            update_method, NULL, main_update_postfix);
        log_msg(MOD_LOG_LEVEL_INFO, "UpdateFoundActiveNPCs Hook: %s",
                g_update_hook != PATCH_HOOK_INVALID_ID ? "成功" : "失败");
    }
    patchlib_free(update_method);

    /*
     * SpawnBoss 是生成 Boss 根 NPC 的入口；不 Hook NewNPC，避免
     * 多部件 Boss 的头、身体、手臂分别被复制。
     * 签名为 static void (int x, int y, int type, int target,
     * float ai0, float ai1, float ai2, float ai3)。
     */
    g_spawn_boss_method =
        patchlib_type_get_method_by_param_count(npc_type, "SpawnBoss", 8);
    if (g_spawn_boss_method) {
        patch_method_signature_t sig = {0};
        bool valid = patchlib_method_get_signature(
                         g_spawn_boss_method, &sig) &&
                     !sig.is_instance &&
                     sig.return_type == PATCH_VOID &&
                     tefstd_vector_size(&sig.arg_types) == 8;
        if (valid) {
            for (size_t i = 0; i < 8; ++i) {
                patch_type_t* arg = (patch_type_t*)tefstd_vector_at(&sig.arg_types, i);
                patch_type_t expected = i < 4 ? PATCH_INT32 : PATCH_FLOAT;
                if (!arg || *arg != expected) valid = false;
            }
        }
        if (sig.method) patchlib_method_signature_free(&sig);

        if (valid) {
            g_spawn_boss_hook = patchlib_install_prepost_hook(
                g_spawn_boss_method, NULL, spawn_boss_postfix);
            log_msg(MOD_LOG_LEVEL_INFO,
                    "SpawnBoss 签名验证通过(static int,int,int,int,float,float,float,float)，Boss Hook %s",
                    g_spawn_boss_hook != PATCH_HOOK_INVALID_ID ? "成功" : "失败");
        } else {
            log_msg(MOD_LOG_LEVEL_WARNING,
                    "SpawnBoss 签名与预期不符，安全跳过 Boss Hook");
            patchlib_free(g_spawn_boss_method);
            g_spawn_boss_method = PATCH_NULL;
        }
    } else {
        log_msg(MOD_LOG_LEVEL_WARNING,
                "没有找到 SpawnBoss(8 参数)，安全跳过 Boss Hook");
    }

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
    if (g_spawn_boss_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_spawn_boss_hook);
    }
    if (g_summon_item_check_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_summon_item_check_hook);
    }
    if (g_item_check_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_item_check_hook);
    }

    patchlib_free(g_max_spawns_field);
    patchlib_free(g_spawn_rate_field);
    patchlib_free(g_main_npc_array_field);
    patchlib_free(g_npc_active_field);
    patchlib_free(g_npc_type_field);
    patchlib_free(g_npc_boss_field);
    patchlib_free(g_npc_friendly_field);
    patchlib_free(g_npc_town_field);
    patchlib_free(g_new_npc_method);
    patchlib_free(g_spawn_boss_method);
    patchlib_free(g_item_type_field);
    patchlib_free(g_summon_item_check_method);
    patchlib_free(g_item_check_method);

    g_max_spawns_field = PATCH_NULL;
    g_spawn_rate_field = PATCH_NULL;
    g_update_hook = PATCH_HOOK_INVALID_ID;
    g_new_npc_hook = PATCH_HOOK_INVALID_ID;
    g_spawn_npc_hook = PATCH_HOOK_INVALID_ID;
    g_spawn_boss_hook = PATCH_HOOK_INVALID_ID;
    g_summon_item_check_hook = PATCH_HOOK_INVALID_ID;
    g_item_check_hook = PATCH_HOOK_INVALID_ID;
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
