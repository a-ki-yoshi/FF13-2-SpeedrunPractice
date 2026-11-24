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

#ifndef FF132_INTERNAL_H
#define FF132_INTERNAL_H

#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ---- names on disk --------------------------------------------------------------------------
// The ini sits beside the exe because you place it by hand; everything the mod WRITES goes into
// MOD_DATA_DIR beside the exe instead of loose next to the game's own files.
#define MOD_DATA_DIR  "ff13-2-mods"
#define FF132_INI_NAME   "ff13-2-srpractice.ini"
#define FF132_INI_TMP_NAME "ff13-2-srpractice.ini.tmp"
#define FF132_LOG_NAME   "ff13-2-srpractice.log"
#define FF132_SLOTS_NAME "ff13-2-srpractice.slots.txt"
#define FF132_STATE_NAME "ff13-2-srpractice.state.txt"
// The category in use by name, and the journal a switch runs behind. Both are the player's to
// look at, so they are spelled the way the menu's words are.
#define FF132_CATNAME_FILE "ff13-2-srpractice.category.txt"
#define FF132_CATJNL_NAME  "ff13-2-srpractice.categories.jnl"
#define FF132_RENJNL_NAME  "ff13-2-srpractice.renum.jnl"
// ff13-2-run.bat reads both by these names. The first marks the category the unmodded game is
// shown (it travels with that category's files); the second says the launcher lent it out.
#define FF132_NOMOD_NAME   "ff13-2-srpractice.nomod"
#define FF132_LENT_NAME    "ff13-2-srpractice.lent"
#define FF132_PREV_NAME  "prev.log"
#define FF132_TAG        "[FF13-2-SRP] "

// ---- hotkeys ----------------------------------------------------------------------------------
// A binding is a virtual-key code with its modifier bits packed above it, so it travels as one
// UINT.
#define HK_CTRL   0x0100u
#define HK_SHIFT  0x0200u
#define HK_ALT    0x0400u
#define HK_MODS   0x0700u
#define hk_key(x)  ((UINT)((x) & 0xFFu))
#define hk_mods(x) ((UINT)((x) & HK_MODS))
#include "ff13_keys.h"
#include "ff13_ini.h"
#define HK_POLL_MS 80

int hk_edge(UINT b, int* was);
int hk_down(UINT b);        // is this binding down right now, modifiers matched exactly
int hk_active(void);        // may a hotkey fire at all (the global_hotkeys test)
void vk_label(UINT b, char* out, size_t n);   // a binding as its on-screen label
UINT hk_mods_now(void);     // the modifier bits held right now
// The [hotkeys] bindings in the ini's table order: the name, and the variable it sets.
int hk_binding_count(void);
const char* hk_binding_name(int i);
UINT* hk_binding_vk(int i);

// ---- the input gate (ff132_input.c) --------------------------------------------------------------
// Armed while a menu of this mod is on screen: the game's own DirectInput reads come back empty, so
// the keys driving the menu do not also drive the game. Every arm expires on its own -- a gate a bug
// left standing would eat the player's input for good -- and whoever holds one re-arms before half
// its life is gone.
#define FF132_DI_GATE_MAX_MS 30000
void ff132_di_wrap(void* di8);            // dllmain: every IDirectInput8 the game is handed
void ff132_di_gate_arm(void);
void ff132_di_gate_lift(void);
int  ff132_di_gate_left_ms(void);         // -1 = not gated, else the ms left
void ff132_di_fake_dik(int dik, DWORD ms);  // the one key the game may still see while gated
#define FF132_DIK_LEFT  0xCB
#define FF132_DIK_RIGHT 0xCD
void menu_gate_release(void);          // dllmain: lift, or wait for the confirm keys to come up

// ---- what the mod says, and in which language (ff132_text.c) ------------------------------------
#include "ff132_text.h"
// One line in the language in force; English whenever ui_ascii_only. T_COUNT gives "".
const char* ui_text(ff132_msg m);
const char* ui_text_en(ff132_msg m);     // the English column always (for the log)
int  ui_english(void);                 // the language in force is English
// 1 = draw ASCII only: English is in force, or the machine has no Japanese font.
int  ui_ascii_only(void);
// `value` between the left/right arrows a value row carries, and the mark after a row that opens
// the editor; both turn ASCII under ui_ascii_only.
void ui_arrows(char* out, size_t n, const char* value);
const char* ui_ellipsis(void);

// ---- logging (MOD_DATA_DIR beside the exe) ----------------------------------------------------
// ff132_loga = always; ff132_log = normal (g_logLevel>=1); ff132_logv = verbose (>=2).
void ff132_log_reset(void);
void ff132_loga(const char* fmt, ...);
void ff132_log(const char* fmt, ...);
void ff132_logv(const char* fmt, ...);
int  data_file(char* out, size_t n, const char* name);

// ---- crash report (ff132_crash.c) ------------------------------------------------------------------
void ff132_crash_install(void);              // worker, once: resolves the log path, takes the seat
void ff132_crash_rearm(void);                // worker, every 2s: the game can take the filter seat back

// ---- config -----------------------------------------------------------------------------------
void config_load(void);
int  cfg_ieq(const char* a, const char* b);
// Worker thread only. Rewrites the one line [section] key in the ini beside the exe (added when
// missing), leaving every other byte as it was. 1 = written; 0 = logged and flashed.
int  config_write(const char* section, const char* key, const char* value);

// ---- hooks (ff132_save.c) ------------------------------------------------------------------------
void save_install_hooks(void);
// The full path of a slot's ff13_2-NN.dat, built by the game's own builder; 0 = the builder's bytes
// did not check out, so no path can be had.
int  save_slot_path(int slot, char* out, size_t n);
// The slot every automatic save is aimed at (the ini's, or the game's last one); -1 = the working
// slot is off (then that number is an ordinary slot) or not resolvable yet. The save list puts it
// first whatever the sort is.
int  save_work_slot(void);
// The working-slot files this mod knows it put there (slot, ftLastWriteTime), oldest first, kept in
// the state file. The adder is for ff132_state_load only.
#define FF132_WI_MAX 64                    // working-slot markers kept, oldest dropped first
int  save_work_ident_snapshot(int* slots, uint64_t* idents, int max);   // any thread; returns the count
void save_work_ident_add(int slot, uint64_t ident);
void save_work_recheck(void);              // after a category switch
int  ff132_cats_parked_idents(int slot, uint64_t* out, int max);   // any thread; -1 = could not tell
// The game or the mod finished writing slot, whose file now has this write time.
void save_work_note_written(int slot, uint64_t ident, const char* why);
// The slot the game is playing out of, which is the one its next automatic save lands in; -1 = the
// pointer walk did not resolve. Nothing may move or delete this slot's file, nor the working one's.
int  save_cur_slot(void);
// How many slots the game itself keeps (40); 0 = it has not written the count yet.
int  save_slot_count(void);
void save_request_picker(void);           // hotkey thread -> game thread handshake (flag only)
void save_toggle_autosave_redirect(void); // worker (settings screen) -> game thread (flag only)
// Worker: 1 = the slot holds a save the working slot's markers do not name, so switching the working
// slot on there is refused. Silent: the settings screen asks on every move.
int  save_work_slot_foreign(int slot);
// GameMain's FSM state id, read by walking the pointers instead of calling the getter, so it is
// safe off the game thread. -1 = a pointer in the walk was not there.
int  ff132_main_state(void);
enum { FF132_MS_TITLE = 4, FF132_MS_ZONE = 5, FF132_MS_ZONE_PAUSE = 12, FF132_MS_LOAD = 15 };
// Zone::getCurrentState()'s value read by walking the pointers instead of calling the getter, so
// it is safe off the game thread (the hotkey thread). -1 = a pointer in the walk was not there.
int  zone_state_peek(void);
// Polls until the bytes at addr match; returns the wait in ms, or -1 on timeout. FF13-2's .text is
// plaintext (no SteamStub), so this normally returns 0 on the first compare; the wait is for a site
// that IS written late, so it is not patched into the wrong bytes.
int  wait_for_bytes(uintptr_t addr, const uint8_t* expect, int len, int timeoutMs);
// How long a site is waited for before the hook is given up on: the budget only covers a site the
// game itself writes during startup.
#define FF132_SITE_WAIT_MS 30000

// ---- overlay (ff132_overlay.c) -------------------------------------------------------------------
void ff132_overlay_install(void);            // worker thread: install the EndScene/Reset hook once
void ff132_overlay_poll(void);               // game thread: retry adopting the game's device until it takes
void ff132_flash(const char* utf8);          // any thread: show a message ~4s (default grey)
void ff132_flash_c(const char* utf8, unsigned long argb);  // any thread: coloured variant (opaque ARGB)
// Drawing, for features that put their own rows on the overlay. Render thread ONLY, and only from
// inside the draw pass: the sprite has already been begun and none of these takes a lock.
int  ff132_ovl_char_w(void);                 // ASCII cell width in pixels; 0 = no font yet
int  ff132_ovl_text_w(const char* utf8);     // drawn width in pixels, measured as ff132_ovl_say draws
int  ff132_ovl_ready(void);                  // 1 = the fonts exist, so a flash is seen
// Pads v with spaces to the widest of cands, so what is drawn after it (a menu row's right arrow)
// stays put as v changes.
void ff132_ovl_fit(char* v, size_t n, const char* const* cands, int cn);
int  ff132_ovl_space(char* out, size_t n, int px);  // blanks about px wide; returns the px left over
void ff132_ovl_say(int x, int y, const char* utf8, unsigned long argb);   // language-aware line
void ff132_ovl_ascii(int x, int y, const char* s, unsigned long argb);    // monospaced ASCII line
int  ff132_ovl_jp_missing(void);             // any thread: the Japanese font was tried and not made
// A blended quad behind a block of rows, and a line that breaks itself across at most three rows.
void ff132_ovl_backdrop(int x0, int y0, int x1, int y1, unsigned long argb);
int  ff132_ovl_say_wrap(int x, int contX, int y, const char* utf8, unsigned long argb, int lineH);
int  ff132_ovl_wrap_count(int x, int contX, const char* utf8);   // the rows ff132_ovl_say_wrap would take
// The rows utf8 takes with `tail` put after its last row, or on a row of its own at contX when it
// does not fit there; *tailX / *tailRow (0-based) say where to draw the tail.
int  ff132_ovl_wrap_tail(int x, int contX, const char* utf8, const char* tail, int* tailX,
                      int* tailRow);
// The text size ([overlay] font_scale). Everything a block lays out in pixels is designed at 100% and
// goes through these, so the rows grow with the text. Render thread, like the drawing helpers.
int  ff132_ovl_line_h(void);                 // one overlay row at the size in force
int  ff132_ovl_px(int px);                   // a distance designed at 100%, at the size in force
int  ff132_ovl_full_w(void);                 // a backdrop's right edge: the whole screen's width
int  ff132_ovl_screen_h(void);               // the back buffer's height; 0 = not known yet
int  ff132_ovl_scale_pct(void);              // the size in force, in percent (auto resolved)
#define FF132_OVL_LINE_H (ff132_ovl_line_h())   // one overlay row
#define FF132_OVL_FULL_W (ff132_ovl_full_w())
// The menus' palette.
#define FF132_COL_MENU     0xFFFFD9E7
#define FF132_COL_MENU_SEL 0xFFFFD168
#define FF132_COL_MENU_OFF 0xFFC9CBE0
#define FF132_COL_MENU_BAND     0x336767FA   // the quad behind the selected row
// The band behind a selected row of `rows` rows at y: drawn after the backdrop, before any text.
#define FF132_MENU_BAND(y, rows) \
    ff132_ovl_backdrop(0, (y) - ff132_ovl_px(2), FF132_OVL_FULL_W, \
                       (y) + (rows) * FF132_OVL_LINE_H + ff132_ovl_px(2), FF132_COL_MENU_BAND)
#define FF132_COL_HEAD     0xFFC4CFCF
#define FF132_COL_TITLE    0xFFFFFFFF
#define FF132_COL_TEXT     0xFFFFFFFF   // menu lines that are not rows: key help, explanations
#define FF132_COL_ON       0xFF7BFFB4   // flash: something switched on
#define FF132_COL_ERR      0xFFFF6A81   // flash and editor line: refused or failed

// ---- direct load (ff132_load.c) ------------------------------------------------------------------
void ff132_load_install(void);               // worker thread: verify the trigger sites, read the sidecar
void ff132_load_toggle(void);                // worker (settings screen): flag + log + flash, no bytes written
extern volatile unsigned char g_directLoad;
extern volatile unsigned char g_skipRecap;  // [load] skip_story_recap; no hotkey
extern volatile unsigned char g_keepNote;   // [save] keep_note; no hotkey
// The sidecar: which save file was written in a world and which at the Historia Crux, so only a
// field save is sent straight into its world. All five run on the GAME thread.
void ff132_load_note_written(int slot, int atHistoria);   // slot's file is final as of now
void ff132_load_note_pending(int slot, int atHistoria);   // a silent save started on slot
void ff132_load_frame(int saveInFlight);                  // once per frame: commit a pending record
void ff132_load_note_copied(int src, int dst);            // the mod copied src's file over dst
int  ff132_load_slot_stat(int slot, uint64_t* mtime, uint32_t* size);  // 0 = no path or no file
void ff132_load_on_loaded(int slot);                      // slot's file was just read: decide the load
// Worker thread: the sidecar on disk is a different file now (a category switch moved it), so drop
// every record and read it again.
void ff132_load_sidecar_reload(void);
// Worker thread: src's record written out as the sidecar line for dstSlot, for grafting into
// another category's sidecar. 0 = src has no record, or its file no longer matches the one it has.
int  ff132_load_rec_row(int srcSlot, int dstSlot, char* out, size_t n);
// Worker thread: slot's file is gone from this category, so drop its record and rewrite the sidecar.
void ff132_load_rec_forget(int slot);
// Worker thread: the renumber renamed the save files, so the records follow them. newSlotOf[s] is
// the number slot s's record moves to; a negative entry leaves that slot's record where it is. The
// table is rebuilt into a fresh array -- a run that moves only some of the saves is not a bijection,
// which is what an in-place cycle walk would need.
void ff132_load_rec_permute(const int* newSlotOf, int n);

// ---- the save header's own text (ff132_load.c) ---------------------------------------------------
// Past the game's own two lines, a save's plaintext header carries up to two lines of this mod's:
// the world label, and a note the player typed. The note's line is the one that starts with this
// prefix -- which is what tells the two apart in a save that has only one of them.
#define FF132_HDR_NOTE_PREFIX "memo:"
// The two mod lines inside a header's text block, as (pointer, length) into that block; a line the
// header does not carry comes back NULL. Neither is NUL-terminated in place.
void ff132_hdr_mod_lines(const char* text, size_t cap, const char** label, size_t* labelLen,
                      const char** note, size_t* noteLen);
// Rewrites those two lines in slot's file, keeping the game's two. Each argument is cp932: NULL
// keeps whatever the file has, "" removes that line. The file's timestamps are put back exactly --
// they are the date the save list shows and the identity the sidecar records. 0 = nothing written,
// and `why` says what stopped it.
int ff132_hdr_write_lines(int slot, const char* label, const char* note, char* why, size_t whyN);

// ---- battle overlay (ff132_battle.c) -------------------------------------------------------------
void ff132_battle_frame(void);               // render thread: the REAL timer's edges, EVERY pass
int  ff132_battle_visible(void);             // render thread: the toggle is on and a fight is up
int  ff132_battle_draw(int x, int y);        // render thread: draws the block, returns the y below it
void ff132_battle_toggle(void);              // any thread: flag + log + flash, nothing else
int  ff132_battle_in_battle(void);
void ff132_battle_hold_after(void);          // game thread: this fight's TIME outlives it (auto-retry)

// ---- game speed (ff132_speed.c) ------------------------------------------------------------------
// [game_speed] multiplier is a list; the hotkey steps through it and then off.
#define FF132_GS_STEPS_MAX 8
extern UINT g_gameSpeedVk;                // [hotkeys] game_speed
// Written by config_load and by ff132_speed_steps_set (worker); read on the game thread.
extern double g_gsSteps[FF132_GS_STEPS_MAX];
extern int g_gsStepN;
void ff132_speed_toggle(void);               // hotkey thread: next step + log + flash + state file
void ff132_speed_pump(void);                 // game thread, once per frame: the only writer of the speed
int  ff132_speed_step_get(void);             // 0 = off, n = the nth factor (the state file's value)
void ff132_speed_step_set(int n);            // after config_load; out of range = off
// Worker thread: a new list (1..FF132_GS_STEPS_MAX factors). The factor that was on stays on if it is
// still in the list, else the speed goes off. Saves the state file.
void ff132_speed_steps_set(const double* v, int n);
int  ff132_speed_visible(void);              // render thread
int  ff132_speed_draw(int x, int y);         // render thread: the status line, returns the y below it
void ff132_gs_fmt(double v, char* out, size_t n);   // a factor as the screens show it: 3, 2.25, 0.5
double ff132_speed_frame_factor(void);     // the engine's frame speed now; 1 if unreadable
const char* ff132_gs_steps_arrow(void);      // the help row's cycle, e.g. "3x→OFF" (ff132_core.c)

// ---- random-encounter suppression (ff132_encount.c) ----------------------------------------------
extern UINT g_enemySuppressVk;            // [hotkeys] enemy_suppress
void ff132_encount_init(void);               // worker: verify the site
void ff132_encount_pump(void);               // game thread (on_frame): keep our mask bit in step with the choice
void ff132_encount_toggle(void);             // worker: flip the choice + log + flash + state file
int  ff132_encount_on(void);                 // the player's choice (the state file's value)
void ff132_encount_set(int v);               // ff132_state_load, before ff132_encount_init
int  ff132_encount_visible(void);            // render thread
int  ff132_encount_draw(int x, int y);       // render thread: the status line, returns the y below it

// ---- kill mode: instant win + one-hit kill (ff132_kill.c) ---------------------------------------
// One key cycles OFF -> INSTANT WIN (+ one-hit kill) -> ONE-HIT KILL -> OFF, as in FF13.
enum { FF132_KILL_OFF = 0, FF132_KILL_ONEHIT = 1, FF132_KILL_INSTANT = 2 };   // the state file's values
extern UINT g_killModeVk;                 // [hotkeys] kill_mode
void ff132_kill_init(void);                  // worker: verify the sites, install the two hooks
void ff132_kill_pump(void);                  // game thread (on_frame): per-battle counters and summary
void ff132_kill_toggle(void);                // worker: next mode + log + flash + state file
int  ff132_kill_mode(void);                  // the player's choice (the state file's value)
void ff132_kill_set(int v);                  // ff132_state_load, before ff132_kill_init
// Game thread: the activate gate kept at least one enemy out of the battle now running, so its
// win is this mod's doing (auto-retry must not retry it, or win -> retry -> instant win loops).
int  ff132_kill_emptied(void);
int  ff132_kill_visible(void);               // render thread
int  ff132_kill_draw(int x, int y);          // render thread: the status line, returns the y below it

// ---- battle auto-retry (ff132_autoretry.c) -------------------------------------------------------
extern UINT g_battleRestartAutoVk;        // [hotkeys] battle_restart_auto
void ff132_autoretry_init(void);             // worker: verify the sites, install the decided-branch cave
void ff132_autoretry_pump(void);             // game thread (on_frame): the retry steps after a won fight
void ff132_autoretry_toggle(void);           // worker: flip auto retry + log + flash + state file
int  ff132_autoretry_on(void);               // the player's choice (the state file's value)
void ff132_autoretry_set(int v);             // ff132_state_load, before ff132_autoretry_init
int  ff132_autoretry_visible(void);          // render thread
int  ff132_autoretry_draw(int x, int y);     // render thread: the status line, returns the y below it

// ---- the state file (ff132_core.c) ---------------------------------------------------------------
// FF132_STATE_NAME, "<key> <int>" lines: the tidy sort and filter, the game-speed step, the
// encounter suppression, the kill mode and the working slot's markers. Any thread (ff132_write_whole
// serialises the writes); a failed write is retried by ff132_state_tick.
void ff132_state_load(void);                 // after config_load, before any hook can read a step
void ff132_state_save(void);
void ff132_state_tick(void);                 // worker loop
int  ff132_write_whole(const char* path, const char* text, size_t len);   // 0 = the old file stays
int  ff132_tidy_state_sort(void);            // the values the file should hold, which is not always
int  ff132_tidy_state_filter(void);          // what is in force (see ff132_tidy_install)
void ff132_tidy_state_set_sort(int v);       // range-checked; out of range = the default
void ff132_tidy_state_set_filter(int v);
// The filter as the file holds it ("world" line): not a table index, which a regenerated table moves.
// A "filter" line (the episode filter's key) is not read, so such a file falls back to every save.
int  ff132_tidy_state_filter_code(void);
void ff132_tidy_state_set_filter_code(int v);

// ---- save tidy (ff132_tidy.c) --------------------------------------------------------------------
// The menu that shapes the game's own save list. Everything but the two draw calls and the sort
// hook runs on the hotkey worker thread.
void ff132_tidy_install(void);               // worker: settings, the first header scan, the sort hook
// 1 = a renumber was cut short and its journal is still on disk: nothing may move or delete a save
// until the next start settles it. ff132_recovery_owns_saves() folds this in, so ask that instead.
int  ff132_tidy_renum_pending(void);
void ff132_tidy_rescan(void);                // worker: re-read every slot's keys and note from disk
void ff132_tidy_open_menu(void);             // worker: open it, or say why it will not open
void ff132_tidy_confirm(void);               // worker: run the selected row
void ff132_tidy_close(void);                 // worker: shut the menu (the gate is the caller's)
void ff132_tidy_move(int dy);                // worker: walk the rows, wrapping
void ff132_tidy_cycle(int dir);              // worker: change the selected row's value
int  ff132_tidy_is_open(void);
void ff132_tidy_ok_phrase(char* out, size_t n);   // the confirm gesture as one phrase, for any menu
void ff132_tidy_screen_tick(void);           // worker: close the menu when the game changes screen
int  ff132_tidy_visible(void);               // render thread: draw it this pass?
int  ff132_tidy_draw(int x, int y);          // render thread: draws the blocks, returns the y below
int  ff132_save_list_up(void);               // any thread: a save or load list of the game's is up
int  ff132_list_gui_recent(void);            // any thread: the list GUI's update ran in the last 500 ms
// The second menu: it acts on the save under the GAME's cursor while the game's own save/load list
// is on screen, and follows that cursor as the game moves it. Worker thread, all of it.
int  ff132_tidy_list_open(void);             // 0 = no list on screen, so the key was not this menu's
void ff132_tidy_list_close(void);
int  ff132_tidy_list_is_open(void);
void ff132_tidy_list_move(int dy);
void ff132_tidy_list_go(void);               // run the selected row
void ff132_tidy_list_step(int dir);          // hand the game ONE left/right through the input gate
void ff132_tidy_list_cycle(int dir);         // change the selected row's value (the send destination)
void ff132_tidy_list_follow(void);           // every tick: take the slot the game's cursor is on now
// The note editor. The window procedure (the game's message thread) only edits the buffer; the
// file work is ff132_tidy_edit_tick on the worker.
int  ff132_tidy_edit_is_open(void);
void ff132_tidy_edit_finish(int commit);     // worker: 1 = write it, 0 = drop it
void ff132_tidy_edit_tick(void);             // worker: the file work for a finished edit
// Game thread: the game rebuilt this slot's header (so its note is gone), or the mod copied one
// save's file over another (so the note travelled with it).
void ff132_tidy_note_dropped(int slot);
int  ff132_tidy_note_peek(int slot, char* out, size_t n);   // cp932; 0 when the slot has none
// The first save after NEW GAME is made in Valhalla before any history is set, so its history id
// field is still all zero. Such a save is filed, and shown, as this world.
#define FF132_NO_ID_WORLD "h_va_NA0000_a"
int  ff132_tidy_slot_no_id(int slot);        // 1 = the save's id field read empty at the last list open
// A world as the list and the filter name it, UTF-8: "<area> <age>", or the area alone for that world.
void ff132_world_display(int row, int english, char* out, size_t n);
void ff132_tidy_note_copied(int src, int dst);
// Where the blocks start: clear of the flash line at the top left, and of the save list's own
// thumbnails (y ~250 and below at 1280x720), which is what the list menu is drawn over. At 100%: the
// overlay scales it with the text, and lifts the blocks when a short screen would cut them off.
#define FF132_TIDY_TOP 160
// Render thread: how tall ff132_tidy_draw's blocks come out (0 = none is up).
int  ff132_tidy_height(void);

// ---- the hotkeys-and-settings screen (ff132_setup.c) ----------------------------------------------
// Everything but the two draw calls runs on the hotkey worker; the caller lifts the input gate
// (menu_gate_release) after a close, as with the tidy menu.
void ff132_setup_open(void);                 // arms the input gate
void ff132_setup_close(void);
int  ff132_setup_is_open(void);
void ff132_setup_move(int dy);               // up/down, wrapping, over the heading rows
void ff132_setup_cycle(int dir);             // left/right: the selected row's value or sub-cursor
void ff132_setup_ok(void);                   // confirm: start a key capture, or toggle
int  ff132_setup_capturing(void);            // a key row is waiting for its new key
void ff132_setup_capture_tick(void);         // every tick while capturing
// 1 while the key that ended the last capture is still down: the caller's gesture state still
// tracks the OLD bindings, so a key just made a menu key would otherwise fire as one.
int  ff132_setup_settling(void);
int  ff132_setup_visible(void);              // render thread: draw it this pass? (the lapse guard)
int  ff132_setup_draw(int x, int y);         // render thread: draws the block, returns the y below

// ---- save categories (ff132_cats.c) --------------------------------------------------------------
// The whole set of saves, parked in and out of {save folder}\categories\{name}\. All of it runs on
// the worker; the name getters are called from the render thread, and ff132_cats_parked_idents
// from whichever thread writes into the working slot.
#define FF132_CATNAME_MAX   24       // characters; the name is used as a directory name
#define FF132_CATNAME_BYTES 64       // ...at cp932's worst, and the NUL
#define FF132_CATNAME_U8    (FF132_CATNAME_BYTES * 3)
#define FF132_CAT_MAX       16
void ff132_cats_install(void);       // worker: the name file, then any interrupted switch
void ff132_cats_refresh(void);       // worker: re-read which categories are parked
int  ff132_cats_count(void);         // how many are parked (the one in use is never among them)
int  ff132_cats_name(int i, char* utf8, size_t n);   // parked category i, with its save count
int  ff132_cats_name_q(int i, char* utf8, size_t n); // the same, quoted in English (the delete row)
void ff132_cats_current(char* utf8, size_t n);       // the one in use
int  ff132_cats_pick(char* utf8, size_t n);          // the delete target; 0 = none to pick
int  ff132_cats_cycle(int dir);                      // walk that pick; -1 = nothing is parked
int  ff132_cats_switch_pick(char* utf8, size_t n);   // the switch target; 0 = none picked
void ff132_cats_switch_cycle(int dir);               // worker: walk it, "none" included
// Worker thread: parked category i as a send needs it -- the folder its saves live in and the
// cp932 name the log calls it by. Either buffer may be NULL. 0 = there is no such category.
int  ff132_cats_dest(int i, char* dir, size_t dirN, char* name932, size_t nameN);
// To the switch row's pick, which is cleared either way. 1 = switched. outN/inN take the file
// counts moved each way, and may be NULL.
int  ff132_cat_switch(int* outN, int* inN);
int  ff132_cat_new(const char* name932, int all);    // seed (newest save, or every file) then switch
// An empty category folder and its path: no seed and no switch, for a send into a new category.
int  ff132_cat_make(const char* name932, char* dir, size_t n);
// The first save number with no file in a category folder (-1 = full), and how many it has free.
int  ff132_cat_free_slot(const char* dir);
int  ff132_cat_free_count(const char* dir);
int  ff132_cat_rename(const char* name932);          // the one in use; the name file and nothing else
// 0 = the first ask (the row now reads as the confirm), 1 = deleted, -1 = refused or failed.
int  ff132_cat_delete_hold(void);
void ff132_cat_del_poll(int rowActive);              // worker, every tick: stand that confirm down
int  ff132_cat_del_armed(void);
// 0 = usable, 1 = the name itself is refused, 2 = a category of that name already exists.
int  ff132_cat_check_name(const wchar_t* w, int rename, char* out932, size_t n);
int  ff132_cat_name_char_ok(wchar_t c);              // may this character go in a category's name?
// One save copied into another category's folder, its sidecar record grafted in behind it. recOk
// may be NULL; it comes back 0 only when there WAS a record and the destination's sidecar refused
// it -- which is what stops a move from deleting the source.
int  ff132_send_one(int srcSlot, const char* dstDir, int dstSlot, int* recOk);
// Files to the Windows Recycle Bin. `list` is SHFileOperation's own form: absolute paths, each
// NUL-terminated, the last one twice. `what` names them in the log. 0 = the shell refused.
int  ff132_shell_recycle(const wchar_t* list, const char* what);
// 1 = a switch was cut short and its journal is still on disk: nothing may move or delete a save.
int  ff132_recovery_owns_saves(void);
// Worker thread, before anything reads the save folder: ff13_2-NN.dat.parked in its root back to
// .dat (parked by the launcher for the unmodded game). An existing .dat is never overwritten.
void ff132_saves_unpark(void);
// Worker thread, before ff132_saves_unpark: when the launcher lent a category to the unmodded game,
// every save in the save folder's root goes back into that category's folder. 0 = not all of them
// did (or it could not tell where): the lent mark stays, and ff132_saves_unpark must not run.
int  ff132_nomod_return(void);
// Any thread, while the mod runs: a lend ff132_nomod_return could not finish is still standing.
int  ff132_nomod_stuck(void);
// The category the unmodded game is shown: -2 = none, -1 = the one in use, i = parked category i.
// The getter and the name are safe from the render thread; the setter runs on the worker and
// returns 0 when refused (Steam Cloud on, an unfinished switch, a file that would not write).
int  ff132_nomod_at(void);
int  ff132_nomod_set(int at);
void ff132_nomod_label(int at, char* utf8, size_t n);
// Worker thread: the save number the last refused switch found past what the game can list, or -1
// when the last switch was not refused for that.
int  ff132_cat_switch_high(void);
// Worker thread: every ff13_2-NN.dat in the save folder and in every folder under categories\.
// Returns the highest NN (-1 = none, -2 = the save folder could not be resolved); *above (may be
// NULL) counts those numbered `from` or higher; `where` (may be NULL) takes the highest one's
// folder.
int  ff132_cats_scan_numbers(int from, int* above, char* where, size_t whereN);
// 1 = this game's saves are synced by Steam Cloud, 0 = they are not, -1 = Steam would not say.
int  steam_cloud_on(void);

// ---- the number of save slots (ff132_slots.c) ----------------------------------------------------
#define FF132_SLOTS_GAME      40     // what the game itself offers: 00..39
// The most [save] slot_count may ask for while the list GUI is unpatched: it builds 50 rows, and
// the save list spends one of them on "new save".
#define FF132_SLOTS_MAX_NOGUI 49
// With the list GUI widened to 100 rows (ff132_listgui.c): 2-digit file names end at 99.
#define FF132_SLOTS_MAX       99
void ff132_slots_tick(void);         // worker, every tick: applies an owed change once no list is up
void ff132_slots_mark_dirty(void);   // worker: the setting changed, or a switch moved save files
// Any thread: the count the game wrote itself, before any write of this mod's; 0 = not yet written.
int  ff132_slots_game(void);
int  ff132_slots_pending(void);      // any thread: a change may still be owed
// Any thread: the last slot of the save slot limit as the game numbers it (the setting, held to the
// count the game has now); 0 = not known before the first evaluation.
int  ff132_slots_last(void);
// Worker thread (they read the disk or Steam's config):
// 0 = the count can be changed, 1 = Steam Cloud is on, 2 = the game's own count is not 40.
int  ff132_slots_unavailable(void);
int  ff132_slots_reach(void);        // the most the count can be made right now
// The highest save number on disk plus one, never below the game's own count; -1 = unreadable.
int  ff132_slots_needed(void);
int  ff132_slots_saves_from(int from);   // saves on disk numbered `from` or higher; -1 = unreadable

// ---- the save list GUI's row count (ff132_listgui.c) ---------------------------------------------
// Worker thread. n = rows, a multiple of 10 in 50..127. Writes only while no list exists, all of
// its sites or none; 1 = the GUI builds n rows (or more) from the next list on. Never reverted.
int  ff132_listgui_apply(int n);
int  ff132_listgui_can(int n);       // 1 = already n rows or more, or every site checks out for n
int  ff132_listgui_rows(void);       // rows the GUI builds now: 50 = the game's own

// ---- shared state -----------------------------------------------------------------------------
extern uintptr_t g_base;              // ffxiii2img.exe base (GetModuleHandle(NULL))
extern int g_logLevel;                // 0 quiet / 1 normal / 2 verbose
extern volatile int g_hkGlobal;       // [hotkeys] global_hotkeys
extern UINT g_helpVk;                 // [hotkeys] help
extern UINT g_slotSelectVk;           // [hotkeys] slot_select
extern UINT g_battleOverlayVk;        // [hotkeys] battle_overlay
extern UINT g_saveTidyVk;             // [hotkeys] save_tidy
extern UINT g_menuOkVk;               // [hotkeys] menu_ok
extern UINT g_menuUpVk, g_menuDownVk, g_menuLeftVk, g_menuRightVk;
extern UINT g_listPrevVk, g_listNextVk;
extern UINT g_cancelVk;
extern int g_autosaveSlot;            // [save] autosave_slot: -1 = the game's own last slot
extern volatile int g_slotCount;      // [save] slot_count: FF132_SLOTS_GAME = the game's own
// Single bytes so reads and writes stay naturally atomic across the hotkey thread and whatever
// game thread the save hook reads them from.
extern volatile unsigned char g_autosaveRedirect;
extern volatile unsigned char g_battleOverlay;
// [overlay] font_scale: the size of everything the overlay draws, in percent of the original
// (100 = 20px text, 24px rows), or FF132_SCALE_AUTO. Written by config_load and the settings screen
// (worker), read by the render thread; a plain int, so a read never sees half a write.
#define FF132_SCALE_AUTO 0
#define FF132_SCALE_MIN  50
#define FF132_SCALE_MAX  400
extern volatile int g_ovlScale;

// ---- reading the game's memory -----------------------------------------------------------------
// A pointer that does not check out is a read skipped, never a fault.
static inline int rd_ok(const void* p, size_t n) {
    return p != NULL && !IsBadReadPtr(p, (UINT_PTR)n);
}
static inline uint32_t rd32(uintptr_t p) {
    return *(const volatile uint32_t*)p;
}
// The exe's bytes at rva are sig: the check every patch and every read of a code site waits on.
static inline int sig_at(uintptr_t rva, const uint8_t* sig, size_t n) {
    const void* at = (const void*)(g_base + rva);
    return rd_ok(at, n) && memcmp(at, sig, n) == 0;
}
static inline uint64_t ft_u64(FILETIME t) {
    return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
}
static inline int cp932_lead(unsigned char c) {
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}
// The battle actor a handle names, readable for `need` bytes from its start; 0 = none (ff132_battle.c).
uintptr_t ff132_chara_at(uintptr_t bm, uint32_t handle, size_t need);

#endif // FF132_INTERNAL_H
