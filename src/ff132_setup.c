/*
 * FF13-2-SpeedrunPractice — speedrun practice mod for FINAL FANTASY XIII-2 (Steam).
 * Copyright (C) 2026 Akiyoshi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// The hotkeys-and-settings screen: every ini setting, changed in place and written back.
//
// Threads: everything but ff132_setup_visible/ff132_setup_draw runs on the hotkey worker; those two run
// on the render thread. Shared state is volatile with one writer each, except the lapse guard's
// close (as in ff132_tidy.c).
#include "ff132_internal.h"

// What each binding is called when another one wants its key.
static const struct { const char* name; ff132_msg label; } kFn[] = {
    { "help", T_FN_HELP },               { "slot_select", T_FN_SLOT_SELECT },
    { "battle_overlay", T_FN_BATTLE_OVERLAY },
    { "save_tidy", T_FN_SAVE_TIDY },     { "menu_ok", T_FN_MENU_OK },
    { "menu_up", T_FN_MENU_UP },         { "menu_down", T_FN_MENU_DOWN },
    { "menu_left", T_FN_MENU_LEFT },     { "menu_right", T_FN_MENU_RIGHT },
    { "list_prev", T_FN_LIST_PREV },     { "list_next", T_FN_LIST_NEXT },
    { "cancel", T_FN_CANCEL },
    { "game_speed", T_FN_GAME_SPEED },   { "enemy_suppress", T_FN_ENEMY_SUPPRESS },
    { "battle_restart_auto", T_FN_AR_AUTO }, { "kill_mode", T_FN_KILL_MODE },
};
static const char* fn_label(const char* name) {
    for (int i = 0; i < (int)(sizeof kFn / sizeof kFn[0]); i++)
        if (!strcmp(kFn[i].name, name)) return ui_text(kFn[i].label);
    return name;
}

// ---- the rows ---------------------------------------------------------------------------------
enum {
    R_H_KEYS, R_K_HELP, R_K_SPEED, R_K_KILL, R_K_ENC, R_K_RAUTO, R_K_SLOT, R_K_TIDY, R_K_BATTLE,
    R_KEYS,
    R_H_SET, R_FOCUS, R_SLOTS, R_REDIR, R_KNOTE, R_NOMOD,
    R_HIST, R_RECAP, R_MULT, R_FONT, R_LOG, R_N
};
enum { K_HEAD, K_KEY, K_KEYS, K_BOOL, K_SLOT, K_SLOTS, K_NOMOD, K_MULT, K_FONT, K_LOG };
static int row_kind(int r) {
    if (r == R_H_KEYS || r == R_H_SET) return K_HEAD;
    if (r >= R_K_HELP && r <= R_K_BATTLE) return K_KEY;
    if (r == R_KEYS) return K_KEYS;
    if (r == R_REDIR) return K_SLOT;
    if (r == R_SLOTS) return K_SLOTS;
    if (r == R_NOMOD) return K_NOMOD;
    if (r == R_MULT) return K_MULT;
    if (r == R_FONT) return K_FONT;
    if (r == R_LOG) return K_LOG;
    return K_BOOL;
}
static const char* const kKeyRowName[R_KEYS] = {
    NULL, "help", "game_speed", "kill_mode", "enemy_suppress", "battle_restart_auto", "slot_select",
    "save_tidy", "battle_overlay",
};
#define SUB_N 8
static const char* const kSubName[SUB_N] = {
    "menu_ok", "menu_up", "menu_down", "menu_left", "menu_right", "cancel", "list_prev", "list_next",
};
// T_COUNT = no short name.
static const ff132_msg kSubShort[SUB_N] = { T_SUB_OK, T_SUB_UP, T_SUB_DOWN, T_SUB_LEFT, T_SUB_RIGHT,
                                         T_SUB_CLOSE, T_SUB_LIST, T_COUNT };

static int bind_idx(const char* name) {
    for (int i = 0; i < hk_binding_count(); i++)
        if (!strcmp(hk_binding_name(i), name)) return i;
    return -1;
}
// The binding that already holds b, other than `self`; -1 = none. Exact match: vk AND modifiers.
static int bind_owner(UINT b, int self) {
    if (!b) return -1;
    for (int j = 0; j < hk_binding_count(); j++)
        if (j != self && *hk_binding_vk(j) == b) return j;
    return -1;
}
// menu_ok on the tidy key's PHYSICAL key is the pair config_load moves off (ff132_core.c).
static int ok_tidy_clash(const char* name, UINT b) {
    if (!b) return 0;
    if (!strcmp(name, "menu_ok"))   return g_saveTidyVk && hk_key(g_saveTidyVk) == hk_key(b);
    if (!strcmp(name, "save_tidy")) return g_menuOkVk && hk_key(g_menuOkVk) == hk_key(b);
    return 0;
}
static int bind_dup(int bi) {
    if (bi < 0) return 0;
    UINT v = *hk_binding_vk(bi);
    return bind_owner(v, bi) >= 0 || ok_tidy_clash(hk_binding_name(bi), v);
}

// ---- state ------------------------------------------------------------------------------------
static volatile int  g_open   = 0;
static volatile int  g_sel    = R_K_SPEED;
static volatile int  g_sub    = 0;          // the key row's item under the sub-cursor
static volatile int  g_msub   = 0;          // the multiplier candidate under the sub-cursor
static volatile int  g_cap    = -1;         // the binding being captured, -1 = none
static volatile UINT g_settle = 0;          // the key that ended the last capture
static volatile int  g_slotOwn = 0;         // ff132_slots_game() at the last refresh
static volatile int  g_slotsNa = 0;         // ff132_slots_unavailable() at the last refresh
static volatile int  g_slotReach = 0;       // ff132_slots_reach() at the last refresh
static volatile int  g_slotNeed = -1;       // ff132_slots_needed() at the last refresh
static volatile int  g_slotAbove = 0;       // saves numbered past the game's own count, same
static volatile int  g_nomodCloud = 0;      // steam_cloud_on() == 1, same
// The working slot's ON stop, when a save of the player's own blocks it at the last refresh (-1 = free).
static volatile int  g_naLast = -1;
static int wslot_last_target(void);
static unsigned char g_prev[256];           // worker only: the capture's key states last tick

int ff132_setup_is_open(void) { return g_open; }
int ff132_setup_capturing(void) { return g_open && g_cap >= 0; }

static void slot_refresh(void) {
    g_slotOwn = ff132_slots_game();
    g_slotsNa = ff132_slots_unavailable();
    g_slotReach = ff132_slots_reach();
    g_slotNeed = ff132_slots_needed();
    g_slotAbove = g_slotNeed > FF132_SLOTS_GAME ? ff132_slots_saves_from(FF132_SLOTS_GAME) : 0;
    g_nomodCloud = steam_cloud_on() == 1;
    int t = wslot_last_target();
    g_naLast = (t > 0 && save_work_slot_foreign(t)) ? t : -1;
}

void ff132_setup_open(void) {
    if (g_open) return;
    g_cap = -1;
    g_settle = 0;
    if (row_kind(g_sel) == K_HEAD) g_sel = R_K_SPEED;
    slot_refresh();
    ff132_di_gate_arm();                // before the flag: the render thread's lapse guard reads both
    g_open = 1;
    ff132_log(FF132_TAG "settings: opened\n");
}
void ff132_setup_close(void) {
    if (!g_open) return;
    g_open = 0;
    g_cap = -1;
    ff132_log(FF132_TAG "settings: closed\n");
}

void ff132_setup_move(int dy) {
    if (!g_open || g_cap >= 0) return;
    int s = g_sel;
    do {
        s += dy > 0 ? 1 : -1;
        if (s < 0) s = R_N - 1;
        if (s >= R_N) s = 0;
    } while (row_kind(s) == K_HEAD);
    g_sel = s;
    slot_refresh();
}

// ---- writing a setting ------------------------------------------------------------------------
static void setting_write(const char* sec, const char* key, const char* val) {
    int ok = config_write(sec, key, val);
    ff132_log(FF132_TAG "settings: %s.%s = %s (%s)\n", sec, key, val[0] ? val : "(empty)",
          ok ? "ini written" : "ini NOT written");
}
static const char* on_off(int v) { return v ? "on" : "off"; }

static void bool_toggle(int r) {
    unsigned char before;
    switch (r) {
    case R_FOCUS:
        g_hkGlobal = !g_hkGlobal;
        setting_write("hotkeys", "global_hotkeys", g_hkGlobal ? "1" : "0");
        break;
    case R_KNOTE:
        g_keepNote = g_keepNote ? 0 : 1;
        setting_write("save", "keep_note", on_off(g_keepNote));
        break;
    case R_HIST:
        before = g_directLoad;
        ff132_load_toggle();
        if (g_directLoad != before)
            setting_write("load", "skip_historia_crux", on_off(g_directLoad));
        break;
    case R_RECAP:
        g_skipRecap = g_skipRecap ? 0 : 1;
        setting_write("load", "skip_story_recap", on_off(g_skipRecap));
        break;
    }
}

// ---- the multiplier list ----------------------------------------------------------------------
static const double kCand[] = { 0.5, 1.5, 2, 3, 4, 5, 6, 8, 10 };
#define CAND_MAX ((int)(sizeof kCand / sizeof kCand[0]) + FF132_GS_STEPS_MAX)
static int near_eq(double a, double b) { double d = a - b; return d < 0.005 && d > -0.005; }
static void sort_asc(double* v, int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
}
static int mult_cands(double* out) {
    int n = 0;
    for (int i = 0; i < (int)(sizeof kCand / sizeof kCand[0]); i++) out[n++] = kCand[i];
    int sn = g_gsStepN;
    if (sn > FF132_GS_STEPS_MAX) sn = FF132_GS_STEPS_MAX;
    for (int i = 0; i < sn; i++) {
        int dup = 0;
        for (int j = 0; j < n; j++) if (near_eq(out[j], g_gsSteps[i])) dup = 1;
        if (!dup) out[n++] = g_gsSteps[i];
    }
    sort_asc(out, n);
    return n;
}
static int mult_has(double v) {
    for (int i = 0; i < g_gsStepN && i < FF132_GS_STEPS_MAX; i++) if (near_eq(g_gsSteps[i], v)) return 1;
    return 0;
}
// Why the candidate cannot be put in or taken out; T_COUNT = it can.
static ff132_msg mult_blocked(double v) {
    int n = g_gsStepN < FF132_GS_STEPS_MAX ? g_gsStepN : FF132_GS_STEPS_MAX;
    if (mult_has(v)) return n <= 1 ? T_SET_MULT_MIN : T_COUNT;
    return n >= FF132_GS_STEPS_MAX ? T_SET_MULT_MAX : T_COUNT;
}

static void mult_toggle(void) {
    double c[CAND_MAX];
    int n = mult_cands(c);
    int at = g_msub;
    if (at < 0 || at >= n) at = n - 1;
    double v = c[at];
    double list[FF132_GS_STEPS_MAX + 1];
    int m = 0, had = 0;
    for (int i = 0; i < g_gsStepN && i < FF132_GS_STEPS_MAX; i++) {
        if (near_eq(g_gsSteps[i], v)) had = 1;
        else list[m++] = g_gsSteps[i];
    }
    if (mult_blocked(v) != T_COUNT) return;     // the footer already says why
    if (!had) list[m++] = v;
    sort_asc(list, m);
    ff132_speed_steps_set(list, m);
    // The sub-cursor stays on the value; a factor of the player's own that left the list also left
    // the candidates, so the cursor takes the next one up.
    n = mult_cands(c);
    at = n - 1;
    for (int i = 0; i < n; i++) if (c[i] >= v - 0.005) { at = i; break; }
    g_msub = at;
    char val[FF132_GS_STEPS_MAX * 12], f[16];
    int o = 0;
    val[0] = 0;
    for (int i = 0; i < m; i++) {
        ff132_gs_fmt(list[i], f, sizeof f);
        o += snprintf(val + o, sizeof val - (size_t)o, "%s%s", i ? ", " : "", f);
    }
    setting_write("game_speed", "multiplier", val);
}

// ---- the slot count ---------------------------------------------------------------------------
static const int kSlotStops[] = { FF132_SLOTS_GAME, FF132_SLOTS_MAX_NOGUI, FF132_SLOTS_MAX };
#define SLOT_STOPS ((int)(sizeof kSlotStops / sizeof kSlotStops[0]))

static int slots_next(int v, int dir) {
    if (dir > 0) {
        for (int i = 0; i < SLOT_STOPS; i++) if (kSlotStops[i] > v) return kSlotStops[i];
        return kSlotStops[0];
    }
    for (int i = SLOT_STOPS - 1; i >= 0; i--) if (kSlotStops[i] < v) return kSlotStops[i];
    return kSlotStops[SLOT_STOPS - 1];
}

// Why a stop cannot be picked now; SN_OPEN = it can. The refusals in slots_cycle stay behind these
// for what changes between a refresh and the press (a list opening, the folder going unreadable).
enum { SN_OPEN, SN_SAVES, SN_WSLOT, SN_GUI, SN_N };
static int slots_why(int s) {
    int v = g_slotCount;
    if (s == v) return SN_OPEN;
    if (s < v) {
        if (g_slotNeed >= 0 && s < g_slotNeed) return SN_SAVES;
        if (g_autosaveSlot >= s) return SN_WSLOT;
    } else if (s > g_slotReach) {
        return SN_GUI;
    }
    return SN_OPEN;
}
static int slots_any_open(void) {
    for (int i = 0; i < SLOT_STOPS; i++)
        if (kSlotStops[i] != g_slotCount && slots_why(kSlotStops[i]) == SN_OPEN) return 1;
    return 0;
}

static void slots_refuse(const char* msg, const char* why) {
    ff132_flash_c(msg, FF132_COL_MENU_OFF);
    ff132_log(FF132_TAG "settings: save.slot_count stays %d -- %s\n", g_slotCount, why);
}

// The working slot on as the last slot would move with the limit.
static int slots_locked(void) { return g_autosaveRedirect && g_autosaveSlot < 0; }

static void slots_cycle(int dir) {
    char msg[256], why[128];
    slot_refresh();
    if (g_slotsNa == 1) { slots_refuse(ui_text(T_SET_SLOTS_CLOUD), "Steam Cloud is on"); return; }
    if (g_slotsNa == 2) {
        snprintf(msg, sizeof msg, ui_text(T_SET_SLOTS_ODD), g_slotOwn);
        snprintf(why, sizeof why, "the game's own count is %d", g_slotOwn);
        slots_refuse(msg, why);
        return;
    }
    if (ff132_save_list_up()) { slots_refuse(ui_text(T_SET_SLOTS_LIST), "a save list is up"); return; }
    if (slots_locked()) {
        slots_refuse(ui_text(T_SET_SLOTS_WS_ON), "the working slot is on as the last slot");
        return;
    }
    int v = g_slotCount, nv = v;
    for (int i = 0; i < SLOT_STOPS; i++) {
        nv = slots_next(nv, dir);
        if (nv == v || slots_why(nv) == SN_OPEN) break;
    }
    if (nv == v || slots_why(nv) != SN_OPEN) return;
    if (nv < v) {
        int need = ff132_slots_needed();
        if (need < 0) {
            slots_refuse(ui_text(T_SET_SLOTS_SCAN), "the save folder could not be resolved");
            return;
        }
        if (nv < need) {
            int n = ff132_slots_saves_from(nv);
            snprintf(msg, sizeof msg, ui_text(T_SET_SLOTS_BLOCKED), nv + 1, n);
            snprintf(why, sizeof why, "%d save(s) numbered %d or higher (highest %d) block %d",
                     n, nv, need - 1, nv);
            slots_refuse(msg, why);
            return;
        }
        if (g_autosaveSlot >= nv) {
            snprintf(msg, sizeof msg, ui_text(T_SET_SLOTS_WSLOT), g_autosaveSlot + 1);
            snprintf(why, sizeof why, "the working slot is %d, past %d", g_autosaveSlot, nv);
            slots_refuse(msg, why);
            return;
        }
    } else if (nv > g_slotReach) {
        snprintf(msg, sizeof msg, ui_text(T_SLOTS_GUI_FAIL), FF132_SLOTS_MAX_NOGUI);
        snprintf(why, sizeof why, "the save list cannot be widened past %d rows (reach %d)",
                 ff132_listgui_rows(), g_slotReach);
        slots_refuse(msg, why);
        return;
    }
    g_slotCount = nv;
    char val[16] = "";
    if (nv != FF132_SLOTS_GAME) snprintf(val, sizeof val, "%d", nv);
    setting_write("save", "slot_count", val);
    ff132_slots_mark_dirty();
}

// ---- the working slot ---------------------------------------------------------------------------
// Its stops: the last slot of the limit, OFF. A slot set by hand in the ini sits where the last
// slot does.
enum { WS_LAST, WS_OFF, WS_N };
static int wslot_stop(void) { return g_autosaveRedirect ? WS_LAST : WS_OFF; }

// The slot the last stop stands for: a slot set by hand in the ini while it is the stop in force,
// otherwise the last of the slot limit; -1 = not known yet.
static int wslot_last_target(void) {
    if (wslot_stop() == WS_LAST && g_autosaveSlot >= 0) return g_autosaveSlot;
    int last = ff132_slots_last();
    return last > 0 ? last - 1 : -1;
}
// A stop in force always counts as open: the check already let it on.
static int wslot_open(int stop) {
    if (stop == wslot_stop()) return 1;
    if (stop == WS_LAST)  return g_naLast < 0;
    return 1;
}

static int wslot_any_open(void) {
    for (int st = 0; st < WS_N; st++) if (st != wslot_stop() && wslot_open(st)) return 1;
    return 0;
}

static void wslot_cycle(int dir) {
    int was = g_autosaveSlot, wasOn = g_autosaveRedirect;
    slot_refresh();
    int from = wslot_stop(), to = from;
    do to = (to + (dir > 0 ? 1 : WS_N - 1)) % WS_N;
    while (to != from && !wslot_open(to));
    if (to == from) return;
    // Off before the slot changes: the switch on then checks the new slot for a save of the
    // player's own, which the copy into it would destroy.
    if (wasOn) save_toggle_autosave_redirect();
    if (to != WS_OFF && !g_autosaveRedirect) {
        g_autosaveSlot = -1;
        save_toggle_autosave_redirect();       // may refuse; it says so itself
        if (!g_autosaveRedirect) g_autosaveSlot = was;
    }
    slot_refresh();
    if (g_autosaveSlot != was) {
        char val[16] = "";
        if (g_autosaveSlot >= 0) snprintf(val, sizeof val, "%d", g_autosaveSlot);
        setting_write("save", "autosave_slot", val);
    }
    if (g_autosaveRedirect != wasOn)
        setting_write("save", "autosave_redirect", on_off(g_autosaveRedirect));
}

// ---- the category the unmodded game is shown -------------------------------------------------
// Its stops: none, the category in use, then every parked one.
static void nomod_cycle(int dir) {
    slot_refresh();
    if (g_nomodCloud) { ff132_flash_c(ui_text(T_SET_NOMOD_CLOUD), FF132_COL_MENU_OFF); return; }
    int n = ff132_cats_count();
    int pos = (ff132_nomod_at() + 2 + (dir > 0 ? 1 : n + 1)) % (n + 2);
    if (!ff132_nomod_set(pos - 2)) ff132_flash_c(ui_text(T_TD_CAT_FAIL), FF132_COL_MENU_OFF);
}

// ---- the text size ----------------------------------------------------------------------------
// Its stops: auto, then sizes in percent of the original. A size set by hand in the ini that is not
// a stop steps to the nearest stop the way the key points.
static const int kScaleStops[] = { FF132_SCALE_AUTO, 75, 100, 125, 150, 175, 200, 250, 300 };
#define SCALE_STOPS ((int)(sizeof kScaleStops / sizeof kScaleStops[0]))

// The ini's spelling: "auto", or the factor as the ini takes it ("1.25", "2").
static void scale_ini(int pct, char* out, size_t n) {
    if (pct == FF132_SCALE_AUTO) { snprintf(out, n, "auto"); return; }
    snprintf(out, n, "%d.%02d", pct / 100, pct % 100);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '0') out[--l] = 0;
    if (l > 1 && out[l - 1] == '.') out[--l] = 0;
}

static void scale_cycle(int dir) {
    int v = g_ovlScale, at = -1, nv;
    for (int i = 0; i < SCALE_STOPS; i++) if (kScaleStops[i] == v) at = i;
    if (at >= 0) {
        nv = kScaleStops[(at + (dir > 0 ? 1 : SCALE_STOPS - 1)) % SCALE_STOPS];
    } else {
        // Off the stops: the first one past it the way the key points, else round to auto.
        nv = FF132_SCALE_AUTO;
        if (dir > 0) {
            for (int i = 1; i < SCALE_STOPS; i++) if (kScaleStops[i] > v) { nv = kScaleStops[i]; break; }
        } else {
            for (int i = SCALE_STOPS - 1; i >= 1; i--) if (kScaleStops[i] < v) { nv = kScaleStops[i]; break; }
        }
    }
    g_ovlScale = nv;                    // the render thread remakes the faces on its next pass
    char val[16];
    scale_ini(nv, val, sizeof val);
    setting_write("overlay", "font_scale", val);
}

// ---- left/right and confirm -------------------------------------------------------------------
static const char* const kLogName[3] = { "quiet", "normal", "verbose" };

void ff132_setup_cycle(int dir) {
    if (!g_open || g_cap >= 0) return;
    int r = g_sel;
    switch (row_kind(r)) {
    case K_KEYS:
        g_sub = (g_sub + (dir > 0 ? 1 : SUB_N - 1)) % SUB_N;
        break;
    case K_MULT: {
        double c[CAND_MAX];
        int n = mult_cands(c);
        int s = g_msub + (dir > 0 ? 1 : -1);
        g_msub = s < 0 ? n - 1 : s >= n ? 0 : s;
        break;
    }
    case K_BOOL:
        bool_toggle(r);
        break;
    case K_SLOT:
        wslot_cycle(dir);
        break;
    case K_SLOTS:
        slots_cycle(dir);
        break;
    case K_NOMOD:
        nomod_cycle(dir);
        break;
    case K_FONT:
        scale_cycle(dir);
        break;
    case K_LOG: {
        int l = g_logLevel;
        if (l < 0 || l > 2) l = 1;
        l = (l + (dir > 0 ? 1 : 2)) % 3;
        g_logLevel = l;
        int ok = config_write("log", "level", kLogName[l]);
        // At every level: a change to quiet would otherwise drop its own line.
        ff132_loga(FF132_TAG "settings: log.level = %s (%s)\n", kLogName[l],
               ok ? "ini written" : "ini NOT written");
        break;
    }
    }
}

static void cap_start(int bi) {
    if (bi < 0) return;
    for (int vk = 0; vk < 256; vk++) g_prev[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
    g_cap = bi;
    ff132_logv(FF132_TAG "settings: waiting for a key for %s\n", hk_binding_name(bi));
}

void ff132_setup_ok(void) {
    if (!g_open || g_cap >= 0) return;
    int r = g_sel;
    switch (row_kind(r)) {
    case K_KEY:  cap_start(bind_idx(kKeyRowName[r])); break;
    case K_KEYS: cap_start(bind_idx(kSubName[g_sub])); break;
    case K_MULT: mult_toggle(); break;
    case K_BOOL: bool_toggle(r); break;
    }
}

// ---- the key capture --------------------------------------------------------------------------
// Keys that cannot be a binding: modifiers (they ride on another key), Esc (the game's quit
// dialog), the Windows keys, and the IME / packet codes a layout sends on its own.
static int cap_excluded(int vk) {
    if (vk < 0x08 || vk > 0xFE) return 1;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) return 1;
    if (vk >= 0xA0 && vk <= 0xA5) return 1;
    if (vk == VK_ESCAPE || vk == VK_LWIN || vk == VK_RWIN) return 1;
    if (vk >= 0x15 && vk <= 0x19) return 1;
    if (vk >= 0x1C && vk <= 0x1F) return 1;
    if (vk == 0xE5 || vk == 0xE7 || vk == 0xE8) return 1;
    if (vk >= 0xF6) return 1;
    return 0;
}

static void cap_end(UINT vk) {
    g_cap = -1;
    g_settle = vk;
}

static void cap_assign(int bi, UINT b, UINT vk) {
    const char* name = hk_binding_name(bi);
    *hk_binding_vk(bi) = b;
    char lab[32] = "";
    if (b) vk_label(b, lab, sizeof lab);
    cap_end(vk);
    int ok = config_write("hotkeys", name, lab);
    ff132_log(FF132_TAG "settings: hotkey %s = %s (%s)\n", name, b ? lab : "(unbound)",
          ok ? "ini written" : "ini NOT written");
}

void ff132_setup_capture_tick(void) {
    int bi = g_cap;
    if (!g_open || bi < 0) return;
    int act = hk_active();
    UINT got = 0;
    for (int vk = 0x08; vk <= 0xFE; vk++) {
        unsigned char d = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
        if (d && !g_prev[vk] && !got && act && !cap_excluded(vk)) got = (UINT)vk;
        g_prev[vk] = d;
    }
    if (!got) return;
    UINT b = got | hk_mods_now();
    UINT mine = *hk_binding_vk(bi);
    const char* name = hk_binding_name(bi);
    char lab[32], msg[160];
    vk_label(b, lab, sizeof lab);
    if ((b == g_helpVk || b == g_cancelVk) && b != mine) {
        cap_end(got);
        ff132_logv(FF132_TAG "settings: key for %s not changed\n", name);
        return;
    }
    if (b == mine) {
        if (!strcmp(name, "help")) {
            ff132_flash_c(ui_text(T_SET_CANT_UNBIND_HELP), FF132_COL_MENU_OFF);
            return;
        }
        if (!strcmp(name, "menu_ok")) {
            ff132_flash_c(ui_text(T_SET_CANT_UNBIND_OK), FF132_COL_MENU_OFF);
            return;
        }
        cap_assign(bi, 0, got);
        return;
    }
    int other = bind_owner(b, bi);
    if (other >= 0) {
        snprintf(msg, sizeof msg, ui_text(T_SET_USED), lab, fn_label(hk_binding_name(other)));
        ff132_flash_c(msg, FF132_COL_MENU_OFF);
        ff132_log(FF132_TAG "settings: refused %s: used by %s\n", lab, hk_binding_name(other));
        return;
    }
    if (ok_tidy_clash(name, b)) {
        ff132_flash_c(ui_text(!strcmp(name, "menu_ok") ? T_SET_OK_IS_TIDY : T_SET_TIDY_IS_OK),
                   FF132_COL_MENU_OFF);
        ff132_log(FF132_TAG "settings: refused %s for %s: the same physical key as %s\n", lab, name,
              !strcmp(name, "menu_ok") ? "save_tidy" : "menu_ok");
        return;
    }
    cap_assign(bi, b, got);
}

int ff132_setup_settling(void) {
    UINT k = g_settle;
    if (!k) return 0;
    if (GetAsyncKeyState((int)k) & 0x8000) return 1;
    g_settle = 0;
    return 0;
}

// ---- drawing ----------------------------------------------------------------------------------
// Render thread. The same lapse guard as ff132_tidy_visible: a screen nobody can dismiss must not
// survive the gate that was keeping its keys from the game.
int ff132_setup_visible(void) {
    static DWORD staleSince = 0;
    if (!g_open) { staleSince = 0; return 0; }
    if (ff132_di_gate_left_ms() > 0) { staleSince = 0; return 1; }
    DWORD now = GetTickCount();
    if (!staleSince) { staleSince = now; return 1; }
    if (now - staleSince <= 2000) return 1;
    staleSince = 0;
    g_open = 0;
    g_cap = -1;
    ff132_loga(FF132_TAG "settings: the input gate lapsed under the screen -- closed\n");
    return 0;
}

static void key_col(UINT v, int capturing, char* out, size_t n) {
    char l[32];
    if (capturing) { snprintf(out, n, "[???]"); return; }
    if (!v) { snprintf(out, n, "-"); return; }
    vk_label(v, l, sizeof l);
    snprintf(out, n, "[%s]", l);
}
// Shown as the game numbers its saves (file index + 1); the ini keeps the file index.
static void wslot_value(char* out, size_t n) {
    int last = ff132_slots_last();
    if (wslot_stop() == WS_OFF) { snprintf(out, n, "%s", ui_text(T_SET_OFF)); return; }
    if (g_autosaveSlot >= 0)  snprintf(out, n, ui_text(T_SET_SLOT_LAST_N), g_autosaveSlot + 1);
    else if (last > 0)        snprintf(out, n, ui_text(T_SET_SLOT_LAST_N), last);
    else                      snprintf(out, n, "%s", ui_text(T_SET_SLOT_LAST));
}

// ui_arrows around `value`, padded to the widest value any row with arrows can show (`pool`), so
// the right arrows of every row stand in one column. `bare` lays spaces where the arrows would be,
// for a row that cannot change but should still read in the same column.
static void arrows_fit_ex(char* out, size_t n, const char* value, int bare) {
    char s0[48], s3[16], s4[16];
    snprintf(s0, sizeof s0, ui_text(T_SET_SLOT_LAST_N), FF132_SLOTS_MAX);
    snprintf(s3, sizeof s3, "%d", FF132_SLOTS_MAX);
    snprintf(s4, sizeof s4, "%d%%", FF132_SCALE_MAX);
    const char* const pool[] = { ui_text(T_SET_ON), ui_text(T_SET_OFF), s0, s3,
                                 kLogName[0], kLogName[1], kLogName[2],
                                 ui_text(T_SET_FONT_AUTO), s4 };
    // Centred between the arrows: the gap to the widest value is split in px, each half laid from
    // measured spaces, so a value one cell short of the widest does not sit half a cell off.
    int mx = 0;
    for (size_t i = 0; i < sizeof pool / sizeof pool[0]; i++) {
        int w = ff132_ovl_text_w(pool[i]);
        if (w > mx) mx = w;
    }
    int gap = mx - ff132_ovl_text_w(value);
    if (gap < 0) gap = 0;
    char lp[64], rp[64], v[160];
    int carry = ff132_ovl_space(lp, sizeof lp, gap / 2);
    ff132_ovl_space(rp, sizeof rp, gap - gap / 2 + carry);
    snprintf(v, sizeof v, "%s%s%s", lp, value, rp);
    if (!bare) { ui_arrows(out, n, v); return; }
    char a[32], sp[64];
    ui_arrows(a, sizeof a, "");
    char* cut = strchr(a, ' ');
    if (cut) cut[1] = 0;                    // the left arrow and its space
    ff132_ovl_space(sp, sizeof sp, ff132_ovl_text_w(a));
    snprintf(out, n, "%s%s", sp, v);
}
static void arrows_fit(char* out, size_t n, const char* value) { arrows_fit_ex(out, n, value, 0); }

// A settings row is "label:" ROW_TAB "value"; the draw puts every value at one column, past the
// widest label, since the Japanese labels are measured rather than counted.
#define ROW_TAB "\t"
static unsigned long row_text(int r, int sel, int cap, char* out, size_t n) {
    char k[256];
    const char* mark = (r == sel) ? ">" : " ";
    int dim = 0;
    switch (row_kind(r)) {
    case K_HEAD:
        snprintf(out, n, "%s", ui_text(r == R_H_KEYS ? T_SET_HEAD_KEYS : T_SET_HEAD_SET));
        return FF132_COL_HEAD;
    case K_KEY: {
        int bi = bind_idx(kKeyRowName[r]);
        key_col(bi >= 0 ? *hk_binding_vk(bi) : 0, cap >= 0 && cap == bi, k, sizeof k);
        snprintf(out, n, "%s %-16s %s", mark, k, fn_label(kKeyRowName[r]));
        dim = bind_dup(bi);
        break;
    }
    case K_KEYS: {
        int o = snprintf(out, n, "%s %s", mark, ui_text(T_SET_KEYS_LABEL));
        int sub = g_sub;
        for (int i = 0; i < SUB_N && o < (int)n; i++) {
            int bi = bind_idx(kSubName[i]);
            UINT v = bi >= 0 ? *hk_binding_vk(bi) : 0;
            char l[32];
            if (cap >= 0 && cap == bi) snprintf(l, sizeof l, "???");
            else vk_label(v, l, sizeof l);
            o += snprintf(out + o, n - (size_t)o, "%s%s%s[%s]", i < SUB_N - 1 ? " " : "",
                          (r == sel && i == sub) ? ">" : "", ui_text(kSubShort[i]), l);
            if (bind_dup(bi)) dim = 1;
        }
        break;
    }
    case K_BOOL: {
        int v = 0, t = T_SET_L_FOCUS;
        switch (r) {
        case R_FOCUS:   v = g_hkGlobal;         t = T_SET_L_FOCUS;   break;
        case R_KNOTE:   v = g_keepNote;         t = T_SET_L_KNOTE;   break;
        case R_HIST:    v = g_directLoad;       t = T_SET_L_HIST;    break;
        case R_RECAP:   v = g_skipRecap;        t = T_SET_L_RECAP;   break;
        }
        const char* const onOff[2] = { ui_text(T_SET_ON), ui_text(T_SET_OFF) };
        arrows_fit(k, sizeof k, onOff[v ? 0 : 1]);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(t), k);
        break;
    }
    case K_SLOT: {
        char sv[48];
        wslot_value(sv, sizeof sv);
        dim = !wslot_any_open();
        arrows_fit_ex(k, sizeof k, sv, dim);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_REDIR), k);
        break;
    }
    case K_SLOTS: {
        char sv[16];
        snprintf(sv, sizeof sv, "%d", g_slotCount);
        dim = g_slotsNa != 0 || slots_locked() || !slots_any_open();
        arrows_fit_ex(k, sizeof k, sv, dim);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_SLOTS), k);
        break;
    }
    case K_NOMOD: {
        char nv[FF132_CATNAME_U8];
        ff132_nomod_label(ff132_nomod_at(), nv, sizeof nv);
        dim = g_nomodCloud;
        arrows_fit_ex(k, sizeof k, nv, dim);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_NOMOD), k);
        break;
    }
    case K_MULT: {
        double c[CAND_MAX];
        int cn = mult_cands(c), ms = g_msub;
        int o = snprintf(out, n, "%s %s:" ROW_TAB, mark, ui_text(T_SET_L_MULT));
        for (int i = 0; i < cn && o < (int)n; i++) {
            char f[16];
            ff132_gs_fmt(c[i], f, sizeof f);
            char v[24];
            snprintf(v, sizeof v, ui_text(mult_has(c[i]) ? T_SET_V_MULT_IN : T_SET_V_MULT_OUT), f);
            o += snprintf(out + o, n - (size_t)o, "%s%s%s", i ? " " : "",
                          (r == sel && i == ms) ? ">" : "", v);
        }
        break;
    }
    case K_FONT: {
        char sv[24];
        int v = g_ovlScale;
        if (v == FF132_SCALE_AUTO) snprintf(sv, sizeof sv, "%s", ui_text(T_SET_FONT_AUTO));
        else                       snprintf(sv, sizeof sv, "%d%%", v);
        arrows_fit(k, sizeof k, sv);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_FONT), k);
        break;
    }
    case K_LOG: {
        int l = g_logLevel;
        arrows_fit(k, sizeof k, kLogName[(l >= 0 && l <= 2) ? l : 1]);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_LOG), k);
        break;
    }
    }
    if (r == sel) return FF132_COL_MENU_SEL;
    return dim ? FF132_COL_MENU_OFF : FF132_COL_MENU;
}

// Which working-slot stops a save of the player's own blocks; empty = none.
static void wslot_na_text(char* out, size_t n) {
    int last = wslot_stop() != WS_LAST ? g_naLast : -1;
    char lab[48];
    out[0] = 0;
    if (last >= 0) snprintf(lab, sizeof lab, ui_text(T_SET_SLOT_LAST_N), last + 1);
    if (last >= 0) snprintf(out, n, ui_text(T_SET_WS_NA_ONE), lab, last + 1);
}

// A line break the footer always takes; a "\n" in a text only says where to break if it wraps.
#define FOOT_BREAK "\f"

// The slot-limit stops that cannot be picked now, one line per reason; empty = none.
static void slots_na_text(char* out, size_t n) {
    int o = 0;
    out[0] = 0;
    for (int why = SN_SAVES; why < SN_N; why++) {
        char list[96] = "", item[24];
        int l = 0;
        for (int i = 0; i < SLOT_STOPS; i++) {
            if (kSlotStops[i] == g_slotCount || slots_why(kSlotStops[i]) != why) continue;
            snprintf(item, sizeof item, ui_text(T_SET_NA_ITEM), kSlotStops[i]);
            l += snprintf(list + l, sizeof list - (size_t)l, "%s%s", l ? ui_text(T_SET_NA_SEP) : "", item);
        }
        if (!l || o >= (int)n) continue;
        o += snprintf(out + o, n - (size_t)o, "%s", o ? FOOT_BREAK : "");
        if (why == SN_SAVES)
            o += snprintf(out + o, n - (size_t)o, ui_text(T_SET_SLOTS_NA_SAVES), list, FF132_SLOTS_GAME + 1,
                          g_slotAbove);
        else if (why == SN_WSLOT)
            o += snprintf(out + o, n - (size_t)o, ui_text(T_SET_SLOTS_NA_WSLOT), list, g_autosaveSlot + 1);
        else
            o += snprintf(out + o, n - (size_t)o, ui_text(T_SET_SLOTS_NA_GUI), list);
    }
}

static void footer_text(int sel, int cap, char* out, size_t n) {
    char a[32], b[32], ok[48];
    if (cap >= 0) {
        vk_label(g_helpVk, a, sizeof a);
        vk_label(g_cancelVk, b, sizeof b);
        snprintf(out, n, ui_text(T_SET_F_CAPTURE), a, b);
        return;
    }
    vk_label(g_menuLeftVk, a, sizeof a);
    vk_label(g_menuRightVk, b, sizeof b);
    ff132_tidy_ok_phrase(ok, sizeof ok);
    char body[768];
    int dup = 0;
    switch (sel) {
    case R_K_SPEED:  snprintf(body, sizeof body, ui_text(T_SET_F_SPEED), ff132_gs_steps_arrow()); break;
    case R_K_KILL:   snprintf(body, sizeof body, "%s", ui_text(T_SET_F_KILL));   break;
    case R_K_ENC:    snprintf(body, sizeof body, "%s", ui_text(T_SET_F_ENC));    break;
    case R_K_RAUTO:  snprintf(body, sizeof body, "%s", ui_text(T_SET_F_AR_AUTO)); break;
    case R_K_SLOT:   snprintf(body, sizeof body, "%s", ui_text(T_SET_F_SLOT));   break;
    case R_K_TIDY:   snprintf(body, sizeof body, "%s", ui_text(T_SET_F_TIDY));   break;
    case R_K_BATTLE: snprintf(body, sizeof body, "%s", ui_text(T_SET_F_BATTLE)); break;
    case R_K_HELP:   snprintf(body, sizeof body, "%s", ui_text(T_SET_F_HELP));   break;
    case R_KEYS:
        snprintf(body, sizeof body, ui_text(T_SET_F_KEYS), a, b, ok);
        dup = bind_dup(bind_idx(kSubName[g_sub]));
        break;
    case R_FOCUS:    snprintf(body, sizeof body, "%s", ui_text(T_SET_F_FOCUS));   break;
    case R_REDIR: {
        char na[320];
        wslot_na_text(na, sizeof na);
        snprintf(body, sizeof body, "%s%s%s", ui_text(T_SET_F_REDIR), na[0] ? FOOT_BREAK : "", na);
        break;
    }
    case R_HIST:     snprintf(body, sizeof body, "%s", ui_text(T_SET_F_HIST));    break;
    case R_SLOTS:
        if (g_slotsNa == 1)      snprintf(body, sizeof body, "%s", ui_text(T_SET_SLOTS_CLOUD));
        else if (g_slotsNa == 2) snprintf(body, sizeof body, ui_text(T_SET_SLOTS_ODD), g_slotOwn);
        else if (slots_locked()) snprintf(body, sizeof body, "%s", ui_text(T_SET_SLOTS_WS_ON));
        else {
            char na[384];
            int o = snprintf(body, sizeof body, ui_text(T_SET_F_SLOTS),
                             g_slotReach > 0 ? g_slotReach : FF132_SLOTS_MAX_NOGUI);
            slots_na_text(na, sizeof na);
            if (na[0] && o > 0 && o < (int)sizeof body) snprintf(body + o, sizeof body - (size_t)o, FOOT_BREAK "%s", na);
        }
        break;
    case R_KNOTE:    snprintf(body, sizeof body, "%s", ui_text(T_SET_F_KNOTE));  break;
    case R_NOMOD:
        snprintf(body, sizeof body, "%s", ui_text(g_nomodCloud ? T_SET_NOMOD_CLOUD : T_SET_F_NOMOD));
        break;
    case R_RECAP:    snprintf(body, sizeof body, "%s", ui_text(T_SET_F_RECAP));  break;
    case R_MULT: {
        int o = snprintf(body, sizeof body, ui_text(T_SET_F_MULT), a, b, ok, ff132_gs_steps_arrow());
        double c[CAND_MAX];
        int cn = mult_cands(c), ms = g_msub;
        ff132_msg why = (ms >= 0 && ms < cn) ? mult_blocked(c[ms]) : T_COUNT;
        if (why != T_COUNT && o > 0 && o < (int)sizeof body)
            snprintf(body + o, sizeof body - (size_t)o, FOOT_BREAK "%s", ui_text(why));
        break;
    }
    case R_FONT:     snprintf(body, sizeof body, ui_text(T_SET_F_FONT), ff132_ovl_scale_pct()); break;
    case R_LOG:      snprintf(body, sizeof body, "%s", ui_text(T_SET_F_LOG));    break;
    default:         body[0] = 0; break;
    }
    if (row_kind(sel) == K_KEY) dup = bind_dup(bind_idx(kKeyRowName[sel]));
    snprintf(out, n, "%s%s", dup ? ui_text(T_SET_F_DUP) : "", body);
}

// The footer, one wrapped block per FOOT_BREAK; rows used. Cuts `foot` in place while it draws.
static int foot_say(int x, int y, char* foot, int draw) {
    int rows = 0;
    char* p = foot;
    for (;;) {
        char* e = strchr(p, FOOT_BREAK[0]);
        if (e) *e = 0;
        rows += draw ? ff132_ovl_say_wrap(x, x, y + rows * FF132_OVL_LINE_H, p, FF132_COL_TEXT, FF132_OVL_LINE_H)
                     : ff132_ovl_wrap_count(x, x, p);
        if (!e) break;
        *e = FOOT_BREAK[0];
        p = e + 1;
    }
    return rows;
}

// The first row drawn while the screen is too short to show them all. Render thread; kept from frame
// to frame, so the window only moves when the selected row would leave it.
static int g_scrollTop = 0;

int ff132_setup_draw(int x, int y) {
    static char rows[R_N][384];
    static unsigned long cols[R_N];
    char title[384], foot[896], hint[96];
    char hp[32], up[32], dn[32], lf[32], rt[32], ok[48];
    int sel = g_sel, cap = g_cap;
    int cw = ff132_ovl_char_w();
    int contX = x + 17 * cw;
    vk_label(g_helpVk, hp, sizeof hp);
    vk_label(g_menuUpVk, up, sizeof up);   vk_label(g_menuDownVk, dn, sizeof dn);
    vk_label(g_menuLeftVk, lf, sizeof lf); vk_label(g_menuRightVk, rt, sizeof rt);
    ff132_tidy_ok_phrase(ok, sizeof ok);
    snprintf(title, sizeof title, ui_text(T_SET_TITLE), hp, up, dn, lf, rt, ok);
    for (int r = 0; r < R_N; r++) cols[r] = row_text(r, sel, cap, rows[r], sizeof rows[0]);
    footer_text(sel, cap, foot, sizeof foot);
    hint[0] = 0;
    if (cap < 0 && sel >= 0 && sel < R_N && (row_kind(sel) == K_KEY || row_kind(sel) == K_KEYS))
        snprintf(hint, sizeof hint, ui_text(T_SET_HINT_CHANGE), ok);
    char* vals[R_N];
    int valX = x;
    for (int r = 0; r < R_N; r++) {
        char* t = strchr(rows[r], '\t');
        vals[r] = t;
        if (!t) continue;
        *t = 0;
        int w = x + ff132_ovl_text_w(rows[r]) + cw;
        if (w > valX) valX = w;
    }
    int lh = FF132_OVL_LINE_H;
    int titleRows = ff132_ovl_wrap_count(x, x, title);
    int footRows = foot_say(x, 0, foot, 0);
    int nr[R_N], body = 0;
    int hintX = x, hintRow = 0;
    for (int r = 0; r < R_N; r++) {
        nr[r] = vals[r] ? ff132_ovl_wrap_count(valX, contX, vals[r] + 1)
              : (r == sel && hint[0]) ? ff132_ovl_wrap_tail(x, contX, rows[r], hint, &hintX, &hintRow)
                                      : ff132_ovl_wrap_count(x, contX, rows[r]);
        body += nr[r];
    }
    // A screen too short for every row (a large text size, or a small window) shows a window of them
    // between the title and the footer, kept around the selected row, with a mark on the side rows
    // are left out on. Two rows stay free under the screen for the flash, which the overlay draws
    // there: it is what answers a setting that could not be changed.
    int first = 0, last = R_N, marks = 0;
    int scrH = ff132_ovl_screen_h();
    if (scrH > 0 && y + (titleRows + body + footRows) * lh + ff132_ovl_px(8) > scrH) {
        int s = (sel >= 0 && sel < R_N) ? sel : R_K_SPEED;
        int room = (scrH - y - ff132_ovl_px(12) - ff132_ovl_px(8)) / lh - titleRows - footRows
                 - 2 /* the marks */ - 2 /* the flash */;
        if (room < nr[s]) room = nr[s];
        int top = g_scrollTop;
        if (top < 0 || top >= R_N) top = 0;
        if (top > s) top = s;
        // A heading right above the selected row comes into view with it.
        if (top == s && s > 0 && row_kind(s - 1) == K_HEAD && nr[s - 1] + nr[s] <= room) top = s - 1;
        int used = 0;
        for (int r = top; r <= s; r++) used += nr[r];
        while (used > room && top < s) used -= nr[top++];
        last = s + 1;
        while (last < R_N && used + nr[last] <= room) used += nr[last++];
        // At the end of the list, rows above take up what is left instead of the space going empty.
        while (last == R_N && top > 0 && used + nr[top - 1] <= room) used += nr[--top];
        g_scrollTop = first = top;
        marks = 1;
    }
    int lines = titleRows + footRows + (marks ? 2 : 0);
    for (int r = first; r < last; r++) lines += nr[r];
    int selY = y + (titleRows + marks) * lh, selRows = 1;
    for (int r = first; r < last && r < sel; r++) selY += nr[r] * lh;
    if (sel >= first && sel < last) selRows = nr[sel];
    ff132_ovl_backdrop(0, y - ff132_ovl_px(4), FF132_OVL_FULL_W, y + lines * lh + ff132_ovl_px(8),
                       0x99000000);
    if (sel >= first && sel < last) FF132_MENU_BAND(selY, selRows);
    y += lh * ff132_ovl_say_wrap(x, x, y, title, FF132_COL_TITLE, lh);
    if (marks) {
        if (first > 0) ff132_ovl_say(x + 2 * cw, y, "\xE2\x96\xB2", FF132_COL_MENU_OFF);   // U+25B2
        y += lh;
    }
    for (int r = first; r < last; r++) {
        int n;
        if (vals[r]) {
            ff132_ovl_say(x, y, rows[r], cols[r]);
            n = ff132_ovl_say_wrap(valX, contX, y, vals[r] + 1, cols[r], lh);
        } else {
            n = ff132_ovl_say_wrap(x, contX, y, rows[r], cols[r], lh);
        }
        if (!vals[r] && r == sel && hint[0]) {
            ff132_ovl_say(hintX, y + hintRow * lh, hint, FF132_COL_MENU_OFF);
            n = nr[r];
        }
        y += lh * n;
    }
    if (marks) {
        if (last < R_N) ff132_ovl_say(x + 2 * cw, y, "\xE2\x96\xBC", FF132_COL_MENU_OFF);  // U+25BC
        y += lh;
    }
    y += lh * foot_say(x, y, foot, 1);
    return y + ff132_ovl_px(12);
}
