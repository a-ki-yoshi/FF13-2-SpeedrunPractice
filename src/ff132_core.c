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

#include "ff132_internal.h"

// ---- config defaults -- these ARE the shipped layout ------------------------------------------
// The README states these defaults; changing one here means changing every place that documents it.
UINT g_helpVk = VK_F1;                          // [hotkeys] help (default F1)
UINT g_slotSelectVk = VK_F7;                    // [hotkeys] slot_select (default F7)
volatile unsigned char g_autosaveRedirect = 0;  // [save] autosave_redirect: working slot (default off)
int g_autosaveSlot = -1;                        // [save] autosave_slot: -1 = game's own last slot
volatile int g_slotCount = FF132_SLOTS_GAME;       // [save] slot_count: the game's own (default empty)
UINT g_battleOverlayVk = VK_F10;                // [hotkeys] battle_overlay (default F10)
UINT g_saveTidyVk = VK_F9;                      // [hotkeys] save_tidy (default F9)
UINT g_menuOkVk = VK_RETURN;                    // [hotkeys] menu_ok (default Enter)
UINT g_menuUpVk = VK_UP;                        // [hotkeys] menu_up (default Up)
UINT g_menuDownVk = VK_DOWN;                    // [hotkeys] menu_down (default Down)
UINT g_menuLeftVk = VK_LEFT;                    // [hotkeys] menu_left (default Left)
UINT g_menuRightVk = VK_RIGHT;                  // [hotkeys] menu_right (default Right)
UINT g_listPrevVk = 'A';                        // [hotkeys] list_prev (default A)
UINT g_listNextVk = 'D';                        // [hotkeys] list_next (default D)
UINT g_cancelVk = VK_BACK;                      // [hotkeys] cancel (default Backspace)
UINT g_gameSpeedVk = VK_F2;                     // [hotkeys] game_speed (default F2)
UINT g_killModeVk = VK_F3;                      // [hotkeys] kill_mode (default F3)
UINT g_enemySuppressVk = VK_F5;                 // [hotkeys] enemy_suppress (default F5)
UINT g_battleRestartAutoVk = VK_F6;             // [hotkeys] battle_restart_auto (default F6)
double g_gsSteps[FF132_GS_STEPS_MAX] = { 3.0 };    // [game_speed] multiplier (default 3.0)
int g_gsStepN = 1;
volatile int g_ovlScale = 100;                  // [overlay] font_scale (default 1 = the original size)
// The defaults that are NOT here: g_battleOverlay (restored from the state file) and g_directLoad
// ([load] skip_historia_crux) sit beside the toggles that are the only other things to write them.
// GetAsyncKeyState reads the keyboard, not the window, so without this every hotkey here fires
// from inside a browser. A setting rather than a rule because a wrong focus test takes EVERY
// hotkey with it, and 0 is the only way back from inside the game.
volatile int g_hkGlobal = 0;
int g_logLevel = 1;

uintptr_t g_base = 0;

// ---- where the files this mod writes go -------------------------------------------------------
static int exe_sibling(char* out, size_t n, const char* name) {
    char p[MAX_PATH];
    if (!GetModuleFileNameA(NULL, p, MAX_PATH)) return 0;
    char* s = strrchr(p, '\\'); if (!s) return 0;
    *(s + 1) = 0;
    if (strlen(p) + strlen(name) >= n) return 0;
    snprintf(out, n, "%s%s", p, name);
    return 1;
}
static int  g_dataDirOk = -1;                      // -1 = not tried, 1 = the folder, 0 = beside the exe
static char g_dataDirPath[MAX_PATH] = "";
static void data_dir_ready(void) {
    if (g_dataDirOk >= 0) return;
    char dir[MAX_PATH];
    if (!exe_sibling(dir, sizeof(dir), MOD_DATA_DIR)) { g_dataDirOk = 0; return; }
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_dataDirOk = 0;                           // read-only install: carry on beside the exe
        return;
    }
    snprintf(g_dataDirPath, sizeof(g_dataDirPath), "%s\\", dir);
    g_dataDirOk = 1;
}
int data_file(char* out, size_t n, const char* name) {
    data_dir_ready();
    if (g_dataDirOk != 1) return exe_sibling(out, n, name);
    if (strlen(g_dataDirPath) + strlen(name) >= n) return 0;
    snprintf(out, n, "%s%s", g_dataDirPath, name);
    return 1;
}

// A small file written whole: a temp file beside it, flushed, then renamed over it, so a crash
// leaves the old file or the new one, never half of either. One lock for every caller, so the one
// temp name is never shared and a crash leaves no file the next write would not replace. A rename a
// scanner holds up is retried. 0 = not written (the old file, if any, is still there), with
// GetLastError saying why. A caller whose snapshot can race another thread's holds its own lock
// around taking it and writing it.
static SRWLOCK g_wholeLock = SRWLOCK_INIT;
int ff132_write_whole(const char* path, const char* text, size_t len) {
    char tmp[MAX_PATH + 8];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    AcquireSRWLockExclusive(&g_wholeLock);
    DWORD err = 0;
    int moved = 0;
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
    } else {
        DWORD wrote = 0;
        int ok = WriteFile(h, text, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len;
        if (!ok) err = GetLastError();
        FlushFileBuffers(h);          // best effort: an unflushed rename still beats no write
        CloseHandle(h);
        for (int i = 0; ok && !moved && i < 5; i++) {
            if (i) Sleep(20);
            moved = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
            if (!moved) err = GetLastError();
        }
        if (!moved) DeleteFileA(tmp);
    }
    ReleaseSRWLockExclusive(&g_wholeLock);
    if (!moved) SetLastError(err);
    return moved;
}

// ---- logging ----------------------------------------------------------------------------------
#ifndef FF132_BUILD_ID
#define FF132_BUILD_ID "unknown"
#endif
static void ff132_log_write(const char* fmt, va_list ap) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), FF132_LOG_NAME)) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    vfprintf(f, fmt, ap);
    fclose(f);
}
// One log per launch, with the launch before it kept: a crash investigation needs the lines from
// the run that died, which an unrotated fresh log would wipe.
void ff132_log_reset(void) {
    char path[MAX_PATH], prev[MAX_PATH];
    if (!data_file(path, sizeof(path), FF132_LOG_NAME)) return;
    if (data_file(prev, sizeof(prev), FF132_PREV_NAME))
        MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, FF132_TAG "==== FF13-2-SpeedrunPractice, build %s ====\n", FF132_BUILD_ID);
    fclose(f);
}
void ff132_loga(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); ff132_log_write(fmt, ap); va_end(ap);
}
void ff132_log(const char* fmt, ...) {
    if (g_logLevel < 1) return;
    va_list ap; va_start(ap, fmt); ff132_log_write(fmt, ap); va_end(ap);
}
void ff132_logv(const char* fmt, ...) {
    if (g_logLevel < 2) return;
    va_list ap; va_start(ap, fmt); ff132_log_write(fmt, ap); va_end(ap);
}

// ---- hotkey polling ---------------------------------------------------------------------------
// Asked of the foreground window's PROCESS, not of a window handle: the game creates more than one
// window over its life, so a handle captured at startup would stop matching.
int hk_active(void) {
    if (g_hkGlobal) return 1;
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
UINT hk_mods_now(void) {
    UINT m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= HK_CTRL;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= HK_SHIFT;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= HK_ALT;
    return m;
}
// The modifier match is EXACT: a modified binding must not fire the plain key, and the plain key
// must not fire while a modifier is down. A binding that IS a modifier excuses itself, since it
// would otherwise never be down.
int hk_down(UINT b) {
    UINT k = hk_key(b);
    if (!(GetAsyncKeyState((int)k) & 0x8000)) return 0;
    if (k == VK_CONTROL || k == VK_SHIFT || k == VK_MENU) return 1;
    return hk_mods_now() == hk_mods(b);
}
// One press = one fire, edge-detected on GetAsyncKeyState's MSB "currently down" bit -- the LSB
// "recently pressed" bit is documented as unreliable. The down/up state is tracked EVEN WHILE THE
// GAME IS NOT IN FRONT and only the firing is gated, so a key held in another window does not fire
// on alt-tab back.
int hk_edge(UINT b, int* was) {
    if (!b) return 0;                              // 0 is never a real binding: unbound
    int now = hk_down(b);
    int fire = now && !*was && hk_active();
    *was = now;
    return fire;
}

// ---- ini parser (no deps): [section], key = value, ; / # comments ------------------------------
static void cfg_trim(char* s) {
    char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l && (s[l-1]==' ' || s[l-1]=='\t' || s[l-1]=='\r' || s[l-1]=='\n')) s[--l] = 0;
}
static int cfg_ieqn(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}
int cfg_ieq(const char* a, const char* b) {
    size_t la = strlen(a);
    return la == strlen(b) && cfg_ieqn(a, b, la);
}
static int cfg_on(const char* v) {
    return cfg_ieq(v, "1") || cfg_ieq(v, "on") || cfg_ieq(v, "true") || cfg_ieq(v, "yes");
}
// [hotkeys] name -> the key it sets. What each one DOES is documented in the README, not here.
static const struct { const char* name; UINT* vk; } kHotkeys[] = {
    { "help", &g_helpVk },
    { "slot_select", &g_slotSelectVk },
    { "battle_overlay", &g_battleOverlayVk },
    { "save_tidy", &g_saveTidyVk },
    { "menu_ok", &g_menuOkVk },
    { "menu_up", &g_menuUpVk },       { "menu_down", &g_menuDownVk },
    { "menu_left", &g_menuLeftVk },   { "menu_right", &g_menuRightVk },
    { "list_prev", &g_listPrevVk },   { "list_next", &g_listNextVk },
    { "cancel", &g_cancelVk },
    { "game_speed", &g_gameSpeedVk },
    { "kill_mode", &g_killModeVk },
    { "enemy_suppress", &g_enemySuppressVk },
    { "battle_restart_auto", &g_battleRestartAutoVk },
};

void vk_label(UINT b, char* out, size_t n) { ff13_vk_label(b, out, n); }

int hk_binding_count(void) { return (int)(sizeof kHotkeys / sizeof kHotkeys[0]); }
const char* hk_binding_name(int i) {
    return (i >= 0 && i < hk_binding_count()) ? kHotkeys[i].name : NULL;
}
UINT* hk_binding_vk(int i) {
    return (i >= 0 && i < hk_binding_count()) ? kHotkeys[i].vk : NULL;
}

// "3.00" or "2.00 / 3.00 / 0.50" -- the steps the game-speed hotkey walks through, in order.
static const char* gs_steps_str(void) {
    static char buf[FF132_GS_STEPS_MAX * 8];
    int n = 0;
    buf[0] = 0;
    for (int i = 0; i < g_gsStepN && n < (int)sizeof(buf) - 8; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%.2f", i ? " / " : "", g_gsSteps[i]);
    return buf;
}
void ff132_gs_fmt(double v, char* out, size_t n) {
    snprintf(out, n, "%.2f", v);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '0') out[--l] = 0;
    if (l > 1 && out[l - 1] == '.') out[--l] = 0;
}
const char* ff132_gs_steps_arrow(void) {
    static char buf[FF132_GS_STEPS_MAX * 16 + 8];
    int n = 0;
    buf[0] = 0;
    for (int i = 0; i < g_gsStepN && n < (int)sizeof(buf) - 16; i++) {
        char f[16];
        ff132_gs_fmt(g_gsSteps[i], f, sizeof f);
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%sx%s", f,
                      ui_ascii_only() ? "->" : "\xE2\x86\x92");
    }
    snprintf(buf + n, sizeof buf - (size_t)n, "OFF");
    return buf;
}
// One typo should not take the other steps with it: a bad entry is dropped and reported, and the
// rest of the list is kept.
static void gs_parse(const char* val) {
    double v[FF132_GS_STEPS_MAX];
    int n = 0;
    const char* p = val;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        if (n >= FF132_GS_STEPS_MAX) {
            ff132_loga(FF132_TAG "config: multiplier takes %d steps at most -> \"%s\" is not used\n",
                   FF132_GS_STEPS_MAX, p);
            break;
        }
        char* end = NULL;
        double fv = strtod(p, &end);
        if (end == p) {
            const char* c = strchr(p, ',');
            size_t len = c ? (size_t)(c - p) : strlen(p);
            ff132_loga(FF132_TAG "config: multiplier entry '%.*s' is not a number -> dropped\n",
                   (int)len, p);
            p += len;
            continue;
        }
        p = end;
        if (*p && *p != ',' && *p != ' ' && *p != '\t') {   // "3x" and the like
            const char* c = strchr(p, ',');
            ff132_loga(FF132_TAG "config: multiplier entry ending '%.*s' is not a number -> dropped\n",
                   (int)(c ? (size_t)(c - p) : strlen(p)), p);
            p = c ? c : p + strlen(p);
            continue;
        }
        if (fv >= 0.1 && fv <= 10.0) v[n++] = fv;
        else ff132_loga(FF132_TAG "config: multiplier %.2f is outside 0.1..10 -> dropped\n", fv);
    }
    if (!n) {
        ff132_loga(FF132_TAG "config: multiplier=%s has no usable value -> 3.0\n", val);
        v[n++] = 3.0;
    }
    for (int i = 0; i < n; i++) g_gsSteps[i] = v[i];
    g_gsStepN = n;
}

// [overlay] font_scale: "auto", a factor ("1.5") or a percentage ("150%"). Empty = the default. A
// size outside FF132_SCALE_MIN..MAX is pulled to the nearer end rather than dropped: it was asked for,
// and the end is the closest the overlay can come.
static void scale_parse(const char* val) {
    if (!val[0]) { g_ovlScale = 100; return; }
    if (cfg_ieq(val, "auto")) { g_ovlScale = FF132_SCALE_AUTO; return; }
    char* end = NULL;
    double v = strtod(val, &end);
    if (end == val) {
        ff132_loga(FF132_TAG "config: font_scale=%s is not auto or a number -> 1\n", val);
        g_ovlScale = 100;
        return;
    }
    while (*end == ' ' || *end == '\t') end++;
    if (*end == '%') {
        v /= 100.0;
        end++;
        while (*end == ' ' || *end == '\t') end++;
    }
    if (*end || v != v) {                        // trailing text, or "nan"
        ff132_loga(FF132_TAG "config: font_scale=%s is not auto or a number -> 1\n", val);
        g_ovlScale = 100;
        return;
    }
    int pct = v <= 0.0 ? 0 : v >= 100.0 ? FF132_SCALE_MAX + 1 : (int)(v * 100.0 + 0.5);
    if (pct < FF132_SCALE_MIN || pct > FF132_SCALE_MAX) {
        int to = pct < FF132_SCALE_MIN ? FF132_SCALE_MIN : FF132_SCALE_MAX;
        ff132_loga(FF132_TAG "config: font_scale=%s is outside %d%%..%d%% -> %d%%\n", val,
                   FF132_SCALE_MIN, FF132_SCALE_MAX, to);
        pct = to;
    }
    g_ovlScale = pct;
}

void config_load(void) {
    char path[MAX_PATH];
    FILE* f = exe_sibling(path, sizeof(path), FF132_INI_NAME) ? fopen(path, "r") : NULL;
    if (f) {
        char line[512], section[64] = {0};
        while (fgets(line, sizeof(line), f)) {
            cfg_trim(line);
            if (!line[0] || line[0]==';' || line[0]=='#') continue;
            if (line[0]=='[') {
                char* end = strchr(line, ']');
                if (end) { *end = 0; strncpy(section, line+1, sizeof(section)-1);
                           section[sizeof(section)-1]=0; cfg_trim(section); }
                continue;
            }
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char key[64], val[MAX_PATH];
            strncpy(key, line, sizeof(key)-1); key[sizeof(key)-1]=0;
            strncpy(val, eq+1, sizeof(val)-1); val[sizeof(val)-1]=0;
            cfg_trim(key); cfg_trim(val);
            for (char* c = val; *c; c++)
                if ((*c==';' || *c=='#') && c > val && (c[-1]==' ' || c[-1]=='\t')) { *c = 0; break; }
            cfg_trim(val);
            if (cfg_ieq(section, "hotkeys")) {
                if (cfg_ieq(key, "global_hotkeys")) {
                    g_hkGlobal = cfg_on(val);
                    continue;
                }
                // Settings screen only, on purpose: a stray press must not move the autosave target.
                // Still read so an old ini says why its line does nothing.
                if (cfg_ieq(key, "autosave_redirect") || cfg_ieq(key, "direct_load")) {
                    ff132_loga(FF132_TAG "config: [hotkeys] %s is no longer a hotkey -> ignored (switch it"
                                  " on the settings screen)\n", key);
                    continue;
                }
                for (int i = 0; i < (int)(sizeof kHotkeys / sizeof kHotkeys[0]); i++) {
                    if (!cfg_ieq(key, kHotkeys[i].name)) continue;
                    // Any key can be unbound: 0 is never a real binding and hk_edge cannot fire on
                    // it. NOT "0": that is the digit key, a binding.
                    if (!val[0] || cfg_ieq(val, "off") || cfg_ieq(val, "none")) {
                        *kHotkeys[i].vk = 0;
                        break;
                    }
                    UINT vk = ff13_vk_parse(val);
                    if (vk) *kHotkeys[i].vk = vk;
                    else ff132_loga(FF132_TAG "config: unrecognized hotkey '%s=%s' -> keeping 0x%X\n",
                                key, val, *kHotkeys[i].vk);
                    break;
                }
            } else if (cfg_ieq(section, "save")) {
                if (cfg_ieq(key, "autosave_redirect")) g_autosaveRedirect = cfg_on(val) ? 1 : 0;
                else if (cfg_ieq(key, "keep_note")) g_keepNote = cfg_on(val) ? 1 : 0;
                else if (cfg_ieq(key, "autosave_slot")) {
                    if (!val[0] || cfg_ieq(val, "auto") || cfg_ieq(val, "last")) {
                        g_autosaveSlot = -1;
                    } else {
                        long s = strtol(val, NULL, 10);
                        if (s >= 0 && s <= 999) g_autosaveSlot = (int)s;
                        else ff132_loga(FF132_TAG "config: autosave_slot=%s is outside 0..999 -> ignored,"
                                           " using the game's last slot\n", val);
                    }
                } else if (cfg_ieq(key, "slot_count")) {
                    char* end = NULL;
                    long s = val[0] ? strtol(val, &end, 10) : FF132_SLOTS_GAME;
                    if (val[0] && (end == val || *end || s < FF132_SLOTS_GAME
                                   || s > FF132_SLOTS_MAX)) {
                        ff132_loga(FF132_TAG "config: slot_count=%s is not %d..%d -> %d (the game's"
                                      " own)\n", val, FF132_SLOTS_GAME + 1, FF132_SLOTS_MAX,
                               FF132_SLOTS_GAME);
                        s = FF132_SLOTS_GAME;
                    }
                    g_slotCount = (int)s;
                }
            } else if (cfg_ieq(section, "load")) {
                // Starting states, not master switches: the hooks go in either way, so the settings
                // screen can turn either one on at runtime.
                if (cfg_ieq(key, "skip_historia_crux")) g_directLoad = cfg_on(val) ? 1 : 0;
                else if (cfg_ieq(key, "skip_story_recap")) g_skipRecap = cfg_on(val) ? 1 : 0;
            } else if (cfg_ieq(section, "game_speed")) {
                if (cfg_ieq(key, "multiplier")) gs_parse(val);
            } else if (cfg_ieq(section, "overlay")) {
                if (cfg_ieq(key, "font_scale")) scale_parse(val);
            } else if (cfg_ieq(section, "log")) {
                if (cfg_ieq(key, "level")) {
                    if      (cfg_ieq(val, "quiet"))   g_logLevel = 0;
                    else if (cfg_ieq(val, "verbose")) g_logLevel = 2;
                    else {
                        g_logLevel = 1;
                        if (!cfg_ieq(val, "normal"))
                            ff132_loga(FF132_TAG "config: level=%s is not quiet/normal/verbose -> normal\n",
                                   val);
                    }
                }
            }
        }
        fclose(f);
        ff132_loga(FF132_TAG "config: loaded %s\n", path);
    } else {
        ff132_loga(FF132_TAG "config: no %s beside the exe -> defaults\n", FF132_INI_NAME);
    }
    // menu_ok is the menus' only confirm, so it is never left empty, nor on the tidy key's PHYSICAL
    // key (by hk_key: a modifier does not tell the two apart once it is let up first), whose press
    // closes the menu.
    if (!g_menuOkVk || (g_saveTidyVk && hk_key(g_menuOkVk) == hk_key(g_saveTidyVk))) {
        UINT was = g_menuOkVk;
        // The first stand-in no other binding sits on: a clash with cancel would close the menu on
        // the very press that confirms.
        static const UINT kOk[] = { VK_RETURN, VK_SPACE, VK_TAB, 'E' };
        g_menuOkVk = 0;
        for (size_t c = 0; c < sizeof kOk / sizeof kOk[0] && !g_menuOkVk; c++) {
            int taken = 0;
            for (int i = 0; i < hk_binding_count(); i++)
                if (kHotkeys[i].vk != &g_menuOkVk && *kHotkeys[i].vk &&
                    hk_key(*kHotkeys[i].vk) == kOk[c]) taken = 1;
            if (!taken) g_menuOkVk = kOk[c];
        }
        if (!g_menuOkVk) g_menuOkVk = VK_RETURN;
        char l[32];
        vk_label(g_menuOkVk, l, sizeof l);
        ff132_loga(FF132_TAG "config: menu_ok %s -> %s (the menus have no other confirm)\n",
                   was ? "is the save_tidy key itself" : "is empty", l);
    }
    for (int i = 0; i < hk_binding_count(); i++)
        for (int j = i + 1; j < hk_binding_count(); j++) {
            UINT a = *kHotkeys[i].vk;
            if (!a || a != *kHotkeys[j].vk) continue;
            char l[32];
            vk_label(a, l, sizeof l);
            ff132_loga(FF132_TAG "config: %s and %s are both bound to %s\n", kHotkeys[i].name,
                   kHotkeys[j].name, l);
        }
    {
        char t[32], ok[32], up[32], dn[32], lf[32], rt[32], cn[32];
        vk_label(g_saveTidyVk, t, sizeof t);  vk_label(g_menuOkVk, ok, sizeof ok);
        vk_label(g_menuUpVk, up, sizeof up);  vk_label(g_menuDownVk, dn, sizeof dn);
        vk_label(g_menuLeftVk, lf, sizeof lf); vk_label(g_menuRightVk, rt, sizeof rt);
        vk_label(g_cancelVk, cn, sizeof cn);
        ff132_loga(FF132_TAG "config: save_tidy=%s menu_ok=%s menu_up=%s menu_down=%s menu_left=%s"
                      " menu_right=%s cancel=%s\n", t, ok, up, dn, lf, rt, cn);
    }
    {
        char gs[32], hp[32], es[32], ra[32], km[32];
        vk_label(g_gameSpeedVk, gs, sizeof gs);
        vk_label(g_killModeVk, km, sizeof km);
        vk_label(g_helpVk, hp, sizeof hp);
        vk_label(g_enemySuppressVk, es, sizeof es);
        vk_label(g_battleRestartAutoVk, ra, sizeof ra);
        ff132_loga(FF132_TAG "config: help=%s game_speed=%s multiplier=%s then off, kill_mode=%s"
                      " enemy_suppress=%s battle_restart_auto=%s\n",
               hp, gs, gs_steps_str(), km, es, ra);
    }
    ff132_loga(FF132_TAG "config: slot_select key=0x%X battle_overlay key=0x%X"
                  " autosave_slot=%d (-1 = last) slot_count=%d (%d = the game's own)"
                  " global_hotkeys=%d log=%d,"
                  " working slot %s, historia-crux skip %s, story recap %s, keep note %s\n",
           g_slotSelectVk, g_battleOverlayVk, g_autosaveSlot, g_slotCount, FF132_SLOTS_GAME,
           g_hkGlobal, g_logLevel,
           g_autosaveRedirect ? "on" : "off", g_directLoad ? "on" : "off",
           g_skipRecap ? "skipped" : "played", g_keepNote ? "on" : "off");
    if (g_ovlScale == FF132_SCALE_AUTO)
        ff132_loga(FF132_TAG "config: font_scale=auto (follows the screen height, 100%% at 720)\n");
    else
        ff132_loga(FF132_TAG "config: font_scale=%d%%\n", g_ovlScale);
}

// ---- writing one setting back ----------------------------------------------------------------
static int cfg_write_fail(const char* what) {
    ff132_loga(FF132_TAG "config: cannot write %s\n", what);
    ff132_flash_c(ui_text(T_INI_FAIL), FF132_COL_MENU_OFF);
    return 0;
}

int config_write(const char* section, const char* key, const char* value) {
    char path[MAX_PATH], tmp[MAX_PATH], err[MAX_PATH + 64];
    if (!exe_sibling(path, sizeof path, FF132_INI_NAME) || !exe_sibling(tmp, sizeof tmp, FF132_INI_TMP_NAME))
        return cfg_write_fail(FF132_INI_NAME " (the path is too long)");
    ff13_ini_notes nt;
    int ok = ff13_ini_write(path, tmp, section, key, value,
        "; Written by FF13-2-SpeedrunPractice's settings screen, where every key can be changed;"
        " the README explains them.\n"
        "; ホットキーと設定の画面から書かれたファイル。全キーをその画面で変えられ、説明はREADMEにある。\n",
        &nt, err, sizeof err);
    if (nt.commentDropped)
        ff132_logv(FF132_TAG "config: %s.%s's inline comment dropped with its value\n", section, key);
    return ok ? 1 : cfg_write_fail(err);
}

// ---- the state file ---------------------------------------------------------------------------
// Written where the change happens, so a marker is on the disk before the game can go down. A write
// that fails is retried from the worker's tick until one lands.
static volatile LONG g_stateRetry = 0;
static int g_stateFailLogged = 0;
static SRWLOCK g_stateLock = SRWLOCK_INIT;  // the snapshot and its write, so the newest lands last
void ff132_state_tick(void) { if (InterlockedExchange(&g_stateRetry, 0)) ff132_state_save(); }
void ff132_state_save(void) {
    char path[MAX_PATH], buf[96 + FF132_WI_MAX * 40];
    if (!data_file(path, sizeof path, FF132_STATE_NAME)) return;
    AcquireSRWLockExclusive(&g_stateLock);
    int n = snprintf(buf, sizeof buf, "sort %d\nworld %d\nspeed %d\nsuppress %d\nkill %d\noverlay %d\nrestart %d\n",
                     ff132_tidy_state_sort(), ff132_tidy_state_filter_code(), ff132_speed_step_get(),
                     ff132_encount_on(), ff132_kill_mode(), g_battleOverlay ? 1 : 0, ff132_autoretry_on());
    // Last on purpose: a build that reads "<key> <int>" with one fscanf stops at its third field.
    int ws[FF132_WI_MAX];
    uint64_t wi[FF132_WI_MAX];
    for (int i = 0, k = save_work_ident_snapshot(ws, wi, FF132_WI_MAX); i < k && n < (int)sizeof buf; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "work_ident %d %08X%08X\n", ws[i],
                      (unsigned)(wi[i] >> 32), (unsigned)wi[i]);
    if (n >= (int)sizeof buf) SetLastError(ERROR_INSUFFICIENT_BUFFER);
    if (n < (int)sizeof buf && ff132_write_whole(path, buf, (size_t)n)) {
        g_stateFailLogged = 0;
    } else {
        DWORD e = GetLastError();
        InterlockedExchange(&g_stateRetry, 1);
        if (!g_stateFailLogged) {
            g_stateFailLogged = 1;
            ff132_loga(FF132_TAG "state: %s could not be written (err %lu) -- the worker retries until it is\n",
                   FF132_STATE_NAME, (unsigned long)e);
        }
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}
void ff132_state_load(void) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof path, FF132_STATE_NAME)) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[128], key[32], hex[24];
    int val;
    // Every line is "<key> <int> [...]", read a line at a time: a key this build does not know is
    // claimed by no branch, so a file carrying a retired setting still parses.
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31s %d", key, &val) != 2) continue;
        if (cfg_ieq(key, "work_ident")) {
            if (sscanf(line, "%*s %*d %16s", hex) == 1 && strlen(hex) == 16) {
                char hi[9];
                memcpy(hi, hex, 8);
                hi[8] = 0;
                uint64_t id = ((uint64_t)strtoul(hi, NULL, 16) << 32) | strtoul(hex + 8, NULL, 16);
                save_work_ident_add(val, id);
            }
            continue;
        }
        if      (cfg_ieq(key, "sort"))   ff132_tidy_state_set_sort(val);
        else if (cfg_ieq(key, "world"))  ff132_tidy_state_set_filter_code(val);
        else if (cfg_ieq(key, "speed"))  ff132_speed_step_set(val);
        else if (cfg_ieq(key, "suppress")) ff132_encount_set(val);
        else if (cfg_ieq(key, "kill"))   ff132_kill_set(val);
        else if (cfg_ieq(key, "overlay")) g_battleOverlay = val ? 1 : 0;
        else if (cfg_ieq(key, "restart")) ff132_autoretry_set(val);
    }
    fclose(f);
}

int wait_for_bytes(uintptr_t addr, const uint8_t* expect, int len, int timeoutMs) {
    for (int t = 0; t < timeoutMs; t += 100) {
        if (!IsBadReadPtr((const void*)addr, (UINT_PTR)len) &&
            memcmp((const void*)addr, expect, (size_t)len) == 0) return t;
        Sleep(100);
    }
    return -1;
}
