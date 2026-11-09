/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Undertone unit panel and display. Replaces the stock FM1 Open UI.
 * (Kept as screen.c: the fixed build gives this entry unit native, non-LTO
 * compilation, which the SDK's control loop requires.)
 *
 *   SELECT     module: VOICE / PERC / DRONE
 *   ALGORITHM  page within the module (the last three pages are FX, FX MOD, LFO)
 *   KNOB 1-4   the four values on screen
 *   PRESETS    presets for the module (or FX presets on the FX/LFO pages)
 *   HOME       first page          SCL  harmony (VOICE)
 *   ENV        envelope/gate page  EDIT next page
 *   FX         FX pages            LFO  LFO page
 *   OCT- OCT+  octave              PLAY HOLD (latch keys / drone voices)
 *   GLO        all notes off
 *   MASTER     output level */
#include "fm1.h"
#include "instrument.h"
#include "unit.h"
#include <string.h>

enum { OCT_DOWN, OCT_UP, B_FX, B_SCL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC };
#define BUTTON(b) (1U << (b))
enum { E_SELECT, E_ALGO, E_K1, E_K2, E_K3, E_K4, E_PRESETS };

/* ---------- value formats ---------- */
enum { FMT_NUM, FMT_BIP, FMT_DIV, FMT_ONOFF, FMT_MODE, FMT_SCALE, FMT_KEY, FMT_WAVE, FMT_COAST,
       FMT_PITCH, FMT_ROUTE, FMT_TARGET, FMT_SHAPE, FMT_COUNT9, FMT_DWAVE };
struct param { const char *name; uint8_t id, fmt; };
struct page { const char *title; struct param p[4]; };

static const struct page voice_pages[] = {
    {"TONE",     {{"WAVE", V_WAVE, FMT_WAVE}, {"FOLD", V_FOLD, FMT_NUM}, {"CUTOFF", V_CUTOFF, FMT_NUM}, {"RESO", V_RESO, FMT_NUM}}},
    {"COAST",    {{"COAST", V_COAST, FMT_COAST}, {"ENV AMT", V_ENVAMT, FMT_BIP}, {"DRIVE", V_DRIVE, FMT_NUM}, {"GLIDE", V_GLIDE, FMT_NUM}}},
    {"SUBS",     {{"SUB 1", V_DIV1, FMT_DIV}, {"SUB 2", V_DIV2, FMT_DIV}, {"SUB 3", V_DIV3, FMT_DIV}, {"CLOSE", V_CLOSE, FMT_ONOFF}}},
    {"LEVELS",   {{"MAIN", V_LVL0, FMT_NUM}, {"SUB 1", V_LVL1, FMT_NUM}, {"SUB 2", V_LVL2, FMT_NUM}, {"SUB 3", V_LVL3, FMT_NUM}}},
    {"HARMONY",  {{"MODE", V_MODE, FMT_MODE}, {"SCALE", V_SCALE, FMT_SCALE}, {"ROOT", V_ROOT, FMT_KEY}, {"DETUNE", V_DETUNE, FMT_BIP}}},
    {"ENVELOPE", {{"ATTACK", V_ATTACK, FMT_NUM}, {"DECAY", V_DECAY, FMT_NUM}, {"SUSTAIN", V_SUSTAIN, FMT_NUM}, {"RELEASE", V_RELEASE, FMT_NUM}}},
};
static const struct page perc_pages[] = {
    {"STRIKE",   {{"TUNE", P_TUNE, FMT_BIP}, {"RATIO", P_RATIO, FMT_NUM}, {"FM", P_FM, FMT_NUM}, {"FOLD", P_FOLD, FMT_NUM}}},
    {"GATE",     {{"DECAY", P_DECAY, FMT_NUM}, {"TONE", P_TONE, FMT_NUM}, {"SWEEP", P_SWEEP, FMT_NUM}, {"NOISE", P_NOISE, FMT_NUM}}},
    {"PLAY",     {{"SPREAD", P_SPREAD, FMT_NUM}, {"BOUNCE", P_BOUNCE, FMT_COUNT9}, {"VELO", P_VELO, FMT_NUM}, {"LEVEL", P_LEVEL, FMT_NUM}}},
};
static const struct page drone_pages[] = {
    {"PAIRS",    {{"PAIR 1", D_PITCH1, FMT_PITCH}, {"PAIR 2", D_PITCH2, FMT_PITCH}, {"PAIR 3", D_PITCH3, FMT_PITCH}, {"PAIR 4", D_PITCH4, FMT_PITCH}}},
    {"MOD",      {{"MOD", D_MOD, FMT_NUM}, {"ROUTE", D_ROUTE, FMT_ROUTE}, {"SPREAD", D_SPREAD, FMT_NUM}, {"SHARP", D_SHARP, FMT_NUM}}},
    {"VOICE",    {{"WAVE", D_WAVE, FMT_DWAVE}, {"LFO RATE", D_LFORATE, FMT_NUM}, {"LFO DEPTH", D_LFODEPTH, FMT_NUM}, {"LEVEL", D_LEVEL, FMT_NUM}}},
};
static const struct page fx_pages[] = {
    {"FX",       {{"DRIVE", F_DRIVE, FMT_NUM}, {"TIME", F_TIME, FMT_NUM}, {"FEEDBACK", F_FEEDBACK, FMT_NUM}, {"MIX", F_MIX, FMT_NUM}}},
    {"FX MOD",   {{"MOD RATE", F_MODRATE, FMT_NUM}, {"MOD DEPTH", F_MODDEPTH, FMT_NUM}, {"TONE", F_TONE, FMT_NUM}, {"SEND", F_SEND, FMT_NUM}}},
    {"LFO",      {{"RATE", F_LFORATE, FMT_NUM}, {"DEPTH", F_LFODEPTH, FMT_NUM}, {"TARGET", F_LFOTARGET, FMT_TARGET}, {"SHAPE", F_LFOSHAPE, FMT_SHAPE}}},
};
static const struct page *const module_pages[3] = {voice_pages, perc_pages, drone_pages};
static const uint8_t module_page_count[3] = {6, 3, 3};
static const char *const module_names[3] = {"VOICE", "PERC", "DRONE"};
static const uint16_t module_colors[4] = {0xFD20, 0x07B9, 0xB21F, 0x5FE8};   /* amber, teal, violet, green */

/* ---------- presets ---------- */
#define PRESETS 6
struct preset { const char *name; uint8_t v[UNIT_PARAMS]; };
static const struct preset voice_presets[PRESETS] = {
    {"UNDERTONE",   {0, 0, 80, 30,  0, 80, 0, 0,  23, 38, 0, 127,  100, 89, 76, 0,  127, 0, 0, 64,  10, 70, 90, 70}},
    {"WEST PLUCK",  {127, 60, 30, 10,  127, 64, 10, 0,  23, 38, 16, 127,  100, 70, 60, 50,  127, 0, 0, 64,  0, 50, 0, 60}},
    {"JI CHOIR",    {40, 0, 60, 15,  30, 70, 0, 20,  23, 38, 31, 0,  90, 80, 70, 60,  0, 0, 0, 70,  70, 90, 110, 100}},
    {"BUCHLA BELL", {110, 90, 20, 0,  110, 64, 0, 0,  46, 61, 0, 127,  100, 60, 50, 0,  64, 0, 0, 64,  0, 60, 0, 75}},
    {"SUB BASS",    {20, 0, 45, 50,  0, 90, 40, 30,  16, 23, 0, 0,  100, 100, 60, 0,  64, 0, 0, 64,  2, 60, 80, 40}},
    {"DORIAN PAD",  {64, 20, 70, 20,  60, 70, 0, 10,  23, 38, 53, 127,  90, 75, 65, 55,  127, 22, 22, 70,  80, 90, 100, 100}},
};
static const struct preset perc_presets[PRESETS] = {
    {"BONGO",       {70, 16, 40, 10,  40, 60, 50, 10,  40, 0, 80, 100}},
    {"WOOD",        {80, 70, 60, 30,  25, 80, 20, 20,  64, 0, 80, 100}},
    {"METAL",       {64, 110, 90, 50,  70, 90, 10, 5,  30, 0, 80, 90}},
    {"BOUNCE",      {60, 32, 50, 20,  35, 70, 40, 10,  50, 80, 80, 100}},
    {"KICK",        {20, 16, 20, 0,  50, 40, 100, 5,  20, 0, 80, 110}},
    {"NOISE SNARE", {64, 60, 30, 40,  30, 100, 30, 90,  30, 0, 80, 100}},
};
static const struct preset drone_presets[PRESETS] = {
    {"FIFTHS",      {44, 59, 70, 85,  30, 0, 15, 30,  20, 30, 20, 100}},
    {"BEATING",     {44, 44, 59, 59,  20, 0, 35, 20,  10, 20, 30, 100}},
    {"CHAOS",       {30, 52, 70, 90,  90, 70, 40, 60,  70, 60, 40, 100}},
    {"RING",        {40, 50, 60, 75,  60, 40, 20, 40,  40, 40, 30, 100}},
    {"PLUCKED",     {44, 59, 70, 85,  40, 0, 10, 120,  100, 30, 10, 100}},
    {"SLOW LFO",    {35, 47, 59, 71,  50, 110, 25, 15,  30, 10, 60, 100}},
};
static const struct preset fx_presets[PRESETS] = {
    {"CLEAN",       {0, 70, 40, 25,  30, 10, 100, 100,  40, 0, 0, 0}},
    {"DRONE SPACE", {30, 110, 85, 55,  20, 40, 70, 100,  30, 0, 0, 0}},
    {"DIRTY ECHO",  {90, 80, 70, 45,  40, 30, 55, 100,  40, 0, 0, 0}},
    {"TAPE WOBBLE", {20, 60, 60, 40,  60, 90, 80, 100,  40, 0, 0, 0}},
    {"VIBRATO",     {0, 60, 30, 20,  30, 10, 100, 100,  70, 30, 0, 0}},
    {"S+H TONE",    {10, 70, 50, 30,  30, 20, 90, 100,  80, 60, 43, 127}},
};
static const struct preset *const presets[MOD_COUNT] = {voice_presets, perc_presets, drone_presets, fx_presets};

/* ---------- state ---------- */
static uint8_t values[MOD_COUNT][UNIT_PARAMS];
static uint8_t preset_index[MOD_COUNT];
static unsigned module, page[3], octave_shift, hold;   /* octave_shift: 0..4, 2 = none */
static uint32_t previous_keys, previous_buttons, held_keys, latched_keys;
static uint8_t key_note[27];
static unsigned dirty, band, frame_again, drone_shown;
static const char *notice;
static char notice_text[24];
static uint32_t notice_until;
static uint32_t overruns_seen, health_at;
static unsigned led_cache_keys, led_cache_buttons, led_valid;
static uint16_t band_pixels[FM1_SCREEN_DMA_PIXELS];
static unsigned band_y;

/* ---------- font: 5x7, ASCII 32..95, column bytes, bit 0 = top ---------- */
static const uint8_t font[64][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},{0x00,0x40,0x34,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},
    {0x3E,0x41,0x5D,0x59,0x4E},{0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x41,0x51,0x73},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},
    {0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4D,0x43},{0x00,0x7F,0x41,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x41,0x7F},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
};

/* ---------- band painter ---------- */
#define BG      0x0841
#define TILE    0x18E3
#define BARBG   0x31A6
#define TEXT    0xFFFF
#define DIM     0x8C51
static void rect(int x, int y, int w, int h, uint16_t c)
{
    int y0 = y < (int)band_y ? (int)band_y : y;
    int y1 = y + h > (int)band_y + 16 ? (int)band_y + 16 : y + h;
    int x0 = x < 0 ? 0 : x, x1 = x + w > 240 ? 240 : x + w;
    for (int r = y0; r < y1; ++r) {
        uint16_t *row = band_pixels + (r - (int)band_y) * 240;
        for (int col = x0; col < x1; ++col) row[col] = c;
    }
}
static int text_width(const char *s, int scale) { int n = (int)strlen(s); return n ? n * 6 * scale - scale : 0; }
static void text(int x, int y, int scale, uint16_t c, const char *s)
{
    if (y >= (int)band_y + 16 || y + 7 * scale <= (int)band_y) return;
    for (; *s; ++s, x += 6 * scale) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = '?';
        const uint8_t *g = font[ch - 32];
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (g[col] & (1U << row)) rect(x + col * scale, y + row * scale, scale, scale, c);
    }
}
static void text_center(int cx, int y, int scale, uint16_t c, const char *s) { text(cx - text_width(s, scale) / 2, y, scale, c, s); }

/* ---------- formatting ---------- */
static void number(char *out, int v)
{
    char tmp[8]; int n = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    do { tmp[n++] = (char)('0' + u % 10U); u /= 10U; } while (u && n < 7);
    if (neg) *out++ = '-';
    while (n) *out++ = tmp[--n];
    *out = 0;
}
static const char *const note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void format(char *out, unsigned fmt, unsigned v)
{
    static const char *const modes[3] = {"JI", "12-ET", "SCALE"};
    static const char *const scale_names[12] = {"MAJOR", "MINOR", "DORIAN", "PHRYG", "LYDIAN", "MIXO",
                                                "HARM MIN", "MEL MIN", "MAJ PENT", "MIN PENT", "WHOLE", "OCTATON"};
    static const char *const routes[4] = {"PAIR", "RING", "CHAOS", "LFO"};
    static const char *const targets[3] = {"PITCH", "TONE", "FX TIME"};
    static const char *const shapes[4] = {"SINE", "TRI", "SQUARE", "S+H"};
    switch (fmt) {
    case FMT_BIP: number(out, (int)v - 64); break;
    case FMT_DIV: { unsigned d = v * 17U / 128U; if (!d) strcpy(out, "OFF"); else { out[0] = 'F'; out[1] = '/'; number(out + 2, (int)d); } break; }
    case FMT_ONOFF: strcpy(out, v >= 64 ? "ON" : "OFF"); break;
    case FMT_MODE: strcpy(out, modes[v * 3U / 128U]); break;
    case FMT_SCALE: strcpy(out, scale_names[v * 12U / 128U]); break;
    case FMT_KEY: strcpy(out, note_names[v * 12U / 128U]); break;
    case FMT_WAVE: strcpy(out, v < 21 ? "SAW" : v < 43 ? "SAW>SQR" : v < 64 ? "SQR" : v < 85 ? "SQR>TRI" : "TRI"); break;
    case FMT_DWAVE: strcpy(out, v < 21 ? "SINE" : v < 64 ? "SIN>TRI" : v < 85 ? "TRI" : "FOLDED"); break;
    case FMT_COAST: strcpy(out, v < 16 ? "EAST" : v > 111 ? "WEST" : v < 64 ? "EAST+W" : "WEST+E"); break;
    case FMT_PITCH: {
        unsigned n = 24U + (v * 60U + 63U) / 127U;
        strcpy(out, note_names[n % 12U]); number(out + strlen(out), (int)(n / 12U) - 1); break;
    }
    case FMT_ROUTE: strcpy(out, routes[v * 4U / 128U]); break;
    case FMT_TARGET: strcpy(out, targets[v * 3U / 128U]); break;
    case FMT_SHAPE: strcpy(out, shapes[v * 4U / 128U]); break;
    case FMT_COUNT9: { unsigned n = v * 9U / 128U; if (!n) strcpy(out, "OFF"); else number(out, (int)n); break; }
    default: number(out, (int)v); break;
    }
}

/* ---------- pages ---------- */
static unsigned page_count(void) { return module_page_count[module] + 3U; }
static const struct page *current_page(unsigned *owner)
{
    unsigned p = page[module], n = module_page_count[module];
    if (p < n) { *owner = module; return &module_pages[module][p]; }
    *owner = MOD_FX;
    return &fx_pages[p - n];
}
static void show(const char *text_, uint32_t now, uint32_t ms)
{
    notice = text_; notice_until = now + ms; dirty = 1;
}
static void set_value(unsigned owner, unsigned id, int v)
{
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    if (values[owner][id] == (uint8_t)v) return;
    values[owner][id] = (uint8_t)v;
    (void)unit_param(SRC_PANEL, owner, id, (unsigned)v);
    dirty = 1;
}
static void load_preset(unsigned owner, unsigned index, uint32_t now)
{
    const struct preset *pr = &presets[owner][index];
    preset_index[owner] = (uint8_t)index;
    for (unsigned i = 0; i < UNIT_PARAMS; ++i) {
        values[owner][i] = pr->v[i];
        (void)unit_param(SRC_PANEL, owner, i, pr->v[i]);
    }
    show(pr->name, now, 1500U);
}

/* ---------- drawing ---------- */
static void draw_band(void)
{
    unsigned owner;
    const struct page *pg = current_page(&owner);
    uint16_t accent = module_colors[owner == MOD_FX ? 3 : module];
    char buf[24];
    rect(0, (int)band_y, 240, 16, BG);
    /* header */
    rect(0, 0, 240, 34, module_colors[module]);
    text(8, 6, 3, 0x0000, module_names[module]);
    if (hold) { rect(170, 6, 62, 22, 0x0000); text(178, 10, 2, module_colors[module], "HOLD"); }
    else if (module != MOD_DRONE && octave_shift != 2U) {
        buf[0] = 'O'; buf[1] = 'C'; buf[2] = 'T'; buf[3] = ' ';
        int o = (int)octave_shift - 2; if (o > 0) { buf[4] = '+'; number(buf + 5, o); } else number(buf + 4, o);
        text(232 - text_width(buf, 2), 10, 2, 0x0000, buf);
    }
    /* page title */
    text(8, 42, 2, accent, pg->title);
    number(buf, (int)page[module] + 1); strcat(buf, "/"); number(buf + strlen(buf), (int)page_count());
    text(232 - text_width(buf, 2), 42, 2, DIM, buf);
    /* four tiles */
    for (unsigned i = 0; i < 4; ++i) {
        int x = (i & 1U) ? 122 : 4, y = (i & 2U) ? 144 : 62;
        const struct param *p = &pg->p[i];
        unsigned v = values[owner][p->id];
        rect(x, y, 114, 78, TILE);
        number(buf, (int)i + 1);
        text(x + 6, y + 6, 1, accent, buf);
        text(x + 16, y + 6, 2, DIM, p->name);
        format(buf, p->fmt, v);
        text_center(x + 57, y + 30, text_width(buf, 3) <= 108 ? 3 : 2, TEXT, buf);
        rect(x + 6, y + 64, 102, 6, BARBG);
        rect(x + 6, y + 64, (int)(v * 102U / 127U), 6, accent);
    }
    /* footer: notice, else preset name; drone shows its eight voices */
    if (module == MOD_DRONE && !notice) {
        unsigned active = unit_drone_active();
        for (unsigned i = 0; i < 8; ++i)
            rect(8 + (int)i * 29, 228, 24, 8, (active & (1U << i)) ? accent : BARBG);
    } else {
        const char *line = notice ? notice : presets[owner][preset_index[owner]].name;
        text_center(120, 226, 2, notice ? TEXT : DIM, line);
    }
}
static void paint(void)
{
    if (dirty && band >= 15U) { band = 0; dirty = 0; }
    else if (dirty) frame_again = 1;
    for (unsigned n = 0; n < 2 && band < 15U; ++n) {
        if (fm1_screen_pending() > 0) return;
        band_y = band * 16U;
        draw_band();
        if (fm1_screen_submit(0, band_y, 240, 16, band_pixels, FM1_SCREEN_DMA_PIXELS) == 1) return;
        ++band;
    }
    if (band >= 15U && frame_again) { frame_again = 0; dirty = 1; }
}

/* ---------- notes ---------- */
static unsigned key_to_note(unsigned key)
{
    if (module == MOD_DRONE) return 53U + key;
    int n = 53 + (int)key + ((int)octave_shift - 2) * 12;
    return n < 0 ? 0U : n > 127 ? 127U : (unsigned)n;
}
static unsigned drone_key(unsigned key)
{
    static const uint8_t white[8] = {0, 2, 4, 6, 7, 9, 11, 12};
    for (unsigned i = 0; i < 8; ++i) if (white[i] == key) return 1;
    return 0;
}
static void release_keys(void)
{
    for (unsigned k = 0; k < 27; ++k)
        if (key_note[k] != 0xff) { (void)unit_note(SRC_PANEL, module, key_note[k], 0); key_note[k] = 0xff; }
    held_keys = 0;
}
static void handle_keys(uint32_t keys)
{
    uint32_t down = keys & ~previous_keys, up = previous_keys & ~keys;
    for (unsigned k = 0; k < 27; ++k) {
        uint32_t bit = 1U << k;
        if (module == MOD_DRONE) {
            if (!drone_key(k)) continue;
            if (hold) {
                if (down & bit) {
                    latched_keys ^= bit;
                    (void)unit_note(SRC_PANEL, MOD_DRONE, 53U + k, (latched_keys & bit) ? 100U : 0U);
                }
            } else {
                if (down & bit) { (void)unit_note(SRC_PANEL, MOD_DRONE, 53U + k, 100U); key_note[k] = (uint8_t)(53U + k); }
                if ((up & bit) && key_note[k] != 0xff) { (void)unit_note(SRC_PANEL, MOD_DRONE, key_note[k], 0); key_note[k] = 0xff; }
            }
            continue;
        }
        if (down & bit) {
            if (hold && (held_keys & bit)) {   /* tap again to release a held note */
                (void)unit_note(SRC_PANEL, module, key_note[k], 0); key_note[k] = 0xff; held_keys &= ~bit; continue;
            }
            unsigned note = key_to_note(k);
            (void)unit_note(SRC_PANEL, module, note, 100U);
            key_note[k] = (uint8_t)note;
            if (hold) held_keys |= bit;
        }
        if ((up & bit) && !hold && key_note[k] != 0xff) {
            (void)unit_note(SRC_PANEL, module, key_note[k], 0); key_note[k] = 0xff;
        }
    }
}
static void set_hold(unsigned on, uint32_t now)
{
    if (hold == on) return;
    hold = on;
    if (!on) {
        release_keys();
        for (unsigned k = 0; k < 27; ++k)
            if (latched_keys & (1U << k)) (void)unit_note(SRC_PANEL, MOD_DRONE, 53U + k, 0);
        latched_keys = 0;
    }
    show(on ? "HOLD ON" : "HOLD OFF", now, 900U);
}
static void select_module(int delta, uint32_t now)
{
    int m = (int)module + delta;
    if (m < 0) m = 0;
    if (m > 2) m = 2;
    if ((unsigned)m == module) return;
    release_keys();          /* latched drone voices keep sounding */
    module = (unsigned)m;
    unit_midi_module = module;
    (void)now;
    notice = 0;
    dirty = 1;
}
static void goto_page(unsigned p) { if (p < page_count()) { page[module] = p; dirty = 1; } }

/* ---------- LEDs ---------- */
static void update_leds(uint32_t keys)
{
    uint32_t lit = 0;
    if (module == MOD_DRONE) {
        unsigned active = unit_drone_active();
        static const uint8_t white[8] = {0, 2, 4, 6, 7, 9, 11, 12};
        for (unsigned i = 0; i < 8; ++i) if (active & (1U << i)) lit |= 1U << white[i];
        lit |= keys & 0x1AD5U;   /* the eight drone keys while pressed */
    } else lit = keys | held_keys;
    uint32_t buttons = (hold ? BUTTON(B_PLAY) : 0U);
    if (led_valid && lit == led_cache_keys && buttons == led_cache_buttons) return;
    for (unsigned k = 0; k < 27; ++k)
        if (!led_valid || ((lit ^ led_cache_keys) & (1U << k))) fm1_led_set(14U + k, (lit >> k) & 1U);
    for (unsigned b = 0; b < 14; ++b)
        if (!led_valid || ((buttons ^ led_cache_buttons) & (1U << b))) fm1_led_set(b, (buttons >> b) & 1U);
    led_cache_keys = lit; led_cache_buttons = buttons; led_valid = 1;
}

/* ---------- entry points ---------- */
/* The fixed build requires the Bluetooth services to stay linked even though
 * this UI has no Bluetooth menu (BLE MIDI still works with its defaults). */
static const void *const keep_linked[] = {
    (const void *)fm1_ble_set_role, (const void *)fm1_ble_scan, (const void *)fm1_ble_connect,
    (const void *)fm1_ble_disconnect, (const void *)fm1_ble_get_status, (const void *)fm1_ble_get_device,
    (const void *)fm1_ble_forget_bonds, (const void *)fm1_bluetooth_configure, (const void *)fm1_bluetooth_get_status,
};
static const void *const *volatile keep_anchor;
void groove_screen_init(void)
{
    keep_anchor = keep_linked;
    unit_init();
    for (unsigned m = 0; m < MOD_COUNT; ++m)
        for (unsigned i = 0; i < UNIT_PARAMS; ++i) values[m][i] = unit_defaults[m][i];
    memset(preset_index, 0, sizeof preset_index);
    memset(key_note, 0xff, sizeof key_note);
    module = MOD_VOICE; page[0] = page[1] = page[2] = 0;
    octave_shift = 2; hold = 0;
    previous_keys = previous_buttons = held_keys = latched_keys = 0;
    notice = "UNDERTONE"; notice_until = fm1_millis() + 1500U;
    led_valid = 0;
    fm1_controls_init();
    for (unsigned key = 0; key < 41; ++key) fm1_led_set(key, 0);
    fm1_screen_init();
    struct fm1_audio_stats dma; fm1_audio_stats_read(&dma); overruns_seen = dma.overruns;
    health_at = fm1_millis();
    band = 15; dirty = 1; frame_again = 0;
    paint();
}
static int accelerate(int d) { int a = d < 0 ? -d : d; return a >= 3 ? d * 4 : a == 2 ? d * 2 : d; }
void groove_screen_control(void)
{
    struct fm1_controls c;
    fm1_controls_read(&c);
    uint32_t now = fm1_millis();
    uint32_t pressed = c.buttons & ~previous_buttons;

    if (groove_master_update(c.knob)) {
        unsigned level = c.knob >= 1016U ? 100U : c.knob * 100U / 1016U;
        strcpy(notice_text, "MASTER "); number(notice_text + 7, (int)level);
        show(notice_text, now, 1000U);
    }
    if (now - health_at >= 500U) {
        struct fm1_audio_stats dma; fm1_audio_stats_read(&dma);
        if (dma.overruns != overruns_seen) show("AUDIO OVERLOAD", now, 2000U);
        overruns_seen = dma.overruns; health_at = now;
    }

    /* buttons */
    if (pressed & BUTTON(OCT_DOWN)) { if (octave_shift) --octave_shift; dirty = 1; }
    if (pressed & BUTTON(OCT_UP)) { if (octave_shift < 4U) ++octave_shift; dirty = 1; }
    if (pressed & BUTTON(B_PLAY)) set_hold(!hold, now);
    if (pressed & BUTTON(B_GLO)) {
        release_keys(); latched_keys = 0; hold = 0;
        (void)unit_all_off(SRC_PANEL); show("ALL NOTES OFF", now, 900U);
    }
    if (pressed & BUTTON(B_HOME)) goto_page(0);
    if (pressed & BUTTON(B_EDIT)) goto_page((page[module] + 1U) % page_count());
    if (pressed & BUTTON(B_SCL)) goto_page(module == MOD_VOICE ? 4U : 0U);
    if (pressed & BUTTON(B_ENV)) goto_page(module == MOD_VOICE ? 5U : 1U);
    if (pressed & BUTTON(B_FX)) {
        unsigned fx0 = module_page_count[module];
        goto_page(page[module] == fx0 ? fx0 + 1U : fx0);
    }
    if (pressed & BUTTON(B_LFO)) goto_page(module_page_count[module] + 2U);
    if (pressed & (BUTTON(B_SAVE) | BUTTON(B_ARP) | BUTTON(B_SEQ) | BUTTON(B_REC)))
        show("NOT USED YET", now, 900U);

    /* encoders */
    if (c.encoders[E_SELECT]) select_module(c.encoders[E_SELECT] > 0 ? 1 : -1, now);
    if (c.encoders[E_ALGO]) {
        int p = (int)page[module] + (c.encoders[E_ALGO] > 0 ? 1 : -1);
        if (p >= 0 && p < (int)page_count()) goto_page((unsigned)p);
    }
    unsigned owner;
    const struct page *pg = current_page(&owner);
    for (unsigned i = 0; i < 4; ++i) {
        int d = c.encoders[E_K1 + i];
        if (!d) continue;
        unsigned id = pg->p[i].id;
        set_value(owner, id, (int)values[owner][id] + accelerate(d));
    }
    if (c.encoders[E_PRESETS]) {
        int p = (int)preset_index[owner] + (c.encoders[E_PRESETS] > 0 ? 1 : -1);
        if (p < 0) p = PRESETS - 1;
        if (p >= PRESETS) p = 0;
        load_preset(owner, (unsigned)p, now);
    }

    handle_keys(c.keys);
    if (notice && (int32_t)(now - notice_until) >= 0) { notice = 0; dirty = 1; }
    if (module == MOD_DRONE) {
        unsigned active = unit_drone_active();
        if (active != drone_shown) { drone_shown = active; dirty = 1; }
    }
    update_leds(c.keys);
    previous_keys = c.keys;
    previous_buttons = c.buttons;
    paint();
}

/* ---------- hooks kept for the SDK, transfer protocol and browser preview ---------- */
void groove_screen_mmc(unsigned command) { (void)command; }
void groove_screen_tap(uint32_t now) { (void)now; }
void groove_screen_snapshot(uint8_t state[8])
{
    if (!state) return;
    memset(state, 0, 8);
    state[0] = 0; state[1] = (uint8_t)page[module]; state[3] = (uint8_t)module;
}
int groove_screen_open(unsigned target) { (void)target; dirty = 1; return 0; }
int groove_screen_preset(unsigned slot)
{
    if (slot >= PRESETS) return -1;
    load_preset(module, slot, fm1_millis());
    return 0;
}
unsigned groove_screen_preset_position(void) { return preset_index[module]; }
const char *groove_screen_web_knobs(void) { return ""; }
unsigned groove_screen_web_first(void) { return 0; }
int groove_screen_web_step(unsigned step, unsigned toggle) { (void)step; (void)toggle; return -1; }
const void *groove_screen_web_targets(void) { return 0; }
unsigned groove_screen_web_target_count(void) { return 0; }
unsigned groove_screen_web_context(void) { return 0; }
unsigned groove_screen_web_mask(void) { return 0; }
int groove_screen_web_adjust(unsigned context, unsigned action, unsigned index, unsigned owner, int delta)
{ (void)context; (void)action; (void)index; (void)owner; (void)delta; return -1; }
void groove_screen_web_fine(unsigned fine) { (void)fine; }
