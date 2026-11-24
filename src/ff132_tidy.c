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

// Save tidy. Two menus over the game's own save list:
//   the title screen's -- which saves the list shows and in what order, the categories, and the
//                         renumbering into story order
//   the list's own      -- what to do with the save the game's cursor is on (or a marked range):
//                          its note, a copy or move to another category, a delete
// A note lives in the save file's plaintext header and is shown on the heading line of the list's
// detail panel; the only bytes written into a save from here are that header's text -- everything
// else renames, copies or recycles whole files.
//
// Threads: both menus, the editor's file work and the header scans run on the hotkey worker; the
// sort hook, the cursor hook and the heading hook run on the game thread; the window procedure runs
// on the game's message thread and only edits the buffer; the drawing runs on the render thread.
// Shared state is volatile with one writer each.
#include "ff132_internal.h"
#include "ff132_addrs.h"
#include "ff132_progress.h"
#include "ff132_worlds.h"
#include "ff13_hook.h"


// ---- rows and settings ------------------------------------------------------------------------
// Menu order, and g_sel indexes it. A row's group heading is kGroup below, where -1 means "this
// row continues the group above it" -- which is how a row is added to a group that already exists.
enum { TIDY_FILTER = 0, TIDY_CAT_PICK, TIDY_CAT_NEW, TIDY_CAT_NEW_ALL, TIDY_CAT_RENAME,
       TIDY_CAT_DEL, TIDY_SORT, TIDY_RENUM, TIDY_ROWS };
enum { SORT_TIME = 0, SORT_LATE, SORT_EARLY, SORT_NUMBER, SORT_MODES };

static volatile int g_open   = 0;     // worker writes, render thread may clear on a lapsed gate
static volatile int g_sel    = 0;
static volatile int g_sort   = SORT_TIME;   // worker writes, the comparator reads
// The filter is a world bucket: the first kWorlds row that ff132_world_display names alike, so two
// ids the list labels alike are one choice. WORLD_UNKNOWN collects saves with no world the table names.
#define WORLD_N       ((int)(sizeof kWorlds / sizeof kWorlds[0]))
#define WORLD_UNKNOWN WORLD_N
#define FILTER_ALL (-1)
static volatile int g_filter = FILTER_ALL;

// ---- the note a save carries -------------------------------------------------------------------
// It lives in the save file's own header (ff132_load.c writes it), so it travels with the file; this
// is the copy the heading hook reads, because that hook runs while the list is up and the list
// re-reads the folder only when it opens. Filled by every header read below, by the editor, and by
// the two calls ff132_load.c makes when a save is rewritten or copied. cp932, the encoding the file
// and the list both use. `have` is cleared before the text is replaced and set after it, so a
// reader on the other thread sees a whole note or none.
#define TIDY_SLOTS    100  // every two-digit number, whatever the game's count is raised to
#define FF132_NOTE_CHARS 24   // the same cap as FF13-SpeedrunPractice's notes
#define FF132_NOTE_BYTES 64   // cp932 spends two bytes on most non-ASCII characters
static struct { volatile unsigned char have; char t[FF132_NOTE_BYTES]; } g_note[TIDY_SLOTS];

static void note_put(int slot, const char* cp932) {
    if (slot < 0 || slot >= TIDY_SLOTS) return;
    g_note[slot].have = 0;
    if (!cp932 || !cp932[0]) { g_note[slot].t[0] = 0; return; }
    snprintf(g_note[slot].t, sizeof g_note[slot].t, "%s", cp932);
    g_note[slot].have = 1;
}
static int note_get(int slot, char* out, size_t n) {
    if (!n) return 0;
    out[0] = 0;
    if (slot < 0 || slot >= TIDY_SLOTS || !g_note[slot].have) return 0;
    snprintf(out, n, "%s", g_note[slot].t);
    return out[0] != 0;
}
void ff132_tidy_note_dropped(int slot) { note_put(slot, NULL); }
int ff132_tidy_note_peek(int slot, char* out, size_t n) { return note_get(slot, out, n); }
void ff132_tidy_note_copied(int src, int dst) {
    char t[FF132_NOTE_BYTES];
    note_get(src, t, sizeof t);
    note_put(dst, t);
}

// ---- the per-slot keys ------------------------------------------------------------------------
// Everything the sort orders by that is not in the list entry itself, read out of each save's
// 0x210-byte plaintext header, plus the area counter out of its body (ff132_progress.c). Filled on the
// game thread when a list opens, and on the worker when the menu opens before any list has.
// `aplay` is what the story orders compare instead of `play`: see progress_adjust.
#define TIDY_EPS     16     // the header's step runs 0..12
typedef struct {
    unsigned char have, ep; uint16_t frag; uint32_t play;
    int hist; int32_t area; uint32_t aplay; int world; unsigned char noId;
} tidy_key;
static tidy_key g_key[TIDY_SLOTS];
static int g_worldCount[WORLD_N + 1];
static uint64_t g_worldFirst[WORLD_N + 1];  // the earliest (episode, play time) in each bucket
static int g_keyTotal = 0;
static const tidy_key kNoKey = { 0, 0, 0, 0, FF132_PROG_NONE, FF132_PROG_NONE, 0, WORLD_UNKNOWN, 0 };

static const tidy_key* key_of(int slot) {
    return (slot >= 0 && slot < TIDY_SLOTS) ? &g_key[slot] : &kNoKey;
}
int ff132_tidy_slot_no_id(int slot) {
    const tidy_key* k = key_of(slot);
    return k->have && k->noId;
}
static int world_count(int w) { return (w >= 0 && w <= WORLD_N) ? g_worldCount[w] : 0; }

static int world_of(const char* hid) {
    if (!hid[0]) hid = FF132_NO_ID_WORLD;
    for (int i = 0; i < WORLD_N; i++) {
        if (strcmp(kWorlds[i].id, hid)) continue;
        char me[96], other[96];
        ff132_world_display(i, 0, me, sizeof me);
        for (int j = 0; j <= i; j++) {
            ff132_world_display(j, 0, other, sizeof other);
            if (!strcmp(me, other)) return j;
        }
    }
    return WORLD_UNKNOWN;
}
// UTF-8, the save list's own third line without the Historia Crux wrapper.
static void world_name(int w, char* out, size_t n) {
    if (w < 0 || w >= WORLD_N) { if (n) out[0] = 0; }
    else ff132_world_display(w, ui_english(), out, n);
}
static void world_count_add(const tidy_key* k, int d) {
    int w = (k->world >= 0 && k->world <= WORLD_N) ? k->world : WORLD_UNKNOWN;
    if (d < 0 && g_worldCount[w] <= 0) return;
    g_worldCount[w] += d;
    uint64_t at = ((uint64_t)k->ep << 32) | k->play;
    if (d > 0 && at < g_worldFirst[w]) g_worldFirst[w] = at;
}

// The state file holds a bucket as a hash of its row's id: a row index would point elsewhere once
// the table is regenerated. -1 = every save, -2 = the unknown bucket.
static int world_code(int w) {
    if (w < 0) return FILTER_ALL;
    if (w >= WORLD_N) return -2;
    uint32_t h = 2166136261u;
    for (const char* c = kWorlds[w].id; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    return (int)(h & 0x7FFFFFFFu);
}
static int world_from_code(int v) {
    if (v == -2) return WORLD_UNKNOWN;
    if (v < 0) return FILTER_ALL;
    for (int w = 0; w < WORLD_N; w++) if (world_code(w) == v) return w;
    return FILTER_ALL;
}

// cp932 lead bytes take a trail byte with them, and a trail byte can look like an ASCII digit --
// so the scan below walks characters, not bytes.
// The two numbers the list itself prints, taken the way the game's own scanners (0x8954B0 /
// 0x895510) take them: the first ddd:dd:dd is the play time and the first ddd/ddd the fragments.
static void header_text_keys(const char* t, size_t n, uint32_t* play, uint16_t* frag) {
    *play = 0; *frag = 0;
    for (size_t i = 0; i < n && t[i]; ) {
        unsigned char c = (unsigned char)t[i];
        if (cp932_lead(c) && i + 1 < n && t[i + 1]) { i += 2; continue; }
        if (c < '0' || c > '9') { i++; continue; }
        unsigned v[3] = { 0, 0, 0 };
        size_t j = i;
        int part = 0, digits = 0, sep = 0;
        while (j < n && t[j] && part < 3) {
            char d = t[j];
            if (d >= '0' && d <= '9') { v[part] = v[part] * 10 + (unsigned)(d - '0'); digits++; j++; }
            else if ((d == ':' || d == '/') && digits && (!sep || sep == d)) {
                sep = d; part++; digits = 0; j++;
            } else break;
        }
        if (sep == ':' && part == 2 && digits && !*play) *play = v[0] * 3600u + v[1] * 60u + v[2];
        else if (sep == '/' && part == 1 && digits && !*frag)
            *frag = (uint16_t)(v[0] > 0xFFFFu ? 0xFFFFu : v[0]);
        if (*play && *frag) return;
        i = (j > i) ? j : i + 1;
    }
}

// ---- the area counter, per file ----------------------------------------------------------------
// Up to 33 small reads per save, so each slot's answer is kept against the file's last-write time
// and size and a list reopened over unchanged saves reads nothing. Both threads call this; the lock
// guards only the table, never the file reads.
typedef struct {
    unsigned char have, noId; uint64_t ft, size; int hist; int32_t area; int world;
} prog_memo;
static prog_memo g_progMemo[TIDY_SLOTS];
static SRWLOCK g_progLock = SRWLOCK_INIT;
static int g_progReads = 0;                 // files actually read since keys_begin, for the log

static void slot_progress(int slot, const char* path, int* hist, int32_t* area, int* world,
                          unsigned char* noId) {
    *hist = FF132_PROG_NONE;
    *area = FF132_PROG_NONE;
    *world = WORLD_UNKNOWN;
    *noId = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (slot < 0 || slot >= TIDY_SLOTS || !GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
        return;
    uint64_t ft = ft_u64(fa.ftLastWriteTime);
    uint64_t sz = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    AcquireSRWLockExclusive(&g_progLock);
    prog_memo m = g_progMemo[slot];
    ReleaseSRWLockExclusive(&g_progLock);
    if (m.have && m.ft == ft && m.size == sz) {
        *hist = m.hist; *area = m.area; *world = m.world; *noId = m.noId;
        return;
    }
    g_progReads++;
    char hid[FF132_PROG_HID + 1];
    if (ff132_save_progress_ex(path, hid, NULL, hist, area) < 0) return;  // unreadable now: not remembered
    *world = world_of(hid);
    *noId = !hid[0];
    m.have = 1; m.ft = ft; m.size = sz; m.hist = *hist; m.area = *area; m.world = *world;
    m.noId = *noId;
    AcquireSRWLockExclusive(&g_progLock);
    g_progMemo[slot] = m;
    ReleaseSRWLockExclusive(&g_progLock);
}

// 1 = the header read and looked like one. The step byte is what the game draws on the thumbnail:
// 0 = the prologue, 6 = AF500, 7 and up after the ending.
static int slot_keys(int slot, tidy_key* out, char* pathOut, size_t pathN) {
    char path[MAX_PATH], note[FF132_NOTE_BYTES] = "";
    int ok = 0;
    if (save_slot_path(slot, path, sizeof path)) {
        if (pathOut && pathN) snprintf(pathOut, pathN, "%s", path);
        FILE* f = fopen(path, "rb");
        if (f) {
            unsigned char hdr[FF132_HDR_BYTES];
            size_t got = fread(hdr, 1, sizeof hdr, f);
            fclose(f);
            if (got == sizeof hdr && hdr[0] == FF132_HDR_MAGIC) {
                int ep = (hdr[FF132_HDR_EPISODE] >> 4) - 2;
                out->ep   = (unsigned char)(ep > 0 ? (ep < TIDY_EPS ? ep : TIDY_EPS - 1) : 0);
                out->have = 1;
                const char* text = (const char*)hdr + FF132_HDR_TEXT_AT;
                header_text_keys(text, FF132_HDR_TEXT, &out->play, &out->frag);
                const char* lbl; const char* nt;
                size_t lblN, ntN;
                ff132_hdr_mod_lines(text, FF132_HDR_TEXT, &lbl, &lblN, &nt, &ntN);
                // A longer line is left out rather than cut: a cut would land mid-character.
                if (nt && ntN && ntN < sizeof note) { memcpy(note, nt, ntN); note[ntN] = 0; }
                ok = 1;
            }
        }
        if (ok) {
            slot_progress(slot, path, &out->hist, &out->area, &out->world, &out->noId);
            out->aplay = out->play;
        }
    }
    note_put(slot, note);
    return ok;
}

// The story orders' play time for every save in `k` (ff132_progress_adjust), in place.
static void progress_adjust(tidy_key* const* k, const int* slot, int n) {
    ff132_prog_in in[TIDY_SLOTS];
    uint32_t ap[TIDY_SLOTS];
    if (n > TIDY_SLOTS) n = TIDY_SLOTS;
    for (int i = 0; i < n; i++) {
        in[i].slot = slot[i];
        in[i].ep   = k[i]->ep;
        in[i].frag = k[i]->frag;
        in[i].hist = k[i]->hist;
        in[i].area = k[i]->area;
        in[i].play = k[i]->play;
    }
    ff132_progress_adjust(in, n, ap);
    for (int i = 0; i < n; i++) k[i]->aplay = ap[i];
}

static void keys_begin(void) {
    memset(g_key, 0, sizeof g_key);
    memset(g_worldCount, 0, sizeof g_worldCount);
    memset(g_worldFirst, 0xFF, sizeof g_worldFirst);
    g_keyTotal = 0;
    g_progReads = 0;
}
static void keys_add(int slot) {
    if (slot < 0 || slot >= TIDY_SLOTS || g_key[slot].have) return;
    tidy_key k = kNoKey;
    if (!slot_keys(slot, &k, NULL, 0)) return;
    g_key[slot] = k;
    g_keyTotal++;
    world_count_add(&k, 1);
}
static void keys_end(void) {
    tidy_key* k[TIDY_SLOTS];
    int slot[TIDY_SLOTS], n = 0, withArea = 0;
    for (int s = 0; s < TIDY_SLOTS; s++) {
        if (!g_key[s].have) continue;
        k[n] = &g_key[s];
        slot[n] = s;
        if (g_key[s].area >= 0) withArea++;
        n++;
    }
    progress_adjust(k, slot, n);
    ff132_log(FF132_TAG "save tidy: story counter on %d of %d save(s) (%d file(s) read, the rest"
                 " remembered)\n", withArea, n, g_progReads);
    for (int i = 0; i < n; i++)
        ff132_logv(FF132_TAG "save tidy: slot %02d hist %d area %d play %u -> %u\n", slot[i],
               k[i]->hist, (int)k[i]->area, (unsigned)k[i]->play, (unsigned)k[i]->aplay);
}

// The file behind this slot has gone. The table is rebuilt whole on the next list open, so this
// only keeps the per-world counts the menu draws from going stale in the meantime.
static void keys_forget(int slot) {
    if (slot < 0 || slot >= TIDY_SLOTS || !g_key[slot].have) return;
    world_count_add(&g_key[slot], -1);
    g_key[slot] = kNoKey;
    if (g_keyTotal > 0) g_keyTotal--;
}

// The whole folder, for the menu opened before any list has been. The slot count is the game's own.
static void keys_scan_disk(void) {
    int n = save_slot_count();
    if (n < 1 || n > TIDY_SLOTS) n = TIDY_SLOTS;
    keys_begin();
    for (int s = 0; s < n; s++) keys_add(s);
    keys_end();
    ff132_logv(FF132_TAG "save tidy: %d save(s) scanned from disk\n", g_keyTotal);
}
// A category switch swaps the whole folder: a slot left empty would otherwise keep the note of the
// save it held in the category before, and keep_note would put that note on a new save there.
void ff132_tidy_rescan(void) { keys_scan_disk(); }

// ---- the settings file ------------------------------------------------------------------------
// The file itself is ff132_core.c's (it carries the game-speed step and the encounter suppression as
// well). While Steam Cloud has the sort and the filter forced off, the file keeps the player's own
// values so they come back once Cloud is off; g_cloudHold says the in-force pair is not the one to
// write.
static int g_cloudHold = 0, g_sortFile = SORT_TIME, g_filterFile = FILTER_ALL;
int ff132_tidy_state_sort(void)   { return g_cloudHold ? g_sortFile : g_sort; }
int ff132_tidy_state_filter(void) { return g_cloudHold ? g_filterFile : g_filter; }
void ff132_tidy_state_set_sort(int v)   { g_sort   = (v >= 0 && v < SORT_MODES) ? v : SORT_TIME; }
int ff132_tidy_state_filter_code(void) { return world_code(ff132_tidy_state_filter()); }
void ff132_tidy_state_set_filter(int v) { g_filter = (v >= 0 && v <= WORLD_N) ? v : FILTER_ALL; }
void ff132_tidy_state_set_filter_code(int v) { ff132_tidy_state_set_filter(world_from_code(v)); }
// A change made in the menu is the player's, so it ends the hold.
static void state_save(void) {
    g_cloudHold = 0;
    ff132_state_save();
}

// ---- Steam Cloud ------------------------------------------------------------------------------
// Read out of Steam's own config file, never steam_api.dll: the cloud switches are vtable slots on
// a version-specific interface, and a wrong slot called in a live game is a crash.
#define STEAM_APPID      "292140"     // FF13-2's Steam app id
#define VDF_MAX          (4u << 20)   // sharedconfig.vdf runs to tens of KB; anything huge is not it
#define VDF_DEPTH        16
#define VDF_TOK          64
#define CLOUD_RECHECK_MS 3000

static int reg_str(HKEY root, const char* sub, const char* val, char* out, DWORD n) {
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, sz = n;
    LONG r = RegQueryValueExA(k, val, NULL, &type, (BYTE*)out, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ || !sz) return 0;
    out[(sz < n) ? sz : n - 1] = 0;             // the value can come back without its terminator
    return out[0] != 0;
}
static DWORD reg_dword(HKEY root, const char* sub, const char* val) {
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, v = 0, sz = sizeof v;
    LONG r = RegQueryValueExA(k, val, NULL, &type, (BYTE*)&v, &sz);
    RegCloseKey(k);
    return (r == ERROR_SUCCESS && type == REG_DWORD) ? v : 0;
}

// Depth is counted past the stack's end so braces still pair when nesting exceeds VDF_DEPTH.
static void vdf_cloud(const char* buf, int* acct, int* app) {
    char stack[VDF_DEPTH][VDF_TOK], pend[VDF_TOK] = "";
    int depth = 0, havePend = 0;
    for (const char* p = buf; *p; ) {
        if (*p == '"') {
            char tok[VDF_TOK]; size_t n = 0;
            for (p++; *p && *p != '"'; p++) {
                if (*p == '\\' && p[1]) p++;
                if (n + 1 < sizeof tok) tok[n++] = (char)tolower((unsigned char)*p);
            }
            tok[n] = 0;
            if (*p == '"') p++;
            if (!havePend) { memcpy(pend, tok, n + 1); havePend = 1; continue; }
            if (!strcmp(pend, "cloudenabled") && depth > 0 && depth <= VDF_DEPTH) {
                int on = strcmp(tok, "0") ? 1 : 0;
                if (!strcmp(stack[depth - 1], "steam")) *acct = on;
                else if (depth > 1 && !strcmp(stack[depth - 1], STEAM_APPID)
                                   && !strcmp(stack[depth - 2], "apps")) *app = on;
            }
            havePend = 0;
        } else if (*p == '{') {
            if (depth < VDF_DEPTH) snprintf(stack[depth], VDF_TOK, "%s", havePend ? pend : "");
            depth++; havePend = 0; p++;
        } else if (*p == '}') {
            if (depth) depth--;
            havePend = 0; p++;
        } else if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
        } else p++;
    }
}
static int vdf_read_cloud(const char* path, int* acct, int* app) {
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    char* buf = (sz != INVALID_FILE_SIZE && sz < VDF_MAX) ? (char*)malloc((size_t)sz + 1) : NULL;
    int ok = buf && ReadFile(h, buf, sz, &got, NULL) && got == sz;
    CloseHandle(h);
    if (ok) { buf[sz] = 0; vdf_cloud(buf, acct, app); }
    free(buf);
    return ok;
}
// userdata\<id> is per Steam account: the wrong one answers about somebody else's switches.
static int steam_user_dir(const char* root, char* out, size_t n) {
    DWORD id = reg_dword(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser");
    if (id) { snprintf(out, n, "%s\\userdata\\%lu", root, (unsigned long)id); return 1; }
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\userdata\\*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0; char only[64] = "";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] < '1' || fd.cFileName[0] > '9') continue;   // "." ".." and "0"
        if (++found == 1) snprintf(only, sizeof only, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (found != 1) return 0;
    snprintf(out, n, "%s\\userdata\\%s", root, only);
    return 1;
}
// 1 = this game's saves are synced, 0 = they are not, -1 = Steam would not say.
static int steam_cloud_query(char* why, size_t n) {
    char root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    if (!reg_str(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", root, sizeof root)
     && !reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath", root, sizeof root)) {
        snprintf(why, n, "Steam is not in the registry -- its cloud switch is unknown");
        return -1;
    }
    for (char* s = root; *s; s++) if (*s == '/') *s = '\\';   // SteamPath is written with slashes
    if (!steam_user_dir(root, dir, sizeof dir)) {
        snprintf(why, n, "no single Steam account under %s\\userdata -- the cloud switch is "
                 "unknown", root);
        return -1;
    }
    snprintf(path, sizeof path, "%s\\7\\remote\\sharedconfig.vdf", dir);
    int acct = -1, app = -1;
    if (!vdf_read_cloud(path, &acct, &app)) {
        snprintf(why, n, "%s could not be read -- the cloud switch is unknown", path);
        return -1;
    }
    int on = !(acct == 0 || app == 0);           // unset counts as on; either 0 means not synced
    snprintf(why, n, "sync is %s for this game (account switch %s, game switch %s, from %s)",
             on ? "ON" : "off",
             acct < 0 ? "unset" : acct ? "on" : "off",
             app  < 0 ? "unset" : app  ? "on" : "off", path);
    return on;
}
int steam_cloud_on(void) {
    static int ans = -2, said = -2;
    static DWORD at = 0;
    DWORD now = GetTickCount();
    if (ans == -2 || (int)(now - at) >= CLOUD_RECHECK_MS) {
        char why[MAX_PATH + 128] = "";
        ans = steam_cloud_query(why, sizeof why);
        at = now;
        if (ans != said) { said = ans; ff132_loga(FF132_TAG "cloud: %s\n", why); }
    }
    return ans;
}

// ---- the comparator ---------------------------------------------------------------------------
// The game sorts the entry array with qsort and prints it so that the array's LAST element is the
// list's FIRST row, so a positive answer here means "a belongs nearer the front". Never 0 for two
// different saves: the slot number is the last tiebreak, and an order that is not total would make
// the same list come out differently from one open to the next.
static int g_workSlot = -1;          // cached per sort by the hook; the comparator must not log

static uint64_t entry_time(uintptr_t e) {
    return ((uint64_t)rd32(e + FF132_ENTRY_TIME + 4) << 32) | rd32(e + FF132_ENTRY_TIME);
}
static int __cdecl tidy_cmp(const void* A, const void* B) {
    uintptr_t a = (uintptr_t)A, b = (uintptr_t)B;
    int sa = *(const volatile signed char*)(a + FF132_ENTRY_SLOT);
    int sb = *(const volatile signed char*)(b + FF132_ENTRY_SLOT);
    if (sa != sb) {                                   // the working slot heads every mode
        if (sa == g_workSlot) return 1;
        if (sb == g_workSlot) return -1;
    }
    uint64_t ta = entry_time(a), tb = entry_time(b);
    const tidy_key* ka = key_of(sa);
    const tidy_key* kb = key_of(sb);
    switch (g_sort) {
    case SORT_LATE:
        if (ka->ep    != kb->ep)    return ka->ep    > kb->ep    ? 1 : -1;
        if (ka->frag  != kb->frag)  return ka->frag  > kb->frag  ? 1 : -1;
        if (ka->aplay != kb->aplay) return ka->aplay > kb->aplay ? 1 : -1;
        if (ta != tb)               return ta > tb ? 1 : -1;
        return sa > sb ? 1 : sa < sb ? -1 : 0;
    case SORT_EARLY:                                  // the exact mirror, every key included
        if (ka->ep    != kb->ep)    return ka->ep    < kb->ep    ? 1 : -1;
        if (ka->frag  != kb->frag)  return ka->frag  < kb->frag  ? 1 : -1;
        if (ka->aplay != kb->aplay) return ka->aplay < kb->aplay ? 1 : -1;
        if (ta != tb)               return ta < tb ? 1 : -1;
        return sa < sb ? 1 : sa > sb ? -1 : 0;
    case SORT_NUMBER:
        return sa < sb ? 1 : sa > sb ? -1 : 0;
    default:
        if (ta != tb) return ta > tb ? 1 : -1;
        return sa < sb ? 1 : sa > sb ? -1 : 0;
    }
}

// ---- the sort hook ----------------------------------------------------------------------------
static volatile int g_sortOk = 0;         // the comparator's push was patched
static int g_barePathSaid = 0;

// The entry's own path field carries only a FILE NAME when sgw.dll's VFS enumerated the folder, so
// every save is resolved through the game's path builder instead. Logged the first time the two
// disagree, because which branch ran is not otherwise visible.
static void keys_from_list(uintptr_t base, int count) {
    keys_begin();
    for (int r = 0; r < count; r++) {
        uintptr_t e = base + (uintptr_t)r * FF132_ENTRY_BYTES;
        int slot = *(const volatile signed char*)(e + FF132_ENTRY_SLOT);
        if (slot < 0 || slot >= TIDY_SLOTS || g_key[slot].have) continue;
        tidy_key k = kNoKey;
        char path[MAX_PATH];
        if (!slot_keys(slot, &k, path, sizeof path)) continue;
        if (!g_barePathSaid) {
            const char* ep = (const char*)(e + FF132_ENTRY_PATH);
            if (rd_ok(ep, 0x101) && !memchr(ep, '/', 0x101) && !memchr(ep, '\\', 0x101)) {
                g_barePathSaid = 1;
                ff132_loga(FF132_TAG "save list: entry path is a bare name (vfs) -- resolved via the"
                              " game's folder\n");
            }
        }
        g_key[slot] = k;
        g_keyTotal++;
        world_count_add(&k, 1);
    }
    keys_end();
}

// Whether the sgw.dll VFS is feeding the list, and how many of its timestamps are whole seconds:
// the VFS hands the game a time_t, so a list built through it cannot tell two saves made in the
// same second apart. Said once, and again if the VFS pointer ever changes.
static void sort_diag(uintptr_t base, int count) {
    static void* said = (void*)-1;
    uintptr_t vp = g_base + FF132_RVA_SGW_VFS;
    void* vfs = rd_ok((const void*)vp, 4) ? (void*)(uintptr_t)rd32(vp) : NULL;
    if (vfs == said) return;
    said = vfs;
    int whole = 0;
    for (int r = 0; r < count; r++) {
        uint64_t t = entry_time(base + (uintptr_t)r * FF132_ENTRY_BYTES);
        if (t && t % 10000000ull == 0) whole++;
    }
    ff132_log(FF132_TAG "save list: vfs=%p, %d/%d stamps are whole seconds\n", vfs, whole, count);
}

// Rows the filter drops are moved behind the ones it keeps and the count is shrunk: the game reads
// exactly `count` entries and rebuilds the array from the folder every time a list opens, so
// nothing is lost by it. The save list is never filtered -- a slot you cannot see is a slot you
// cannot overwrite.
static int filter_rows(uintptr_t mgr, uintptr_t base, int count) {
    int want = g_filter;
    if (want < 0) return count;
    int work = g_workSlot;
    int keep = 0, hit = 0;
    for (int r = 0; r < count; r++) {
        int slot = *(const volatile signed char*)(base + (uintptr_t)r * FF132_ENTRY_BYTES
                                                  + FF132_ENTRY_SLOT);
        const tidy_key* k = key_of(slot);
        if (slot == work || !k->have || k->world == want) keep++;
        // The rows kept whatever the filter says do not make it a match.
        if (slot != work && k->have && k->world == want) hit++;
    }
    if (keep == count) return count;
    if (!hit) {
        // Kept as the setting, not applied: a folder that is briefly unreadable must not silently
        // forget what was chosen.
        ff132_log(FF132_TAG "filter: world %d holds no save now -- the whole list is shown\n", want);
        return count;
    }
    int w = 0;
    for (int r = 0; r < count; r++) {
        uintptr_t row = base + (uintptr_t)r * FF132_ENTRY_BYTES;
        int slot = *(const volatile signed char*)(row + FF132_ENTRY_SLOT);
        const tidy_key* k = key_of(slot);
        if (!(slot == work || !k->have || k->world == want)) continue;
        if (w != r) memmove((void*)(base + (uintptr_t)w * FF132_ENTRY_BYTES), (const void*)row,
                            FF132_ENTRY_BYTES);
        w++;
    }
    *(volatile signed char*)(mgr + FF132_OFF_SM_COUNT) = (signed char)w;
    signed char cur = *(const volatile signed char*)(mgr + FF132_OFF_SM_CUR);
    if (cur >= w) *(volatile signed char*)(mgr + FF132_OFF_SM_CUR) = (signed char)(w ? w - 1 : 0);
    return w;
}

// Game thread, at the entry of the list's own sort -- so the count this writes is the one qsort
// and the renumbering behind it both use.
static void __cdecl on_list_sort(uint32_t sm) {
    uintptr_t mgr = (uintptr_t)sm;
    if (!g_sortOk || !rd_ok((const void*)mgr, FF132_OFF_SM_ISSAVE + 1)) return;
    uintptr_t base = (uintptr_t)rd32(mgr + FF132_OFF_SM_ENTRIES);
    int count = *(const volatile signed char*)(mgr + FF132_OFF_SM_COUNT);
    int isSave = *(const volatile unsigned char*)(mgr + FF132_OFF_SM_ISSAVE) != 0;
    if (!base || count <= 0) return;
    if (IsBadWritePtr((void*)base, (size_t)count * FF132_ENTRY_BYTES)) return;
    g_workSlot = save_work_slot();
    keys_from_list(base, count);
    sort_diag(base, count);
    int n = isSave ? count : filter_rows(mgr, base, count);
    ff132_log(FF132_TAG "save list: %d rows sorted (mode %d, filter %d, save list %d)\n",
          n, g_sort, g_filter, isSave);
}

// ---- the game's own list: which save its cursor is on ------------------------------------------
// The GUI object is built and thrown away with every list, so its address is taken from the entry
// of its update function on the game thread and stamped there. The worker only trusts it while the
// list is still open and the stamp is young: the object belongs to the game thread, and a pointer
// kept past its list would be a read into freed memory.
#define GUI_FRESH_MS 500
static volatile uintptr_t g_gui   = 0;
static volatile DWORD     g_guiAt = 0;
static volatile int       g_guiOk = 0;

static void __cdecl on_list_update(uint32_t ecx) {
    g_gui   = (uintptr_t)ecx;
    g_guiAt = GetTickCount();
}

static uintptr_t sdc(void) {
    uintptr_t gm = (uintptr_t)rd32(g_base + FF132_RVA_GAMEMAIN_PTR);
    if (!rd_ok((const void*)gm, FF132_OFF_SAVE_CREATOR + 4)) return 0;
    uintptr_t c = (uintptr_t)rd32(gm + FF132_OFF_SAVE_CREATOR);
    return rd_ok((const void*)c, FF132_OFF_SDC_LIST_OPEN + 4) ? c : 0;
}
// 1 = a save or load list is on screen right now.
static int list_is_up(void) {
    uintptr_t c = sdc();
    return c && rd32(c + FF132_OFF_SDC_LIST_OPEN) != 0;
}
int ff132_save_list_up(void) { return list_is_up(); }
int ff132_list_gui_recent(void) {
    return g_guiOk && g_gui && (int)(GetTickCount() - g_guiAt) <= GUI_FRESH_MS;
}
static uintptr_t gui_now(void) {
    if (!g_guiOk || !list_is_up()) return 0;
    if ((int)(GetTickCount() - g_guiAt) > GUI_FRESH_MS) return 0;
    uintptr_t g = g_gui;
    return rd_ok((const void*)g, FF132_OFF_GUI_NEWROW_B + 4) ? g : 0;
}

// The slot under the game's cursor, or -1 for the "new save" row and for a list that cannot be read
// this instant. Reads only, and from the worker.
static int list_slot_now(void) {
    uintptr_t g = gui_now();
    if (!g) return -1;
    int cur  = (int)rd32(g + FF132_OFF_GUI_CURSOR);
    int rows = (int)rd32(g + FF132_OFF_GUI_ROWS);
    if (cur < 0 || cur >= rows) return -1;
    if (rd32(g + FF132_OFF_GUI_ISSAVE) && cur + 1 == rows
        && !rd32(g + FF132_OFF_GUI_NEWROW_A) && !rd32(g + FF132_OFF_GUI_NEWROW_B)) return -1;
    uintptr_t c = sdc();
    uintptr_t mgr = c ? (uintptr_t)rd32(c + FF132_OFF_SDC_SAVEMGR) : 0;
    if (!rd_ok((const void*)mgr, FF132_OFF_SM_ISSAVE + 1)) return -1;
    uintptr_t base = (uintptr_t)rd32(mgr + FF132_OFF_SM_ENTRIES);
    int count = *(const volatile signed char*)(mgr + FF132_OFF_SM_COUNT);
    if (!base || cur >= count) return -1;
    uintptr_t e = base + (uintptr_t)cur * FF132_ENTRY_BYTES;
    if (!rd_ok((const void*)e, FF132_ENTRY_SLOT + 1)) return -1;
    int slot = *(const volatile signed char*)(e + FF132_ENTRY_SLOT);
    return (slot >= 0 && slot < TIDY_SLOTS) ? slot : -1;
}

// The heading is rebuilt only while this flag stands, so a note written under an open list would
// otherwise not show until the list was opened again.
static void gui_mark_dirty(void) {
    uintptr_t g = gui_now();
    if (!g || IsBadWritePtr((void*)(g + FF132_OFF_GUI_DIRTY), 4)) return;
    *(volatile uint32_t*)(g + FF132_OFF_GUI_DIRTY) = 1;
}

// Game thread, in that same update function: the heading the detail panel is about to be given,
// already cut at its second comma. The frame layout is in ff132_addrs.h; the "new save" row and the
// unreadable-entry row both branch past this site, so the two digits here always name a real save.
static void __cdecl on_list_title_built(uint32_t esp) {
    uintptr_t b = (uintptr_t)esp;
    if (!rd_ok((const void*)(b + FF132_TITLE_DIGITS), 2)) return;
    const unsigned char* d = (const unsigned char*)(b + FF132_TITLE_DIGITS);
    if (d[0] < '0' || d[0] > '9' || d[1] < '0' || d[1] > '9') return;
    int slot = (d[0] - '0') * 10 + (d[1] - '0') - 1;   // the list counts saves from 1
    char* buf = (char*)(b + FF132_TITLE_TEXT_AT);
    if (!rd_ok(buf, FF132_TITLE_TEXT)) return;
    size_t len = 0;
    while (len < FF132_TITLE_TEXT && buf[len]) len++;
    if (len == FF132_TITLE_TEXT) return;
    // The working slot is where the automatic saves land, so its heading says so up front.
    static const char kAuto[] = "(AUTO) ";
    const size_t pre = sizeof kAuto - 1;
    if (slot == g_workSlot && len + pre + 1 <= FF132_TITLE_TEXT) {
        memmove(buf + pre, buf, len + 1);
        memcpy(buf, kAuto, pre);
        len += pre;
    }
    char note[FF132_NOTE_BYTES];
    if (!note_get(slot, note, sizeof note)) return;
    size_t add = strlen(note);
    if (len + 3 + add + 1 > FF132_TITLE_TEXT) return;
    memcpy(buf + len, " - ", 3);
    memcpy(buf + len + 3, note, add + 1);
}

// ---- the note editor ---------------------------------------------------------------------------
// The window procedure runs on the game's message thread and only edits the buffer; the file work
// happens on the worker when it sees the commit flag (ff132_tidy_edit_tick).
// One editor for the note and every category name. The note has to stay mode 0 so a reader that
// only knows about notes still reads right.
enum { EDIT_NOTE = 0, EDIT_CAT_NEW, EDIT_CAT_NEW_ALL, EDIT_CAT_RENAME, EDIT_CAT_SEND };
// What is wrong with the text right now (0 = nothing). INPUT comes from typing and the next
// keystroke clears it; the other two come from a refused commit and stand until the text changes.
enum { EDIT_BAD_INPUT = 1, EDIT_BAD_EXISTS, EDIT_BAD_NAME };
#define EDIT_CHARS (FF132_NOTE_CHARS > FF132_CATNAME_MAX ? FF132_NOTE_CHARS : FF132_CATNAME_MAX)
#define EDIT_BUF   (EDIT_CHARS + 1)
static volatile int g_edit     = 0;
static volatile int g_editMode = EDIT_NOTE;
static volatile int g_editSlot = -1;
static volatile int g_editLen  = 0;
static volatile int g_editDone = 0;      // 1 = write it, -1 = drop it
static volatile int g_editBad  = 0;
static wchar_t      g_editBuf[EDIT_BUF];
static WNDPROC      g_prevProc = NULL;

int ff132_tidy_edit_is_open(void) { return g_edit; }
void ff132_tidy_edit_finish(int commit) { g_editDone = commit ? 1 : -1; }

// cp932 is what the file, the list and the game's font all speak: a character it has no code for
// would reach the save as "?" and read as corruption, so it is refused at the keystroke.
static int edit_ok_char(wchar_t c) {
    if (c < L' ' || c == 0x7F) return 0;
    wchar_t one[2] = { c, 0 };
    char out[8];
    BOOL bad = FALSE;
    int n = WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, one, 1, out, (int)sizeof out, NULL, &bad);
    if (n <= 0 || bad) return 0;
    // A category name becomes a directory name, so it takes the filesystem's rules on top.
    return g_editMode == EDIT_NOTE ? 1 : ff132_cat_name_char_ok(c);
}
static int edit_cap(void) {
    return (g_editMode == EDIT_NOTE ? FF132_NOTE_CHARS : FF132_CATNAME_MAX) + 1;
}
static void edit_insert(const wchar_t* w) {
    int cap = edit_cap();
    for (int i = 0; w[i]; i++) {
        if (g_editLen >= cap - 1) { g_editBad = EDIT_BAD_INPUT; break; }
        if (!edit_ok_char(w[i])) { g_editBad = EDIT_BAD_INPUT; continue; }
        g_editBuf[g_editLen++] = w[i];
    }
    g_editBuf[g_editLen] = 0;
}

static int clipboard_text(wchar_t* out, int cap) {
    int opened = 0;
    for (int i = 0; i < 6 && !(opened = OpenClipboard(NULL)); i++) Sleep(15);
    if (!opened) return 0;
    int got = 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* w = (const wchar_t*)GlobalLock(h);
        if (w) {
            int n = 0;
            while (w[n] && w[n] != L'\r' && w[n] != L'\n' && n < cap - 1) { out[n] = w[n]; n++; }
            out[n] = 0;
            got = 1;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return got;
}

static LRESULT CALLBACK ff132_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (g_edit) {
        // Keys are swallowed so typing does not walk the game's menus. Esc is the deliberate
        // exception -- the game's own procedure turns it into the quit dialog.
        if ((m == WM_KEYDOWN || m == WM_KEYUP) && wp != VK_ESCAPE) return 0;
        if (m == WM_CHAR) {
            wchar_t c = (wchar_t)wp;
            // No Enter case HERE on purpose: as a character it is only '\r' (filtered out below),
            // and the commit lives on the worker, whose gate lift then waits out the key's release
            // so it cannot land in the save list behind the editor.
            // Backspace clears the refusal too: fixing a name that was turned down starts with
            // one, and a reason that stays on screen reads as "still no".
            if (c == 0x08) { g_editBad = 0; if (g_editLen) g_editBuf[--g_editLen] = 0; }
            else if (c == 0x16) {                                     // Ctrl+V
                wchar_t w[256];
                g_editBad = 0;
                if (clipboard_text(w, 256)) edit_insert(w); else g_editBad = EDIT_BAD_INPUT;
            } else if (c >= L' ') {
                g_editBad = 0;
                wchar_t one[2] = { c, 0 };
                edit_insert(one);
            }
            return 0;
        }
    }
    return CallWindowProcW(g_prevProc, h, m, wp, lp);
}

struct wnd_pick { HWND h; long area; };
static BOOL CALLBACK find_game_wnd(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    if (!GetClientRect(h, &r)) return TRUE;
    long a = (long)(r.right - r.left) * (long)(r.bottom - r.top);
    struct wnd_pick* p = (struct wnd_pick*)lp;
    if (a > p->area) { p->area = a; p->h = h; }
    return TRUE;
}
// Installed once and never removed: unhooking would have to happen with no message in flight, and
// there is no moment this mod can be sure of that.
static int edit_hook_window(void) {
    if (g_prevProc) return 1;
    struct wnd_pick pick = { NULL, 0 };
    EnumWindows(find_game_wnd, (LPARAM)&pick);
    if (!pick.h) {
        ff132_loga(FF132_TAG "labels: the game window was not found\n");
        return 0;
    }
    g_prevProc = (WNDPROC)SetWindowLongPtrW(pick.h, GWLP_WNDPROC, (LONG_PTR)ff132_wndproc);
    if (!g_prevProc) {
        ff132_loga(FF132_TAG "labels: cannot take the window (error %lu)\n", (unsigned long)GetLastError());
        return 0;
    }
    ff132_log(FF132_TAG "labels: editor ready on the game window\n");
    return 1;
}

static int slot_has_file(int slot) {
    char path[MAX_PATH];
    return slot >= 0 && slot < TIDY_SLOTS && save_slot_path(slot, path, sizeof path)
           && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int edit_open(int slot) {
    if (g_edit) return 1;
    if (!edit_hook_window()) {
        ff132_flash(ui_text(T_TD_NOTE_NO_WINDOW));
        return 0;
    }
    // Straight off the file rather than out of the cache: what is edited has to be what the save
    // carries, even if it was written by a build whose list hooks never ran.
    tidy_key k = kNoKey;
    slot_keys(slot, &k, NULL, 0);
    char note[FF132_NOTE_BYTES];
    note_get(slot, note, sizeof note);
    g_editBuf[0] = 0;
    g_editLen = 0;
    if (note[0]) {
        int n = MultiByteToWideChar(932, 0, note, -1, g_editBuf, EDIT_BUF);
        g_editLen = (n > 1) ? n - 1 : 0;
        g_editBuf[g_editLen] = 0;
    }
    g_editSlot = slot;
    g_editMode = EDIT_NOTE;
    g_editDone = 0;
    g_editBad  = 0;
    g_edit     = 1;
    return 1;
}

// The same editor, naming a category. A rename opens on the name in use so it can be edited rather
// than retyped.
static int cat_edit_open(int mode) {
    if (g_edit) return 1;
    if (!edit_hook_window()) {
        ff132_flash(ui_text(T_TD_NOTE_NO_WINDOW));
        return 0;
    }
    g_editBuf[0] = 0;
    g_editLen = 0;
    if (mode == EDIT_CAT_RENAME) {
        char cur[FF132_CATNAME_U8];
        ff132_cats_current(cur, sizeof cur);
        int n = MultiByteToWideChar(CP_UTF8, 0, cur, -1, g_editBuf, EDIT_BUF);
        g_editLen = (n > 1) ? n - 1 : 0;
        if (g_editLen > FF132_CATNAME_MAX) g_editLen = FF132_CATNAME_MAX;
        g_editBuf[g_editLen] = 0;
    }
    g_editSlot = -1;
    g_editMode = mode;
    g_editDone = 0;
    g_editBad  = 0;
    g_edit     = 1;
    return 1;
}

// Defined with the list menu below. Naming a category is one of the three doors into the send, so
// the send itself -- and the words it says -- stay in one place.
static void send_new_cat_commit(const char* name932, const wchar_t* w);

static void cat_fail_flash(void) {
    char msg[256];
    int hi = ff132_cat_switch_high();
    if (hi < 0) { ff132_flash(ui_text(T_TD_CAT_FAIL)); return; }
    snprintf(msg, sizeof msg, ui_text(T_TD_CAT_HIGH), hi + 1, ff132_slots_reach());
    ff132_flash(msg);
}

// Worker thread, never the message thread: the file work for a finished category name. The editor
// STAYS OPEN on a refusal -- closing it would throw the typed name away and put the reason far
// from where it was typed.
static void cat_edit_commit(int mode) {
    int b = g_editLen;
    if (b < 0 || b > EDIT_BUF - 1) b = 0;
    int a = 0;
    while (a < b && g_editBuf[a] == L' ') a++;
    while (b > a && g_editBuf[b - 1] == L' ') b--;
    wchar_t w[EDIT_BUF];
    int m = 0;
    for (int i = a; i < b && m < EDIT_BUF - 1; i++) w[m++] = g_editBuf[i];
    w[m] = 0;
    char name[FF132_CATNAME_BYTES], msg[256];
    int bad = ff132_cat_check_name(w, mode == EDIT_CAT_RENAME, name, sizeof name);
    if (bad) {
        g_editBad = (bad == 2) ? EDIT_BAD_EXISTS : EDIT_BAD_NAME;
        return;
    }
    g_edit = 0;
    g_editMode = EDIT_NOTE;
    if (mode == EDIT_CAT_RENAME) {
        if (!ff132_cat_rename(name)) { ff132_flash(ui_text(T_TD_CAT_FAIL)); return; }
        char cur[FF132_CATNAME_U8];
        ff132_cats_current(cur, sizeof cur);
        snprintf(msg, sizeof msg, ui_text(T_TD_CAT_RENAMED), cur);
        ff132_flash(msg);
        return;
    }
    if (mode == EDIT_CAT_SEND) { send_new_cat_commit(name, w); return; }
    ff132_flash(ui_text(T_TD_CAT_COPYING));
    if (!ff132_cat_new(name, mode == EDIT_CAT_NEW_ALL)) {
        cat_fail_flash();
        return;
    }
    char cur[FF132_CATNAME_U8];
    ff132_cats_current(cur, sizeof cur);
    snprintf(msg, sizeof msg, ui_text(T_TD_CAT_MADE), cur);
    ff132_flash(msg);
}

// Worker thread, never the message thread: the file work for a finished edit.
void ff132_tidy_edit_tick(void) {
    if (!g_edit || !g_editDone) return;
    int slot = g_editSlot, commit = g_editDone, mode = g_editMode;
    g_editDone = 0;
    if (commit < 0) {
        g_edit = 0;
        g_editMode = EDIT_NOTE;
        return;
    }
    if (mode != EDIT_NOTE) { cat_edit_commit(mode); return; }
    g_edit = 0;
    int b = g_editLen;
    if (b < 0 || b > EDIT_BUF - 1) b = 0;
    int a = 0;
    while (a < b && g_editBuf[a] == L' ') a++;
    while (b > a && g_editBuf[b - 1] == L' ') b--;
    g_editBuf[b] = 0;
    char note[FF132_NOTE_BYTES] = "";
    BOOL bad = FALSE;
    if (b > a && (WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, g_editBuf + a, -1, note,
                                      (int)sizeof note, NULL, &bad) <= 0 || bad)) {
        ff132_loga(FF132_TAG "labels: save %d -> the text does not convert to cp932, nothing written\n",
               slot + 1);
        ff132_flash(ui_text(T_TD_NOTE_FAIL));
        return;
    }
    char why[160];
    if (!ff132_hdr_write_lines(slot, NULL, note, why, sizeof why)) {
        ff132_loga(FF132_TAG "labels: save %d -> %s, nothing written\n", slot + 1, why);
        ff132_flash(ui_text(T_TD_NOTE_FAIL));
        return;
    }
    note_put(slot, note);
    gui_mark_dirty();
    ff132_log(FF132_TAG "labels: save %d set from the editor\n", slot + 1);
    ff132_flash(ui_text(note[0] ? T_TD_NOTE_SET : T_TD_NOTE_CLEARED));
}

// ---- the list's own menu -----------------------------------------------------------------------
// It acts on the save the GAME's cursor is on, and follows that cursor: the keys that walk the list
// are handed to the game one press at a time through the gate, so the game moves its own cursor
// under the menu. With a range marked the send and delete rows act on every save between the mark
// and that cursor instead.
enum { LSEL_NOTE = 0, LSEL_COPY, LSEL_MOVE, LSEL_MARK, LSEL_DEL, LSEL_ROWS };
// One press of the game's own left/right, long enough for the list to see it at any frame rate.
#define FF132_LIST_STEP_MS 60

static volatile int g_lselOpen = 0;
static volatile int g_lselSel  = 0;
static volatile int g_lselSlot = -1;

int ff132_tidy_list_is_open(void) { return g_lselOpen; }
void ff132_tidy_list_close(void) { g_lselOpen = 0; }

// ---- where a send goes ------------------------------------------------------------------------
// One pick shared by the copy row and the move row, as an index into the parked list with one stop
// past its end for a category that does not exist yet. Taken fresh when the menu opens and kept
// while it is up.
static volatile int g_sendDest = 0;
static int g_sendPendMove = 0;          // which row the name being typed belongs to

static int send_dest_is_new(void) { return g_sendDest < 0 || g_sendDest >= ff132_cats_count(); }
static void send_cycle(int dir) {
    int n = ff132_cats_count() + 1;
    int at = (g_sendDest < 0 || g_sendDest >= n) ? n - 1 : g_sendDest;
    g_sendDest = (at + (dir > 0 ? 1 : n - 1)) % n;
}
// What the rows and the messages call the destination: the parked name with its save count, which
// is also what says whether there is room in it.
static void send_dest_label(char* out, size_t n) {
    if (send_dest_is_new() || !ff132_cats_name(g_sendDest, out, n))
        snprintf(out, n, "%s", ui_text(T_TD_SEND_NEWCAT));
}
// The same destination as the file work needs it. 0 = <new category>, so there is nothing to send
// into yet.
static int send_dest(char* dir, size_t dirN, char* name932, size_t nameN) {
    return !send_dest_is_new() && ff132_cats_dest(g_sendDest, dir, dirN, name932, nameN);
}

// ---- the range mark ---------------------------------------------------------------------------
static volatile int g_rangeAnchor = -1;
static volatile int g_rangeShownN = 0;
static void range_del_disarm(void);
// The delete confirm belongs to a mark, so it goes when the mark does -- here rather than at each
// caller, none of which can then forget it.
static void range_clear(void) { g_rangeAnchor = -1; g_rangeShownN = 0; range_del_disarm(); }

// The saves the game would draw between the two ends, in the order it draws them. After the sort
// hook the entry array IS that order and the filter has already shrunk the count, so the members
// are the inclusive index span between the anchor's entry and the cursor's -- which is why no file
// date is read here. Worker thread, so every read is guarded the way list_slot_now's are.
//   -2 = the anchor is not in the list any more (the mark can never resolve again)
//   -1 = the list could not be read this instant (whatever was counted before still stands)
//    0 = the cursor end is not a save row of this list
static int range_members(int* out, int cap) {
    if (!gui_now()) return -1;
    uintptr_t c = sdc();
    uintptr_t mgr = c ? (uintptr_t)rd32(c + FF132_OFF_SDC_SAVEMGR) : 0;
    if (!rd_ok((const void*)mgr, FF132_OFF_SM_ISSAVE + 1)) return -1;
    uintptr_t base = (uintptr_t)rd32(mgr + FF132_OFF_SM_ENTRIES);
    int count = *(const volatile signed char*)(mgr + FF132_OFF_SM_COUNT);
    int anc = g_rangeAnchor, cur = g_lselSlot;
    if (!base || count <= 0 || anc < 0) return -1;
    if (!rd_ok((const void*)base, (size_t)count * FF132_ENTRY_BYTES)) return -1;
    int ia = -1, ib = -1;
    for (int r = 0; r < count; r++) {
        int slot = *(const volatile signed char*)(base + (uintptr_t)r * FF132_ENTRY_BYTES
                                                  + FF132_ENTRY_SLOT);
        if (slot == anc && ia < 0) ia = r;
        if (slot == cur && ib < 0) ib = r;
    }
    if (ia < 0) return -2;
    if (cur < 0 || ib < 0) return 0;
    int lo = ia < ib ? ia : ib, hi = ia < ib ? ib : ia, n = 0;
    for (int r = lo; r <= hi && n < cap; r++) {
        int slot = *(const volatile signed char*)(base + (uintptr_t)r * FF132_ENTRY_BYTES
                                                  + FF132_ENTRY_SLOT);
        if (slot >= 0 && slot < TIDY_SLOTS) out[n++] = slot;
    }
    return n;
}

// Every tick the menu is up, so the rows follow a list the game rebuilt under them.
static void range_recount(void) {
    if (g_rangeAnchor < 0) return;
    int m[TIDY_SLOTS];
    int n = range_members(m, TIDY_SLOTS);
    if (n == -2) {
        range_clear();
        ff132_log(FF132_TAG "save list: the marked save is no longer in the list -- range cleared\n");
        ff132_flash(ui_text(T_TD_RANGE_GONE));
        return;
    }
    if (n >= 0) g_rangeShownN = n;
}
// 1 = the delete row can run: it is the one row acting on more than the save under the cursor, and
// what it would delete is what the send rows offer to send.
static int range_del_ready(void) { return g_rangeAnchor >= 0 && g_rangeShownN > 0; }

// 1 = this save has to stay where it is. The game plays out of its current slot and the mod writes
// every automatic save into the working one, and the game's own list refuses to delete its current
// slot for the same reason. Only taking the file AWAY is barred -- a copy is fine.
static int slot_pinned(int slot) {
    int cur = save_cur_slot(), work = save_work_slot();
    return slot >= 0 && ((cur >= 0 && slot == cur) || (work >= 0 && slot == work));
}

// ---- sending a save to another category --------------------------------------------------------
static int send_refused(void) {
    if (!ff132_recovery_owns_saves()) return 0;
    ff132_loga(FF132_TAG "send: refused -- an unfinished switch owns these files\n");
    ff132_flash(ui_text(T_TD_SEND_FAIL));
    return 1;
}

// One save into `dir`. quiet = a range member: a range says one thing at the end, not one per save.
static int send_apply_from(int src, const char* dir, const char* name932, const char* u8,
                           int move, int quiet) {
    char sp[MAX_PATH], msg[384];
    if (src < 0 || src >= TIDY_SLOTS || !dir || !dir[0] || !save_slot_path(src, sp, sizeof sp)
        || !slot_has_file(src)) {
        ff132_loga(FF132_TAG "send: save %d cannot be sent to '%s'\n", src + 1, name932);
        if (!quiet) ff132_flash(ui_text(T_TD_SEND_FAIL));
        return 0;
    }
    int dst = ff132_cat_free_slot(dir);
    if (dst < 0) {
        ff132_loga(FF132_TAG "send: '%s' has no free save number\n", name932);
        if (!quiet) {
            snprintf(msg, sizeof msg, ui_text(T_TD_SEND_FULL), u8);
            ff132_flash(msg);
        }
        return 0;
    }
    int recOk = 1;
    if (!ff132_send_one(src, dir, dst, &recOk)) {
        if (!quiet) ff132_flash(ui_text(T_TD_SEND_FAIL));
        return 0;
    }
    // Copy then delete, never a move: a send cut short leaves the save where it was rather than
    // nowhere. Past this point the copy stands -- what the two refusals below cancel is the DELETE.
    if (move && !recOk) {
        ff132_loga(FF132_TAG "send: a sidecar would not take save %d's row -- copied, not moved\n",
               src + 1);
        if (!quiet) ff132_flash(ui_text(T_TD_SEND_NOMOVE));
        return 0;
    }
    if (move) {
        if (!DeleteFileA(sp)) {
            ff132_loga(FF132_TAG "send: save %d's file would not go (err %lu) -- the copy stands, the"
                          " original too\n", src + 1, (unsigned long)GetLastError());
            if (!quiet) ff132_flash(ui_text(T_TD_SEND_FAIL));
            return 0;
        }
        ff132_load_rec_forget(src);
        ff132_tidy_note_dropped(src);
        keys_forget(src);
    }
    ff132_log(FF132_TAG "send: save %d -> '%s' save %d (%s)\n", src + 1, name932, dst + 1,
          move ? "move" : "copy");
    if (!quiet) {
        snprintf(msg, sizeof msg, ui_text(move ? T_TD_SEND_MOVED : T_TD_SEND_COPIED),
                 src + 1, u8, dst + 1);
        ff132_flash(msg);
    }
    return 1;
}

// Room is checked up front so a move never strands half the range in each place. A destination too
// small keeps the mark (pick another and go again); any other exit past that drops it -- the files
// have moved on and a re-run would double the copies.
static int send_range_apply(const char* dir, const char* name932, const char* u8, int move) {
    char msg[384];
    int m[TIDY_SLOTS];
    int n = range_members(m, TIDY_SLOTS);
    if (n == -2) {
        range_clear();
        ff132_flash(ui_text(T_TD_RANGE_GONE));
        return 0;
    }
    if (n <= 0) {                                   // transient: the mark stands
        ff132_flash(ui_text(T_TD_RANGE_ROW_GONE));
        return 0;
    }
    int pinned = 0;
    if (move) for (int i = 0; i < n; i++) if (slot_pinned(m[i])) pinned++;
    int want = n - pinned;
    if (want <= 0) {
        ff132_log(FF132_TAG "send: every picked save is the current or working slot -- nothing moved\n");
        ff132_flash(ui_text(T_TD_SEND_PINNED));
        return 0;
    }
    int freeN = ff132_cat_free_count(dir);
    if (freeN < want) {
        ff132_loga(FF132_TAG "send: not enough room in '%s' (%d free, %d needed)\n", name932, freeN, want);
        snprintf(msg, sizeof msg, ui_text(T_TD_SEND_N_FULL), u8, freeN, want);
        ff132_flash(msg);
        return 0;
    }
    range_clear();
    int done = 0, stopped = 0;
    for (int i = 0; i < n; i++) {
        if (move && slot_pinned(m[i])) {
            ff132_log(FF132_TAG "send: save %d is the current or working slot -- not moved\n", m[i] + 1);
            continue;
        }
        if (!send_apply_from(m[i], dir, name932, u8, move, 1)) { stopped = 1; break; }
        done++;
    }
    if (stopped) {
        ff132_loga(FF132_TAG "send: stopped at %d of %d save(s) -> '%s' (%s)\n", done, want, name932,
               move ? "move" : "copy");
        snprintf(msg, sizeof msg, ui_text(T_TD_SEND_N_PART), done, want);
        ff132_flash(msg);
        return 0;
    }
    ff132_log(FF132_TAG "send: %d save(s) -> '%s' (%s)\n", done, name932, move ? "move" : "copy");
    if (pinned) snprintf(msg, sizeof msg, ui_text(T_TD_SEND_N_MOVED_SKIP), done, u8, pinned);
    else        snprintf(msg, sizeof msg, ui_text(move ? T_TD_SEND_N_MOVED : T_TD_SEND_N_COPIED), done,
                         u8);
    ff132_flash(msg);
    return 1;
}

// The one funnel: the copy row, the move row and the editor's commit all land here, so single and
// range cannot be decided twice and one guard covers all three. The menu closes on success -- the
// save it was opened on may not be there any more.
static int send_go(const char* dir, const char* name932, const char* u8, int move) {
    if (send_refused()) return 0;
    // Decided on the predicate the rows were DRAWN with: a row showing the single-save form must
    // do that one thing.
    int ok;
    if (g_rangeAnchor >= 0 && g_rangeShownN > 0) {
        ok = send_range_apply(dir, name932, u8, move);
    } else if (move && slot_pinned(g_lselSlot)) {
        ff132_log(FF132_TAG "send: save %d is the current or working slot -- not moved\n", g_lselSlot + 1);
        ff132_flash(ui_text(T_TD_SEND_PINNED));
        return 0;
    } else {
        ok = send_apply_from(g_lselSlot, dir, name932, u8, move, 0);
    }
    if (ok) {
        g_lselOpen = 0;
        if (!g_edit) menu_gate_release();
    }
    return ok;
}

// The editor's commit, once the name has been checked. `w` is that name as it was typed, which is
// where the overlay's UTF-8 form comes from without a second conversion.
static void send_new_cat_commit(const char* name932, const wchar_t* w) {
    char dir[MAX_PATH], u8[FF132_CATNAME_U8];
    if (send_refused()) return;
    if (!ff132_cat_make(name932, dir, sizeof dir)) {
        ff132_flash(ui_text(T_TD_CAT_FAIL));
        return;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, (int)sizeof u8, NULL, NULL) <= 0)
        snprintf(u8, sizeof u8, "%s", name932);
    // A range send stopped part way may leave copies behind: RemoveDirectoryA refuses a folder
    // with anything in it, so those survive for the log to point at rather than going with it.
    if (send_go(dir, name932, u8, g_sendPendMove)) ff132_cats_refresh();
    else RemoveDirectoryA(dir);
}

// ---- deleting the marked range ------------------------------------------------------------------
// To the Recycle Bin behind two confirms, the way a category goes. There is no row for deleting
// ONE save: the game's own list does that.
static volatile int   g_delArm = 0;
static volatile int   g_delArmN = 0;
static volatile int   g_delArmAnchor = -1;   // the mark and the cursor it armed on: a change disarms
static volatile int   g_delArmSlot = -1;

static void range_del_disarm(void) {
    g_delArm = 0; g_delArmN = 0; g_delArmAnchor = -1; g_delArmSlot = -1;
}

// Worker, every tick. The ways an arm goes away, in ONE place: the row was left, the menu closed,
// the cursor moved to another save, or the mark was re-picked (range_clear disarms too, which
// covers the mark being dropped). Spread over the callers, the one that is
// forgotten is the one that deletes something nobody asked for.
static void range_del_poll(int rowActive) {
    if (!g_delArm) return;
    if (!rowActive || g_delArmAnchor != g_rangeAnchor || g_delArmSlot != g_lselSlot)
        range_del_disarm();
}

// One file per call rather than one list for the whole range, so the loop stops at the first
// refusal with a count that says how far it got.
static int slot_recycle(int slot) {
    char p[MAX_PATH];
    wchar_t w[MAX_PATH + 2];
    if (!save_slot_path(slot, p, sizeof p)) return 0;
    // The ANSI calls that wrote this file read the bytes in the ANSI code page.
    int n = MultiByteToWideChar(CP_ACP, 0, p, -1, w, MAX_PATH);
    if (n <= 0) return 0;
    w[n] = 0;                                   // the double NUL SHFileOperation wants
    return ff132_shell_recycle(w, p);
}

static int range_del_apply(void) {
    char msg[384];
    int m[TIDY_SLOTS];
    int n = range_members(m, TIDY_SLOTS);
    if (n == -2) {
        range_clear();
        ff132_flash(ui_text(T_TD_RANGE_GONE));
        return 0;
    }
    if (n <= 0) {
        ff132_flash(ui_text(T_TD_RANGE_ROW_GONE));
        return 0;
    }
    if (ff132_recovery_owns_saves()) {
        ff132_loga(FF132_TAG "delete: refused -- an unfinished switch owns these files\n");
        ff132_flash(ui_text(T_TD_DEL_FAIL));
        return 0;
    }
    int pinned = 0;
    for (int i = 0; i < n; i++) if (slot_pinned(m[i])) pinned++;
    int want = n - pinned;
    if (want <= 0) {
        ff132_log(FF132_TAG "delete: every picked save is the current or working slot -- nothing"
                     " deleted\n");
        ff132_flash(ui_text(T_TD_SEND_PINNED));
        return 0;
    }
    range_clear();
    int done = 0;
    for (int i = 0; i < n; i++) {
        if (slot_pinned(m[i])) {
            ff132_log(FF132_TAG "delete: save %d is the current or working slot -- kept\n", m[i] + 1);
            continue;
        }
        if (!slot_recycle(m[i])) break;
        ff132_load_rec_forget(m[i]);
        ff132_tidy_note_dropped(m[i]);
        keys_forget(m[i]);
        done++;
    }
    ff132_log(FF132_TAG "delete: %d of %d save(s) to the recycle bin\n", done, want);
    if (done < want) {
        ff132_flash(ui_text(T_TD_DEL_FAIL));
        return 0;
    }
    if (pinned) snprintf(msg, sizeof msg, ui_text(T_TD_DEL_N_DONE_SKIP), done, pinned);
    else        snprintf(msg, sizeof msg, ui_text(T_TD_DEL_N_DONE), done);
    ff132_flash(msg);
    return 1;
}

// The first confirm arms, the second runs. 0 = the menu stays open, 2 = it closes. Tested with the
// predicate the row is DRAWN with, so a row drawn dim can never answer a confirm by deleting.
static int range_del_row(void) {
    char msg[384], ok[40];
    if (!range_del_ready()) {
        range_del_disarm();
        ff132_flash(ui_text(g_rangeAnchor < 0 ? T_TD_DEL_NEED_RANGE : T_TD_RANGE_ROW_GONE));
        return 0;
    }
    if (!g_delArm || g_delArmAnchor != g_rangeAnchor || g_delArmSlot != g_lselSlot) {
        int n = g_rangeShownN;
        g_delArm = 1; g_delArmN = n;
        g_delArmAnchor = g_rangeAnchor; g_delArmSlot = g_lselSlot;
        ff132_log(FF132_TAG "delete: %d save(s) picked -- confirm again to run\n", n);
        // The question is put in both places: the row is in the middle of the screen and the flash
        // is at its top left, and either one alone is missed.
        ff132_tidy_ok_phrase(ok, sizeof ok);
        snprintf(msg, sizeof msg, ui_text(T_TD_LSEL_DEL_ARM), n, ok);
        ff132_flash(msg);
        return 0;
    }
    range_del_disarm();
    return range_del_apply() ? 2 : 0;
}

// ---- the rows ----------------------------------------------------------------------------------
static int lsel_note_row(void) {
    if (g_lselSlot < 0 || !slot_has_file(g_lselSlot)) {
        ff132_log(FF132_TAG "labels: the new-save row takes no note\n");
        ff132_flash(ui_text(T_TD_NOTE_NO_SAVE));
        return 0;
    }
    edit_open(g_lselSlot);
    return 0;                       // the editor draws above this menu, which stays open behind it
}

static int lsel_send_row(int move) {
    char dir[MAX_PATH], name932[FF132_CATNAME_BYTES], u8[FF132_CATNAME_U8];
    send_dest_label(u8, sizeof u8);
    if (send_dest(dir, sizeof dir, name932, sizeof name932)) {
        send_go(dir, name932, u8, move);    // it closes the menu itself when the send went through
        return 0;
    }
    // <new category>: the name is typed first and the editor's commit does the send. Refused here
    // as well, so no folder is made for a send that could not run anyway.
    if (send_refused()) return 0;
    g_sendPendMove = move;
    cat_edit_open(EDIT_CAT_SEND);
    return 0;                       // the editor draws above this menu, which stays open behind it
}

static int lsel_mark_row(void) {
    char msg[256];
    if (g_rangeAnchor >= 0) {
        range_clear();              // the menu stays open; the send rows shrink back to one save
        ff132_log(FF132_TAG "save list: range cleared\n");
        ff132_flash(ui_text(T_TD_RANGE_CLEARED));
        return 0;
    }
    if (g_lselSlot < 0 || !slot_has_file(g_lselSlot)) {
        ff132_flash(ui_text(T_TD_RANGE_ROW_GONE));
        return 0;
    }
    g_rangeAnchor = g_lselSlot;
    range_recount();
    // The cursor STAYS on this row, which now reads "clear the range": a second press is the undo,
    // never a send -- on the copy row a double press would ship the range.
    snprintf(msg, sizeof msg, ui_text(T_TD_RANGE_MARKED), g_lselSlot + 1);
    ff132_flash(msg);
    ff132_log(FF132_TAG "save list: range marked from save %d\n", g_lselSlot + 1);
    return 0;
}

int ff132_tidy_list_open(void) {
    if (g_lselOpen) return 1;
    if (!g_guiOk || !list_is_up()) return 0;
    // Left to the title menu's own refusal, which says why in one place.
    if (steam_cloud_on() == 1) return 0;
    g_lselSlot = list_slot_now();
    ff132_cats_refresh();          // what the destination pick offers, as the folder is right now
    g_sendDest = 0;
    // A mark can only survive a menu closed and reopened over the SAME list, and even then the
    // list may have been rebuilt under it.
    range_recount();
    g_lselSel  = (g_rangeAnchor >= 0) ? LSEL_COPY : LSEL_NOTE;
    g_lselOpen = 1;
    if (g_lselSlot >= 0) ff132_log(FF132_TAG "save list: menu opened on save %d\n", g_lselSlot + 1);
    else                 ff132_log(FF132_TAG "save list: menu opened on the new-save row\n");
    return 1;
}

void ff132_tidy_list_move(int dy) {
    if (!g_lselOpen) return;
    int s = g_lselSel + dy;
    g_lselSel = (s < 0) ? LSEL_ROWS - 1 : (s >= LSEL_ROWS) ? 0 : s;
}

// The list is a horizontal carousel with the newest save at the LEFT, so "previous" is left.
void ff132_tidy_list_step(int dir) {
    if (!g_lselOpen) return;
    ff132_di_fake_dik(dir < 0 ? FF132_DIK_LEFT : FF132_DIK_RIGHT, FF132_LIST_STEP_MS);
    ff132_logv(FF132_TAG "save list: handing the game a %s from save %d\n", dir < 0 ? "left" : "right",
           g_lselSlot + 1);
}

void ff132_tidy_list_cycle(int dir) {
    if (!g_lselOpen) return;
    if (g_lselSel == LSEL_COPY || g_lselSel == LSEL_MOVE) send_cycle(dir);
}

void ff132_tidy_list_follow(void) {
    if (g_lselOpen && gui_now()) {           // nothing readable this tick: keep the save we had
        int s = list_slot_now();
        if (s != g_lselSlot) {
            g_lselSlot = s;
            if (s >= 0) ff132_logv(FF132_TAG "save list: the cursor moved to save %d under the menu\n",
                               s + 1);
            else ff132_logv(FF132_TAG "save list: the cursor moved to the new-save row under the menu\n");
        }
        range_recount();
    }
    // After the cursor above, so a move off the save the confirm was armed on stands it down on
    // that same tick.
    range_del_poll(g_lselOpen && g_lselSel == LSEL_DEL);
}

// A row that closed the menu takes the gate with it -- unless it opened an editor, which holds it.
void ff132_tidy_list_go(void) {
    if (!g_lselOpen) return;
    int r = 0;
    switch (g_lselSel) {
    case LSEL_NOTE: r = lsel_note_row(); break;
    case LSEL_COPY: r = lsel_send_row(0); break;
    case LSEL_MOVE: r = lsel_send_row(1); break;
    case LSEL_MARK: r = lsel_mark_row(); break;
    case LSEL_DEL:  r = range_del_row(); break;
    default: break;
    }
    if (r == 2 && g_lselOpen) {
        g_lselOpen = 0;
        if (!g_edit) menu_gate_release();
    }
}

// ---- renumber: numbers that follow the story ----------------------------------------------------
// The one thing here that renames the player's own files. Between the two passes every moving save
// sits under a temp name and the game's list would come up short, so the journal goes down before
// the first rename and is dropped only once the last one has landed. The temp name carries the OLD
// number, which is what lets a rollback run with no journal at all.
#define RENUM_TMP ".renum"

static int path_exists(const char* p) {
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}
static int renum_journal_path(char* p, size_t n) { return data_file(p, n, FF132_RENJNL_NAME); }
static int renum_tmp_for(int slot, char* out, size_t n) {
    char base[MAX_PATH];
    if (!save_slot_path(slot, base, sizeof base)) return 0;
    snprintf(out, n, "%s" RENUM_TMP, base);
    return 1;
}
static int renum_tmps_left(void) {
    for (int s = 0; s < TIDY_SLOTS; s++) {
        char t[MAX_PATH];
        if (renum_tmp_for(s, t, sizeof t) && path_exists(t)) return 1;
    }
    return 0;
}
int ff132_tidy_renum_pending(void) {
    char p[MAX_PATH];
    return renum_journal_path(p, sizeof p) && path_exists(p);
}

static int renum_journal_write(const int* from, const int* to, int n) {
    char p[MAX_PATH], buf[TIDY_SLOTS * 12 + 1];
    if (!renum_journal_path(p, sizeof p)) return 0;
    int len = 0;
    for (int i = 0; i < n && len < (int)sizeof buf; i++)
        len += snprintf(buf + len, sizeof buf - (size_t)len, "%d %d\n", from[i], to[i]);
    return len < (int)sizeof buf && ff132_write_whole(p, buf, (size_t)len);
}
// Appended once every source sits under a temp name. A journal without this line means pass 1 was
// cut short: the only safe way on from there is BACK -- landing would be guesswork.
static int renum_mark_lifted(void) {
    char p[MAX_PATH];
    if (!renum_journal_path(p, sizeof p)) return 0;
    FILE* f = fopen(p, "a");
    if (!f) return 0;
    int ok = fputs("lifted\n", f) >= 0 && fflush(f) == 0;
    fclose(f);
    return ok;
}
static void renum_journal_drop(void) {
    char p[MAX_PATH];
    if (renum_journal_path(p, sizeof p)) DeleteFileA(p);
}

// Undo pass 1: every surviving temp goes back under its own old number.
static int renum_rollback_tmps(void) {
    int back = 0;
    for (int s = 0; s < TIDY_SLOTS; s++) {
        char t[MAX_PATH], p[MAX_PATH];
        if (renum_tmp_for(s, t, sizeof t) && path_exists(t) && save_slot_path(s, p, sizeof p)
            && MoveFileExA(t, p, 0)) back++;
    }
    return back;
}

// Pass 2: put each save down as its new number. A pair whose temp is gone already landed; one that
// will not land is left where it is, and the count of those is what keeps the journal alive.
// MOVEFILE_REPLACE_EXISTING is deliberately absent throughout: a rename onto a file that is there
// fails and is logged, rather than writing over a save.
static int renum_land(const int* from, const int* to, int n, int* moved) {
    int left = 0, m = 0;
    for (int i = 0; i < n; i++) {
        char tmp[MAX_PATH], dst[MAX_PATH];
        if (!renum_tmp_for(from[i], tmp, sizeof tmp) || !save_slot_path(to[i], dst, sizeof dst))
            continue;
        if (!path_exists(tmp)) continue;
        if (MoveFileExA(tmp, dst, 0)) { m++; continue; }
        ff132_loga(FF132_TAG "renumber: slot %d could not be put down -- kept for the next start (err"
                      " %lu)\n", from[i], (unsigned long)GetLastError());
        left++;
    }
    if (moved) *moved = m;
    return left;
}

// Everything else this mod keys by save number. The note cache and the sort keys are rebuilt from
// the folder by keys_scan_disk, so the direct-load sidecar is the only table that has to be
// CARRIED. A per-slot table added later that keys_scan_disk does not rebuild is added HERE.
static void renum_follow(const int* from, const int* to, int n) {
    int map[TIDY_SLOTS];
    for (int s = 0; s < TIDY_SLOTS; s++) map[s] = -1;
    for (int i = 0; i < n; i++)
        if (from[i] >= 0 && from[i] < TIDY_SLOTS) map[from[i]] = to[i];
    ff132_load_rec_permute(map, TIDY_SLOTS);
    keys_scan_disk();
}

// One save, plus the one key the order needs that tidy_key does not hold: the file's last-write
// time (nothing here ever re-stamps one, so it is the save's real date).
typedef struct { int slot; tidy_key k; uint64_t ft; } renum_ent;

// SORT_EARLY's key chain, ascending: the list's front-to-back order is the numbering order.
static int __cdecl renum_cmp(const void* A, const void* B) {
    const renum_ent* a = (const renum_ent*)A;
    const renum_ent* b = (const renum_ent*)B;
    if (a->k.ep    != b->k.ep)    return a->k.ep    < b->k.ep    ? -1 : 1;
    if (a->k.frag  != b->k.frag)  return a->k.frag  < b->k.frag  ? -1 : 1;
    if (a->k.aplay != b->k.aplay) return a->k.aplay < b->k.aplay ? -1 : 1;
    if (a->ft      != b->ft)      return a->ft      < b->ft      ? -1 : 1;
    return a->slot < b->slot ? -1 : a->slot > b->slot ? 1 : 0;
}

// How many numbers the game itself offers: a save renamed past that makes the game abort when a
// list opens, so an unreadable count falls back to the game's own rather than to the table size.
static int renum_slots(void) {
    int slots = save_slot_count();
    if (slots < 1) return FF132_SLOTS_GAME;
    return slots < TIDY_SLOTS ? slots : TIDY_SLOTS;
}

static int renum_collect(renum_ent* e, int cap, int work) {
    int slots = renum_slots();
    int m = 0;
    for (int s = 0; s < slots && m < cap; s++) {
        if (s == work) continue;
        char path[MAX_PATH];
        tidy_key k = kNoKey;
        if (!slot_keys(s, &k, path, sizeof path)) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        uint64_t ft = 0;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
            ft = ft_u64(fa.ftLastWriteTime);
        e[m].slot = s;
        e[m].k    = k;
        e[m].ft   = ft;
        m++;
    }
    tidy_key* k[TIDY_SLOTS];
    int slot[TIDY_SLOTS];
    for (int i = 0; i < m; i++) { k[i] = &e[i].k; slot[i] = e[i].slot; }
    progress_adjust(k, slot, m);
    return m;
}

// The rename cannot be undone from the menu, so the first press only asks.
static volatile int   g_renumArmN = 0;       // saves the armed run would move; 0 = not armed
static void renum_arm_poll(int rowActive) {
    if (g_renumArmN && !rowActive) g_renumArmN = 0;
}

static void renum_close_menu(void) {
    g_open = 0;
    menu_gate_release();
}

static void renum_apply(void) {
    char msg[192];
    int st = ff132_main_state();
    if (st != FF132_MS_TITLE) {
        ff132_log(FF132_TAG "renumber: refused -- main state %d, not the title screen\n", st);
        ff132_flash(ui_text(T_TD_NOTITLE));
        return;
    }
    // Re-running would truncate a kept journal over live temps, whose pairs may differ.
    if (ff132_recovery_owns_saves() || renum_tmps_left()) {
        ff132_loga(FF132_TAG "renumber: refused -- an unfinished run or switch owns these files\n");
        ff132_flash(ui_text(T_TD_RENUM_BUSY));
        return;
    }
    int work = save_work_slot();
    renum_ent e[TIDY_SLOTS];
    int n = renum_collect(e, TIDY_SLOTS, work);
    if (n <= 0) {
        ff132_log(FF132_TAG "renumber: no save to renumber (working slot %d)\n", work);
        ff132_flash(ui_text(T_TD_RENUM_NONE));
        return;
    }
    qsort(e, (size_t)n, sizeof e[0], renum_cmp);
    // 00 upwards in story order, stepping over the working slot's own number: that save keeps its
    // number, and no other save may be given it -- the next automatic save still lands on it.
    // A file whose header would not read is not in e[] but still holds its number: landing a save
    // there fails for good, and the journal would then own the folder on every start.
    int from[TIDY_SLOTS], to[TIDY_SLOTS], dst[TIDY_SLOTS], m = 0, next = 0, placed = 0;
    int slots = renum_slots();
    unsigned char held[TIDY_SLOTS] = { 0 };
    for (int s = 0; s < slots && s < TIDY_SLOTS; s++) {
        char p[MAX_PATH];
        if (s == work || !save_slot_path(s, p, sizeof p) || !path_exists(p)) continue;
        held[s] = 1;
        for (int i = 0; i < n; i++) if (e[i].slot == s) { held[s] = 0; break; }
        if (held[s]) ff132_loga(FF132_TAG "renumber: slot %d would not read -- it keeps its number\n", s);
    }
    for (int i = 0; i < n; i++) {
        while (next == work || (next < TIDY_SLOTS && held[next])) next++;
        if (next >= slots) break;
        dst[i] = next;
        placed = i + 1;
        if (e[i].slot != next) { from[m] = e[i].slot; to[m] = next; m++; }
        next++;
    }
    // Every save's answer, moved or not: this is what an offline prediction is compared against,
    // and the journal that holds the moving pairs is gone by the time anyone reads the log.
    for (int i = 0; i < placed; i++)
        ff132_log(FF132_TAG "renumber: %02d -> %02d (ep %d, frag %d, hist %d, area %d, play %us -> %us)\n",
              e[i].slot, dst[i], (int)e[i].k.ep, (int)e[i].k.frag, e[i].k.hist, (int)e[i].k.area,
              (unsigned)e[i].k.play, (unsigned)e[i].k.aplay);
    if (!m) {
        ff132_log(FF132_TAG "renumber: the numbers already follow the story\n");
        ff132_flash(ui_text(T_TD_RENUM_NONE));
        renum_close_menu();
        return;
    }
    if (g_renumArmN != m) {
        // Re-asked when the count changed under the arm, so the number confirmed is the one run.
        char ok[40];
        g_renumArmN = m;
        ff132_tidy_ok_phrase(ok, sizeof ok);
        snprintf(msg, sizeof msg, ui_text(T_TD_RENUM_ARM), m, ok);
        ff132_flash(msg);
        ff132_log(FF132_TAG "renumber: %d save(s) would move -- confirm again to run\n", m);
        return;
    }
    g_renumArmN = 0;
    if (!renum_journal_write(from, to, m)) {
        ff132_loga(FF132_TAG "renumber: the journal would not write -- not starting\n");
        ff132_flash(ui_text(T_TD_RENUM_FAIL));
        return;
    }
    // Pass 1: everything that moves goes to a temp name first, so no rename can land on a save
    // that has not been picked up yet.
    int lifted = 1;
    for (int i = 0; i < m; i++) {
        char src[MAX_PATH], tmp[MAX_PATH];
        if (save_slot_path(from[i], src, sizeof src) && renum_tmp_for(from[i], tmp, sizeof tmp)
            && MoveFileExA(src, tmp, 0)) continue;
        ff132_loga(FF132_TAG "renumber: slot %d could not be lifted (err %lu) -- rolling back\n",
               from[i], (unsigned long)GetLastError());
        lifted = 0;
        break;
    }
    if (lifted && !renum_mark_lifted()) {
        ff132_loga(FF132_TAG "renumber: the journal would not take the lifted line -- rolling back\n");
        lifted = 0;
    }
    if (!lifted) {
        int back = renum_rollback_tmps();
        if (renum_tmps_left()) {
            // The journal stays: with no lifted line the next start rolls back what is left.
            ff132_loga(FF132_TAG "renumber: the rollback is still short (%d put back) -- the next start"
                          " finishes it\n", back);
            ff132_flash(ui_text(T_TD_RENUM_PART));
        } else {
            renum_journal_drop();
            ff132_log(FF132_TAG "renumber: rolled back whole (%d save(s) put back) -- nothing changed\n",
                  back);
            ff132_flash(ui_text(T_TD_RENUM_FAIL));
        }
        renum_close_menu();
        return;
    }
    int left = renum_land(from, to, m, NULL);
    if (left) {
        // Some saves are under new numbers while the sidecar still says the old ones: the journal
        // stays, and the next start lands the rest before anything reads the folder.
        ff132_loga(FF132_TAG "renumber: %d of %d save(s) stayed under a temp name -- the next start"
                      " finishes it\n", left, m);
        ff132_flash(ui_text(T_TD_RENUM_PART));
        renum_close_menu();
        return;
    }
    renum_journal_drop();
    renum_follow(from, to, m);
    ff132_log(FF132_TAG "renumber: %d of %d saves renumbered into story order\n", m, n);
    snprintf(msg, sizeof msg, ui_text(T_TD_RENUM_DONE), m);
    ff132_flash(msg);
    renum_close_menu();
}

// A leftover journal means the renames were cut short: settle them before anything draws a list.
static void renum_recover(void) {
    char jp[MAX_PATH];
    if (!renum_journal_path(jp, sizeof jp)) return;
    if (!path_exists(jp)) {
        // A temp with no journal at all: pass 1 died before the journal reached the disk, or the
        // journal was taken away by hand. The temp name still carries the number it came from.
        if (!renum_tmps_left()) return;
        int back = renum_rollback_tmps();
        ff132_loga(FF132_TAG "renumber: an interrupted run was rolled back (%d save(s) put back) -- run"
                      " it again\n", back);
        return;
    }
    FILE* f = fopen(jp, "r");
    if (!f) return;
    int from[TIDY_SLOTS], to[TIDY_SLOTS], n = 0, lifted = 0;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int a, b;
        if (strncmp(line, "lifted", 6) == 0) { lifted = 1; continue; }
        if (sscanf(line, "%d %d", &a, &b) != 2) continue;
        if (a < 0 || a >= TIDY_SLOTS || b < 0 || b >= TIDY_SLOTS) { n = 0; break; }
        if (n < TIDY_SLOTS) { from[n] = a; to[n] = b; n++; }
    }
    fclose(f);
    // No lifted line, or nothing readable: landing from here would move temps onto numbers that
    // were never emptied. Rolling back never guesses -- the temp names carry the answer.
    if (!lifted || n <= 0) {
        int back = renum_rollback_tmps();
        if (renum_tmps_left()) {
            ff132_loga(FF132_TAG "renumber: the rollback is still short (%d put back) -- retried on the"
                          " next start\n", back);
            return;
        }
        renum_journal_drop();
        ff132_loga(FF132_TAG "renumber: an interrupted run was rolled back (%d save(s) put back) -- run"
                      " it again\n", back);
        return;
    }
    ff132_loga(FF132_TAG "renumber: finishing an interrupted run (%d pair(s))\n", n);
    int moved = 0;
    int left = renum_land(from, to, n, &moved);
    if (left) {
        ff132_loga(FF132_TAG "renumber: %d save(s) are still under a temp name -- the journal is kept for"
                      " the next start\n", left);
        return;
    }
    renum_journal_drop();
    renum_follow(from, to, n);
    ff132_loga(FF132_TAG "renumber: an interrupted run was finished (%d save(s) put down)\n", moved);
}

// ---- install ----------------------------------------------------------------------------------
static void sort_install(void) {
    uintptr_t at = g_base + FF132_RVA_LIST_SORT;
    if (wait_for_bytes(at, FF132_SIG_LIST_SORT, (int)sizeof FF132_SIG_LIST_SORT, FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "save list: sort hook @0x%08X FAILED (bytes never matched)\n", (unsigned)at);
        return;
    }
    uintptr_t push = g_base + FF132_RVA_LIST_SORT_CMP_PUSH;
    if (wait_for_bytes(push, FF132_SIG_LIST_SORT_CMP_PUSH, (int)sizeof FF132_SIG_LIST_SORT_CMP_PUSH,
                       FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "save list: sort hook FAILED (the comparator's push @0x%08X did not match)\n",
               (unsigned)push);
        return;
    }
    void* cave = ff13_install_ecx_hook(at, FF132_SIG_LIST_SORT, (int)sizeof FF132_SIG_LIST_SORT,
                                       (void*)on_list_sort);
    if (!cave) {
        ff132_loga(FF132_TAG "save list: sort hook @0x%08X FAILED (not patched)\n", (unsigned)at);
        return;
    }
    // Only after the signature matched, and only the one imm32: the push stays a push.
    uintptr_t imm = push + FF132_OFF_LIST_SORT_CMP_IMM;
    DWORD old;
    if (!VirtualProtect((void*)imm, 4, PAGE_EXECUTE_READWRITE, &old)) {
        ff132_loga(FF132_TAG "save list: sort hook FAILED (the comparator's operand is not writable)\n");
        return;
    }
    *(volatile uint32_t*)imm = (uint32_t)(uintptr_t)tidy_cmp;
    VirtualProtect((void*)imm, 4, old, &old);
    g_sortOk = 1;
    ff132_loga(FF132_TAG "save list: sort hook OK (cave=%p, comparator=%p)\n", cave, (void*)tidy_cmp);
}

// The cursor hook and the heading hook, both inside GuiSaveLoadX360::update. A miss on the first
// costs the list menu; a miss on the second costs only the display -- a note is still written into
// the save's own header either way.
static void gui_install(void) {
    uintptr_t at = g_base + FF132_RVA_LIST_UPDATE;
    if (wait_for_bytes(at, FF132_SIG_LIST_UPDATE, (int)sizeof FF132_SIG_LIST_UPDATE, FF132_SITE_WAIT_MS) >= 0
        && ff13_install_ecx_hook(at, FF132_SIG_LIST_UPDATE, (int)sizeof FF132_SIG_LIST_UPDATE,
                                 (void*)on_list_update)) {
        g_guiOk = 1;
        ff132_loga(FF132_TAG "save list: cursor hook @0x%08X OK\n", (unsigned)at);
    } else {
        ff132_loga(FF132_TAG "save list: cursor hook @0x%08X FAILED -- the list menu cannot open\n",
               (unsigned)at);
    }
    uintptr_t tt = g_base + FF132_RVA_LIST_TITLE_BUILT;
    if (wait_for_bytes(tt, FF132_SIG_LIST_TITLE_BUILT, (int)sizeof FF132_SIG_LIST_TITLE_BUILT,
                       FF132_SITE_WAIT_MS) >= 0
        && ff13_install_esp_hook(tt, FF132_SIG_LIST_TITLE_BUILT, (int)sizeof FF132_SIG_LIST_TITLE_BUILT,
                                 (void*)on_list_title_built)) {
        ff132_loga(FF132_TAG "labels: hook @0x%08X OK\n", (unsigned)tt);
    } else {
        ff132_loga(FF132_TAG "labels: hook @0x%08X FAILED -- notes are written but not shown\n",
               (unsigned)tt);
    }
}

void ff132_tidy_install(void) {
    // First of all: an interrupted category switch has to be finished before anything below reads
    // a save folder that is half one category and half another, and an interrupted renumber before
    // anything reads a folder where half the saves are under a temp name. Ahead of both, saves the
    // launcher parked for the unmodded game come back, so recovery and every scan see them -- after
    // the category it lent that game has taken its saves back out of the root.
    if (ff132_nomod_return()) ff132_saves_unpark();
    ff132_cats_install();
    renum_recover();
    // Here, on the worker and before the sort hook goes in: otherwise the first list opened reads
    // every save's progress inside the game thread's sort hook, and the list freezes before it
    // appears. Cloud or not, the hook reads it.
    {
        DWORD t0 = GetTickCount();
        keys_scan_disk();
        ff132_loga(FF132_TAG "save tidy: %d save(s) read for the list in %lu ms\n", g_keyTotal,
               (unsigned long)(GetTickCount() - t0));
    }
    if (steam_cloud_on() == 1) {
        // With the key refused there is no way left to clear either setting, and a list that is
        // short or in an unexpected order would read as saves lost. The file is NOT rewritten, so
        // both come back once Cloud is off for this game.
        ff132_loga(FF132_TAG "save tidy: Steam Cloud is on for this game -- the menu key is refused, and"
                      " the sort and the filter stay off (sort %d, filter %d kept in the file)\n",
               g_sort, g_filter);
        g_sortFile = g_sort;
        g_filterFile = g_filter;
        g_cloudHold = 1;
        g_sort = SORT_TIME;
        g_filter = FILTER_ALL;
    } else if (g_filter >= 0) {
        char wn[96];
        world_name(g_filter, wn, sizeof wn);
        if (world_count(g_filter) <= 0)
            ff132_log(FF132_TAG "filter: world %d (%s) holds no save now -- the whole list is shown\n",
                  g_filter, wn);
        else
            ff132_log(FF132_TAG "filter: world %d (%s) restored (%d save(s))\n", g_filter, wn,
                  world_count(g_filter));
    }
    sort_install();
    gui_install();
    ff132_loga(FF132_TAG "save tidy: sort %d, filter %d\n", g_sort, g_filter);
}

// ---- the menu ---------------------------------------------------------------------------------
int ff132_tidy_is_open(void) { return g_open; }

// A picked switch target is carried out when the row is left by any of Enter, the cursor or the
// menu closing; picking alone moves nothing.
static void cat_switch_go(void) {
    char u8[FF132_CATNAME_U8], msg[320];
    int out = 0, in = 0;
    if (!ff132_cats_switch_pick(NULL, 0)) return;
    if (!ff132_cat_switch(&out, &in)) {
        cat_fail_flash();
        return;
    }
    ff132_cats_current(u8, sizeof u8);
    snprintf(msg, sizeof msg, ui_text(T_TD_CAT_DONE), u8, out, in);
    ff132_flash(msg);
}

void ff132_tidy_close(void) {
    if (g_open && g_sel == TIDY_CAT_PICK) cat_switch_go();
    g_open = 0;
}

void ff132_tidy_open_menu(void) {
    if (g_open) return;
    if (steam_cloud_on() == 1) {
        ff132_log(FF132_TAG "save tidy: refused -- Steam Cloud is on for this game\n");
        ff132_flash(ui_text(T_TD_CLOUD));
        return;
    }
    int st = ff132_main_state();
    if (st != FF132_MS_TITLE) {
        ff132_log(FF132_TAG "save tidy: refused -- main state %d, not the title screen\n", st);
        ff132_flash(ui_text(T_TD_NOTITLE));
        return;
    }
    keys_scan_disk();          // the counts the filter row offers are only right if they are fresh
    ff132_cats_refresh();
    g_sel = 0;
    g_open = 1;
    ff132_log(FF132_TAG "save tidy: menu opened (sort %d, filter %d, %d save(s) known)\n",
          g_sort, g_filter, g_keyTotal);
}

void ff132_tidy_move(int dy) {
    if (!g_open) return;
    if (g_sel == TIDY_CAT_PICK) cat_switch_go();
    int s = g_sel + dy;
    g_sel = (s < 0) ? TIDY_ROWS - 1 : (s >= TIDY_ROWS) ? 0 : s;
}

// FILTER_ALL, then the worlds that actually hold a save in story order (each world's earliest
// save), then the unknown bucket. An empty one is never offered, because an empty list is not a
// state to hand the game.
static int filter_choices(int* list) {
    int n = 0;
    list[n++] = FILTER_ALL;
    for (int w = 0; w < WORLD_N; w++) {
        if (g_worldCount[w] <= 0) continue;
        int i = n++;
        while (i > 1 && g_worldFirst[list[i - 1]] > g_worldFirst[w]) { list[i] = list[i - 1]; i--; }
        list[i] = w;
    }
    if (g_worldCount[WORLD_UNKNOWN] > 0) list[n++] = WORLD_UNKNOWN;
    return n;
}

static void filter_cycle(int dir) {
    int list[WORLD_N + 2], at = 0;
    int n = filter_choices(list);
    for (int i = 0; i < n; i++) if (list[i] == g_filter) at = i;
    at = (at + (dir > 0 ? 1 : n - 1)) % n;
    g_filter = list[at];
    state_save();
    char wn[96];
    world_name(g_filter, wn, sizeof wn);
    if (g_filter >= 0) ff132_log(FF132_TAG "filter: world %d '%s' (%d save(s))\n", g_filter, wn,
                             world_count(g_filter));
    else               ff132_log(FF132_TAG "filter: off (every save shown)\n");
}

// The row's value for one choice.
static void filter_text(int f, char* out, size_t n) {
    char wn[96];
    if (f < 0)                    snprintf(out, n, "%s", ui_text(T_TD_FILTER_ALL));
    else if (f == WORLD_UNKNOWN)  snprintf(out, n, ui_text(T_TD_FILTER_UNKNOWN), world_count(f));
    else {
        world_name(f, wn, sizeof wn);
        snprintf(out, n, ui_text(T_TD_FILTER_ONE), wn, world_count(f));
    }
}

void ff132_tidy_cycle(int dir) {
    if (!g_open) return;
    if (g_sel == TIDY_FILTER) {
        filter_cycle(dir);
    } else if (g_sel == TIDY_CAT_PICK) {
        ff132_cats_switch_cycle(dir);
    } else if (g_sel == TIDY_CAT_DEL) {
        ff132_cats_cycle(dir);
    } else if (g_sel == TIDY_SORT) {
        int m = g_sort + (dir > 0 ? 1 : SORT_MODES - 1);
        g_sort = m % SORT_MODES;
        state_save();
        ff132_log(FF132_TAG "save list: order = mode %d (it applies the next time a list opens)\n", g_sort);
    }
}

// The menu stays up whatever the row does, with the renumber the one exception: it renames the
// files the list is built from, so it closes and lets the next list be read fresh. The filter and
// the sort change with the arrows alone, and each category row's receipt is the row above it
// wearing the new name.
void ff132_tidy_confirm(void) {
    if (!g_open) return;
    char u8[FF132_CATNAME_U8], msg[320];
    switch (g_sel) {
    case TIDY_FILTER:
        if (g_filter >= 0) {
            g_filter = FILTER_ALL;
            state_save();
            ff132_log(FF132_TAG "filter: off (every save shown)\n");
        }
        break;
    case TIDY_CAT_PICK:
        cat_switch_go();
        break;
    case TIDY_CAT_NEW:
    case TIDY_CAT_NEW_ALL:
    case TIDY_CAT_RENAME:
        cat_edit_open(g_sel == TIDY_CAT_RENAME  ? EDIT_CAT_RENAME
                    : g_sel == TIDY_CAT_NEW_ALL ? EDIT_CAT_NEW_ALL : EDIT_CAT_NEW);
        break;
    case TIDY_CAT_DEL: {
        if (!ff132_cats_pick(u8, sizeof u8)) break;
        int r = ff132_cat_delete_hold();
        if (r > 0) {
            snprintf(msg, sizeof msg, ui_text(T_TD_CAT_DEL_DONE), u8);
            ff132_flash(msg);
        } else if (r < 0) {
            ff132_flash(ui_text(T_TD_CAT_DEL_FAIL));
        } else {
            // The question is put in both places: the row is in the middle of the screen and the
            // flash is at its top left, and either one alone is missed.
            char ok[40];
            ff132_tidy_ok_phrase(ok, sizeof ok);
            snprintf(msg, sizeof msg, ui_text(T_TD_CAT_DEL_ARM), u8, ok);
            ff132_flash(msg);
        }
        break;
    }
    case TIDY_RENUM:
        renum_apply();
        break;
    default:
        break;
    }
}

// Worker, every tick. A menu belongs to what it was opened on -- the screen for the title menu,
// the game's list for the other -- and either of those going away is the one case where nothing the
// player pressed ends it.
void ff132_tidy_screen_tick(void) {
    static int at = -2;
    static int listWas = 0;
    // Every tick and ahead of the returns below: the one place the CATEGORY delete's confirm is
    // stood down, so the row that is no longer selected cannot answer one. The range delete's own
    // confirm is stood down in ff132_tidy_list_follow instead -- it has to be read after that tick's
    // cursor move, not before it.
    ff132_cat_del_poll(g_open && g_sel == TIDY_CAT_DEL);
    renum_arm_poll(g_open && g_sel == TIDY_RENUM);
    int st = ff132_main_state();
    int listNow = list_is_up();
    if (listWas && !listNow) {
        // The rows the mark points into left the screen with the list, whether or not the menu was
        // still up over them.
        if (g_rangeAnchor >= 0)
            ff132_log(FF132_TAG "save list: the list closed -- the range mark goes with it\n");
        range_clear();
        if (g_lselOpen || g_edit) {
            ff132_log(FF132_TAG "save list: the list closed (main state %d) -- the menu closes with it\n",
                  st);
            if (g_edit) ff132_tidy_edit_finish(0);
            g_lselOpen = 0;
            menu_gate_release();
        }
    }
    listWas = listNow;
    if (st < 0 || st == at) return;
    int had = g_open || g_lselOpen || g_edit;
    if (at != -2 && had)
        ff132_logv(FF132_TAG "save tidy: screen changed %d -> %d -- the menu closes with it\n", at, st);
    at = st;
    range_clear();                    // the list the mark pointed into left the screen with it
    if (!had) return;
    if (g_edit) ff132_tidy_edit_finish(0);
    g_open = 0;
    g_lselOpen = 0;
    // Not a plain lift: a key still down would reach the new screen as a fresh press.
    menu_gate_release();
}

// ---- drawing ----------------------------------------------------------------------------------
// Render thread. The lapse guard is enforced here because a stalled worker cannot enforce anything;
// the delay is what keeps the frame between "menu opened" and "input gate armed" from counting.
int ff132_tidy_visible(void) {
    static DWORD staleSince = 0;
    if (!g_open && !g_lselOpen && !g_edit) { staleSince = 0; return 0; }
    if (ff132_di_gate_left_ms() > 0) { staleSince = 0; return 1; }
    DWORD now = GetTickCount();
    if (!staleSince) { staleSince = now; return 1; }
    if (now - staleSince <= 2000) return 1;
    staleSince = 0;
    g_open = 0;
    g_lselOpen = 0;
    if (g_edit) g_editDone = -1;      // the worker owns the buffer; it closes on its next tick
    ff132_loga(FF132_TAG "save tidy: the input gate lapsed under the menu -- closed\n");
    return 0;
}

void ff132_tidy_ok_phrase(char* out, size_t n) {
    char k[32];
    vk_label(g_menuOkVk, k, sizeof k);
    snprintf(out, n, ui_text(T_TD_OK_KEY), k);
}

// The heading each row opens a group with; -1 = the row continues the group above it.
static const int kGroup[TIDY_ROWS] = { T_TD_GRP_FILTER,
                                       T_TD_GRP_CAT, -1, -1, -1, -1,
                                       T_TD_GRP_SORT, -1 };

// The backdrop behind one block, and the lowest edge any block has drawn this frame: what
// ff132_tidy_height reports, so the overlay can lift the blocks clear of a short screen's bottom.
// Render thread.
static int g_boxBottom = 0;
static void tidy_box(int y0, int y1) {
    ff132_ovl_backdrop(0, y0, FF132_OVL_FULL_W, y1, 0x99000000);
    if (y1 > g_boxBottom) g_boxBottom = y1;
}
// A footer's rows never count as fewer than the three the boxes were sized at before they were
// measured: the box only grows past its old size when a large text size wraps the footer further.
#define FOOT_ROWS_MIN 3
static int foot_rows(int n) { return n > FOOT_ROWS_MIN ? n : FOOT_ROWS_MIN; }

// A category value padded to the widest name it can show (and <new category>, where that is a
// stop), so the row's right arrow stays put as left and right walk it. Render thread.
static void fit_cat(char* v, size_t n, const char* extra, int quoted) {
    static char names[FF132_CAT_MAX][FF132_CATNAME_U8];
    const char* cands[FF132_CAT_MAX + 1];
    int cn = 0, cc = ff132_cats_count();
    for (int i = 0; i < cc && i < FF132_CAT_MAX; i++)
        if ((quoted ? ff132_cats_name_q : ff132_cats_name)(i, names[i], sizeof names[i]))
            cands[cn++] = names[i];
    if (extra) cands[cn++] = extra;
    ff132_ovl_fit(v, n, cands, cn);
}

static int tidy_draw_menu(int x, int y) {
    char up[32], down[32], left[32], right[32], cancel[32], ok[40], val[320], buf[448];
    char cur[FF132_CATNAME_U8], pick[FF132_CATNAME_U8], pickA[FF132_CATNAME_U8 * 2 + 16];
    char swA[FF132_CATNAME_U8 * 2 + 16];
    static char row[TIDY_ROWS][384];
    unsigned long col[TIDY_ROWS];
    vk_label(g_menuUpVk, up, sizeof up);       vk_label(g_menuDownVk, down, sizeof down);
    vk_label(g_menuLeftVk, left, sizeof left); vk_label(g_menuRightVk, right, sizeof right);
    vk_label(g_cancelVk, cancel, sizeof cancel);
    ff132_tidy_ok_phrase(ok, sizeof ok);
    ff132_cats_current(cur, sizeof cur);
    int havePick = ff132_cats_pick(pick, sizeof pick);
    {
        char pp[FF132_CATNAME_U8 * 2];
        snprintf(pp, sizeof pp, "%s", pick);
        fit_cat(pp, sizeof pp, NULL, 1);
        ui_arrows(pickA, sizeof pickA, pp);
        if (!ff132_cats_switch_pick(pp, sizeof pp))
            snprintf(pp, sizeof pp, "%s", ui_text(T_TD_CAT_UNPICKED));
        fit_cat(pp, sizeof pp, ui_text(T_TD_CAT_UNPICKED), 0);
        ui_arrows(swA, sizeof swA, pp);
    }

    static const int kSortText[SORT_MODES] = { T_TD_SORT_TIME, T_TD_SORT_LATE, T_TD_SORT_EARLY,
                                               T_TD_SORT_NUMBER };
    int sel = g_sel;
    for (int i = 0; i < TIDY_ROWS; i++) {
        int dim = 0;
        char* r = row[i];
        size_t rn = sizeof row[i];
        switch (i) {
        case TIDY_FILTER:
            filter_text(g_filter, val, sizeof val);
            {
                static char fs[WORLD_N + 2][128];
                const char* cands[WORLD_N + 2];
                int list[WORLD_N + 2];
                int cn = filter_choices(list);
                for (int c = 0; c < cn; c++) {
                    filter_text(list[c], fs[c], sizeof fs[c]);
                    cands[c] = fs[c];
                }
                ff132_ovl_fit(val, sizeof val, cands, cn);
            }
            ui_arrows(r, rn, val);
            // A filter row with nothing to offer is drawn as what it is: no save to pick from.
            dim = !g_keyTotal;
            break;
        case TIDY_CAT_PICK:
            if (havePick) snprintf(r, rn, ui_text(T_TD_CAT_PICK), swA, cur);
            else          snprintf(r, rn, ui_text(T_TD_CAT_PICK_NONE), cur);
            dim = !havePick;
            break;
        case TIDY_CAT_DEL:
            if (!havePick)               snprintf(r, rn, "%s", ui_text(T_TD_CAT_DEL_NONE));
            else if (ff132_cat_del_armed()) snprintf(r, rn, ui_text(T_TD_CAT_DEL_ARM), pick, ok);
            else                         snprintf(r, rn, ui_text(T_TD_CAT_DEL), pickA);
            dim = !havePick;
            break;
        case TIDY_SORT:
            {
                const char* cands[SORT_MODES];
                for (int s = 0; s < SORT_MODES; s++) cands[s] = ui_text(kSortText[s]);
                snprintf(val, sizeof val, "%s", ui_text(kSortText[g_sort]));
                ff132_ovl_fit(val, sizeof val, cands, SORT_MODES);
            }
            ui_arrows(r, rn, val);
            break;
        case TIDY_RENUM:
            if (g_renumArmN) snprintf(r, rn, ui_text(T_TD_RENUM_ARM), g_renumArmN, ok);
            else             snprintf(r, rn, "%s", ui_text(T_TD_RENUM));
            dim = !g_keyTotal;
            break;
        default:
            snprintf(r, rn, "%s%s",
                     ui_text(i == TIDY_CAT_NEW     ? T_TD_CAT_NEW
                         : i == TIDY_CAT_NEW_ALL ? T_TD_CAT_NEW_ALL : T_TD_CAT_RENAME),
                     ui_ellipsis());
            break;
        }
        col[i] = (i == sel) ? FF132_COL_MENU_SEL : dim ? FF132_COL_MENU_OFF : FF132_COL_MENU;
    }

    int heads = 0, selY = y + FF132_OVL_LINE_H;
    for (int i = 0; i < TIDY_ROWS; i++) {
        if (kGroup[i] >= 0) { heads++; if (i <= sel) selY += FF132_OVL_LINE_H; }
        if (i < sel) selY += FF132_OVL_LINE_H;
    }
    // The two footer lines are made first, so the box can be sized to what they wrap to.
    char foot[2][448], tk[32];
    snprintf(foot[0], sizeof foot[0], ui_text(T_TD_KEYS_MOVE), up, down, left, right);
    vk_label(g_saveTidyVk, tk, sizeof tk);
    if      (g_sel == TIDY_FILTER)   snprintf(foot[1], sizeof foot[1], ui_text(T_TD_KEYS_RESET), ok, cancel, tk);
    else if (g_sel == TIDY_CAT_PICK) snprintf(foot[1], sizeof foot[1], ui_text(T_TD_CAT_KEYS), left, right, ok);
    else if (g_sel == TIDY_CAT_DEL)  snprintf(foot[1], sizeof foot[1], ui_text(T_TD_CAT_DEL_KEYS), left,
                                              right, ok);
    else if (g_sel == TIDY_RENUM)    snprintf(foot[1], sizeof foot[1], ui_text(T_TD_RENUM_KEYS), ok, cancel, tk);
    else                             snprintf(foot[1], sizeof foot[1], ui_text(T_TD_KEYS_CLOSE), cancel, tk);
    int footN = ff132_ovl_wrap_count(x, x, foot[0]) + ff132_ovl_wrap_count(x, x, foot[1]);
    // Title + headings + the rows + the footer.
    int rows = 1 + heads + TIDY_ROWS + foot_rows(footN);
    tidy_box(y - ff132_ovl_px(4), y + rows * FF132_OVL_LINE_H + ff132_ovl_px(8));
    if (sel >= 0 && sel < TIDY_ROWS) FF132_MENU_BAND(selY, 1);
    ff132_ovl_say(x, y, ui_text(T_TD_HEAD), FF132_COL_TITLE); y += FF132_OVL_LINE_H;
    for (int i = 0; i < TIDY_ROWS; i++) {
        if (kGroup[i] >= 0) { ff132_ovl_say(x, y, ui_text(kGroup[i]), FF132_COL_HEAD); y += FF132_OVL_LINE_H; }
        snprintf(buf, sizeof buf, "   %s %s", (i == sel) ? ">" : " ", row[i]);
        ff132_ovl_say(x, y, buf, col[i]); y += FF132_OVL_LINE_H;
    }
    for (int f = 0; f < 2; f++)
        y += FF132_OVL_LINE_H * ff132_ovl_say_wrap(x, x, y, foot[f], FF132_COL_TEXT, FF132_OVL_LINE_H);
    return y + FF132_OVL_LINE_H / 2;      // half a line, so two blocks read as two blocks
}

// The buffer the message thread is typing into, as one line with the caret on its end. Converted
// here rather than kept as UTF-8 because a backspace has to take a whole character with it.
static int edit_draw(int x, int y) {
    char buf[384], text[EDIT_BUF * 3], help[384], keys[384];
    int mode = g_editMode;
    int bad = g_editBad;
    // The two lines under the input are made first, so the box can be sized to what they wrap to.
    if (mode == EDIT_NOTE) {
        if (bad) snprintf(help, sizeof help, ui_text(T_TD_EDIT_BAD), FF132_NOTE_CHARS);
        else     snprintf(help, sizeof help, "%s", ui_text(T_TD_EDIT_HELP));
    } else {
        // A refused keystroke shows the help line itself in red: what may go in a name IS the
        // answer to it, and a second wording for the same rule would only be one more place to
        // keep in step.
        snprintf(help, sizeof help, "%s",
                 ui_text(bad == EDIT_BAD_EXISTS ? T_TD_EDIT_CAT_EXISTS
                     : bad == EDIT_BAD_NAME   ? T_TD_EDIT_CAT_BADNAME : T_TD_EDIT_CAT_HELP));
    }
    {
        char k[32], ok[40];
        vk_label(g_saveTidyVk, k, sizeof k);
        ff132_tidy_ok_phrase(ok, sizeof ok);
        snprintf(keys, sizeof keys, ui_text(mode == EDIT_NOTE ? T_TD_EDIT_KEYS
                                                     : T_TD_EDIT_CAT_KEYS), ok, k);
    }
    int footN = ff132_ovl_wrap_count(x, x, help) + ff132_ovl_wrap_count(x, x, keys);
    // Title + the input line + the two lines under it.
    tidy_box(y - ff132_ovl_px(4), y + (2 + foot_rows(footN)) * FF132_OVL_LINE_H + ff132_ovl_px(8));
    if (mode == EDIT_NOTE) snprintf(buf, sizeof buf, ui_text(T_TD_EDIT_HEAD), FF132_NOTE_CHARS);
    else snprintf(buf, sizeof buf,
                  ui_text(mode == EDIT_CAT_RENAME  ? T_TD_EDIT_CAT_RENAME_HEAD
                      : mode == EDIT_CAT_NEW_ALL ? T_TD_EDIT_CAT_HEAD_ALL
                      : mode == EDIT_CAT_SEND    ? T_TD_EDIT_CAT_SEND_HEAD : T_TD_EDIT_CAT_HEAD),
                  FF132_CATNAME_MAX);
    ff132_ovl_say(x, y, buf, FF132_COL_TITLE); y += FF132_OVL_LINE_H;
    // Copied out by length, not by its terminator: the message thread may be halfway through an
    // insert, and a scan for the NUL would then run off the end of the buffer.
    wchar_t snap[EDIT_BUF];
    int n = g_editLen;
    if (n < 0 || n > EDIT_BUF - 1) n = 0;
    memcpy(snap, g_editBuf, (size_t)n * sizeof snap[0]);
    snap[n] = 0;
    text[0] = 0;
    if (n) WideCharToMultiByte(CP_UTF8, 0, snap, -1, text, (int)sizeof text, NULL, NULL);
    snprintf(buf, sizeof buf, "   > %s_", text);
    ff132_ovl_say(x, y, buf, FF132_COL_MENU_SEL); y += FF132_OVL_LINE_H;
    y += FF132_OVL_LINE_H * ff132_ovl_say_wrap(x, x, y, help, bad ? FF132_COL_ERR : FF132_COL_TEXT,
                                         FF132_OVL_LINE_H);
    y += FF132_OVL_LINE_H * ff132_ovl_say_wrap(x, x, y, keys, FF132_COL_TEXT, FF132_OVL_LINE_H);
    return y + FF132_OVL_LINE_H / 2;
}

// The copy and move rows differ only in their verb, so their arrows are lined up by measuring each
// row up to the arrow and padding the shorter one there (the Japanese verbs differ in width).
static void lsel_align_arrows(char* a, char* b, size_t n) {
    char* rows[2] = { a, b };
    char* at[2];
    int w[2];
    for (int i = 0; i < 2; i++) {
        char* p = strstr(rows[i], "\xE2\x86\x92");        // the Japanese arrow
        if (!p) p = strstr(rows[i], "->");
        if (!p) return;
        at[i] = p;
        char c = *p;
        *p = 0;
        w[i] = ff132_ovl_text_w(rows[i]);
        *p = c;
    }
    int s = w[0] < w[1] ? 0 : 1, gap = w[1 - s] - w[s];
    char pad[96];
    int left = ff132_ovl_space(pad, sizeof pad, gap);
    size_t pl = strlen(pad);
    static int said;
    if (!said) {
        said = 1;
        ff132_logv(FF132_TAG "save list: copy/move arrows -- gap %d px, left %d px\n", gap, left);
    }
    size_t len = strlen(rows[s]);
    if (!pl || len + pl + 1 > n) return;
    memmove(at[s] + pl, at[s], len - (size_t)(at[s] - rows[s]) + 1);
    memcpy(at[s], pad, pl);
}

// No cancel key in the footer: the game's own list reads that one as "back" and would take itself
// away with the menu still up.
static int lsel_draw(int x, int y) {
    char up[32], down[32], left[32], right[32], k[32], ok[40], pick[96];
    char dest[FF132_CATNAME_U8], destA[FF132_CATNAME_U8 * 2 + 16], buf[448];
    static char row[LSEL_ROWS][384];
    unsigned long col[LSEL_ROWS];
    vk_label(g_menuUpVk, up, sizeof up);       vk_label(g_menuDownVk, down, sizeof down);
    vk_label(g_menuLeftVk, left, sizeof left); vk_label(g_menuRightVk, right, sizeof right);
    vk_label(g_saveTidyVk, k, sizeof k);
    ff132_tidy_ok_phrase(ok, sizeof ok);
    send_dest_label(dest, sizeof dest);
    {
        char dp[FF132_CATNAME_U8 * 2];
        snprintf(dp, sizeof dp, "%s", dest);
        fit_cat(dp, sizeof dp, ui_text(T_TD_SEND_NEWCAT), 0);
        ui_arrows(destA, sizeof destA, dp);
    }
    pick[0] = 0;
    if (g_listPrevVk && g_listNextVk) {
        char pp[32], pn[32];
        vk_label(g_listPrevVk, pp, sizeof pp);
        vk_label(g_listNextVk, pn, sizeof pn);
        snprintf(pick, sizeof pick, ui_text(T_TD_LSEL_PICK), pp, pn);
    }
    int slot = g_lselSlot, anc = g_rangeAnchor, rn = g_rangeShownN, sel = g_lselSel;
    int delOk = range_del_ready(), ranged = (anc >= 0 && rn > 0);
    for (int i = 0; i < LSEL_ROWS; i++) {
        char* r = row[i];
        size_t n = sizeof row[i];
        // Dim = this row cannot run as drawn. The range rows are the exception to "no save under
        // the cursor, nothing to do": they act on the mark, not on the cursor.
        int dim = (slot < 0) && !(ranged && (i == LSEL_COPY || i == LSEL_MOVE));
        switch (i) {
        case LSEL_COPY:
        case LSEL_MOVE:
            if (ranged) snprintf(r, n, ui_text(i == LSEL_COPY ? T_TD_SEND_COPY_N : T_TD_SEND_MOVE_N),
                                 anc + 1, slot + 1, rn, destA);
            else snprintf(r, n, ui_text(i == LSEL_COPY ? T_TD_SEND_COPY : T_TD_SEND_MOVE), destA);
            break;
        case LSEL_MARK:
            if (anc >= 0)       snprintf(r, n, "%s", ui_text(T_TD_LSEL_MARK_CLR));
            else if (slot >= 0) snprintf(r, n, ui_text(T_TD_LSEL_MARK_SET), slot + 1);
            else                snprintf(r, n, "%s", ui_text(T_TD_LSEL_MARK_NEW));
            dim = (anc < 0 && slot < 0);
            break;
        case LSEL_DEL:
            if (g_delArm)   snprintf(r, n, ui_text(T_TD_LSEL_DEL_ARM), g_delArmN, ok);
            else if (delOk) snprintf(r, n, ui_text(T_TD_LSEL_DEL), anc + 1, slot + 1, rn);
            else            snprintf(r, n, "%s", ui_text(T_TD_LSEL_DEL_IDLE));
            dim = !delOk;
            break;
        default:
            snprintf(r, n, "%s%s", ui_text(T_TD_LSEL_NOTE), ui_ellipsis());
            break;
        }
        // A row that cannot run stays dim even under the cursor: the ">" marks where you are.
        col[i] = dim ? FF132_COL_MENU_OFF : (i == sel) ? FF132_COL_MENU_SEL : FF132_COL_MENU;
    }
    lsel_align_arrows(row[LSEL_COPY], row[LSEL_MOVE], sizeof row[0]);
    // The footer is made first, so the box can be sized to what it wraps to.
    char foot[448];
    if (g_lselSel == LSEL_COPY || g_lselSel == LSEL_MOVE)
        snprintf(foot, sizeof foot, ui_text(T_TD_LSEL_SEND_KEYS), up, down, left, right,
                 ui_text(g_lselSel == LSEL_COPY ? T_TD_SEND_DEST_COPY : T_TD_SEND_DEST_MOVE), ok, k);
    else snprintf(foot, sizeof foot, ui_text(T_TD_LSEL_KEYS), up, down, ok, k);
    // Title + the rows + the footer.
    int rows = 1 + LSEL_ROWS + foot_rows(ff132_ovl_wrap_count(x, x, foot));
    tidy_box(y - ff132_ovl_px(4), y + rows * FF132_OVL_LINE_H + ff132_ovl_px(8));
    if (sel >= 0 && sel < LSEL_ROWS) FF132_MENU_BAND(y + (1 + sel) * FF132_OVL_LINE_H, 1);
    if (slot >= 0) snprintf(buf, sizeof buf, ui_text(T_TD_LSEL_HEAD), slot + 1, pick);
    else           snprintf(buf, sizeof buf, ui_text(T_TD_LSEL_HEAD_NEW), pick);
    ff132_ovl_say(x, y, buf, FF132_COL_TITLE); y += FF132_OVL_LINE_H;
    for (int i = 0; i < LSEL_ROWS; i++) {
        snprintf(buf, sizeof buf, "   %s %s", (i == sel) ? ">" : " ", row[i]);
        ff132_ovl_say(x, y, buf, col[i]); y += FF132_OVL_LINE_H;
    }
    y += FF132_OVL_LINE_H * ff132_ovl_say_wrap(x, x, y, foot, FF132_COL_TEXT, FF132_OVL_LINE_H);
    return y + FF132_OVL_LINE_H / 2;
}

// Which blocks are up, and the row height they were laid out at: a height measured for another set,
// or at another text size, is not the one to go by.
static int tidy_blocks_key(void) {
    int k = (g_edit ? 1 : 0) | (g_open ? 2 : 0) | (g_lselOpen ? 4 : 0);
    return k ? k | (FF132_OVL_LINE_H << 3) : 0;
}
static int g_tidyH = 0, g_tidyHKey = 0;     // render thread: the last frame's height, and its key

// The blocks' height as the last frame drew them, for the same blocks at the same size; otherwise the
// counts the boxes are sized from, which is what they come to unless a footer wraps further.
int ff132_tidy_height(void) {
    int key = tidy_blocks_key();
    if (!key) return 0;
    if (key == g_tidyHKey && g_tidyH > 0) return g_tidyH;
    int lh = FF132_OVL_LINE_H, heads = 0, h = 0;
    for (int i = 0; i < TIDY_ROWS; i++) if (kGroup[i] >= 0) heads++;
    if (key & 1) h += (2 + FOOT_ROWS_MIN) * lh + lh / 2;
    if (key & 2) h += (1 + heads + TIDY_ROWS + FOOT_ROWS_MIN) * lh + lh / 2;
    if (key & 4) h += (1 + LSEL_ROWS + FOOT_ROWS_MIN) * lh + lh / 2;
    return h + ff132_ovl_px(8);
}

// The editor first so it sits above whichever menu opened it; the two menus never stand together.
int ff132_tidy_draw(int x, int y) {
    int key = tidy_blocks_key(), top = y;
    g_boxBottom = y;
    if (g_edit)     y = edit_draw(x, y);
    if (g_open)     y = tidy_draw_menu(x, y);
    if (g_lselOpen) y = lsel_draw(x, y);
    g_tidyH = (g_boxBottom > y ? g_boxBottom : y) - top;
    g_tidyHKey = key;
    return y;
}
