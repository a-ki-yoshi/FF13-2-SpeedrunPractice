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

// The overlay: a D3D9 EndScene vtable hook draws the flash line and every feature's rows on the
// back-buffer pass, and exports the text helpers those rows are drawn with.
#include "ff132_internal.h"
#include "ff132_addrs.h"

// ---- the flash: a transient status line -------------------------------------------------------
// Written from any thread (hotkey or game), read on the render thread. A rare torn message across
// that boundary is accepted.
static char           g_flash[288];      // UTF-8. Sized so a Japanese line (about 3x the bytes of
                                          // the English one) is not cut mid-character.
static volatile DWORD g_flashUntil = 0;  // g_flash visible until this GetTickCount
#define FF132_OVL_FLASH  0xFFC4CFCF          // default status colour: grey (opaque ARGB)
static volatile DWORD g_flashCol = FF132_OVL_FLASH;

void ff132_flash_c(const char* utf8, unsigned long argb) {
    strncpy(g_flash, utf8, sizeof g_flash - 1);
    g_flash[sizeof g_flash - 1] = 0;
    g_flashCol   = (DWORD)argb;
    g_flashUntil = GetTickCount() + 4000;
}
void ff132_flash(const char* utf8) { ff132_flash_c(utf8, FF132_OVL_FLASH); }

// ---- fonts (at 100%, EXACTLY the FF13 mod's faces and sizes) --------------------------------------
#define OVL_FONT_FACE    "Courier New"
#define OVL_FONT_FACE_JP "Meiryo UI"
// The Japanese face runs two points smaller: at the same requested height Meiryo's ideographs fill
// more of the cell than Courier's letters and read as the larger type on a mixed line.
#define OVL_FONT_H     20
#define OVL_FONT_H_JP  18
#define OVL_LINE_H     24          // one row; FF132_OVL_LINE_H is this at the size in force
#define OVL_MARGIN     14          // the gap kept to the screen's left, top and right edges
// Stands in only until the two faces have been asked for their metrics (see g_jpYoff).
#define OVL_JP_YOFF    ((OVL_FONT_H - OVL_FONT_H_JP) / 2)
// [overlay] font_scale multiplies every size above and every distance a block is laid out with
// (ff132_ovl_px). Auto keeps the overlay the share of the screen it has at 720 lines -- the height
// its layout was made at -- and never goes below 100%.
#define OVL_AUTO_REF_H 720

// ============================================================================
// D3D9 overlay core.
// ----------------------------------------------------------------------------
// Vtable hook, no trampoline: EndScene and Reset are patched in an IDirect3DDevice9 vtable.
// A throwaway device donates one vtable, but on FF13-2 that is NOT the vtable the game's device
// uses (measured: the patched EndScene never fired), so the game's own device is found and ITS
// vtable is patched too -- see overlay_adopt_game_device.
// D3D9/D3DX are resolved at runtime (LoadLibrary), so no link flags are added.
//
// COM types are opaque here: interfaces are void* and their methods are reached through the vtable
// (`void** vt = *(void***)obj; vt[slot]`) with __stdcall signatures. Only the slot numbers matter:
//   IDirect3D9:        CreateDevice = 16
//   IDirect3DDevice9:  Reset = 16, EndScene = 42, Release = 2 (IUnknown)
//   ID3DXFont:         DrawTextA = 14, OnLostDevice = 16, OnResetDevice = 17
//   ID3DXSprite:       Begin = 8, End = 11, OnLostDevice = 12, OnResetDevice = 13

// Minimal, hand-rolled D3DPRESENT_PARAMETERS (all fields are 4 bytes on x86; the layout/order must
// match the real struct because CreateDevice reads it).
typedef struct {
    UINT BackBufferWidth, BackBufferHeight;
    UINT BackBufferFormat;            // D3DFORMAT (enum -> int)
    UINT BackBufferCount;
    UINT MultiSampleType;             // D3DMULTISAMPLE_TYPE
    DWORD MultiSampleQuality;
    UINT SwapEffect;                  // D3DSWAPEFFECT
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    UINT AutoDepthStencilFormat;      // D3DFORMAT
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT PresentationInterval;
} FF132_D3DPP;

typedef void* (__stdcall *Direct3DCreate9_t)(UINT SDKVersion);
typedef HRESULT (__stdcall *CreateDevice_t)(void*, UINT Adapter, DWORD DevType, HWND hFocus,
                                            DWORD BehaviorFlags, FF132_D3DPP*, void** ppDevice);
typedef HRESULT (__stdcall *EndScene_t)(void*);
typedef HRESULT (__stdcall *Reset_t)(void*, FF132_D3DPP*);
typedef ULONG   (__stdcall *Release_t)(void*);
typedef HRESULT (__stdcall *FontOp_t)(void*);   // OnLostDevice / OnResetDevice
typedef HDC     (__stdcall *FontGetDC_t)(void*);
typedef HRESULT (__stdcall *PreloadChars_t)(void*, UINT first, UINT last);
typedef BOOL    (__stdcall *GetTextMetricsA_t)(void* font, TEXTMETRICA* tm);
typedef INT     (__stdcall *DrawTextA_t)(void* font, void* sprite, LPCSTR str, INT count,
                                         RECT* rect, DWORD fmt, DWORD color);
typedef INT     (__stdcall *DrawTextW_t)(void* font, void* sprite, const unsigned short* str,
                                         INT count, RECT* rect, DWORD fmt, DWORD color);
typedef HRESULT (WINAPI *D3DXCreateFontA_t)(void* dev, INT Height, UINT Width, UINT Weight,
                                            UINT MipLevels, BOOL Italic, DWORD CharSet,
                                            DWORD OutputPrecision, DWORD Quality,
                                            DWORD PitchAndFamily, LPCSTR pFaceName, void** ppFont);
typedef HRESULT (WINAPI *D3DXCreateSprite_t)(void* dev, void** ppSprite);
typedef HRESULT (__stdcall *SpriteBegin_t)(void*, DWORD flags);
typedef HRESULT (__stdcall *GetRenderTarget_t)(void*, DWORD idx, void** ppSurface);
typedef HRESULT (__stdcall *GetBackBuffer_t)(void*, UINT swap, UINT bb, DWORD type, void** ppSurface);
typedef HRESULT (__stdcall *CreateStateBlock_t)(void*, DWORD type, void** ppSB);
typedef HRESULT (__stdcall *SetRenderState_t)(void*, DWORD state, DWORD value);
typedef HRESULT (__stdcall *SetTexture_t)(void*, DWORD stage, void* tex);
typedef HRESULT (__stdcall *SetTexStage_t)(void*, DWORD stage, DWORD type, DWORD value);
typedef HRESULT (__stdcall *SetFVF_t)(void*, DWORD fvf);
typedef HRESULT (__stdcall *SetShader_t)(void*, void* shader);
typedef HRESULT (__stdcall *DrawPrimUP_t)(void*, DWORD type, UINT primCount, const void* data,
                                          UINT stride);
typedef HRESULT (__stdcall *SBApply_t)(void*);

#define D3DDEV_VT_RESET           16
#define D3DDEV_VT_GETBACKBUFFER   18
#define D3DDEV_VT_GETRENDERTARGET 38
#define D3DDEV_VT_BEGINSCENE      41
#define D3DDEV_VT_ENDSCENE        42
#define D3DDEV_VT_SETRENDERSTATE  57
#define D3DDEV_VT_CREATESTATEBLOCK 59
#define D3DDEV_VT_SETTEXTURE      65
#define D3DDEV_VT_SETTEXSTAGE     67
#define D3DDEV_VT_DRAWPRIMUP      83
#define D3DDEV_VT_SETFVF          89
#define D3DDEV_VT_SETVSHADER      92
#define D3DDEV_VT_SETPSHADER     107
#define SB_VT_CAPTURE              4
#define SB_VT_APPLY                5
#define IUNK_VT_RELEASE      2
#define FONT_VT_GETTEXTMETRICSA 6
#define FONT_VT_GETDC       8
#define FONT_VT_PRELOADCHARS 10
#define FONT_VT_DRAWTEXTA  14
#define FONT_VT_DRAWTEXTW  15
#define FONT_VT_ONLOST     16
#define FONT_VT_ONRESET    17
#define SPRITE_VT_BEGIN     8
#define SPRITE_VT_END      11
#define SPRITE_VT_ONLOST   12
#define SPRITE_VT_ONRESET  13
#define D3DXSPRITE_ALPHABLEND_F 0x10
#define DT_NOCLIP_FLAG   0x100
#define DT_CALCRECT_FLAG 0x400

// Two vtables can carry the hooks: the throwaway device's (d3d9's own) and the game's wrapper, once
// adopted. Each hook chains to the original of the vtable the call came through: under an
// implementation where every device shares one vtable (DXVK), the wrapper's inner device arrives
// through the throwaway's, and handing it the wrapper's EndScene would crash.
static void* g_origEndScene = NULL;   // the adopted vtable's originals
static void* g_origReset    = NULL;
static void* g_dummyEndScene = NULL;  // the throwaway vtable's originals
static void* g_dummyReset    = NULL;
static void** volatile g_gameVt = NULL;
static void** g_dummyVt     = NULL;
static volatile int g_dummyHooked = 0;  // the throwaway's EndScene patch took
// The back buffer's identity. The pointer is only ever COMPARED, never dereferenced: the device
// owns the surface, and it stays the same surface until a Reset recreates it -- which is why
// hkReset drops the cache.
static void* g_bbCache    = NULL;
static void* g_bbCacheDev = NULL;     // the device g_bbCache belongs to (refetched if it changes)
static int   g_bbWidth    = 0;
static int   g_bbHeight   = 0;        // what auto sizes the text from, and what a menu must fit in
static void* g_font         = NULL;   // ID3DXFont* (Latin; lazily created on the real device)
static void* g_fontJp       = NULL;   // ID3DXFont* (Japanese)
static volatile LONG g_fontJpMissing = 0;   // set once the Japanese font failed to be made
static void* g_sprite       = NULL;   // ID3DXSprite* every DrawText draws through
static void* g_spriteActive = NULL;   // = g_sprite only between Begin/End; what the helpers pass
// How far a Japanese run drops so both faces sit on ONE baseline: the two faces' ascent difference,
// asked of the fonts themselves. Set once at font creation; the fixed nudge stands in until then.
static int g_jpYoff = OVL_JP_YOFF;
static volatile LONG g_fontTried = 0;   // create the font at most once (until a device reset)
// The text size the faces in force were made at, and the sizes that follow from it. Render thread
// only: every reader is a drawing helper, and the faces are only ever swapped between two passes.
static int g_scalePct  = 100;
static int g_fontPx    = OVL_FONT_H;
static int g_fontPxJp  = OVL_FONT_H_JP;
static int g_lineH     = OVL_LINE_H;
static int g_scaleFail = 0;             // a size whose faces could not be made: not asked for again
static D3DXCreateFontA_t g_pCreateFont = NULL;   // kept from the first make, for every remake
// A distance designed at 100%, at the size in force. Rounded, so 100% is exactly the old layout.
static int ovl_scaled(int px, int pct) { return (px * pct + 50) / 100; }
static int ovl_margin(void) { return ovl_scaled(OVL_MARGIN, g_scalePct); }
// Back-buffer-pass detection state. g_bbFallback latches once it is clear this game never binds the
// back buffer on an EndScene pass; the rest are one-shot log guards.
static int g_bbSeen = 0, g_bbMisses = 0, g_bbFallback = 0;
static int g_bbFirstLogged = 0, g_bbNoneLogged = 0;
static int g_devBadLogged = 0;   // one-shot: the device global held something unusable

// The black behind every line: 0 = a single shadow (2 draws per line), 1 = four diagonals (5),
// 2 = all eight neighbours (9). Shipped at 0.
#define OVL_OUTLINE 0
// How far the black sits from the text, in pixels at 100% -- a separate knob from the shape. It grows
// with the text (g_shadowPx), or a large size would wear a hairline of a shadow.
#define OVL_OFFSET_PX 1
static int g_shadowPx = OVL_OFFSET_PX;
// Corners first, then edges, so the 4-offset setting is simply the first four.
static const int kOvlOutline[8][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1},
                                       { 0,-1}, {0, 1}, {-1,0}, {1,0} };
#define OVL_OUTLINE_N (OVL_OUTLINE == 2 ? 8 : 4)

// Draw one ASCII line, outlined in black for readability. All calls guarded.
static void overlay_line(void* font, int x, int y, const char* text, DWORD color) {
    if (!font || !text) return;
    void** vt = *(void***)font;
    if (!vt) return;
    DrawTextA_t p = (DrawTextA_t)vt[FONT_VT_DRAWTEXTA];
    if (!p) return;
    RECT r;
#if OVL_OUTLINE == 0
    r.left = x + g_shadowPx; r.top = y + g_shadowPx;
    r.right = r.left + 1200; r.bottom = r.top + 60;
    p(font, g_spriteActive, text, -1, &r, DT_NOCLIP_FLAG, 0xFF000000);   // shadow
#else
    for (int i = 0; i < OVL_OUTLINE_N; i++) {
        r.left = x + kOvlOutline[i][0] * g_shadowPx;
        r.top  = y + kOvlOutline[i][1] * g_shadowPx;
        r.right = r.left + 1200; r.bottom = r.top + 60;
        p(font, g_spriteActive, text, -1, &r, DT_NOCLIP_FLAG, 0xFF000000);
    }
#endif
    r.left = x; r.top = y; r.right = x + 1200; r.bottom = y + 60;
    p(font, g_spriteActive, text, -1, &r, DT_NOCLIP_FLAG, color);        // body, last so it sits on top
}

// Is a typeface actually installed? Needed because the GDI font mapper substitutes silently instead
// of failing, which makes a missing font look exactly like a working one.
static int CALLBACK font_seen_cb(const LOGFONTA* lf, const TEXTMETRICA* tm, DWORD type, LPARAM p) {
    (void)lf; (void)tm; (void)type;
    *(int*)p = 1;
    return 0;                                    // one hit is enough; stop enumerating
}
static int font_installed(const char* face) {
    HDC dc = GetDC(NULL);
    if (!dc) return 0;
    LOGFONTA lf;
    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    strncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    int found = 0;
    EnumFontFamiliesExA(dc, &lf, font_seen_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

// The ASCII face is monospace, so one measurement gives every column position. Measured through the
// font's own DT_CALCRECT rather than assumed, because the advance depends on the height the font was
// created at. Cached; reset whenever the faces are made again at another size.
static int g_ovlCw = 0;
static int overlay_char_w(void* font) {
    if (g_ovlCw) return g_ovlCw;
    if (!font) return 0;
    void** vt = *(void***)font;
    if (!vt) return 0;
    DrawTextA_t p = (DrawTextA_t)vt[FONT_VT_DRAWTEXTA];
    if (!p) return 0;
    RECT r; r.left = 0; r.top = 0; r.right = 0; r.bottom = 0;
    p(font, NULL, "MMMMMMMMMMMMMMMMMMMM", 20, &r, DT_CALCRECT_FLAG, 0);
    int w = (int)(r.right - r.left);
    // 3/5 of the height = a sane guess if the measure fails (12 at 100%).
    g_ovlCw = (w > 0) ? (w + 19) / 20 : (g_fontPx * 3 + 2) / 5;
    return g_ovlCw;
}

static int font_ascent(void* font) {
    if (!font) return 0;
    void** vt = *(void***)font;
    if (!vt || !vt[FONT_VT_GETTEXTMETRICSA]) return 0;
    TEXTMETRICA tm;
    memset(&tm, 0, sizeof tm);
    if (!((GetTextMetricsA_t)vt[FONT_VT_GETTEXTMETRICSA])(font, &tm)) return 0;
    return (int)tm.tmAscent;
}

// The Japanese text path: UTF-16 through DrawTextW -- the ANSI path would render only on a machine
// whose codepage happens to be 932. A mixed line is split into runs; runs under 0x80 use the ASCII
// face so they stay monospaced, which the column maths below assumes of them. An ASCII run's advance
// is chars * overlay_char_w, which also sidesteps DT_CALCRECT's habit of dropping trailing spaces; a
// Japanese run is measured with DT_CALCRECT. All shadows go down before any body, so a run's outline
// never clips the glyph drawn before it.
#define OVL_RUNS_MAX 24
// The English arrows (U+25C4/U+25BA) are Courier New's own glyphs, drawn in the ASCII face: a
// non-Japanese Windows often lacks Meiryo UI, and its stand-in may lack the arrows. The settings
// screen's scroll marks (U+25B2/U+25BC) are drawn there in either language for the same reason.
static int latin_face(unsigned short c) {
    return c < 0x80 || c == 0x25C4 || c == 0x25BA || c == 0x25B2 || c == 0x25BC;
}
#define JPW_CACHE_N 1024   // per character as well as per run: a menu's footer wraps every frame
static struct { unsigned h; int len; int w; } g_jpw[JPW_CACHE_N];
static int jp_run_width(void* font, const unsigned short* s, int len) {
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ s[i]) * 16777619u;
    unsigned slot = h % JPW_CACHE_N;
    if (g_jpw[slot].h == h && g_jpw[slot].len == len && g_jpw[slot].w > 0) return g_jpw[slot].w;
    void** vt = *(void***)font;
    DrawTextW_t p = vt ? (DrawTextW_t)vt[FONT_VT_DRAWTEXTW] : NULL;
    if (!p) return 0;
    RECT m; m.left = 0; m.top = 0; m.right = 0; m.bottom = 0;
    p(font, NULL, s, len, &m, DT_CALCRECT_FLAG, 0);
    int w = (int)(m.right - m.left);
    if (w > 0) { g_jpw[slot].h = h; g_jpw[slot].len = len; g_jpw[slot].w = w; }
    return w;
}

// Must split and measure exactly as overlay_line_w draws, or anything placed at x + this drifts.
static int overlay_width_w(const unsigned short* text) {
    if (!text || !text[0]) return 0;
    int cw = g_font ? overlay_char_w(g_font) : 0;
    int w = 0, i = 0;
    while (text[i]) {
        int start = i, ascii = latin_face(text[i]);
        while (text[i] && latin_face(text[i]) == ascii) i++;
        if (ascii && g_font && cw) { w += cw * (i - start); continue; }
        if (!g_fontJp) continue;
        w += jp_run_width(g_fontJp, text + start, i - start);
    }
    return w;
}
static int overlay_text_w(const char* utf8) {
    if (!utf8 || !utf8[0]) return 0;
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p && *p < 0x80) p++;
    if (!*p) return (g_font ? overlay_char_w(g_font) : 0) * (int)strlen(utf8);
    // As long as overlay_say_wrap's line: a small text size on a wide screen fits more characters to
    // a row than a 320-unit buffer would take, and a failed conversion measures 0.
    unsigned short w[512];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, (wchar_t*)w, (int)(sizeof w / sizeof w[0])) <= 0)
        return 0;
    return overlay_width_w(w);
}

static void overlay_line_w(int x, int y, const unsigned short* text, DWORD color) {
    if (!text || !text[0]) return;
    struct { void* font; const unsigned short* s; int len; int x; } runs[OVL_RUNS_MAX];
    int nr = 0, cx = x, i = 0;
    int cw = g_font ? overlay_char_w(g_font) : 0;
    while (text[i] && nr < OVL_RUNS_MAX) {
        int start = i, ascii = latin_face(text[i]);
        if (nr == OVL_RUNS_MAX - 1) {          // out of slots: the last run takes all that is left,
            while (text[i]) i++;               // in the Japanese face, which renders either kind
            ascii = 0;
        } else {
            while (text[i] && latin_face(text[i]) == ascii) i++;
        }
        void* f = (ascii && g_font && cw) ? g_font : g_fontJp;
        if (!f) continue;                      // no Japanese face: that run is left out
        runs[nr].font = f; runs[nr].s = text + start; runs[nr].len = i - start; runs[nr].x = cx;
        if (f == g_font) {
            cx += cw * runs[nr].len;
        } else if (text[i]) {                  // the last run's width has no taker: skip the measure
            cx += jp_run_width(f, runs[nr].s, runs[nr].len);
        }
        nr++;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < nr; k++) {
            void** vt = *(void***)runs[k].font;
            if (!vt) return;
            DrawTextW_t p = (DrawTextW_t)vt[FONT_VT_DRAWTEXTW];
            if (!p) return;
            int yy = (runs[k].font == g_fontJp) ? y + g_jpYoff : y;   // onto the ASCII baseline
            RECT r;
            if (pass == 0) {
#if OVL_OUTLINE == 0
                r.left = runs[k].x + g_shadowPx; r.top = yy + g_shadowPx;
                r.right = r.left + 1200; r.bottom = r.top + 60;
                p(runs[k].font, g_spriteActive, runs[k].s, runs[k].len, &r, DT_NOCLIP_FLAG, 0xFF000000);
#else
                for (int o = 0; o < OVL_OUTLINE_N; o++) {
                    r.left = runs[k].x + kOvlOutline[o][0] * g_shadowPx;
                    r.top  = yy + kOvlOutline[o][1] * g_shadowPx;
                    r.right = r.left + 1200; r.bottom = r.top + 60;
                    p(runs[k].font, g_spriteActive, runs[k].s, runs[k].len, &r, DT_NOCLIP_FLAG, 0xFF000000);
                }
#endif
            } else {
                r.left = runs[k].x; r.top = yy; r.right = r.left + 1200; r.bottom = yy + 60;
                p(runs[k].font, g_spriteActive, runs[k].s, runs[k].len, &r, DT_NOCLIP_FLAG, color);
            }
        }
    }
}

// One line of the overlay's own text, whatever language it turned out to be in: callers format into
// a plain char buffer and this picks the path, so only a line with a byte over 0x7F pays for the
// conversion. A failed conversion falls back to the ASCII path -- mojibake looks like exactly what
// it is, where a blank line would look like the feature having stopped working.
static void overlay_say(void* font, int x, int y, const char* utf8, DWORD color) {
    if (!utf8) return;
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p && *p < 0x80) p++;
    if (!*p) { overlay_line(font, x, y, utf8, color); return; }   // pure ASCII fast path
    unsigned short w[512];                         // sized as overlay_text_w's, for the same reason
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, (wchar_t*)w, (int)(sizeof w / sizeof w[0]));
    if (n <= 0) { overlay_line(font, x, y, utf8, color); return; }
    overlay_line_w(x, y, w, color);
}

// A "\n" in the text is a PREFERRED break, not a hard one: it is used only when the line will not
// fit, and otherwise it DISAPPEARS -- put any space the joined text needs BEFORE it, never after.
// The row cap is at 100%; larger text wraps sooner, so the cap grows with it (wrap_rows), or the end
// of a longer text would simply not be drawn.
#define OVL_WRAP_LINES 3
static int wrap_rows(void) {
    int n = (OVL_WRAP_LINES * g_scalePct + 99) / 100;
    return n > OVL_WRAP_LINES ? n : OVL_WRAP_LINES;
}
// draw = 0 only counts the rows, so a backdrop can be sized before the text goes on it.
// endX, when given, gets the pixel x where the last row's text ends.
// One character's advance as overlay_line_w lays it: the ASCII face's cell, or the run face's own
// measure (cached per character by jp_run_width).
static int char_px(const unsigned char* q, int len, int cw) {
    if (len == 1) return cw;
    for (int i = 1; i < len; i++) if (!q[i]) return 2 * cw;   // a cut-off sequence: never read past the NUL
    unsigned short w[2];
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)q, len, (wchar_t*)w, 2);
    if (n <= 0) return 2 * cw;
    if (n == 1 && latin_face(w[0])) return cw;
    int px = g_fontJp ? jp_run_width(g_fontJp, w, n) : 0;
    return px > 0 ? px : 2 * cw;
}
static int is_jp_stop(const unsigned char* q, int len) {      // 。
    return len == 3 && q[0] == 0xE3 && q[1] == 0x80 && q[2] == 0x82;
}
static int is_jp_comma(const unsigned char* q, int len) {     // 、
    return len == 3 && q[0] == 0xE3 && q[1] == 0x80 && q[2] == 0x81;
}

// Measured in pixels, not cells: a Japanese character is narrower than two ASCII cells, and
// counting it as two broke lines well short of the screen's edge. Where a line has to break and the
// text names no place for it, the last 。, then the last 、, then the last space is taken -- a 。 or 、
// only past half the line: an early one would leave the rest more rows than the cap allows.
static int overlay_say_wrap(void* font, int x, int contX, int y, const char* utf8, DWORD color,
                            int lineH, int draw, int* endX) {
    int cw = overlay_char_w(font);
    int margin = ovl_margin(), cap = wrap_rows();
    int budget = (g_bbWidth > 0 && cw > 0) ? (g_bbWidth - x - margin) / cw : 0;
    if (endX) *endX = x;
    if (budget < 16) {
        if (draw) overlay_say(font, x, y, utf8, color);
        if (endX) *endX = x + overlay_text_w(utf8);
        return 1;
    }
    const unsigned char* p = (const unsigned char*)utf8;
    char line[512];
    int used = 0;
    while (*p && used < cap) {
        int lx = used ? contX : x;
        int limPx = g_bbWidth - lx - margin;
        int px = 0, n = 0;
        int wantN = -1, spaceN = -1, stopN = -1, commaN = -1, stopPx = 0, commaPx = 0;
        const unsigned char* wantQ = NULL, *spaceQ = NULL, *stopQ = NULL, *commaQ = NULL;
        const unsigned char* q = p;
        while (*q && n < (int)sizeof line - 4) {
            if (*q == '\n') { wantN = n; wantQ = q + 1; q++; continue; }
            int len = (*q < 0x80) ? 1 : (*q < 0xE0) ? 2 : (*q < 0xF0) ? 3 : 4;
            int w = char_px(q, len, cw);
            if (px + w > limPx) break;
            if (*q == ' ') { spaceN = n; spaceQ = q + 1; }
            int stop = is_jp_stop(q, len), comma = is_jp_comma(q, len);
            for (int i = 0; i < len && *q; i++) line[n++] = (char)*q++;
            px += w;
            if (stop)  { stopN = n;  stopQ = q;  stopPx = px; }
            if (comma) { commaN = n; commaQ = q; commaPx = px; }
        }
        if (*q) {
            if (wantN >= 0)       { n = wantN;  q = wantQ; }
            else if (stopN > 0 && stopPx * 2 >= limPx)   { n = stopN;  q = stopQ; }
            else if (commaN > 0 && commaPx * 2 >= limPx) { n = commaN; q = commaQ; }
            else if (spaceN >= 0 && *q != ' ' && (unsigned char)*q < 0x80) { n = spaceN; q = spaceQ; }
        }
        line[n] = 0;
        if (draw) overlay_say(font, lx, y + used * lineH, line, color);
        if (endX) *endX = lx + overlay_text_w(line);
        used++;
        while (*q == ' ' || *q == '\n') q++;
        p = q;
    }
    return used ? used : 1;
}

// A blended quad behind overlay text. The full device state is captured and put back through a
// state block (D3DSBT_ALL = 1): D3DXFont's own draws follow on this same pass, and enumerating the
// dozen states a quad touches is how one gets missed. No state block = no draw.
// hkReset MUST release this -- an outstanding state block makes the game's own Reset fail.
static void* g_backdropSB = NULL;
static void* g_drawDev    = NULL;    // the device this pass is drawing on; set by ff132_overlay_frame
static void overlay_backdrop(void* dev, float x0, float y0, float x1, float y1, DWORD argb) {
    void** vt = *(void***)dev;
    if (!g_backdropSB)
        ((CreateStateBlock_t)vt[D3DDEV_VT_CREATESTATEBLOCK])(dev, 1, &g_backdropSB);
    void* sb = g_backdropSB;
    if (!sb) return;
    if (((SBApply_t)(*(void***)sb)[SB_VT_CAPTURE])(sb) < 0) return;
    ((SetShader_t)vt[D3DDEV_VT_SETVSHADER])(dev, NULL);
    ((SetShader_t)vt[D3DDEV_VT_SETPSHADER])(dev, NULL);
    ((SetTexture_t)vt[D3DDEV_VT_SETTEXTURE])(dev, 0, NULL);
    SetTexStage_t ts = (SetTexStage_t)vt[D3DDEV_VT_SETTEXSTAGE];
    ts(dev, 0, 1 /*COLOROP*/, 2 /*SELECTARG1*/); ts(dev, 0, 2 /*COLORARG1*/, 0 /*DIFFUSE*/);
    ts(dev, 0, 4 /*ALPHAOP*/, 2);                ts(dev, 0, 5 /*ALPHAARG1*/, 0);
    ts(dev, 1, 1, 1 /*DISABLE*/);                ts(dev, 1, 4, 1);
    SetRenderState_t rs = (SetRenderState_t)vt[D3DDEV_VT_SETRENDERSTATE];
    rs(dev,  27, 1);     // ALPHABLENDENABLE
    rs(dev,  19, 5);     // SRCBLEND  = SRCALPHA
    rs(dev,  20, 6);     // DESTBLEND = INVSRCALPHA
    rs(dev,   7, 0);     // ZENABLE off
    rs(dev,  14, 0);     // ZWRITEENABLE off
    rs(dev,  15, 0);     // ALPHATESTENABLE off
    rs(dev,  22, 1);     // CULLMODE = NONE
    rs(dev,  28, 0);     // FOGENABLE off
    rs(dev,  52, 0);     // STENCILENABLE off
    rs(dev, 168, 0xF);   // COLORWRITEENABLE = all
    rs(dev, 174, 0);     // SCISSORTESTENABLE off
    struct { float x, y, z, rhw; DWORD c; } v[4] = {
        { x0, y0, 0.f, 1.f, argb }, { x1, y0, 0.f, 1.f, argb },
        { x0, y1, 0.f, 1.f, argb }, { x1, y1, 0.f, 1.f, argb },
    };
    ((SetFVF_t)vt[D3DDEV_VT_SETFVF])(dev, 0x44 /*XYZRHW|DIFFUSE*/);
    ((DrawPrimUP_t)vt[D3DDEV_VT_DRAWPRIMUP])(dev, 5 /*TRIANGLESTRIP*/, 2, v, sizeof v[0]);
    ((SBApply_t)(*(void***)sb)[SB_VT_APPLY])(sb);
}

// The helpers other features' rows are drawn with. Render thread, inside the draw pass.
int ff132_ovl_char_w(void) { return g_font ? overlay_char_w(g_font) : 0; }
int ff132_ovl_text_w(const char* utf8) { return g_font ? overlay_text_w(utf8) : 0; }
int ff132_ovl_ready(void) { return g_font != NULL; }
int ff132_ovl_line_h(void) { return g_lineH; }
int ff132_ovl_px(int px) { return ovl_scaled(px, g_scalePct); }
// 4096 was every backdrop's right edge before the screen's width was known to them; kept as the floor.
int ff132_ovl_full_w(void) { return g_bbWidth > 4096 ? g_bbWidth : 4096; }
int ff132_ovl_screen_h(void) { return g_bbHeight; }
int ff132_ovl_scale_pct(void) { return g_scalePct; }
// Spaces of several widths, measured and laid widest-first, so a gap narrower than an ASCII cell can
// still be closed (a kana in the proportional face is a few px). A space the face lacks measures 0
// and is skipped; under ui_ascii_only only the ASCII one is used.
int ff132_ovl_space(char* out, size_t n, int px) {
    static const char* const kFill[] = { "\xE3\x80\x80", " ", "\xE2\x80\x82", "\xE2\x80\x89",
                                         "\xE2\x80\x8A" };
    enum { FILLS = sizeof kFill / sizeof kFill[0] };
    size_t pl = 0;
    if (n) out[0] = 0;
    int ascii = ui_ascii_only();
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < FILLS; i++) {
            if (ascii && i != 1) continue;
            int fw = ff132_ovl_text_w(kFill[i]);
            if (fw <= 0) continue;
            size_t fl = strlen(kFill[i]);
            while (px >= (pass ? (fw + 1) / 2 : fw) && pl + fl < n) {
                memcpy(out + pl, kFill[i], fl);
                pl += fl;
                px -= fw;
                if (pass) break;
            }
        }
    if (n) out[pl < n ? pl : n - 1] = 0;
    return px;
}
void ff132_ovl_fit(char* v, size_t n, const char* const* cands, int cn) {
    int cw = ff132_ovl_char_w();
    if (!cw || !v) return;
    int mx = 0;
    for (int i = 0; i < cn; i++) {
        int w = cands[i] ? overlay_text_w(cands[i]) : 0;
        if (w > mx) mx = w;
    }
    // ASCII spaces alone leave up to half a cell, which reads as the right arrow wandering.
    size_t len = strlen(v);
    int gap = mx - overlay_text_w(v);
    if (gap <= 0 || len + 1 >= n) return;
    ff132_ovl_space(v + len, n - len, gap);
}
void ff132_ovl_say(int x, int y, const char* utf8, unsigned long argb) {
    if (!g_font) return;
    overlay_say(g_font, x, y, utf8, (DWORD)argb);
}
int ff132_ovl_jp_missing(void) { return g_fontJpMissing != 0; }
void ff132_ovl_ascii(int x, int y, const char* s, unsigned long argb) {
    if (!g_font) return;
    overlay_line(g_font, x, y, s, (DWORD)argb);
}
int ff132_ovl_say_wrap(int x, int contX, int y, const char* utf8, unsigned long argb, int lineH) {
    if (!g_font) return 1;
    return overlay_say_wrap(g_font, x, contX, y, utf8, (DWORD)argb, lineH, 1, NULL);
}
int ff132_ovl_wrap_count(int x, int contX, const char* utf8) {
    if (!g_font) return 1;
    return overlay_say_wrap(g_font, x, contX, 0, utf8, 0, 0, 0, NULL);
}
int ff132_ovl_wrap_tail(int x, int contX, const char* utf8, const char* tail, int* tailX,
                     int* tailRow) {
    int endX = x;
    int rows = g_font ? overlay_say_wrap(g_font, x, contX, 0, utf8, 0, 0, 0, &endX) : 1;
    int tw = g_font ? overlay_text_w(tail) : 0;
    if (g_bbWidth <= 0 || endX + tw <= g_bbWidth - ovl_margin()) {
        *tailX = endX; *tailRow = rows - 1;
        return rows;
    }
    *tailX = contX; *tailRow = rows;
    return rows + 1;
}
void ff132_ovl_backdrop(int x0, int y0, int x1, int y1, unsigned long argb) {
    if (!g_drawDev) return;
    overlay_backdrop(g_drawDev, (float)x0, (float)y0, (float)x1, (float)y1, (DWORD)argb);
}

// ---- the text size ------------------------------------------------------------------------------
// The size the faces should be at now: the setting, with auto worked out against the back buffer.
static int scale_wanted(void) {
    int s = g_ovlScale;
    if (s == FF132_SCALE_AUTO) {
        int h = g_bbHeight;
        s = h > 0 ? (h * 100 + OVL_AUTO_REF_H / 2) / OVL_AUTO_REF_H : 100;
        if (s < 100) s = 100;
    }
    if (s < FF132_SCALE_MIN) s = FF132_SCALE_MIN;
    if (s > FF132_SCALE_MAX) s = FF132_SCALE_MAX;
    return s;
}

// The Japanese face is asked for by name ONLY after checking the name is installed: the GDI mapper
// never fails, it substitutes. NULL is the fallback -- the mapper picks a Japanese-capable font.
// Looked up once, at the first make; every remake asks for the same face.
static const char* g_jpFace = NULL;

// One face at px: the ASCII one, or (jp) the Japanese one. NULL = not made (and logged).
static void* font_new(void* dev, int px, int jp) {
    const char* face = jp ? g_jpFace : OVL_FONT_FACE;
    void* f = NULL;
    // Weight=700(bold), CharSet=1(DEFAULT) or 128(SHIFTJIS), Quality=5(CLEARTYPE).
    HRESULT hr = g_pCreateFont(dev, px, 0, 700, 1, FALSE, jp ? 128 : 1, 0, 5, 0, face, &f);
    if (hr < 0 || !f) {
        ff132_loga(FF132_TAG "overlay: D3DXCreateFontA(%s, height %d) failed hr=0x%08X\n",
                   face ? face : "system default", px, (unsigned)hr);
        return NULL;
    }
    // ASCII into the glyph atlas now, not on a character's first appearance mid-message.
    if (!jp) ((PreloadChars_t)(*(void***)f)[FONT_VT_PRELOADCHARS])(f, 0x20, 0x7E);
    return f;
}

static void com_release(void* obj) {
    if (obj) ((Release_t)(*(void***)obj)[IUNK_VT_RELEASE])(obj);
}

// The faces in force were made at pct: every size and measure taken from the old ones starts over.
static void scale_take(int pct) {
    g_scalePct = pct;
    g_fontPx   = ovl_scaled(OVL_FONT_H, pct);
    g_fontPxJp = ovl_scaled(OVL_FONT_H_JP, pct);
    g_lineH    = ovl_scaled(OVL_LINE_H, pct);
    g_shadowPx = ovl_scaled(OVL_OFFSET_PX, pct);
    g_ovlCw = 0;
    memset(g_jpw, 0, sizeof g_jpw);
    int aA = font_ascent(g_font), aJ = font_ascent(g_fontJp);
    g_jpYoff = (aA > 0 && aJ > 0) ? aA - aJ : (g_fontPx - g_fontPxJp) / 2;
    ff132_loga(FF132_TAG "overlay: text at %d%% (heights %d/%d, rows %dpx); ascent ascii %d, jp %d ->"
                  " jp runs drawn %+dpx (one baseline)\n",
               pct, g_fontPx, g_fontPxJp, g_lineH, aA, aJ, g_jpYoff);
}

// The size asked for moved (the setting changed, or auto met another back buffer): both faces are made
// again and swapped in as a pair, the old pair released only once the new one is in, so the faces in
// force are never missing. If a new face cannot be made the old pair stays, and that size is not asked
// for again until another one has been. Render thread, between passes: no draw holds a face across.
static void overlay_rescale(void* dev, int pct) {
    if (!g_pCreateFont || !g_font) return;
    void* font = font_new(dev, ovl_scaled(OVL_FONT_H, pct), 0);
    void* fj = (font && g_fontJp) ? font_new(dev, ovl_scaled(OVL_FONT_H_JP, pct), 1) : NULL;
    if (!font || (g_fontJp && !fj)) {
        com_release(font);
        g_scaleFail = pct;
        ff132_loga(FF132_TAG "overlay: the text could not be made at %d%% -- it stays at %d%%\n",
                   pct, g_scalePct);
        return;
    }
    void* oldA = g_font;
    void* oldJ = g_fontJp;
    g_font = font;
    if (fj) g_fontJp = fj;
    com_release(oldA);
    if (fj) com_release(oldJ);
    g_scaleFail = 0;
    scale_take(pct);
}

// Create the two fonts + the sprite against the REAL device, at most once. Runs on the render thread
// from ff132_overlay_frame. Made at the size asked for as far as it is known: auto has no back
// buffer to go by on the very first pass, so it starts at 100% and is remade once it has one.
static void overlay_make_fonts(void* dev) {
    g_fontTried = 1;
    HMODULE hx = GetModuleHandleA("d3dx9_43.dll");
    if (!hx) hx = LoadLibraryA("d3dx9_43.dll");
    if (!hx) { ff132_loga(FF132_TAG "overlay: d3dx9_43.dll not found -> no text\n"); return; }
    g_pCreateFont = (D3DXCreateFontA_t)GetProcAddress(hx, "D3DXCreateFontA");
    if (!g_pCreateFont) { ff132_loga(FF132_TAG "overlay: D3DXCreateFontA missing\n"); return; }
    int pct = scale_wanted();
    void* font = font_new(dev, ovl_scaled(OVL_FONT_H, pct), 0);
    if (!font) return;
    g_font = font;
    ff132_loga(FF132_TAG "overlay: font ready (%s, height %d)\n", OVL_FONT_FACE, ovl_scaled(OVL_FONT_H, pct));
    // A face without the English arrows would still show them, drawn by GDI from a linked font,
    // so only the face's own glyph table tells. The settings screen's scroll marks are asked too.
    {
        static const WCHAR kArrows[4] = { 0x25C4, 0x25BA, 0x25B2, 0x25BC };
        WORD gi[4];
        HDC fdc = ((FontGetDC_t)(*(void***)font)[FONT_VT_GETDC])(font);
        if (fdc && GetGlyphIndicesW(fdc, kArrows, 4, gi, GGI_MARK_NONEXISTING_GLYPHS) == 4)
            ff132_loga(FF132_TAG "overlay: %s has the English arrows U+25C4/U+25BA: %s (glyphs %u, %u);"
                          " the scroll marks U+25B2/U+25BC: %s\n",
                       OVL_FONT_FACE, (gi[0] != 0xFFFF && gi[1] != 0xFFFF) ? "yes" : "NO",
                       (unsigned)gi[0], (unsigned)gi[1],
                       (gi[2] != 0xFFFF && gi[3] != 0xFFFF) ? "yes" : "NO");
    }
    g_jpFace = font_installed(OVL_FONT_FACE_JP) ? OVL_FONT_FACE_JP : NULL;
    void* fj = font_new(dev, ovl_scaled(OVL_FONT_H_JP, pct), 1);
    if (fj) {
        g_fontJp = fj;
        ff132_loga(FF132_TAG "overlay: Japanese font ready (%s, height %d)\n",
               g_jpFace ? OVL_FONT_FACE_JP : "system default (" OVL_FONT_FACE_JP " not installed)",
               ovl_scaled(OVL_FONT_H_JP, pct));
    } else {
        g_fontJpMissing = 1;
        ff132_loga(FF132_TAG "overlay: no Japanese font -- the mod's own text switches to English\n");
    }
    scale_take(pct);
    D3DXCreateSprite_t pCS = (D3DXCreateSprite_t)GetProcAddress(hx, "D3DXCreateSprite");
    void* sp = NULL;
    if (pCS && pCS(dev, &sp) >= 0 && sp) g_sprite = sp;
    ff132_loga(FF132_TAG "overlay: %s\n",
           g_sprite ? "text batched through one sprite" : "no sprite -- per-call draws");
}

static int in_d3d9_image(void* p);

// Asking the object itself. Checking that some vtable slots land in d3d9.dll is not enough: d3d9's
// vtables are adjacent in .rdata, so a shorter interface (a swap chain, the IDirect3D9) reads past
// its own end into another one and passes. Handing such an object to D3DXCreateFont killed the game
// (measured). Only a real device answers this IID.
static const GUID FF132_IID_IDirect3DDevice9 =
    { 0xd0223b96, 0xbf7a, 0x43fd, { 0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb } };
typedef HRESULT (__stdcall *QueryInterface_t)(void*, const GUID*, void**);

// The known-offset candidate does not get the "its QueryInterface is d3d9's" precheck: that check
// is only a guard for the scan, and if it were wrong about the module it would reject the device
// itself. A readable vtable with a callable slot 0 is enough before asking the IID.
static int is_real_device_lax(void* cand) {
    if (!cand || IsBadReadPtr(cand, sizeof(void*))) return 0;
    void** vt = *(void***)cand;
    if (IsBadReadPtr(vt, sizeof(void*) * (D3DDEV_VT_ENDSCENE + 1))) return 0;
    if (!vt[0] || IsBadCodePtr((FARPROC)vt[0])) return 0;
    void* out = NULL;
    if (((QueryInterface_t)vt[0])(cand, &FF132_IID_IDirect3DDevice9, &out) < 0 || !out) return 0;
    ((Release_t)(*(void***)out)[IUNK_VT_RELEASE])(out);
    return 1;
}

static int is_real_device(void* cand) {
    if (!cand || IsBadReadPtr(cand, sizeof(void*))) return 0;
    void** vt = *(void***)cand;
    if (IsBadReadPtr(vt, sizeof(void*) * (D3DDEV_VT_ENDSCENE + 1))) return 0;
    // QueryInterface itself has to be d3d9's before it is worth calling.
    if (!in_d3d9_image(vt[0]) || !in_d3d9_image(vt[IUNK_VT_RELEASE])) return 0;
    void* out = NULL;
    if (((QueryInterface_t)vt[0])(cand, &FF132_IID_IDirect3DDevice9, &out) < 0 || !out) return 0;
    ((Release_t)(*(void***)out)[IUNK_VT_RELEASE])(out);   // identity confirmed, reference dropped
    return 1;
}

// FF13-2 does not hand EndScene the d3d9 device: it hands its own wrapper, whose vtable sits in the
// exe (EndScene at 0x00C2FE40). Hooking the wrapper is right -- that is where the frame ends -- but
// every D3D call the overlay makes has to go to the REAL device the wrapper holds inside it. Making
// them on the wrapper took the game down on the first frame.
#define OVL_WRAPPER_SCAN 0x200
static void* g_realDev    = NULL;
static void* g_realDevFor = NULL;      // the wrapper g_realDev was pulled out of
static void* real_device_of(void* wrapper) {
    if (!wrapper) return NULL;
    if (wrapper == g_realDevFor) return g_realDev;
    void* found = NULL;
    if (is_real_device(wrapper)) {
        found = wrapper;                               // not wrapped after all
    } else {
        // The known offset first. The scan is only a safety net, and it checks each slot on its own:
        // the wrapper is a 0x14-byte allocation, so asking whether a whole range past it is readable
        // fails and would skip the search entirely.
        void* at = (char*)wrapper + FF132_OFF_WRAPPER_DEVICE;
        if (!IsBadReadPtr(at, sizeof(void*)) && is_real_device_lax(*(void**)at)) {
            found = *(void**)at;
        } else {
            for (int off = 0; off < OVL_WRAPPER_SCAN; off += 4) {
                void* slot = (char*)wrapper + off;
                if (IsBadReadPtr(slot, sizeof(void*))) break;
                if (is_real_device(*(void**)slot)) { found = *(void**)slot; break; }
            }
        }
    }
    g_realDevFor = wrapper;
    g_realDev    = found;
    static int loggedOk = 0, loggedBad = 0;
    if (found && !loggedOk) {
        loggedOk = 1;
        ff132_loga(FF132_TAG "overlay: real d3d9 device %p found inside the wrapper %p\n", found, wrapper);
    } else if (!found && !loggedBad) {
        loggedBad = 1;
        ff132_loga(FF132_TAG "overlay: no d3d9 device inside the wrapper %p -> nothing is drawn\n", wrapper);
    }
    return found;
}

// Game (render) thread, from hkEndScene.
void ff132_overlay_frame(void* wrapper) {
    // Ahead of every early-out: the REAL timer's edges have to be seen on each pass whether or not
    // anything is drawn, or a fight that started with the overlay off is timed from the wrong frame.
    ff132_battle_frame();
    if (!wrapper) return;
    int flashOn = (int)(g_flashUntil - GetTickCount()) > 0;
    int battleOn = ff132_battle_visible();
    // Asked on every pass, not just the ones that draw: it is also the guard that closes a menu
    // whose input gate has lapsed, and a menu nobody can dismiss must not survive an idle overlay.
    int tidyOn = ff132_tidy_visible();
    int speedOn = ff132_speed_visible();
    int encOn = ff132_encount_visible();
    int killOn = ff132_kill_visible();
    int arOn = ff132_autoretry_visible();
    // Asked on every pass for the same lapse guard. The screen steps aside while a save menu is up,
    // since both want the top of the screen.
    int helpOn = ff132_setup_visible() && !tidyOn;
    // Keep the very first pass open even with nothing to show so the fonts exist before the first
    // flash needs them; after that an idle overlay costs one compare per EndScene.
    if (!flashOn && !battleOn && !tidyOn && !speedOn && !encOn && !killOn && !arOn && !helpOn && g_fontTried) return;

    void* dev = real_device_of(wrapper);
    if (!dev) return;                  // never call D3D on the wrapper: that is what crashed it

    // The fonts are created before the back-buffer test, not after it: if that test never matches on
    // some machine the overlay would otherwise never even try to build a font, and the log would
    // show the hook installed with nothing behind it.
    void** dvt = *(void***)dev;
    if (!g_font && !g_fontTried) overlay_make_fonts(dev);
    if (!g_font) return;

    // Draw ONLY on the pass whose render target is the back buffer. FF13/FF13-2 call EndScene many
    // times per frame (scene -> intermediate RTs -> post-processing -> back buffer), and our bright
    // text on an intermediate RT is smeared across the screen by the game's bloom.
    if (dev != g_bbCacheDev) {
        void* bb = NULL;
        ((GetBackBuffer_t)dvt[D3DDEV_VT_GETBACKBUFFER])(dev, 0, 0, 0 /*D3DBACKBUFFER_TYPE_MONO*/, &bb);
        if (!bb) {
            if (!g_bbNoneLogged) {
                g_bbNoneLogged = 1;
                ff132_loga(FF132_TAG "overlay: GetBackBuffer gave nothing -- cannot tell the passes apart\n");
            }
            return;                            // cannot identify the pass -- draw nothing this call
        }
        ((Release_t)(*(void***)bb)[IUNK_VT_RELEASE])(bb);   // identity kept, reference dropped
        g_bbCache = bb; g_bbCacheDev = dev;
        // D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width,
        // Height -- eight DWORDs, so the width is the seventh and the height the eighth.
        // IDirect3DSurface9::GetDesc is slot 12.
        {
            DWORD d[8] = {0};
            typedef HRESULT (__stdcall *GetDesc_t)(void*, void*);
            void** bvt = *(void***)bb;
            if (bvt && ((GetDesc_t)bvt[12])(bb, d) >= 0) {
                if (d[6] > 100) g_bbWidth = (int)d[6];
                if (d[7] > 100) g_bbHeight = (int)d[7];
            }
        }
    }
    void* rt = NULL;
    ((GetRenderTarget_t)dvt[D3DDEV_VT_GETRENDERTARGET])(dev, 0, &rt);
    int onBackbuffer = (rt != NULL && rt == g_bbCache);
    if (rt) ((Release_t)(*(void***)rt)[IUNK_VT_RELEASE])(rt);
    // A game whose final pass never binds the back buffer would leave the overlay invisible with no
    // way to tell from the log, so the passes are counted: once it is clear none of them is the back
    // buffer, the test is abandoned and every pass draws. Bloom may smear the text in that mode, but
    // a smeared line beats a missing one, and the log says which mode is in force.
    if (!g_bbFallback) {
        if (onBackbuffer) {
            g_bbSeen = 1;
        } else if (!g_bbSeen && ++g_bbMisses > 1200) {
            g_bbFallback = 1;
            ff132_loga(FF132_TAG "overlay: no EndScene pass ever bound the back buffer in %d calls"
                          " -- drawing on every pass from now on\n", g_bbMisses);
        }
    }
    if (!g_bbFallback) {
        // EVERY back-buffer pass draws, not just the first of a run: FF13-2 composes more onto the
        // back buffer after its first pass and paints over a line drawn only there (measured).
        if (!onBackbuffer) return;
        if (!g_bbFirstLogged) {
            g_bbFirstLogged = 1;
            ff132_loga(FF132_TAG "overlay: drawing on the back-buffer pass (%dx%d)\n", g_bbWidth, g_bbHeight);
        }
    }
    // Fonts exist now; nothing to draw this pass.
    if (!flashOn && !battleOn && !tidyOn && !speedOn && !encOn && !killOn && !arOn && !helpOn) return;

    // Before the sprite begins, so no face is in use: the setting may have moved since the last pass,
    // or auto may have met a new back buffer (the first one, or one a Reset brought).
    {
        int want = scale_wanted();
        if (want != g_scalePct && want != g_scaleFail) overlay_rescale(dev, want);
    }

    // Plain ALPHABLEND on purpose: SORT_TEXTURE reorders across textures, and a JP run's shadow
    // (2px off) could then land on top of its ASCII neighbour's body.
    if (g_sprite) {
        void** svt = *(void***)g_sprite;
        if (svt && ((SpriteBegin_t)svt[SPRITE_VT_BEGIN])(g_sprite, D3DXSPRITE_ALPHABLEND_F) >= 0)
            g_spriteActive = g_sprite;
    }
    {
        static int drewOnce = 0;
        if (!drewOnce) {
            drewOnce = 1;
            ff132_loga(FF132_TAG "overlay: first line drawn\n");
        }
    }
    int m = ovl_margin(), y = m;
    g_drawDev = dev;                                // ff132_ovl_backdrop's device, for this pass only
    if (helpOn) {
        y = ff132_setup_draw(m, y);
        // Straight under the settings screen, which keeps room for it when it has to scroll: below the
        // status lines it would land off the bottom of the screen, and it answers what was just done.
        if (flashOn) {
            y += g_lineH * overlay_say_wrap(g_font, m, m, y, g_flash, g_flashCol, g_lineH, 1, NULL);
            flashOn = 0;
        }
    }
    if (battleOn) y = ff132_battle_draw(m, y);
    if (killOn)   y = ff132_kill_draw(m, y);
    if (speedOn)  y = ff132_speed_draw(m, y);
    if (encOn)    y = ff132_encount_draw(m, y);
    if (arOn)     y = ff132_autoretry_draw(m, y);
    if (flashOn)  overlay_say_wrap(g_font, m, m, y, g_flash, g_flashCol, g_lineH, 1, NULL);
    if (tidyOn) {
        // Lifted when a short screen (or a large text size) would cut the blocks off at the bottom: a
        // menu that cannot be read whole is worse than one drawn over the status lines.
        int top = ovl_scaled(FF132_TIDY_TOP, g_scalePct), h = ff132_tidy_height();
        if (g_bbHeight > 0 && top + h > g_bbHeight) {
            top = g_bbHeight - h;
            if (top < m) top = m;
        }
        ff132_tidy_draw(m, top);
    }
    g_drawDev = NULL;
    if (g_spriteActive) {                           // buffered text renders here
        void* sp = g_spriteActive;
        g_spriteActive = NULL;
        ((FontOp_t)(*(void***)sp)[SPRITE_VT_END])(sp);
    }
}

// EndScene hook: draw first, then chain to the original.
static HRESULT __stdcall hkEndScene(void* dev) {
    static int liveLogged = 0;
    if (!liveLogged) {
        liveLogged = 1;
        ff132_loga(FF132_TAG "overlay: EndScene hook is live\n");
    }
    ff132_overlay_frame(dev);
    void** vt = *(void***)dev;
    EndScene_t o = (EndScene_t)((g_gameVt && vt == g_gameVt) ? g_origEndScene : g_dummyEndScene);
    return o ? o(dev) : 0;
}

static void com_op(void* obj, int slot) {
    if (!obj) return;
    void** vt = *(void***)obj;
    if (vt && vt[slot]) ((FontOp_t)vt[slot])(obj);
}

// Reset hook: a device reset (resolution change / device lost) invalidates the font and sprite's
// default-pool GPU objects. Release them BEFORE the game's own Reset (an unreleased one makes it
// fail with D3DERR_INVALIDCALL), chain, then restore them on success.
static HRESULT __stdcall hkReset(void* dev, FF132_D3DPP* pp) {
    // Reset recreates it; refetch lazily (and a new height can move an auto text size with it).
    g_bbCache = NULL; g_bbCacheDev = NULL; g_bbWidth = 0; g_bbHeight = 0;
    if (g_backdropSB) {      // an outstanding state block is enough to make the game's Reset fail
        ((Release_t)(*(void***)g_backdropSB)[IUNK_VT_RELEASE])(g_backdropSB);
        g_backdropSB = NULL;
    }
    com_op(g_font,   FONT_VT_ONLOST);
    com_op(g_fontJp, FONT_VT_ONLOST);
    com_op(g_sprite, SPRITE_VT_ONLOST);
    void** vt = *(void***)dev;
    Reset_t o = (Reset_t)((g_gameVt && vt == g_gameVt) ? g_origReset : g_dummyReset);
    HRESULT hr = o ? o(dev, pp) : 0;
    if (hr >= 0) {
        com_op(g_font,   FONT_VT_ONRESET);
        com_op(g_fontJp, FONT_VT_ONRESET);
        com_op(g_sprite, SPRITE_VT_ONRESET);
        g_scaleFail = 0;     // a size that could not be made on the device before is worth one more try
        ff132_loga(FF132_TAG "overlay: device reset handled\n");
    }
    return hr;
}

// A slot already holding repl is left as it is and `saved` untouched: one of our hooks recorded as
// its own original would recurse until the stack runs out.
static int patch_vtable_slot(void** vtbl, int slot, void* repl, void** saved) {
    DWORD old;
    if (vtbl[slot] == repl) return 1;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return 0;
    if (saved) *saved = vtbl[slot];
    vtbl[slot] = repl;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[slot], sizeof(void*));
    return 1;
}

// ---- finding the game's OWN device ------------------------------------------------------------
// d3d9.dll's loaded span: a d3d9 device vtable and every entry in it live inside it. Not proof on
// its own (see dev_vtable_of).
static uintptr_t g_d3d9Lo = 0, g_d3d9Hi = 0;

static int in_d3d9_image(void* p) {
    if (!g_d3d9Hi) {
        HMODULE h = GetModuleHandleA("d3d9.dll");
        if (!h) return 0;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)h;
        if (IsBadReadPtr(dos, sizeof *dos) || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return 0;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const char*)h + dos->e_lfanew);
        if (IsBadReadPtr(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        // SizeOfImage, not the file size: this is the span the loader mapped.
        DWORD span = nt->OptionalHeader.SizeOfImage;
        if (!span) return 0;
        g_d3d9Lo = (uintptr_t)h;
        g_d3d9Hi = g_d3d9Lo + span;
        ff132_loga(FF132_TAG "overlay: d3d9.dll mapped at %p..%p\n", (void*)g_d3d9Lo, (void*)g_d3d9Hi);
    }
    return (uintptr_t)p >= g_d3d9Lo && (uintptr_t)p < g_d3d9Hi;
}

// A vtable slot that holds one of our own hooks counts as a device slot too -- otherwise a second
// pass over an already-patched vtable would reject it and go looking for a different object.
static int dev_slot_ok(void* fn, void* mine) {
    return fn == mine || in_d3d9_image(fn);
}

// Locate the vtable the game's own IDirect3DDevice9 uses. NULL = not yet (graphics init has not run,
// or the pointers are not there); the caller retries.
// A candidate passes only if three device slots land in d3d9.dll. That is not proof on its own --
// d3d9's vtables sit next to each other in .rdata, so a shorter interface's vtable can read as a
// device -- which is why the global the game itself receives the device in is tried first and the
// scan is only the fallback.
static void** dev_vtable_of(void* cand) {
    if (!cand || IsBadReadPtr(cand, sizeof(void*))) return NULL;
    void** vt = *(void***)cand;
    if (IsBadReadPtr(vt, sizeof(void*) * (D3DDEV_VT_ENDSCENE + 1))) return NULL;
    if (!dev_slot_ok(vt[D3DDEV_VT_ENDSCENE], (void*)hkEndScene)) return NULL;
    if (!dev_slot_ok(vt[D3DDEV_VT_RESET], (void*)hkReset)) return NULL;
    if (!in_d3d9_image(vt[D3DDEV_VT_BEGINSCENE])) return NULL;      // never patched, so a pure test
    if (!in_d3d9_image(vt[D3DDEV_VT_GETBACKBUFFER])) return NULL;   // both are called by the overlay
    if (!in_d3d9_image(vt[D3DDEV_VT_GETRENDERTARGET])) return NULL;
    return vt;
}

static void** find_game_device_vtable(void) {
    // The global is what CreateDevice was handed, so whatever sits in it IS the device: it gets the
    // readability checks but NOT the "slots live in d3d9.dll" one. A wrapper (Steam's overlay, a
    // d3d9 shim) legitimately puts the vtable in another module, and rejecting that is what left
    // this hook dead on hardware. The scan below keeps the strict test -- it has no such warrant.
    {
        void** g = (void**)(g_base + FF132_RVA_DEVICE_PTR);
        void* cand = IsBadReadPtr(g, sizeof(void*)) ? NULL : *g;
        if (cand && !IsBadReadPtr(cand, sizeof(void*))) {
            void** vt = *(void***)cand;
            if (!IsBadReadPtr(vt, sizeof(void*) * (D3DDEV_VT_ENDSCENE + 1)) &&
                vt[D3DDEV_VT_ENDSCENE] && vt[D3DDEV_VT_RESET]) {
                static int logged = 0;
                if (!logged) {
                    logged = 1;
                    ff132_loga(FF132_TAG "overlay: game device %p from its own global, vtable=%p"
                                  " (EndScene=%p, %s d3d9; dummy vtable was %p, %s)\n",
                           cand, (void*)vt, vt[D3DDEV_VT_ENDSCENE],
                           in_d3d9_image(vt[D3DDEV_VT_ENDSCENE]) ? "inside" : "OUTSIDE",
                           (void*)g_dummyVt, (vt == g_dummyVt) ? "the same one" : "a different one");
                }
                return vt;
            }
            if (!g_devBadLogged) {
                g_devBadLogged = 1;
                ff132_loga(FF132_TAG "overlay: the device global holds %p but its vtable is unusable"
                              " -- falling back to the renderer scan\n", cand);
            }
        }
    }
    void** slot = (void**)(g_base + FF132_RVA_RENDERER_PTR);
    if (IsBadReadPtr(slot, sizeof(void*))) return NULL;
    char* rend = (char*)*slot;
    if (!rend || IsBadReadPtr(rend, FF132_RENDERER_SCAN_HI)) return NULL;

    // The anchor: an object whose vtable is in d3d9.dll sitting at the IDirect3D9 offset is what
    // says this really is the renderer AND that it has got as far as creating the d3d9 objects.
    void* d3d = *(void**)(rend + FF132_OFF_RENDERER_D3D9);
    if (!d3d || IsBadReadPtr(d3d, sizeof(void*))) return NULL;
    void** d3dvt = *(void***)d3d;
    if (IsBadReadPtr(d3dvt, sizeof(void*) * 17)) return NULL;       // IDirect3D9 has 17 methods
    if (!in_d3d9_image(d3dvt[0]) || !in_d3d9_image(d3dvt[16])) return NULL;

    for (int off = FF132_RENDERER_SCAN_LO; off < FF132_RENDERER_SCAN_HI; off += 4) {
        if (off == FF132_OFF_RENDERER_D3D9) continue;
        void** vt = dev_vtable_of(*(void**)(rend + off));
        if (!vt) continue;
        ff132_loga(FF132_TAG "overlay: game device found at renderer+0x%X, vtable=%p (dummy vtable was %p, %s)\n",
               (unsigned)off, (void*)vt, (void*)g_dummyVt,
               (vt == g_dummyVt) ? "the same one" : "a different one");
        return vt;
    }
    return NULL;
}

// Patch the game device's own vtable, retried until it takes. Called from the install (worker
// thread) and then every frame from the game thread, so one gate keeps two threads out of the
// patch -- saving hkEndScene as its own original would recurse forever.
static volatile LONG g_adoptDone = 0;
static volatile LONG g_adoptBusy = 0;

// Once the game's vtable carries the hooks, the throwaway's are put back: left in, a device sharing
// that vtable (every device, under DXVK) would draw the overlay a second time per frame. A slot that
// no longer holds our hook belongs to whoever hooked it after us and is left alone.
static void dummy_restore(void) {
    void** d = g_dummyVt;
    if (!d || d == g_gameVt) return;
    if (g_dummyEndScene && d[D3DDEV_VT_ENDSCENE] == (void*)hkEndScene)
        patch_vtable_slot(d, D3DDEV_VT_ENDSCENE, g_dummyEndScene, NULL);
    if (g_dummyReset && d[D3DDEV_VT_RESET] == (void*)hkReset)
        patch_vtable_slot(d, D3DDEV_VT_RESET, g_dummyReset, NULL);
}

static void overlay_adopt_game_device(void) {
    if (g_adoptDone) return;
    if (InterlockedCompareExchange(&g_adoptBusy, 1, 0) != 0) return;
    void** vt = find_game_device_vtable();
    if (vt) {
        // The throwaway's vtable already carries the hooks; another hook stacked on ours there
        // chains into hkEndScene, and recording it as the original would loop.
        if ((vt == g_dummyVt && g_dummyHooked) || vt[D3DDEV_VT_ENDSCENE] == (void*)hkEndScene) {
            g_adoptDone = 1;
            ff132_loga(FF132_TAG "overlay: the game's device already uses the patched vtable\n");
        } else {
            // Every EndScene from now on arrives through THIS vtable, so its originals are the ones
            // the hooks must chain to: they replace whatever the throwaway device donated. Read
            // before patching -- a frame landing between the two would otherwise chain to a stale
            // pointer. Reset first, EndScene last, because EndScene is what starts the drawing.
            // A Reset slot already holding hkReset (a retry after a failed EndScene patch, or a vtable
            // copied from d3d9's after the throwaway was patched) keeps the original it chained to;
            // with none known, the hook stays off rather than chain Reset to nothing.
            void* r = vt[D3DDEV_VT_RESET];
            void* origR = r != (void*)hkReset ? r
                        : (g_gameVt == vt && g_origReset) ? g_origReset : g_dummyReset;
            if (!origR || origR == (void*)hkReset) {
                g_adoptDone = 1;
                ff132_loga(FF132_TAG "overlay: the game's vtable already holds our Reset with no original"
                              " known -- not adopted\n");
                InterlockedExchange(&g_adoptBusy, 0);
                return;
            }
            g_origEndScene = vt[D3DDEV_VT_ENDSCENE];
            g_origReset = origR;
            g_gameVt = vt;
            patch_vtable_slot(vt, D3DDEV_VT_RESET, (void*)hkReset, NULL);
            if (patch_vtable_slot(vt, D3DDEV_VT_ENDSCENE, (void*)hkEndScene, NULL)) {
                g_adoptDone = 1;
                dummy_restore();
                ff132_loga(FF132_TAG "overlay: adopted the game's device vtable (EndScene+Reset patched,"
                              " the throwaway's put back)\n");
            }
        }
    }
    InterlockedExchange(&g_adoptBusy, 0);
}

// Game thread, once per frame from the save hook's own frame site: the EndScene hook cannot retry
// this itself, since a missed device is exactly the case where it never runs.
void ff132_overlay_poll(void) {
    if (g_adoptDone) return;
    overlay_adopt_game_device();
}

// One-time overlay install, from the worker thread. Fully non-fatal: on any failure the overlay is
// absent and the mod continues.
void ff132_overlay_install(void) {
    // d3d9 should already be loaded by the game; wait up to ~10s, then give up.
    HMODULE hD3D9 = NULL;
    for (int i = 0; i < 50; i++) {
        hD3D9 = GetModuleHandleA("d3d9.dll");
        if (hD3D9) break;
        Sleep(200);
    }
    if (!hD3D9) hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) { ff132_loga(FF132_TAG "overlay: d3d9 not found -> no overlay\n"); return; }

    Direct3DCreate9_t pDirect3DCreate9 =
        (Direct3DCreate9_t)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!pDirect3DCreate9) { ff132_loga(FF132_TAG "overlay: Direct3DCreate9 missing\n"); return; }
    // The frame hook may have adopted the game's vtable already: then no throwaway device is made
    // at all (creating one beside a live, possibly fullscreen, device is a risk with nothing to gain).
    if (g_adoptDone) {
        ff132_loga(FF132_TAG "overlay: installed through the game's own vtable (adopted first, no throwaway"
                      " device)\n");
        return;
    }

    void* d3d = pDirect3DCreate9(32);              // D3D_SDK_VERSION = 32
    if (!d3d) { ff132_loga(FF132_TAG "overlay: Direct3DCreate9 returned NULL\n"); return; }

    // Hidden throwaway window to own the dummy device.
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "FF132OverlayWnd";
    RegisterClassA(&wc);                            // dup registration is harmless
    HWND hwnd = CreateWindowExA(0, "FF132OverlayWnd", "ff132ovl", WS_OVERLAPPED,
                                0, 0, 32, 32, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        ((Release_t)(*(void***)d3d)[IUNK_VT_RELEASE])(d3d);
        ff132_loga(FF132_TAG "overlay: temp window creation failed\n");
        return;
    }

    FF132_D3DPP pp; memset(&pp, 0, sizeof(pp));
    pp.Windowed         = TRUE;
    pp.SwapEffect       = 1;                        // D3DSWAPEFFECT_DISCARD
    pp.BackBufferFormat = 0;                        // D3DFMT_UNKNOWN
    pp.BackBufferCount  = 1;
    pp.hDeviceWindow    = hwnd;

    // ONE attempt per device type -- no retry loop (the FF13 mod measured a retry loop crashing the
    // game inside here, where a single attempt merely logs "no overlay"). NULLREF is the fallback:
    // it touches neither adapter nor driver, and d3d9.dll hands every device type the one shared
    // vtable, so it donates the same slots.
    void** d3dvt = *(void***)d3d;
    CreateDevice_t pCreateDevice = (CreateDevice_t)d3dvt[16];   // IDirect3D9::CreateDevice
    void* dev = NULL;
    HRESULT hr = pCreateDevice(d3d, 0 /*DEFAULT*/, 1 /*HAL*/, hwnd,
                               0x20 /*SOFTWARE_VERTEXPROCESSING*/, &pp, &dev);
    if (hr < 0 || !dev) {
        dev = NULL;
        HRESULT hr2 = pCreateDevice(d3d, 0 /*DEFAULT*/, 4 /*NULLREF*/, hwnd,
                                    0x20 | 0x800 /*NOWINDOWCHANGES*/, &pp, &dev);
        ff132_loga(FF132_TAG "overlay: HAL CreateDevice failed hr=0x%08X -> NULLREF fallback %s (hr=0x%08X)\n",
               (unsigned)hr, (hr2 >= 0 && dev) ? "OK" : "FAIL", (unsigned)hr2);
        if (hr2 < 0 || !dev) {
            DestroyWindow(hwnd);
            UnregisterClassA("FF132OverlayWnd", wc.hInstance);
            ((Release_t)(*(void***)d3d)[IUNK_VT_RELEASE])(d3d);
            ff132_loga(FF132_TAG "overlay: no vtable donor -> no overlay\n");
            return;
        }
    }

    // The dummy's vtable survives releasing the dummy device. It is patched as the fallback, in
    // case the game's device does share it; the adopt step is what actually carries the hook. The
    // frame hook can adopt before this runs, so the patch takes the adopt gate, and once adopted
    // the throwaway is not touched at all.
    void** dvt = *(void***)dev;
    while (InterlockedCompareExchange(&g_adoptBusy, 1, 0) != 0) Sleep(1);
    int adoptedFirst = g_adoptDone, okEnd = 1;
    if (!adoptedFirst) {
        g_dummyVt = dvt;
        okEnd = patch_vtable_slot(dvt, D3DDEV_VT_ENDSCENE, (void*)hkEndScene, &g_dummyEndScene);
        patch_vtable_slot(dvt, D3DDEV_VT_RESET, (void*)hkReset, &g_dummyReset);
        g_dummyHooked = okEnd;
    }
    InterlockedExchange(&g_adoptBusy, 0);

    // Tear down the dummy device + window; the patched vtable persists.
    ((Release_t)dvt[IUNK_VT_RELEASE])(dev);
    DestroyWindow(hwnd);
    UnregisterClassA("FF132OverlayWnd", wc.hInstance);
    ((Release_t)(*(void***)d3d)[IUNK_VT_RELEASE])(d3d);

    if (adoptedFirst)
        ff132_loga(FF132_TAG "overlay: installed through the game's own vtable (adopted while the"
                      " throwaway device was made; it is left unpatched)\n");
    else if (okEnd)
        ff132_loga(FF132_TAG "overlay: installed (EndScene + Reset patched)\n");
    else
        ff132_loga(FF132_TAG "overlay: EndScene vtable patch failed -> no overlay\n");

    overlay_adopt_game_device();        // usually too early; ff132_overlay_poll keeps trying
}
