#include "mods/svc/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include "d/actor/d_a_demo00.h"
#include "d/actor/d_a_kytag11.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_kankyo_static.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

// Config var backing the "time offset" UI slider: added to the device clock's local hour before
// wrapping into [0, 23]. Minutes and seconds always come straight from the device clock, so time
// flow keeps tracking real time and hours advance normally as minutes/seconds roll over.
static ConfigVarHandle g_cvar_time_offset_hours = 0;

DEFINE_HOOK(&dScnKy_env_light_c::setDaytime, SetDaytime);
DEFINE_HOOK(&daDemo00_c::actPerformance, ActPerformance);
DEFINE_HOOK(&dKy_instant_timechg, InstantTimechg);
// dKy_Create is a file-local static in d_kankyo.cpp; hook by symbol name.
DEFINE_HOOK_SYMBOL("dKy_Create", int(void*), KankyoCreate);
// daKytag11_Execute is a file-local static in d_a_kytag11.cpp; hook by symbol name.
DEFINE_HOOK_SYMBOL("daKytag11_Execute", int(fopAc_ac_c*), Kytag11Execute);

static bool should_sync_time(dScnKy_env_light_c* env_light) {
    if (dKy_darkworld_check()) {
        return false;
    }

    const bool normal_time_progresses =
        !env_light->field_0x130a;
    return normal_time_progresses;
}

static int64_t read_time_offset_hours() {
    int64_t offset_hours = 0;
    svc_config->get_int(mod_ctx, g_cvar_time_offset_hours, &offset_hours);
    return std::clamp<int64_t>(offset_hours, -12, 12);
}

static f32 compute_wall_clock_daytime(int64_t offset_hours) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time {};
#if defined(_WIN32)
    localtime_s(&local_time, &now_time);
#else
    localtime_r(&now_time, &local_time);
#endif

    // Wrap the shifted hour into [0, 23]. Minutes and seconds are always the device clock's own,
    // unshifted values, so they keep advancing with real time and roll the (offset) hour over
    // normally at each boundary.
    const int hour = static_cast<int>(((local_time.tm_hour + offset_hours) % 24 + 24) % 24);

    return hour * 15.0f +
           local_time.tm_min * (15.0f / 60.0f) +
           local_time.tm_sec * (15.0f / 3600.0f);
}

// Tracks the last time-offset value that was applied, so a change to the offset can be detected
// and snapped to immediately rather than being chased gradually by the catch-up logic below.
static int64_t g_last_applied_offset_hours = 0;
static bool g_offset_initialized = false;

// Builds the mod's panel in the host Mods window: a single Time Offset control (hours only,
// -12 to +12, centered on 0) applied on top of the device clock.
static ModResult build_time_override_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = "Time Offset (Hours)";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvar_time_offset_hours;
    control.min = -12;
    control.max = 12;
    control.step = 1;
    return svc_ui->pane_add_control(mod_ctx, panel, &control, nullptr);
}

static void on_set_daytime_post(ModContext*, void* args, void*, void*) {
    dScnKy_env_light_c* env_light = mods::arg<dScnKy_env_light_c*>(args, 0);
    if (env_light == nullptr || !should_sync_time(env_light)) {
        return;
    }

    dComIfGp_roomControl_setTimePass(0);
    if (!should_sync_time(env_light)) {
        return;
    }

    const int64_t offset_hours = read_time_offset_hours();
    const bool offset_changed = !g_offset_initialized || offset_hours != g_last_applied_offset_hours;
    g_last_applied_offset_hours = offset_hours;
    g_offset_initialized = true;

    const f32 calendar_daytime = compute_wall_clock_daytime(offset_hours);

    if (offset_changed) {
        // The offset was just changed (or this is the first sync): snap straight to the new
        // target instead of fast-forwarding through the catch-up logic below.
        env_light->daytime = calendar_daytime;
        dComIfGs_setTime(env_light->daytime);
        return;
    }

    f32 diff_daytime = calendar_daytime - env_light->daytime;
    if (diff_daytime < 0.0f) {
        diff_daytime += 360.0f;
    }

    // True when game time slightly overshot device time (game is ahead by less than 2 degrees
    // in the absolute sense, ruling out the midnight-crossing case where the raw difference
    // would be negative and large).
    const bool game_slightly_ahead = env_light->daytime > calendar_daytime &&
                                     env_light->daytime - calendar_daytime < 2.0f;

    // True when the game just crossed midnight but the device hasn't yet: game is within
    // 2 degrees past midnight while the device is within 2 degrees before midnight.
    // Without this check the hook would advance game time through a full 360-degree cycle
    // chasing the large numeric gap, causing a visible full-day spin before stopping.
    const bool game_just_past_midnight = env_light->daytime < 2.0f && calendar_daytime > 358.0f;

    if (game_slightly_ahead || game_just_past_midnight ||
        (diff_daytime <= 1.0f && env_light->daytime <= calendar_daytime)) {
        // Game is at or just past the target; snap to device time.
        // If the game crossed midnight before the device did, revert that crossing so mDate
        // stays consistent with the pre-midnight position. The natural game tick will re-cross
        // midnight (and re-increment mDate) once the device also passes midnight.
        if (game_just_past_midnight) {
            env_light->mDate--;
            dComIfGs_setDate(env_light->mDate);
        }
        env_light->daytime = calendar_daytime;
    } else {
        // Game is behind the target; advance by one step and wrap to [0, 360).
        env_light->daytime += 1.0f;
        if (env_light->daytime >= 360.0f) {
            env_light->daytime -= 360.0f;
            // Crossed midnight during catch-up; keep mDate and the day-flag in sync.
            env_light->mDate++;
            dComIfGs_setDate(env_light->mDate);
            dKankyo_DayProc();
        }
    }

    dComIfGs_setTime(env_light->daytime);
}

// Saved state for the actPerformance instance currently being suppressed.
// Hook PRE/POST pairs fire sequentially (non-reentrant), so a single slot is sufficient.
static daDemo00_c* g_demo00_suppressed_self = nullptr;
// field_0x6b8: controls the branch in actPerformance that calls dComIfGs_setTime(pos.x * 15.0f).
static u8 g_saved_demo00_field_0x6b8 = 0;

// PRE hook: zero field_0x6b8 so the branch that calls
// dComIfGs_setTime(current.pos.x * 15.0f) is never taken during the cutscene.
static HookAction on_act_performance_pre(ModContext*, void* args, void*, void*) {
    g_demo00_suppressed_self = nullptr;
    daDemo00_c* self = mods::arg<daDemo00_c*>(args, 0);
    if (self == nullptr || self->field_0x6b8 == 0) {
        return HOOK_CONTINUE;
    }
    g_demo00_suppressed_self = self;
    g_saved_demo00_field_0x6b8 = self->field_0x6b8;
    self->field_0x6b8 = 0;
    return HOOK_CONTINUE;
}

// POST hook: restore field_0x6b8 on the same instance after the function returns.
static void on_act_performance_post(ModContext*, void*, void*, void*) {
    if (g_demo00_suppressed_self != nullptr) {
        g_demo00_suppressed_self->field_0x6b8 = g_saved_demo00_field_0x6b8;
        g_demo00_suppressed_self = nullptr;
    }
}

// PRE hook: skip dKy_instant_timechg entirely so that scripted instant-time jumps
// (Sun's Song, event triggers, etc.) cannot override the wall-clock time the mod
// is tracking.
static HookAction on_instant_timechg_pre(ModContext*, void*, void*, void*) {
    return HOOK_SKIP_ORIGINAL;
}

// POST hook: after dKy_Create runs for a new stage, it may have forced the
// in-game time to a stage-header value (or restored an old_time from before
// dark world). Re-apply the wall-clock time so the mod stays in sync.
static void on_kankyo_create_post(ModContext*, void*, void*, void*) {
    if (dKy_darkworld_check()) {
        return;
    }
    const f32 wall_time = compute_wall_clock_daytime(read_time_offset_hours());
    g_env_light.daytime = wall_time;
    dComIfGs_setTime(wall_time);
}

// Saved state for the Kytag11Execute instance currently being suppressed.
// Hook PRE/POST pairs fire sequentially (non-reentrant), so a single slot is sufficient.
static kytag11_class* g_kytag11_suppressed = nullptr;
// Fields saved to prevent the initial forced time-set and per-frame time advancement.
static u8 g_saved_kytag11_mNewTime = 0;
static u8 g_saved_kytag11_mEnvTime = 0;
// mInitTimeChange is restored so that disabling the mod allows the initial set to re-run cleanly.
static u8 g_saved_kytag11_mInitTimeChange = 0;

// PRE hook: when the mod is enabled, suppress daKytag11_Execute's time-overrides by
// replacing the two fields that drive them for the duration of the call.
//
// mNewTime: the function skips the initial time-set when mNewTime == 0x1F (sentinel).
// mEnvTime: controls the per-frame advancement delta; zero makes it a no-op.
// mInitTimeChange is also saved and restored so the suppress does not permanently mark
// the initial-set as done on the actor instance.
static HookAction on_kytag11_execute_pre(ModContext*, void* args, void*, void*) {
    g_kytag11_suppressed = nullptr;
    kytag11_class* self = mods::arg<kytag11_class*>(args, 0);
    if (self == nullptr) {
        return HOOK_CONTINUE;
    }
    g_kytag11_suppressed = self;
    g_saved_kytag11_mNewTime = self->mNewTime;
    g_saved_kytag11_mEnvTime = self->mEnvTime;
    g_saved_kytag11_mInitTimeChange = self->mInitTimeChange;
    self->mNewTime = 0x1F;  // sentinel: skip the initial forced time-set
    self->mEnvTime = 0;     // zero advancement: per-frame delta becomes 0
    return HOOK_CONTINUE;
}

// POST hook: restore the fields modified by on_kytag11_execute_pre.
static void on_kytag11_execute_post(ModContext*, void*, void*, void*) {
    if (g_kytag11_suppressed != nullptr) {
        g_kytag11_suppressed->mNewTime = g_saved_kytag11_mNewTime;
        g_kytag11_suppressed->mEnvTime = g_saved_kytag11_mEnvTime;
        g_kytag11_suppressed->mInitTimeChange = g_saved_kytag11_mInitTimeChange;
        g_kytag11_suppressed = nullptr;
    }
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ModResult result = mods::hook::add_post<SetDaytime>(on_set_daytime_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install on_set_daytime_post");
        return result;
    }

    // Install the POST hook before the PRE hook: if POST fails, PRE is never
    // registered, so field_0x6b8 can never be zeroed without being restored.
    result = mods::hook::add_post<ActPerformance>(on_act_performance_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install on_act_performance_post");
        return result;
    }

    result = mods::hook::add_pre<ActPerformance>(on_act_performance_pre);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install on_act_performance_pre");
        return result;
    }

    result = mods::hook::add_pre<InstantTimechg>(on_instant_timechg_pre);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install on_instant_timechg_pre");
        return result;
    }

    result = mods::hook::add_post<KankyoCreate>(on_kankyo_create_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install on_kankyo_create_post");
        return result;
    }

    // Install the POST hook before the PRE hook: if POST fails, PRE is never
    // registered, so mNewTime/mEnvTime can never be zeroed without being restored.
    result = mods::hook::add_post<Kytag11Execute>(on_kytag11_execute_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install on_kytag11_execute_post");
        return result;
    }

    result = mods::hook::add_pre<Kytag11Execute>(on_kytag11_execute_pre);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install on_kytag11_execute_pre");
        return result;
    }

    ConfigVarDesc time_offset_desc = CONFIG_VAR_DESC_INIT;
    time_offset_desc.name = "timeOffsetHours";
    time_offset_desc.type = CONFIG_VAR_INT;
    time_offset_desc.default_int = 0;
    result = svc_config->register_var(mod_ctx, &time_offset_desc, &g_cvar_time_offset_hours);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to register timeOffsetHours cvar");
        return result;
    }

    UiModsPanelDesc panel_desc = UI_MODS_PANEL_DESC_INIT;
    panel_desc.build = build_time_override_panel;
    result = svc_ui->register_mods_panel(mod_ctx, &panel_desc);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to register time_sync_neo mod panel");
        return result;
    }

    svc_log->info(mod_ctx, "time_sync_neo initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    svc_log->info(mod_ctx, "time_sync_neo shutdown");
    return MOD_OK;
}
}
