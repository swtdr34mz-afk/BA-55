/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55: Bedienung, Seiten, Presets, Sequencer-Editor, Bildschirm. Teil des Unity-Builds (device/unity.c,
 * host/plat_host.c), nach der Plattform: gfx.c und announce.c sind sichtbar. Der Klang ist dsp/ba55_core.c.
 *
 * Bedienung (das Panel der DB-01, auf die Knöpfe des FM-1 verteilt)
 *   K1..K4     die vier Regler der Seite
 *   HOME       VCO: WAVE, SUB, NOISE, DETUNE
 *   SEL        VCF: CUTOFF, RESON, ENV MOD, DECAY; nochmal: MODE (LP/BP), ACCENT, KEY TRACK, HP CUT
 *   ENV        VCA ENV (A D S R), nochmal: VCF ENV, nochmal: PITCH ENV (AMOUNT, DECAY, TUNE, SLIDE)
 *   LFO        M1, M1 ZIELE, M2, M2 ZIELE, M3, M3 ZIELE (nacheinander)
 *   EDIT       SET: GLIDE, GLIDE MODE, LEGATO, VCA LVL; nochmal: VEL>CUT, VEL>VCA, HP RES, DRIVE
 *   FX         FX1, FX2, FX3, FX+ (nacheinander)
 *   GLO        TEMPO, DELAY SYNC; nochmal: die globalen Einstellungen
 *   SEQ        der Sequencer: STEP, NOTE, GATE, ACC/SLIDE; nochmal: LENG, DVS, PROB, SWING
 *   SELECT     CUTOFF (immer)         ALGORITHM   DRIVE          PRESET   die 16 Plätze
 *   PLAY       Sequencer Start / Stop REC   auf den SEQ-Seiten: RECORD; sonst 1 s halten: das Werks-Preset
 *   SAVE       speichern: PRESET wählt den Platz, SAVE nochmal speichert
 *   ARP        HOLD; auf SEQUENCER: Step kopieren / einfügen; auf SEQ SETUP: ein Zufalls-Pattern (RANDOM)
 *   OCT- OCT+  die Oktave der Tasten
 *   Tasten     spielen (mit Anschlag); läuft der Sequencer, transponieren sie ihn (C3 = unverändert);
 *              bei RECORD schreibt ein harter Anschlag einen ACCENT */
#include "fastmath.h"
#include "ba55_core.h"

#define SR 44100.0f

static ba_engine eng;
static ba_patch edit;                    /* der Klang in Arbeit (den die Engine spielt) */

/* ------------------------------------------------------------ Speicher --- */
#define SET_MAGIC 0x31354142u            /* "BA51" */
#define BANK_MAGIC 0x42354142u           /* "BA5B" */
#define NSLOTS 16
typedef struct {
    uint32_t magic;
    uint8_t slot, edited, oct, bend, mch, touch, rsv[2];
    ba_patch edit;
} settings_t;
typedef struct {
    uint32_t magic;
    ba_patch p[8];
} bank_t;
_Static_assert(sizeof(settings_t) <= PLAT_STORE_MAX, "die Einstellungen passen nicht in ein Speicherobjekt");
_Static_assert(sizeof(bank_t) <= PLAT_STORE_MAX, "eine Bank passt nicht in ein Speicherobjekt");
static settings_t set;
static bank_t bank[2];
static uint8_t set_dirty;
static uint32_t dirty_at;

static int patch_ok(const ba_patch *p)
{
    int i, m;
    for (i = 0; i < BA_NP; i++)
        if (p->p[i] > 127u)
            return 0;
    for (m = 0; m < 3; m++)
        for (i = 0; i < BA_NDEST; i++)
            if (p->lvl[m][i] > 127u)
                return 0;
    for (i = 0; i < BA_STEPS; i++)
        if (p->seq[i].note < 12u || p->seq[i].note > 108u || p->seq[i].gate > 127u || p->seq[i].fl > 3u)
            return 0;
    return p->bpm >= 30u && p->bpm <= 300u && p->name[BA_NAMELEN - 1] == 0;
}

static ba_patch *slot_patch(int s) { return &bank[s >= 8].p[s & 7]; }

static void bank_load(int b)
{
    int i, ok = plat_store_load(OBJ_BANK_A + (uint32_t)b, &bank[b], sizeof bank[b]) == (int)sizeof bank[b]
                && bank[b].magic == BANK_MAGIC;
    for (i = 0; ok && i < 8; i++)
        ok = patch_ok(&bank[b].p[i]);
    if (!ok) {                           /* noch nichts gespeichert: die Werks-Presets */
        bank[b].magic = BANK_MAGIC;
        for (i = 0; i < 8; i++)
            ba_preset(&bank[b].p[i], b * 8 + i);
    }
}

static void dirty(void)
{
    set_dirty = 1;
    dirty_at = plat_ms();
}

static void edited(void)                 /* der Klang wurde geändert (noch nicht gespeichert) */
{
    set.edited = 1;
    ba_changed(&eng);
    dirty();
}

static void settings_load(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC
        || set.slot >= NSLOTS || !patch_ok(&set.edit) || set.oct > 6u || set.bend < 1u || set.bend > 12u
        || set.mch > 16u) {
        memset(&set, 0, sizeof set);
        set.magic = SET_MAGIC;
        set.oct = 3;                     /* 3 = keine Verschiebung: die Tasten F2 .. G4 */
        set.bend = 2;
        set.touch = 1;
        set.edit = *slot_patch(0);
    }
    edit = set.edit;
}

static void settings_save(void)
{
    set.magic = SET_MAGIC;
    set.edit = edit;
    if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
        set_dirty = 0;
}

/* ---------------------------------------------- an die Engine (aus der Hauptschleife) --- */
/* die Ereignisse gehen in einen Ring, den der Audio-Interrupt leert; MIDI schreibt im Interrupt hinein, darum
 * hier mit gesperrten Interrupts */
static void e_on(int note, int vel, int force)
{
    uint32_t f = plat_lock();
    if (force)
        ba_note_on_play(&eng, note, vel);
    else
        ba_note_on(&eng, note, vel);
    plat_unlock(f);
}
static void e_off(int note)
{
    uint32_t f = plat_lock();
    ba_note_off(&eng, note);
    plat_unlock(f);
}
static void e_play(int on)
{
    uint32_t f = plat_lock();
    ba_play(&eng, on);
    plat_unlock(f);
}
static void e_alloff(void)
{
    uint32_t f = plat_lock();
    ba_all_off(&eng);
    plat_unlock(f);
}
static void e_hold(int on)
{
    uint32_t f = plat_lock();
    ba_hold(&eng, on);
    plat_unlock(f);
}

/* ------------------------------------------------------------------ Seiten --- */
enum { PG_MAIN, PG_FILTER, PG_FILT2, PG_AENV, PG_FENV, PG_PENV, PG_M1, PG_M1D, PG_M2, PG_M2D, PG_M3, PG_M3D,
       PG_SET1, PG_SET2, PG_FX1, PG_FX2, PG_FX3, PG_FXP, PG_GLO1, PG_GLO2, PG_SEQ1, PG_SEQ2, NPAGES };
static const char *const PAGE_NAME[NPAGES] = {"VCO", "VCF", "VCF 2", "VCA ENV", "VCF ENV", "PITCH ENV", "M1",
                                              "M1 DEST", "M2", "M2 DEST", "M3", "M3 DEST", "SET", "SET 2",
                                              "FX1 DIST", "FX2 ENHANCE", "FX3 REFLECT", "FX MORE", "GLOBAL",
                                              "SETTINGS", "SEQUENCER", "SEQ SETUP"};

enum { K_NONE, K_PARAM, K_LVL, K_DEST, K_MSEL, K_MVAL, K_SSEL, K_SNOTE, K_SGATE, K_SFL, K_TEMPO, K_BEND, K_MCH,
       K_TOUCH };
typedef struct { uint8_t kind, id; } kdef;

static const kdef PAGE_K[NPAGES][4] = {
    {{K_PARAM, P_WAVE}, {K_PARAM, P_SUB}, {K_PARAM, P_NOISE}, {K_PARAM, P_DETUNE}},
    {{K_PARAM, P_CUTOFF}, {K_PARAM, P_RESON}, {K_PARAM, P_EGLVL}, {K_PARAM, P_FD}},
    {{K_PARAM, P_FMODE}, {K_PARAM, P_ACCENT}, {K_PARAM, P_TRACK}, {K_PARAM, P_HPCUT}},
    {{K_PARAM, P_AA}, {K_PARAM, P_AD}, {K_PARAM, P_AS}, {K_PARAM, P_AR}},
    {{K_PARAM, P_FA}, {K_PARAM, P_FD}, {K_PARAM, P_FS}, {K_PARAM, P_FR}},
    {{K_PARAM, P_PENV}, {K_PARAM, P_PDEC}, {K_PARAM, P_TUNE}, {K_PARAM, P_SLIDE}},
    {{0}}, {{0}}, {{0}}, {{0}}, {{0}}, {{0}},          /* die Modulatoren: nach ihrem Modus (kdef_of) */
    {{K_PARAM, P_GLIDE}, {K_PARAM, P_GMODE}, {K_PARAM, P_LEGATO}, {K_PARAM, P_VCALVL}},
    {{K_PARAM, P_VELCUT}, {K_PARAM, P_VELVCA}, {K_PARAM, P_HPRES}, {K_PARAM, P_DRIVE}},
    {{K_PARAM, P_X1TYPE}, {K_PARAM, P_X1GAIN}, {K_PARAM, P_X1LVL}, {K_PARAM, P_X1MIX}},
    {{K_PARAM, P_X2TYPE}, {K_PARAM, P_X2DEPTH}, {K_PARAM, P_X2SPD}, {K_PARAM, P_X2MIX}},
    {{K_PARAM, P_X3TYPE}, {K_PARAM, P_X3P1}, {K_PARAM, P_X3P2}, {K_PARAM, P_X3MIX}},
    {{K_PARAM, P_X2WIDTH}, {K_PARAM, P_X2FEED}, {K_PARAM, P_X3FEED}, {K_PARAM, P_X3DAMP}},
    {{K_TEMPO, 0}, {K_PARAM, P_X3SYNC}, {K_NONE, 0}, {K_NONE, 0}},
    {{K_BEND, 0}, {K_MCH, 0}, {K_TOUCH, 0}, {K_NONE, 0}},
    {{K_SSEL, 0}, {K_SNOTE, 0}, {K_SGATE, 0}, {K_SFL, 0}},
    {{K_PARAM, P_SLEN}, {K_PARAM, P_SDVS}, {K_PARAM, P_SPROB}, {K_PARAM, P_SSWING}},
};

enum { R_HEAD = 1, R_MID = 2, R_OLED = 4, R_ALL = 7 };   /* was neu gezeichnet wird */

static struct {
    char msg[2][24];
    uint32_t msg_until;
    char big[2][20];
    uint32_t big_until;
    uint32_t knob_at[4], enc_at[NE];
    uint32_t keys, btn;
    uint8_t page, playing, hold, redraw;
    uint8_t intro;                       /* die Eröffnung läuft */
    uint32_t intro_at, frame;
    int32_t peak_drawn;
    uint8_t dest[3], msel[3];            /* je Modulator: das Ziel und der Step, die gerade bearbeitet werden */
    uint8_t ssel;                        /* der Step im Sequencer-Editor */
    uint8_t rec;                         /* RECORD auf den SEQ-Seiten */
    int8_t copied;                       /* ein kopierter Step (-1: keiner) */
    ba_step clip;
    uint8_t saving, save_slot;           /* SAVE: der Platz wird gewählt */
    uint32_t save_until;
    uint32_t rec_t;                      /* REC gehalten seit */
    int8_t key_note[NKEYS];              /* die Note jeder gedrückten Taste */
    int32_t step_drawn, lvl_drawn, note_drawn;
    uint32_t load_at, load_shown;
    uint8_t hot;
    float scope[3][96];                  /* die Ausgänge der Modulatoren, zuletzt */
    uint8_t scope_pos;
} ui;

static float fabsf_(float x) { return x < 0.0f ? -x : x; }

static uint32_t txt_len(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void dec1_text(float v, char *b)  /* eine Nachkommastelle */
{
    int32_t d = (int32_t)(v * 10.0f + (v < 0.0f ? -0.5f : 0.5f));
    if (d < 0) {
        *b++ = '-';
        d = -d;
    }
    int_text(d / 10, b);
    text_cat(b, ".");
    int_text(d % 10, b + txt_len(b));
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1200u;
    ui.redraw |= R_ALL;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void say_big(const char *a, const char *b)
{
    uint32_t i;
    for (i = 0; i < 19u && a[i]; i++)
        ui.big[0][i] = a[i];
    ui.big[0][i] = 0;
    for (i = 0; i < 19u && b[i]; i++)
        ui.big[1][i] = b[i];
    ui.big[1][i] = 0;
    ui.big_until = plat_ms() + 900u;
    ui.redraw |= R_ALL;
    if (ann_on()) {
        char t[44] = "";
        text_cat(t, ui.big[1]);
        text_cat(t, " ");
        text_cat(t, ui.big[0]);
        ann_say(t, 0, ANN_NOW);
    }
}

/* die Note als Name: C3 = MIDI 60 */
static void note_text(int n, char *t)
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    t[0] = 0;
    text_cat(t, NN[n % 12]);
    int_text(n / 12 - 2, t + txt_len(t));
}

static void ms_text(float ms, char *t)
{
    t[0] = 0;
    if (ms < 1000.0f) {
        int_text((int32_t)(ms + 0.5f), t);
        text_cat(t, "MS");
    } else {
        dec1_text(ms / 1000.0f, t);
        text_cat(t, "S");
    }
}
static void hz_text(float hz, char *t)
{
    t[0] = 0;
    if (hz < 10.0f) {
        dec1_text(hz, t);
        text_cat(t, "HZ");
    } else if (hz < 1000.0f) {
        int_text((int32_t)(hz + 0.5f), t);
        text_cat(t, "HZ");
    } else {
        dec1_text(hz / 1000.0f, t);
        text_cat(t, "K");
    }
}

/* ---------------------------------------------------------------- die Regler --- */
static int mod_of_page(int pg) { return pg >= PG_M1 && pg <= PG_M3D ? (pg - PG_M1) / 2 : -1; }
static int mode_of(int m) { return ba_list_index(P_MOD(m) + MO_MODE, edit.p[P_MOD(m) + MO_MODE]); }
static int scount(int m) { return ba_list_index(P_MOD(m) + MO_SCOUNT, edit.p[P_MOD(m) + MO_SCOUNT]) + 1; }

static kdef kdef_of(int k)
{
    kdef d = {K_NONE, 0};
    int m = mod_of_page(ui.page);
    if (m < 0)
        return PAGE_K[ui.page][k];
    if ((ui.page - PG_M1) & 1) {         /* die Ziele: DEST, LVL, SYNC */
        static const uint8_t KD[4] = {K_DEST, K_LVL, K_PARAM, K_NONE};
        d.kind = KD[k];
        d.id = d.kind == K_PARAM ? (uint8_t)(P_MOD(m) + MO_SYNC) : (uint8_t)m;
        return d;
    }
    if (k == 0) {
        d.kind = K_PARAM;
        d.id = (uint8_t)(P_MOD(m) + MO_MODE);
        return d;
    }
    switch (mode_of(m)) {
    case MODE_LFO: d.kind = K_PARAM; d.id = (uint8_t)(P_MOD(m) + MO_LRATE + k - 1); break;
    case MODE_ENV: d.kind = K_PARAM; d.id = (uint8_t)(P_MOD(m) + MO_EA + k - 1); break;
    case MODE_RND: d.kind = K_PARAM; d.id = (uint8_t)(P_MOD(m) + MO_RRATE + k - 1); break;
    default:                             /* STEP: #, STEP, ->LVL */
        d.kind = k == 1 ? K_PARAM : k == 2 ? K_MSEL : K_MVAL;
        d.id = k == 1 ? (uint8_t)(P_MOD(m) + MO_SCOUNT) : (uint8_t)m;
        break;
    }
    return d;
}

/* der Name eines Reglers (die FX3-Regler heißen je nach Typ anders) */
static const char *knob_label(kdef d)
{
    switch (d.kind) {
    case K_PARAM:
        if (d.id == P_X3P1 || d.id == P_X3P2) {
            int t = ba_list_index(P_X3TYPE, edit.p[P_X3TYPE]);
            if (t == X3_DELAY)
                return d.id == P_X3P1 ? "TIME L" : "TIME R";
            if (t == X3_PING)
                return d.id == P_X3P1 ? "TIME" : "-";
            return d.id == P_X3P1 ? "PRE" : "SIZE";
        }
        if (d.id == P_X1GAIN && ba_list_index(P_X1TYPE, edit.p[P_X1TYPE]) == X1_CRUSH)
            return "RATE";
        if (d.id == P_X1LVL && ba_list_index(P_X1TYPE, edit.p[P_X1TYPE]) == X1_CRUSH)
            return "BITS";
        if (d.id == P_X3SYNC)
            return "DLY SYNC";
        if (ui.page == PG_FXP) {             /* FX MORE: welcher Block */
            if (d.id == P_X2WIDTH)
                return "FX2 WIDTH";
            if (d.id == P_X2FEED)
                return "FX2 FEED";
            if (d.id == P_X3FEED)
                return "FX3 FEED";
            if (d.id == P_X3DAMP)
                return "FX3 DAMP";
        }
        return BA_PARAMS[d.id].label;
    case K_LVL: return "LVL";
    case K_DEST: return "DEST";
    case K_MSEL: return "STEP";
    case K_MVAL: return "->LVL";
    case K_SSEL: return "STEP";
    case K_SNOTE: return "NOTE";
    case K_SGATE: return "GATE";
    case K_SFL: return "ACC/SLIDE";
    case K_TEMPO: return "TEMPO";
    case K_BEND: return "BEND";
    case K_MCH: return "MIDI CH";
    case K_TOUCH: return "VELOCITY";
    default: return "";
    }
}

/* der Wert eines Reglers als Zahl (0..127 für die Anzeige des Bogens), -1: keiner */
static int knob_get(kdef d)
{
    switch (d.kind) {
    case K_PARAM: return edit.p[d.id];
    case K_LVL: return edit.lvl[d.id][ui.dest[d.id]];
    case K_DEST: return ui.dest[d.id] * 127 / (BA_NDEST - 1);
    case K_MSEL: return scount(d.id) > 1 ? ui.msel[d.id] * 127 / (scount(d.id) - 1) : 0;
    case K_MVAL: return edit.mstep[d.id][ui.msel[d.id]];
    case K_SSEL: return ui.ssel * 127 / (BA_STEPS - 1);
    case K_SNOTE: return (edit.seq[ui.ssel].note - 24) * 127 / 72;
    case K_SGATE: return edit.seq[ui.ssel].gate;
    case K_SFL: return edit.seq[ui.ssel].fl * 127 / 3;
    case K_TEMPO: return ((int)edit.bpm - 30) * 127 / 270;
    case K_BEND: return (set.bend - 1) * 127 / 11;
    case K_MCH: return set.mch * 127 / 16;
    case K_TOUCH: return set.touch ? 127 : 0;
    default: return -1;
    }
}

static int knob_bip(kdef d)
{
    if (d.kind == K_LVL)
        return 1;
    return d.kind == K_PARAM && BA_PARAMS[d.id].kind == MK_BIP;
}

static void val_text(kdef d, char *t)
{
    int v = knob_get(d);
    t[0] = 0;
    switch (d.kind) {
    case K_PARAM: {
        const ba_param_info *pi = &BA_PARAMS[d.id];
        int id = d.id, m = id >= P_M1 && id < P_M1 + 3 * MO_N ? (id - P_M1) / MO_N : -1, o = m >= 0 ? (id - P_M1) % MO_N : -1;
        if (pi->kind == MK_LIST) {
            int i = ba_list_index(id, v);
            if (pi->names)
                text_cat(t, pi->names[i]);
            else
                int_text(i + 1, t);
            return;
        }
        if (pi->kind == MK_SW) {
            text_cat(t, pi->names[v >= 64]);
            return;
        }
        if (o == MO_LRATE || o == MO_RRATE) {
            int sync = ba_list_index(P_MOD(m) + MO_SYNC, edit.p[P_MOD(m) + MO_SYNC]);
            if (sync >= SYNC_BPM)
                text_cat(t, BA_DIV_NAME[(v * (BA_NDIV - 1) + 63) / 127]);
            else
                hz_text(ba_lfo_hz(v), t);
            return;
        }
        if (o == MO_EA || o == MO_EDR) {
            ms_text(ba_env_ms(v, o == MO_EA), t);
            return;
        }
        if (o == MO_RLAG) {
            ms_text((float)v * (float)v * (2000.0f / 16129.0f), t);
            return;
        }
        switch (id) {
        case P_TUNE: {
            float s = ba_tune_semis(v);
            if (s > 0.04f)
                text_cat(t, "+");
            dec1_text(s, t + txt_len(t));
            text_cat(t, "ST");
            return;
        }
        case P_PENV: {
            float b = ((float)v - 64.0f) / 63.0f, s = b * (b < 0.0f ? -b : b) * 36.0f;
            if (s > 0.04f)
                text_cat(t, "+");
            dec1_text(s, t + txt_len(t));
            text_cat(t, "ST");
            return;
        }
        case P_PDEC: ms_text(ba_env_ms(v, 0), t); return;
        case P_SLIDE: ms_text(8.0f + (float)v * (float)v * (400.0f / 16129.0f), t); return;
        case P_CUTOFF: hz_text(ba_cutoff_hz(v), t); return;
        case P_HPCUT:
            if (!v)
                text_cat(t, "OFF");
            else
                hz_text(20.0f * fm_exp2f((float)v / 127.0f * 10.0f), t);
            return;
        case P_FA:
        case P_AA: ms_text(ba_env_ms(v, 1), t); return;
        case P_FD:
        case P_FR:
        case P_AD:
        case P_AR: ms_text(ba_env_ms(v, 0), t); return;
        case P_GLIDE:
            if (!v)
                text_cat(t, "OFF");
            else
                ms_text(ba_glide_ms(v), t);
            return;
        case P_SSWING: int_text(50 + v * 25 / 127, t); text_cat(t, "%"); return;
        case P_TRACK: int_text(v * 100 / 127, t); text_cat(t, "%"); return;
        case P_X3P1:
        case P_X3P2: {
            int ty = ba_list_index(P_X3TYPE, edit.p[P_X3TYPE]);
            if (ty == X3_PING && id == P_X3P2) {
                text_cat(t, "-");
                return;
            }
            if (ty <= X3_PING) {
                if (edit.p[P_X3SYNC] >= 64)
                    text_cat(t, BA_DIV_NAME[3 + (v * (BA_NDIV - 4) + 63) / 127]);
                else
                    ms_text(10.0f * fm_exp2f((float)v / 127.0f * 6.64f), t);
                return;
            }
            if (id == P_X3P1) {
                ms_text((float)v * (100.0f / 127.0f), t);
                return;
            }
            break;
        }
        default: break;
        }
        if (pi->kind == MK_BIP) {
            int b = (v - 64) * 100 / 63;
            if (b > 0)
                text_cat(t, "+");
            int_text(b, t + txt_len(t));
            return;
        }
        int_text(v * 100 / 127, t);
        return;
    }
    case K_LVL: {
        int dd = ui.dest[d.id];
        float b = ((float)v - 64.0f) / 63.0f;
        if (dd == D_CV) {                /* Tonhöhe in Halbtönen */
            float s = b * (b < 0.0f ? -b : b) * 24.0f;
            if (s > 0.04f)
                text_cat(t, "+");
            dec1_text(s, t + txt_len(t));
            text_cat(t, "ST");
        } else {
            int bb = (v - 64) * 100 / 63;
            if (bb > 0)
                text_cat(t, "+");
            int_text(bb, t + txt_len(t));
        }
        return;
    }
    case K_DEST: text_cat(t, BA_DEST_NAME[ui.dest[d.id]]); return;
    case K_MSEL: int_text(ui.msel[d.id] + 1, t); return;
    case K_MVAL: int_text(v * 100 / 127, t); return;
    case K_SSEL: int_text(ui.ssel + 1, t); return;
    case K_SNOTE: note_text(edit.seq[ui.ssel].note, t); return;
    case K_SGATE:
        if (v == BA_REST)
            text_cat(t, "REST");
        else if (v == BA_TIE)
            text_cat(t, "TIE");
        else {
            int_text(v * 100 / 127, t);
            text_cat(t, "%");
        }
        return;
    case K_SFL: {
        static const char *const F[4] = {"-", "ACCENT", "SLIDE", "ACC+SLD"};
        text_cat(t, F[edit.seq[ui.ssel].fl & 3u]);
        return;
    }
    case K_TEMPO: int_text(edit.bpm, t); return;
    case K_BEND: int_text(set.bend, t); text_cat(t, " ST"); return;
    case K_MCH:
        if (!set.mch)
            text_cat(t, "ALL");
        else
            int_text(set.mch, t);
        return;
    case K_TOUCH: text_cat(t, set.touch ? "ON" : "FIXED"); return;
    default: return;
    }
}

static int32_t accel(int k, int32_t e, int32_t mid, int32_t fast)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_at[k];
    ui.enc_at[k] = now;
    if (dt < 35u)
        return e * fast;
    if (dt < 80u)
        return e * mid;
    return e;
}

static int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* wie weit ein Regler pro Raste geht: auf HOME schnell (eine ganze Umdrehung in ~25 Rasten, schnell gedreht in ~6),
 * auf SEL und ENV etwas schneller als sonst, auf den anderen Seiten fein wie bisher */
static int32_t knob_step(int k, int32_t e)
{
    switch (ui.page) {
    case PG_MAIN: return accel(EN_K1 + k, e, 2, 4) * 5;
    case PG_FILTER:
    case PG_FILT2:
    case PG_FENV:
    case PG_AENV:
    case PG_PENV: return accel(EN_K1 + k, e, 2, 4) * 3;
    default: return accel(EN_K1 + k, e, 3, 6);
    }
}

static void knob_turn(int k, int32_t e)
{
    kdef d = kdef_of(k);
    int dir = e > 0 ? 1 : -1, a;
    if (d.kind == K_NONE)
        return;
    ui.knob_at[k] = plat_ms();
    ui.redraw |= R_ALL;
    switch (d.kind) {
    case K_PARAM: {
        const ba_param_info *pi = &BA_PARAMS[d.id];
        int v = edit.p[d.id];
        if (pi->kind == MK_LIST)         /* Listen: ein Eintrag pro Raste */
            v = ba_list_value(d.id, ba_list_index(d.id, v) + dir);
        else if (pi->kind == MK_SW)
            v = dir > 0 ? 127 : 0;
        else
            v = iclamp(v + knob_step(k, e), 0, 127);
        if (v == edit.p[d.id])
            return;
        edit.p[d.id] = (uint8_t)v;
        if (d.id == P_SLEN && ui.ssel >= ba_seq_len(&eng))
            ui.ssel = (uint8_t)(ba_seq_len(&eng) - 1);
        if (d.id == P_MOD(0) + MO_SCOUNT || d.id == P_MOD(1) + MO_SCOUNT || d.id == P_MOD(2) + MO_SCOUNT) {
            int m = (d.id - P_M1) / MO_N;
            if (ui.msel[m] >= scount(m))
                ui.msel[m] = (uint8_t)(scount(m) - 1);
        }
        break;
    }
    case K_LVL:
        a = iclamp(edit.lvl[d.id][ui.dest[d.id]] + accel(EN_K1 + k, e, 2, 5), 0, 127);
        edit.lvl[d.id][ui.dest[d.id]] = (uint8_t)a;
        break;
    case K_DEST: ui.dest[d.id] = (uint8_t)iclamp(ui.dest[d.id] + dir, 0, BA_NDEST - 1); return;
    case K_MSEL: ui.msel[d.id] = (uint8_t)iclamp(ui.msel[d.id] + dir, 0, scount(d.id) - 1); return;
    case K_MVAL:
        edit.mstep[d.id][ui.msel[d.id]] = (uint8_t)iclamp(edit.mstep[d.id][ui.msel[d.id]] + accel(EN_K1 + k, e, 3, 6), 0, 127);
        break;
    case K_SSEL: ui.ssel = (uint8_t)iclamp(ui.ssel + dir, 0, BA_STEPS - 1); return;
    case K_SNOTE: {
        ba_step *s = &edit.seq[ui.ssel];
        s->note = (uint8_t)iclamp(s->note + accel(EN_K1 + k, e, 1, 3), 24, 96);
        if (s->gate == BA_REST)          /* eine Note auf einer Pause: der Step spielt wieder */
            s->gate = 64;
        break;
    }
    case K_SGATE:
        edit.seq[ui.ssel].gate = (uint8_t)iclamp(edit.seq[ui.ssel].gate + accel(EN_K1 + k, e, 3, 6), 0, 127);
        break;
    case K_SFL:
        edit.seq[ui.ssel].fl = (uint8_t)iclamp(edit.seq[ui.ssel].fl + dir, 0, 3);
        break;
    case K_TEMPO: edit.bpm = (uint16_t)iclamp((int)edit.bpm + accel(EN_K1 + k, e, 2, 5), 30, 300); break;
    case K_BEND:
        set.bend = (uint8_t)iclamp(set.bend + dir, 1, 12);
        eng.bend_range = (float)set.bend;
        dirty();
        return;
    case K_MCH: set.mch = (uint8_t)iclamp(set.mch + dir, 0, 16); dirty(); return;
    case K_TOUCH: set.touch = dir > 0; dirty(); return;
    default: return;
    }
    edited();
}

static void page_set(int pg)
{
    int i;
    for (i = 0; i < 4; i++)              /* eine neue Seite: kein Regler leuchtet noch nach */
        ui.knob_at[i] = 0;
    ui.big_until = 0;
    ui.page = (uint8_t)pg;
    ui.redraw |= R_ALL;
    if (ui.rec && pg != PG_SEQ1 && pg != PG_SEQ2) {   /* die SEQ-Seiten verlassen: RECORD endet (wie bei MONSUN) */
        ui.rec = 0;
    }
    app_say(PAGE_NAME[pg], "");
}

/* ----------------------------------------------------------------- Presets --- */
static void slot_load(int s)
{
    char t[24] = "";
    set.slot = (uint8_t)s;
    edit = *slot_patch(s);
    set.edited = 0;
    ba_changed(&eng);
    dirty();
    t[0] = (char)('0' + (s + 1) / 10);
    t[1] = (char)('0' + (s + 1) % 10);
    t[2] = ' ';
    t[3] = 0;
    text_cat(t, edit.name);
    say_big(edit.name, t);
}

static void slot_save(int s)
{
    int b = s >= 8;
    *slot_patch(s) = edit;
    bank[b].magic = BANK_MAGIC;
    if (plat_store_save(OBJ_BANK_A + (uint32_t)b, &bank[b], sizeof bank[b]) != 0) {
        app_say("SAVE FAILED", "");
        return;
    }
    set.slot = (uint8_t)s;
    set.edited = 0;
    dirty();
    app_say("SAVED", edit.name);
}

static void factory(void)                /* REC 1 s: der Platz zurück auf sein Werks-Preset (gespeichert erst mit SAVE) */
{
    ba_preset(&edit, set.slot);
    set.edited = 1;
    ba_changed(&eng);
    dirty();
    app_say("FACTORY", edit.name);
}

/* -------------------------------------------------------------- Sequencer --- */
static void play_toggle(void)
{
    ui.playing = (uint8_t)!ui.playing;
    e_play(ui.playing);
    dirty_at = plat_ms();
    ui.redraw |= R_ALL;
}

static void rec_key(int note, int vel)   /* RECORD: die Taste schreibt den Step und geht weiter (APPEND verlängert) */
{
    ba_step *s = &edit.seq[ui.ssel];
    int len = ba_seq_len(&eng);
    s->note = (uint8_t)iclamp(note, 24, 96);
    s->fl = vel >= 110 ? BA_ACC : 0;     /* ein harter Anschlag: ACCENT */
    s->gate = 64;                        /* 50 % */
    if (ui.ssel + 1 >= len && len < BA_STEPS) {
        edit.p[P_SLEN] = (uint8_t)ba_list_value(P_SLEN, len);   /* APPEND: eine Stufe länger */
    }
    if (ui.ssel + 1 >= BA_STEPS) {
        ui.rec = 0;
        app_say("RECORD", "64 STEPS: END");
    } else {
        ui.ssel++;
    }
    edited();
}

/* ------------------------------------------------------------------ Eingabe --- */
static int key_base(void) { return 53 + 12 * ((int)set.oct - 3); }   /* Taste 0 = F3 (MIDI 53) bei Oktave 0 */

static void keys_input(uint32_t keys)
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit)) {
            int note = key_base() + (int)k, v = set.touch ? touch_vel(k) : 100;
            if (v <= 0)
                v = 100;
            if (note < 12 || note > 115)
                continue;
            ui.key_note[k] = (int8_t)note;
            if (ui.rec) {
                e_on(note, v, 1);
                rec_key(note, v);
            } else {
                e_on(note, v, 0);
            }
        } else if (!(keys & bit) && (ui.keys & bit)) {
            if (ui.key_note[k] >= 0)
                e_off(ui.key_note[k]);
            ui.key_note[k] = -1;
        }
    }
}

static void button(uint32_t i)
{
    int seqp = ui.page == PG_SEQ1 || ui.page == PG_SEQ2;
    if (ui.saving && i != B_SAVE) {      /* SAVE abgebrochen */
        ui.saving = 0;
        app_say("SAVE", "CANCELLED");
        return;
    }
    switch (i) {
    case B_HOME: page_set(PG_MAIN); break;
    case B_SEL: page_set(ui.page == PG_FILTER ? PG_FILT2 : PG_FILTER); break;
    case B_ENV: page_set(ui.page == PG_AENV ? PG_FENV : ui.page == PG_FENV ? PG_PENV : PG_AENV); break;
    case B_LFO: page_set(ui.page >= PG_M1 && ui.page < PG_M3D ? ui.page + 1 : PG_M1); break;
    case B_EDIT: page_set(ui.page == PG_SET1 ? PG_SET2 : PG_SET1); break;
    case B_FX: page_set(ui.page >= PG_FX1 && ui.page < PG_FXP ? ui.page + 1 : PG_FX1); break;
    case B_GLO: page_set(ui.page == PG_GLO1 ? PG_GLO2 : PG_GLO1); break;
    case B_SEQ: page_set(ui.page == PG_SEQ1 ? PG_SEQ2 : PG_SEQ1); break;
    case B_PLAY: play_toggle(); break;
    case B_REC:
        if (seqp) {
            ui.rec = (uint8_t)!ui.rec;
            ui.redraw |= R_ALL;
            app_say(ui.rec ? "RECORD" : "RECORD OFF", ui.rec ? "PLAY KEYS" : "");
        } else {
            ui.rec_t = plat_ms();
        }
        break;
    case B_SAVE:
        if (!ui.saving) {
            ui.saving = 1;
            ui.save_slot = set.slot;
            ui.save_until = plat_ms() + 8000u;
            ui.redraw |= R_ALL;
        } else {
            ui.saving = 0;
            slot_save(ui.save_slot);
        }
        break;
    case B_ARP:
        if (ui.page == PG_SEQ2) {        /* SEQ SETUP: ein Zufalls-Pattern (der Grundton bleibt der des ersten Steps) */
            ba_randomize(&edit, edit.seq[0].note % 12 + 24 + (edit.seq[0].note >= 48 ? 12 : 0), plat_ms() * 2654435761u + ui.frame);
            edited();
            app_say("RANDOM", "NEW PATTERN");
        } else if (seqp) {               /* Step kopieren, dann einfügen */
            if (ui.copied < 0) {
                ui.clip = edit.seq[ui.ssel];
                ui.copied = (int8_t)ui.ssel;
                app_say("COPIED", "STEP");
            } else {
                edit.seq[ui.ssel] = ui.clip;
                ui.copied = -1;
                edited();
                app_say("PASTED", "");
            }
        } else {
            ui.hold = (uint8_t)!ui.hold;
            e_hold(ui.hold);
            app_say("HOLD", ui.hold ? "ON" : "OFF");
        }
        break;
    case B_OCTDN:
    case B_OCTUP: {
        char t[8] = "";
        int o = iclamp((int)set.oct + (i == B_OCTUP ? 1 : -1), 0, 6);
        set.oct = (uint8_t)o;
        dirty();
        if (o > 3)
            text_cat(t, "+");
        int_text(o - 3, t + txt_len(t));
        app_say("OCTAVE", t);
        break;
    }
    default: break;
    }
}

static void held_buttons(uint32_t btn)
{
    if (ui.rec_t) {
        if (!(btn >> B_REC & 1u))
            ui.rec_t = 0;
        else if (plat_ms() - ui.rec_t >= 1000u) {
            ui.rec_t = 0;
            factory();
        }
    }
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), i;
    int32_t e;
    touch_poll();
    keys = touch_keys();
    keys_input(keys);
    for (i = 0; i < NB; i++)
        if ((btn >> i & 1u) && !(ui.btn >> i & 1u))
            button(i);
    held_buttons(btn);
    for (i = 0; i < 4u; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0)
            knob_turn((int)i, e);
    if ((e = plat_enc(EN_SELECT)) != 0) {    /* CUTOFF: der große Regler, auf jeder Seite */
        char t[12];
        edit.p[P_CUTOFF] = (uint8_t)iclamp(edit.p[P_CUTOFF] + accel(EN_SELECT, e, 2, 4), 0, 127);
        edited();
        hz_text(ba_cutoff_hz(edit.p[P_CUTOFF]), t);
        say_big(t, "CUTOFF");
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {      /* DRIVE: der Overdrive auf einem Knopf */
        char t[8];
        edit.p[P_DRIVE] = (uint8_t)iclamp(edit.p[P_DRIVE] + accel(EN_ALGO, e, 3, 6), 0, 127);
        edited();
        int_text(edit.p[P_DRIVE] * 100 / 127, t);
        say_big(t, edit.p[P_DRIVE] ? "DRIVE" : "DRIVE OFF");
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        int s = ((ui.saving ? ui.save_slot : set.slot) + NSLOTS + (e > 0 ? 1 : NSLOTS - 1)) % NSLOTS;
        if (ui.saving) {
            ui.save_slot = (uint8_t)s;
            ui.save_until = plat_ms() + 8000u;
            ui.redraw |= R_ALL;
        } else {
            slot_load(s);
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw |= R_ALL;
    ui.keys = keys;
    ui.btn = btn;
}

void app_input_only(void) { input(); }

static void lights(void)
{
    static const uint8_t PAGE_BTN[NPAGES] = {B_HOME, B_SEL, B_SEL, B_ENV, B_ENV, B_ENV, B_LFO, B_LFO, B_LFO, B_LFO,
                                             B_LFO, B_LFO, B_EDIT, B_EDIT, B_FX, B_FX, B_FX, B_FX, B_GLO, B_GLO, B_SEQ,
                                             B_SEQ};
    uint32_t btn = 1u << PAGE_BTN[ui.page], dimb, lit = 0, k;
    if (ui.playing)
        btn |= LED_PLAY_GREEN;
    if (ui.rec)
        btn |= 1u << B_REC;
    if (ui.hold)
        btn |= 1u << B_ARP;
    if (ui.saving && (plat_ms() / 250u) & 1u)
        btn |= 1u << B_SAVE;
    if (set.oct != 3)
        btn |= 1u << (set.oct > 3 ? B_OCTUP : B_OCTDN);
    for (k = 0; k < NKEYS; k++)          /* C-Tasten glimmen: Orientierung */
        if ((key_base() + (int)k) % 12 == 0)
            lit |= 1u << k;
    dimb = (1u << NB) - 1u;
    plat_lights(btn | (ui.btn & ~(1u << B_PLAY)), ui.keys, dimb & ~(1u << B_PLAY), lit, 1);
}

/* ----------------------------------------------------------------- Zeichnen --- */
/* Die Optik der Erica Synths Bassline DB-01: ein mattschwarzes Aluminium-Panel, weiße Beschriftung und dünne weiße
 * Linien um die Sektionen, schwarze Regler im Moog-Stil (gerändelter Rock, flache Kappe, weißer Strich) über einer
 * gedruckten Skala, oben die rote 7-Segment-Anzeige, unten die Reihe der 16 Step-Tasten, deren LEDs pink-rot
 * leuchten. Die Kurven (Ausgang, Filter, Hüllkurven, Modulatoren, Pattern) stehen in einer roten LED-Punktmatrix,
 * so wie das Gerät selbst nur mit LEDs spricht. Alles kantengeglättet (ui_gfx.h). */
#undef C_PANEL
#define C_PANEL RGB(13, 13, 14)
#undef C_WHITE
#define C_WHITE RGB(236, 236, 232)
#undef C_SUB
#define C_SUB RGB(128, 128, 126)
#undef C_RED
#define C_RED RGB(255, 38, 72)                /* die LEDs des DB-01: pink-rot */
#undef C_RED_DIM
#define C_RED_DIM RGB(70, 8, 20)
#undef C_LINE
#define C_LINE RGB(200, 200, 196)

#include "ui_gfx.h"

/* das Ausgangssignal fürs Oszilloskop (der Audio-Interrupt schreibt, die Anzeige liest) */
#define SCOPE_N 1024u
static float scope_ring[SCOPE_N];
static volatile uint32_t scope_w;
static volatile float out_peak;

static void ctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

/* ein abgerundetes Rechteck: senkrechter Verlauf, glatte Ecken */
static void rrect(int32_t x, int32_t y, int32_t w, int32_t h, float rad, col3 top, col3 bot, float a)
{
    int32_t i, j;
    for (j = 0; j < h; j++) {
        col3 c = mix3(top, bot, h > 1 ? (float)j / (float)(h - 1) : 0.0f);
        float py = (float)j + 0.5f, cy = py < rad ? rad : py > (float)h - rad ? (float)h - rad : py;
        if (!px_ok(0, y + j))
            continue;
        for (i = 0; i < w; i++) {
            float px = (float)i + 0.5f, cx = px < rad ? rad : px > (float)w - rad ? (float)w - rad : px, cov = 1.0f;
            if (cx != px && cy != py) {
                float dx = px - cx, dy = py - cy;
                cov = sat01(rad - fm_sqrtf(dx * dx + dy * dy) + 0.5f);
            }
            if (cov * a >= 0.996f) {
                if (px_ok(x + i, y + j))
                    px_put(x + i, y + j, c);
            } else {
                px_blend(x + i, y + j, c, cov * a);
            }
        }
    }
}

/* das Panel: fast schwarz, ein feines Korn wie gestrahltes, eloxiertes Aluminium */
static void panel_black(int32_t ly0, int32_t h, int32_t screen_y0)
{
    int32_t x, j;
    for (j = 0; j < h; j++) {
        int32_t sy = j + screen_y0, y = ly0 + j;
        for (x = 0; x < (int32_t)cv_w; x++) {
            uint32_t n = (uint32_t)(x * 73856093) ^ (uint32_t)(sy * 19349663);
            float g;
            n = (n ^ (n >> 13)) * 1274126177u;
            g = 12.5f + (float)((n >> 24) & 7u) * 0.42f;
            if (px_ok(x, y))
                px_put(x, y, rgb3(g, g, g + 0.8f));
        }
    }
}

/* die dünnen weißen Linien des Panels */
static void hline(int32_t x0, int32_t x1, int32_t y, float a)
{
    int32_t x;
    for (x = x0; x <= x1; x++)
        px_blend(x, y, c3(C_LINE), a);
}
static void vline(int32_t x, int32_t y0, int32_t y1, float a)
{
    int32_t y;
    for (y = y0; y <= y1; y++)
        px_blend(x, y, c3(C_LINE), a);
}
/* ein Sektionsrahmen wie auf dem Panel: die Linie, oben links unterbrochen für den Namen */
static void section(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const char *name, const char *extra)
{
    int32_t tw = name ? text_w(&FONT_XS, name) : 0;
    if (name) {
        if (extra && extra[0]) {
            cv_text(x0 + 8 + tw + 8, y0 - 6, &FONT_XS, extra, C_SUB);
            tw += 8 + text_w(&FONT_XS, extra);
        }
        hline(x0, x0 + 4, y0, 0.75f);
        hline(x0 + tw + 12, x1, y0, 0.75f);
        cv_text(x0 + 8, y0 - 6, &FONT_XS, name, C_WHITE);
    } else {
        hline(x0, x1, y0, 0.75f);
    }
    hline(x0, x1, y1, 0.75f);
    vline(x0, y0, y1, 0.75f);
    vline(x1, y0, y1, 0.75f);
}

/* ---- die rote 7-Segment-Anzeige ---- */
static uint8_t seg_bits(char c)
{
    /* Segmente: 1 a (oben), 2 b, 4 c, 8 d (unten), 16 e, 32 f, 64 g (Mitte) */
    switch (c) {
    case '0': case 'O': return 63;
    case '1': case 'I': return 6;
    case '2': return 91;
    case '3': return 79;
    case '4': return 102;
    case '5': case 'S': return 109;
    case '6': return 125;
    case '7': return 7;
    case '8': return 127;
    case '9': return 111;
    case 'A': return 119;
    case 'B': case 'b': return 124;
    case 'C': return 57;
    case 'c': return 88;
    case 'D': case 'd': return 94;
    case 'E': return 121;
    case 'F': return 113;
    case 'G': return 61;
    case 'H': return 118;
    case 'L': return 56;
    case 'n': return 84;
    case 'o': return 92;
    case 'P': return 115;
    case 'r': return 80;
    case 't': return 120;
    case 'U': return 62;
    case 'u': return 28;
    case 'Y': return 110;
    case '-': return 64;
    case '_': return 8;
    default: return 0;
    }
}

/* ein Segment: ein schmales Sechseck, als dicke Linie mit spitzen Enden */
static void seg_bar(float x0, float y0, float x1, float y1, float w, int on)
{
    col3 c = on ? c3(C_RED) : rgb3(34, 8, 13);
    float dx = x1 - x0, dy = y1 - y0, len = fm_sqrtf(dx * dx + dy * dy), t;
    for (t = 0.0f; t <= len; t += 0.5f) {
        float k = t / len, ww = w * sat01(sat01(t / (w * 0.9f)) < sat01((len - t) / (w * 0.9f)) ? t / (w * 0.9f) : (len - t) / (w * 0.9f));
        aa_disc(x0 + dx * k, y0 + dy * k, 0.5f + ww * 0.5f, c, 1.0f);
    }
    if (on)
        bloom((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, len * 0.75f, c3(C_RED), 0.16f);
}

/* eine Ziffer: Breite w, Höhe h, leicht kursiv wie echte LED-Anzeigen; dp: der Punkt */
static void seg_digit(float x, float y, float w, float h, char ch, int dp)
{
    uint8_t b = seg_bits(ch);
    float sk = 0.12f, g = 1.2f, hh = h * 0.5f, sw = w * 0.22f;
#define SX(px, py) ((px) + sk * (h - ((py) - y)))
    /* a b c d e f g */
    seg_bar(SX(x + g, y), y, SX(x + w - g, y), y, sw, b & 1);
    seg_bar(SX(x + w, y + g), y + g, SX(x + w, y + hh - g), y + hh - g, sw, b & 2);
    seg_bar(SX(x + w, y + hh + g), y + hh + g, SX(x + w, y + h - g), y + h - g, sw, b & 4);
    seg_bar(SX(x + g, y + h), y + h, SX(x + w - g, y + h), y + h, sw, b & 8);
    seg_bar(SX(x, y + hh + g), y + hh + g, SX(x, y + h - g), y + h - g, sw, b & 16);
    seg_bar(SX(x, y + g), y + g, SX(x, y + hh - g), y + hh - g, sw, b & 32);
    seg_bar(SX(x + g, y + hh), y + hh, SX(x + w - g, y + hh), y + hh, sw, b & 64);
    aa_disc(SX(x + w + 3.0f, y + h), y + h, 1.5f, dp ? c3(C_RED) : rgb3(34, 8, 13), 1.0f);
    if (dp)
        bloom(SX(x + w + 3.0f, y + h), y + h, 4.0f, c3(C_RED), 0.5f);
#undef SX
}

/* vier Stellen hinter rauchigem Glas; s hat bis 4 Zeichen, '.' hängt einen Punkt an die vorige Stelle */
static void seg_display(int32_t x, int32_t y, const char *s)
{
    char d[4] = {' ', ' ', ' ', ' '};
    int p[4] = {0, 0, 0, 0}, n = 0, i;
    for (i = 0; s[i] && n <= 4; i++) {
        if (s[i] == '.') {
            if (n > 0)
                p[n - 1] = 1;
        } else if (n < 4) {
            d[n++] = s[i];
        }
    }
    rrect(x, y, 72, 24, 2.0f, rgb3(22, 4, 8), rgb3(10, 2, 4), 1.0f);
    for (i = 0; i < 4; i++)
        seg_digit((float)(x + 6 + i * 16), (float)(y + 4), 9.0f, 15.0f, d[i], p[i]);
}

/* ---- die LED-Punktmatrix: 75 x 22 Punkte, jeder Punkt eine kleine rote LED ---- */
#define MX_W 75
#define MX_H 22
#define MX_X 8
#define MX_Y 18
static uint8_t mx[MX_H][MX_W];          /* Helligkeit 0..255 */

static void mx_clear(void) { memset(mx, 0, sizeof mx); }
static void mx_dot(int x, int y, int v)
{
    if (x < 0 || x >= MX_W || y < 0 || y >= MX_H)
        return;
    if (v > mx[y][x])
        mx[y][x] = (uint8_t)(v > 255 ? 255 : v);
}
static void mx_line(int x0, int y0, int x1, int y1, int v)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0, sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy, k;
    for (k = 0; k < 400; k++) {
        int e2;
        mx_dot(x0, y0, v);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * e;
        if (e2 >= dy) {
            e += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            e += dx;
            y0 += sy;
        }
    }
}
static void mx_fill_below(int x, int y, int v)   /* eine Spalte unter der Kurve, schwach */
{
    for (y = y < 0 ? 0 : y; y < MX_H; y++)
        mx_dot(x, y, v);
}
static void mx_draw(int32_t oy)
{
    int x, y;
    for (y = 0; y < MX_H; y++)
        for (x = 0; x < MX_W; x++) {
            float cx = (float)(MX_X + x * 3) + 1.0f, cy = (float)(oy + y * 3) + 1.0f, a = (float)mx[y][x] / 255.0f;
            if (a <= 0.0f)
                px_blend((int32_t)cx, (int32_t)cy, rgb3(44, 10, 16), 1.0f);
            else {
                col3 c = mix3(rgb3(70, 8, 22), c3(C_RED), a);
                if (a > 0.85f)
                    c = mix3(c, rgb3(255, 190, 200), (a - 0.85f) * 2.0f);
                px_put((int32_t)cx, (int32_t)cy, c);
                px_blend((int32_t)cx + 1, (int32_t)cy, c, 0.75f);
                px_blend((int32_t)cx, (int32_t)cy + 1, c, 0.75f);
                px_blend((int32_t)cx + 1, (int32_t)cy + 1, c, 0.6f);
                if (a > 0.5f)
                    bloom(cx + 0.5f, cy + 0.5f, 3.2f, c3(C_RED), 0.10f * a);
            }
        }
}

/* ---- der Regler im Moog-Stil, über einer gedruckten Skala; der Wert leuchtet als rote Punkte am Rand ---- */
static void knob_moog(float cx, float cy, float r, float v, int hot, int bip)
{
    float va = -135.0f + 270.0f * sat01(v), a0 = bip ? 0.0f : -135.0f;
    float ux = fm_sinf(va * 0.0174533f), uy = -fm_cosf(va * 0.0174533f), rt = r * 0.70f;
    int32_t x, y, R = (int32_t)(r + 4.0f), i;
    /* die Skala: 11 weiße Striche, die großen bei 0, 5, 10 */
    for (i = 0; i <= 10; i++) {
        float an = (-135.0f + 27.0f * (float)i) * 0.0174533f, sx = fm_sinf(an), sy = -fm_cosf(an);
        float r0 = r + 4.5f, r1 = r + (i % 5 == 0 ? 9.0f : 7.0f);
        aa_line(cx + sx * r0, cy + sy * r0, cx + sx * r1, cy + sy * r1, c3(C_LINE), i % 5 == 0 ? 0.95f : 0.6f);
    }
    /* der Wert: rote LED-Punkte auf einem Bogen innerhalb der Skala */
    for (i = 0; i <= 24; i++) {
        float an = -135.0f + 11.25f * (float)i, rad = an * 0.0174533f, px = cx + fm_sinf(rad) * (r + 2.6f);
        float py = cy - fm_cosf(rad) * (r + 2.6f);
        int on = (an >= (va < a0 ? va : a0) - 0.5f) && (an <= (va < a0 ? a0 : va) + 0.5f) && fabsf_(va - a0) > 0.8f;
        if (on) {
            aa_disc(px, py, 1.1f, c3(C_RED), hot ? 1.0f : 0.8f);
            if (hot)
                bloom(px, py, 3.0f, c3(C_RED), 0.25f);
        }
    }
    for (y = (int32_t)cy - R; y <= (int32_t)cy + R; y++)
        for (x = (int32_t)cx - R; x <= (int32_t)cx + R; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = fm_sqrtf(dx * dx + dy * dy);
            float sdx = dx - 1.0f, sdy = dy - 2.0f, sd = fm_sqrtf(sdx * sdx + sdy * sdy), cov, an, lam, proj, perp;
            col3 c;
            if (!px_ok(x, y))
                continue;
            if (sd < r + 2.0f)               /* der Schatten */
                px_blend(x, y, rgb3(0, 0, 0), sat01((r + 2.0f - sd) / 3.5f) * 0.7f);
            if (d > r + 0.5f)
                continue;
            cov = sat01(r - d + 0.5f);
            an = ang_deg(dx, dy);
            lam = d > 0.5f ? -(dx + dy) * 0.7071f / d : 0.0f;
            if (d > rt) {                    /* der Rock: fein gerändelt, schwarz glänzend */
                float kn = fm_cosf(an * 0.0174533f * 36.0f), g = 16.0f + 9.0f * lam + 5.0f * kn;
                g += 30.0f * sat01(1.0f - fabsf_(d - (rt + 1.0f)) / 1.0f) * sat01(lam + 0.2f);   /* die Kante oben */
                c = rgb3(g, g, g);
            } else {                         /* die Kappe: flach, mattschwarz, ein weicher Glanz oben links */
                float gl = sat01(1.0f - fm_sqrtf((dx + rt * 0.35f) * (dx + rt * 0.35f) + (dy + rt * 0.35f) * (dy + rt * 0.35f)) / (rt * 0.9f));
                float g = 22.0f + 22.0f * gl * gl;
                c = rgb3(g, g, g + 1.0f);
            }
            /* der Zeiger: ein weißer Strich über Kappe und Rock */
            proj = dx * ux + dy * uy;
            perp = fabsf_(dx * uy - dy * ux);
            if (proj > r * 0.25f && proj < r - 0.6f) {
                float pc = sat01(1.15f - perp);
                if (pc > 0.0f)
                    c = mix3(c, rgb3(245, 245, 240), pc);
            }
            px_blend(x, y, c, cov);
        }
}

static void draw_knob(int k, int32_t cx, int32_t y, uint32_t now)
{
    kdef d = kdef_of(k);
    int v = knob_get(d), hot = ui.knob_at[k] && now - ui.knob_at[k] < 700u;
    char t[20];
    if (v < 0 || d.kind == K_NONE)
        return;
    ctext(cx, y, &FONT_XS, knob_label(d), C_WHITE);
    knob_moog((float)cx, (float)(y + 37), 14.0f, (float)v / 127.0f, hot, knob_bip(d));
    val_text(d, t);
    ctext(cx, y + 62, &FONT_XS, t, hot ? C_RED : C_SUB);
    if (hot)
        bloom((float)cx, (float)y + 67.0f, 16.0f, c3(C_RED), 0.10f);
}

/* ---- die Seitengruppen: kleine LEDs, wo man in der Gruppe ist ---- */
static void group_of(int pg, int *i, int *n)
{
    static const uint8_t G0[NPAGES] = {0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 4, 5, 0, 1, 0, 1, 2, 3, 0, 1, 0, 1};
    static const uint8_t GN[NPAGES] = {1, 2, 2, 3, 3, 3, 6, 6, 6, 6, 6, 6, 2, 2, 4, 4, 4, 4, 2, 2, 2, 2};
    *i = G0[pg];
    *n = GN[pg];
}

/* ---- die Anzeigen in der Punktmatrix ---- */
static int mx_y(float v) { int y = (int)(10.5f - v * 10.0f); return y < 0 ? 0 : y > MX_H - 1 ? MX_H - 1 : y; }

static void show_scope(void)             /* VCO: das echte Ausgangssignal, auf einen Nulldurchgang getriggert */
{
    uint32_t w = scope_w, i, best = w - 240u;
    float pk = 0.0f, g;
    int x, py = 0, wi = ba_wave_index(edit.p[P_WAVE]);
    for (i = w - 600u; i != w - 240u; i++) {
        float a = scope_ring[(i - 1u) % SCOPE_N], b = scope_ring[i % SCOPE_N];
        if (a <= 0.0f && b > 0.0f)
            best = i;
    }
    for (x = 0; x < 225; x++) {
        float v = fabsf_(scope_ring[(best + (uint32_t)x) % SCOPE_N]);
        if (v > pk)
            pk = v;
    }
    if (pk < 0.002f) {                   /* still: die Form der Welle, gedimmt */
        for (x = 0; x < MX_W; x++) {
            float ph = (float)x / 37.5f, p = ph - (float)(int)ph, v;
            v = wi == 0 ? 1.0f - 4.0f * fabsf_(p - 0.5f) : wi == 1 ? 2.0f * p - 1.0f : p < 0.5f ? 1.0f : -1.0f;
            v += (float)edit.p[P_SUB] / 127.0f * (fmod_pos(ph * 0.5f, 1.0f) < 0.5f ? 0.4f : -0.4f);
            v *= 0.7f;
            if (x)
                mx_line(x - 1, py, x, mx_y(v), 110);
            py = mx_y(v);
        }
        return;
    }
    g = 0.95f / (pk < 0.08f ? 0.08f : pk);
    for (x = 0; x < MX_W; x++) {
        float v = scope_ring[(best + (uint32_t)x * 3u) % SCOPE_N] * g;
        int y = mx_y(v);
        if (x)
            mx_line(x - 1, py, x, y, 255);
        py = y;
    }
}

static void show_filter(void)            /* die Filterkurve, wie sie gerade wirklich steht; LP oder BP */
{
    float knob = (float)edit.p[P_CUTOFF] / 127.0f, q = (float)edit.p[P_RESON] / 127.0f, fc;
    int x, py = 0, bp = edit.p[P_FMODE] >= 64;
    fc = eng.cut_hz > 0.0f && eng.ae.stage ? sat01(fm_log2f(eng.cut_hz / 20.0f) / 10.0f) : knob;
    for (x = 0; x < MX_W; x++) {
        float f = (float)x / (float)(MX_W - 1), d = (f - fc) * 10.0f, db;
        int y;
        if (bp)
            db = q * 16.0f * fm_exp2f(-fabsf_(d) * 4.0f) - fabsf_(d) * 10.0f;
        else
            db = d < 0.0f ? q * 16.0f * fm_exp2f(d * 3.0f) : q * 16.0f * fm_exp2f(-d * 6.0f) - d * 14.0f;
        y = (int)(9.0f - db * 0.55f);
        y = y < 0 ? 0 : y > MX_H - 1 ? MX_H - 1 : y;
        if (x)
            mx_line(x - 1, py, x, y, 255);
        mx_fill_below(x, y + 1, 38);
        py = y;
    }
    for (x = 0; x < MX_H; x += 2)       /* wo der CUTOFF-Regler steht */
        mx_dot((int)(knob * (float)(MX_W - 1)), x, 90);
}

static void show_env(int which)          /* 0 VCA, 1 Filter: die ADSR-Kurve und wo sie gerade steht */
{
    const uint8_t *p = edit.p;
    int a = which ? p[P_FA] : p[P_AA], dd = which ? p[P_FD] : p[P_AD], s = which ? p[P_FS] : p[P_AS];
    int r = which ? p[P_FR] : p[P_AR], x, py = MX_H - 1;
    const ba_adsr *env = which ? &eng.fe : &eng.ae;
    float wa = fm_sqrtf(ba_env_ms(a, 1)), wd = fm_sqrtf(ba_env_ms(dd, 0)), ws = fm_sqrtf(300.0f);
    float wr = fm_sqrtf(ba_env_ms(r, 0)), tot = wa + wd + ws + wr, sl = (float)s / 127.0f, xa, xd, xs, W = (float)(MX_W - 1);
    xa = wa / tot * W;
    xd = xa + wd / tot * W;
    xs = xd + ws / tot * W;
    for (x = 0; x < MX_W; x++) {
        float fx = (float)x, lv;
        int y;
        if (fx <= xa)
            lv = fx / (xa + 0.01f);
        else if (fx <= xd)
            lv = sl + (1.0f - sl) * fm_exp2f(-(fx - xa) / (xd - xa + 0.01f) * 6.0f);
        else if (fx <= xs)
            lv = sl;
        else
            lv = sl * fm_exp2f(-(fx - xs) / (W - xs + 0.01f) * 6.0f);
        y = (int)((1.0f - lv) * (float)(MX_H - 2)) + 1;
        if (x)
            mx_line(x - 1, py, x, y, 220);
        mx_fill_below(x, y + 1, 30);
        py = y;
    }
    if (env->stage) {
        float lv = env->level, fx;
        int y = (int)((1.0f - lv) * (float)(MX_H - 2)) + 1;
        if (env->stage == 1)
            fx = xa * lv;
        else if (env->stage == 2)
            fx = sl < 0.999f ? xa + (xd - xa) * sat01((1.0f - lv) / (1.0f - sl + 1e-4f)) : xd;
        else
            fx = xs + (W - xs) * sat01(1.0f - lv / (sl > 0.02f ? sl : 0.02f));
        for (x = 0; x < MX_H; x++)
            mx_dot((int)fx, x, 70);
        mx_dot((int)fx, y, 255);
        mx_dot((int)fx + 1, y, 255);
        mx_dot((int)fx, y - 1, 255);
        mx_dot((int)fx + 1, y - 1, 255);
    }
}

static void show_pitch(void)             /* die Tonhöhen-Hüllkurve: ein Fall (oder Anstieg) über die Zeit */
{
    float amt = ((float)edit.p[P_PENV] - 64.0f) / 63.0f, dec = ba_env_ms(edit.p[P_PDEC], 0);
    int x, py = 11;
    for (x = 0; x < MX_W; x++) {
        float t = (float)x * 6.0f, v = amt * fm_exp2f(-t / (dec * 0.145f + 0.5f));   /* 450 ms über die Breite */
        int y = mx_y(v);
        if (x)
            mx_line(x - 1, py, x, y, 230);
        py = y;
    }
    for (x = 0; x < MX_W; x += 2)
        mx_dot(x, 10, 60);
}

static void show_mod(int m, int dest_page)
{
    int mode = mode_of(m), i, x, py = 0;
    if (dest_page) {                     /* die Ziele: 13 Balken von der Mitte; das gewählte hell */
        for (i = 0; i < BA_NDEST; i++) {
            int v = edit.lvl[m][i], sel = i == ui.dest[m], h = (v - 64) * 10 / 63, y, cx = 3 + i * 5 + 2;
            for (y = 0; y < MX_H; y += 3)
                mx_dot(cx, y, sel ? 60 : 0);
            mx_dot(cx, 10, 90);
            for (y = 0; y != h; y += h > 0 ? 1 : -1) {
                mx_dot(cx, 10 - y - (h > 0 ? 1 : -1), sel ? 255 : 150);
                mx_dot(cx + 1, 10 - y - (h > 0 ? 1 : -1), sel ? 255 : 150);
            }
        }
        return;
    }
    if (mode == MODE_STEP) {
        int cnt = scount(m), cur = ba_mod_step(&eng, m) % cnt, w = MX_W / cnt;
        for (i = 0; i < cnt; i++) {
            int h = edit.mstep[m][i] * (MX_H - 1) / 127, y, c;
            for (y = 0; y <= h; y++)
                for (c = 0; c < w - 1; c++)
                    mx_dot(i * w + c, MX_H - 1 - y, i == cur ? 255 : i == ui.msel[m] ? 180 : 90);
        }
        return;
    }
    for (x = 0; x < MX_W; x++) {         /* LFO, Hüllkurve, Zufall: der Ausgang über die Zeit */
        float v = ui.scope[m][(ui.scope_pos + (uint32_t)(x * 96 / MX_W)) % 96u];
        int y = mx_y(v * 0.95f);
        if (x)
            mx_line(x - 1, py, x, y, x == MX_W - 1 ? 255 : 200);
        py = y;
    }
}

static void show_fx(void)                /* FX1 -> FX2 -> FX3: drei Blöcke, so voll wie ihre Mischung */
{
    int i, x, y;
    for (i = 0; i < 3; i++) {
        int mix = edit.p[i == 0 ? P_X1MIX : i == 1 ? P_X2MIX : P_X3MIX], on = ui.page == PG_FX1 + i || ui.page == PG_FXP;
        int x0 = i * 26, h = mix * (MX_H - 3) / 127;
        for (x = x0; x < x0 + 22; x++) {
            mx_dot(x, 0, on ? 200 : 70);
            mx_dot(x, MX_H - 1, on ? 200 : 70);
        }
        for (y = 0; y < MX_H; y++) {
            mx_dot(x0, y, on ? 200 : 70);
            mx_dot(x0 + 21, y, on ? 200 : 70);
        }
        for (y = 0; y < h; y++)
            for (x = x0 + 2; x < x0 + 20; x++)
                mx_dot(x, MX_H - 2 - y, (x + y) & 1 ? 150 : 220);
        if (i < 2)
            mx_line(x0 + 22, 10, x0 + 25, 10, 160);
    }
}

static void show_seq(void)               /* die 16 Steps der Seite als Pianorolle: Note als Höhe, Gate als Länge;
                                          * ACCENT hell, SLIDE verbunden */
{
    int pg = ui.ssel / 16, i, cur = ba_step_now(&eng), lo = 127, hi = 0;
    for (i = 0; i < 16; i++) {
        const ba_step *s = &edit.seq[pg * 16 + i];
        if (s->gate != BA_REST) {
            lo = s->note < lo ? s->note : lo;
            hi = s->note > hi ? s->note : hi;
        }
    }
    if (hi < lo)
        lo = hi = 36;
    if (hi - lo < 12) {
        lo -= (12 - (hi - lo)) / 2;
        hi = lo + 12;
    }
    for (i = 0; i < 16; i++) {          /* je Step 4 Punkte breit, die Vierer-Gruppen mit einer Lücke */
        const ba_step *s = &edit.seq[pg * 16 + i];
        int x0 = 3 + i * 4 + (i / 4) * 2, y, len, c, br;
        if (pg * 16 + i == cur)
            for (y = 0; y < MX_H; y += 2)
                mx_dot(x0 + 1, y, 70);
        mx_dot(x0 + 1, MX_H - 1, 45);       /* die Grundlinie: ein Punkt pro Step */
        if (s->gate == BA_REST)
            continue;
        y = MX_H - 3 - (s->note - lo) * (MX_H - 5) / (hi - lo);
        len = s->gate == BA_TIE || (s->fl & BA_SLD) ? 4 : 2 + s->gate * 2 / 127;
        br = pg * 16 + i == cur ? 255 : s->fl & BA_ACC ? 235 : 130;
        for (c = 0; c < len; c++) {
            mx_dot(x0 + c, y, br);
            if (s->fl & BA_ACC)             /* ACCENT: doppelt so dick */
                mx_dot(x0 + c, y - 1, br);
        }
        if ((s->fl & BA_SLD) && i < 15) {   /* SLIDE: eine schräge Linie zur nächsten Note */
            const ba_step *n = &edit.seq[pg * 16 + i + 1];
            if (n->gate != BA_REST) {
                int x1 = 3 + (i + 1) * 4 + ((i + 1) / 4) * 2;
                mx_line(x0 + 3, y, x1, MX_H - 3 - (n->note - lo) * (MX_H - 5) / (hi - lo), 90);
            }
        }
    }
}

static void show_glo(void)               /* das Tempo als Lauflicht im Takt, die Last als Balken */
{
    uint32_t cpu = plat_cpu_pct();
    int x, n = (int)(cpu > 100u ? 100u : cpu) * MX_W / 100, beat = (int)((plat_ms() * (uint32_t)(ba_bpm(&eng) + 0.5f) / 60000u) % 4u);
    for (x = 0; x < 4; x++) {
        int c, y;
        for (c = 0; c < 14; c++)
            for (y = 2; y < 8; y++)
                mx_dot(4 + x * 18 + c, y, x == beat ? 255 : 40);
    }
    for (x = 0; x < MX_W; x++) {
        mx_dot(x, 15, x < n ? 230 : 30);
        mx_dot(x, 16, x < n ? 230 : 30);
    }
}

static int view_animated(void)           /* diese Seiten bewegen sich: die Anzeige jeden zweiten Frame neu */
{
    return ui.page != PG_SEQ1 && ui.page != PG_SEQ2 && !(ui.page >= PG_FX1 && ui.page <= PG_FXP);
}

static const char *view_title(char *t)
{
    int m = mod_of_page(ui.page);
    t[0] = 0;
    if (ui.saving)
        return "SAVE";
    if (m >= 0) {
        t[0] = 'M';
        t[1] = (char)('1' + m);
        t[2] = 0;
        text_cat(t, (ui.page - PG_M1) & 1 ? " DEST " : " ");
        text_cat(t, (ui.page - PG_M1) & 1 ? BA_DEST_NAME[ui.dest[m]] : BA_MODE_NAME[mode_of(m)]);
        return t;
    }
    switch (ui.page) {
    case PG_MAIN: text_cat(t, "VCO "); text_cat(t, BA_WAVE_NAME[ba_wave_index(edit.p[P_WAVE])]); return t;
    case PG_FILTER:
    case PG_FILT2:
        text_cat(t, edit.p[P_FMODE] >= 64 ? "VCF BP " : "VCF LP ");
        hz_text(eng.cut_hz > 0.0f && eng.ae.stage ? eng.cut_hz : ba_cutoff_hz(edit.p[P_CUTOFF]), t + txt_len(t));
        return t;
    case PG_AENV: return "VCA ENV";
    case PG_FENV: return "VCF ENV";
    case PG_PENV: return "PITCH ENV";
    case PG_SET1:
    case PG_SET2: return "VCA ENV";
    case PG_FX1:
    case PG_FX2:
    case PG_FX3:
    case PG_FXP: return "FX CHAIN";
    case PG_SEQ1:
    case PG_SEQ2: {
        char n[8];
        text_cat(t, "PATTERN ");
        int_text(ui.ssel / 16 + 1, n);
        text_cat(t, n);
        text_cat(t, "/");
        int_text((ba_seq_len(&eng) + 15) / 16, n);
        text_cat(t, n);
        return t;
    }
    default: text_cat(t, eng.ext ? "MIDI CLOCK" : "TEMPO"); return t;
    }
}

/* ---- unten: die Sektion SEQUENCER, die Punktmatrix und die 16 Step-Tasten ---- */
static void draw_steps(int32_t oy)
{
    int len = ba_seq_len(&eng), cur = ba_step_now(&eng), pg = (cur >= 0 && !(ui.page == PG_SEQ1 || ui.page == PG_SEQ2)) ? cur / 16 : ui.ssel / 16;
    int i;
    for (i = 0; i < 16; i++) {
        int st = pg * 16 + i;
        const ba_step *s = &edit.seq[st];
        int32_t x = 8 + i * 14 + (i / 4) * 0, y = oy;
        int in = st < len, note = in && s->gate != BA_REST, play = st == cur;
        float lit = play ? 1.0f : note ? (s->fl & BA_ACC ? 0.55f : 0.30f) : 0.0f;
        /* die Taste: ein dunkelgrauer, leicht gewölbter Gummi-Block */
        rrect(x, y, 12, 16, 2.0f, rgb3(46, 46, 48), rgb3(28, 28, 30), in ? 1.0f : 0.5f);
        rrect(x + 1, y + 1, 10, 6, 1.5f, rgb3(58, 58, 60), rgb3(46, 46, 48), in ? 1.0f : 0.5f);
        /* die LED darüber */
        if (lit > 0.0f) {
            col3 c = mix3(c3(C_RED_DIM), c3(C_RED), lit);
            rrect(x + 3, y + 3, 6, 4, 1.0f, c, c, 1.0f);
            bloom((float)x + 6.0f, (float)y + 5.0f, play ? 9.0f : 5.0f, c3(C_RED), play ? 0.55f : 0.25f * lit);
        } else {
            rrect(x + 3, y + 3, 6, 4, 1.0f, rgb3(40, 10, 16), rgb3(40, 10, 16), 1.0f);
        }
        if (note && (s->fl & BA_SLD))   /* SLIDE: ein Strich zur nächsten Taste */
            hline(x + 10, x + 15, y + 12, 0.8f);
        if ((ui.page == PG_SEQ1 || ui.page == PG_SEQ2) && st == ui.ssel) {   /* der gewählte Step: weiß umrandet */
            hline(x - 1, x + 12, y - 2, 0.95f);
            hline(x - 1, x + 12, y + 17, 0.95f);
            vline(x - 1, y - 2, y + 17, 0.95f);
            vline(x + 12, y - 2, y + 17, 0.95f);
        }
    }
    for (i = 1; i < 4; i++)               /* die Vierer-Gruppen, wie die Teilung am Gerät */
        vline(8 + i * 56 - 1, oy + 3, oy + 13, 0.35f);
}

static void draw_save(void)
{
    char t[24] = "TO ";
    const ba_patch *p = slot_patch(ui.save_slot);
    t[3] = (char)('0' + (ui.save_slot + 1) / 10);
    t[4] = (char)('0' + (ui.save_slot + 1) % 10);
    t[5] = 0;
    ctext(120, 14, &FONT_B, "SAVE", C_WHITE);
    ctext(120, 32, &APP_BIG_FONT, t, C_RED);
    ctext(120, 60, &FONT_XS, p->name, C_SUB);
}

static void draw_low(void)
{
    char t[32];
    const char *title = view_title(t);
    int m = mod_of_page(ui.page);
    section(3, 6, 236, 97, "SEQUENCER", 0);
    {   /* was die Punktmatrix zeigt: rechts oben, in die Linie gesetzt */
        int32_t tw = text_w(&FONT_XS, title);
        rrect(236 - tw - 14, 2, tw + 8, 9, 0.0f, c3(C_PANEL), c3(C_PANEL), 1.0f);
        cv_text(236 - tw - 10, 0, &FONT_XS, title, C_WHITE);
    }
    if (ui.saving) {
        draw_save();
    } else {
        mx_clear();
        if (m >= 0)
            show_mod(m, (ui.page - PG_M1) & 1);
        else
            switch (ui.page) {
            case PG_MAIN: show_scope(); break;
            case PG_FILTER:
            case PG_FILT2: show_filter(); break;
            case PG_FENV: show_env(1); break;
            case PG_PENV: show_pitch(); break;
            case PG_AENV:
            case PG_SET1:
            case PG_SET2: show_env(0); break;
            case PG_FX1:
            case PG_FX2:
            case PG_FX3:
            case PG_FXP: show_fx(); break;
            case PG_SEQ1:
            case PG_SEQ2: show_seq(); break;
            default: show_glo(); break;
            }
        mx_draw(MX_Y - 4);
    }
    if (ui.rec) {                        /* RECORD: eine blinkende LED */
        int on = (plat_ms() / 400u) & 1u;
        aa_disc(14.0f, 7.0f, 2.5f, on ? c3(C_RED) : c3(C_RED_DIM), 1.0f);
        if (on)
            bloom(14.0f, 7.0f, 6.0f, c3(C_RED), 0.6f);
    }
    draw_steps(78);
}

/* ---- der Kopf: BA-55 in Weiß, die 7-Segment-Anzeige, die RUN-LED ---- */
static void head_display(char *t)       /* was die Anzeige zeigt: die klingende Note, sonst den Platz */
{
    t[0] = 0;
    if (ui.saving) {
        t[0] = 'S';
        t[1] = '.';
        t[2] = (char)('0' + (ui.save_slot + 1) / 10);
        t[3] = (char)('0' + (ui.save_slot + 1) % 10);
        t[4] = 0;
        return;
    }
    if (eng.gate && eng.note >= 0) {     /* die Note, wie am DB-01: Buchstabe, Punkt = Kreuz, Oktave */
        static const char L[12] = {'C', 'C', 'D', 'D', 'E', 'F', 'F', 'G', 'G', 'A', 'A', 'B'};
        static const uint8_t SH[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
        int n = eng.note, o = n / 12 - 2;
        t[0] = ' ';
        t[1] = L[n % 12];
        t[2] = SH[n % 12] ? '.' : ' ';
        t[3] = o < 0 ? '-' : (char)('0' + (o > 9 ? 9 : o));
        t[4] = SH[n % 12] ? 0 : 0;
        if (!SH[n % 12]) {
            t[2] = o < 0 ? '-' : (char)('0' + (o > 9 ? 9 : o));
            t[3] = 0;
        }
        return;
    }
    t[0] = 'P';
    t[1] = ' ';
    t[2] = (char)('0' + (set.slot + 1) / 10);
    t[3] = (char)('0' + (set.slot + 1) % 10);
    t[4] = set.edited ? '.' : 0;
    t[5] = 0;
}

static void draw_head(uint32_t now)
{
    char t[24];
    int32_t x;
    cv_begin(240, 30, C_PANEL);
    panel_black(0, 30, 0);
    if (now < ui.msg_until) {
        x = cv_text(8, 7, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 7, &FONT_B, ui.msg[1], C_RED);
    } else {
        cv_text(8, 5, &FONT_B, "BA-55", C_WHITE);
        x = 8 + text_w(&FONT_B, "BA-55");
        cv_text(x + 7, 4, &FONT_XS, "BASSLINE", C_WHITE);
        cv_text(x + 7, 14, &FONT_XS, "SWTDR34MZ", C_SUB);
        /* RUN: eine LED, die im Takt der Steps blinkt; das Tempo darunter */
        {
            int run = ui.playing, beat = run && ba_step_now(&eng) >= 0 && (ba_step_now(&eng) % 4) == 0;
            aa_disc(146.0f, 9.0f, 2.6f, run ? c3(C_RED) : rgb3(50, 12, 18), 1.0f);
            if (beat)
                bloom(146.0f, 9.0f, 7.0f, c3(C_RED), 0.7f);
            int_text((int32_t)(ba_bpm(&eng) + 0.5f), t);
            ctext(146, 15, &FONT_XS, t, eng.ext ? C_RED : C_SUB);
        }
    }
    head_display(t);
    seg_display(162, 3, t);
    hline(0, 239, 29, 0.35f);
    cv_blit(0, 0);
}

/* ---- die Mitte: die Sektion der Seite und ihre vier Regler ---- */
static void draw_mid(uint32_t now)
{
    int gi, gn, i;
    cv_begin(240, 110, C_PANEL);
    panel_black(0, 110, 30);
    section(3, 8, 236, 106, PAGE_NAME[ui.page], edit.name);
    group_of(ui.page, &gi, &gn);
    for (i = 0; i < gn && gn > 1; i++) {     /* wo in der Gruppe: kleine LEDs */
        float x = 228.0f - (float)(gn - 1 - i) * 8.0f;
        aa_disc(x, 8.0f, 2.2f, i == gi ? c3(C_RED) : rgb3(50, 12, 18), 1.0f);
        if (i == gi)
            bloom(x, 8.0f, 5.0f, c3(C_RED), 0.5f);
    }
    if (now < ui.big_until) {
        ctext(120, 28, &FONT_XS, ui.big[1], C_WHITE);
        ctext(120, 46, &APP_BIG_FONT, ui.big[0], C_RED);
        bloom(120.0f, 56.0f, 50.0f, c3(C_RED), 0.08f);
    } else {
        for (i = 0; i < 4; i++)
            draw_knob(i, 33 + i * 58, 18, now);
    }
    cv_blit(0, 30);
}

static void draw(uint32_t mask)
{
    uint32_t now = plat_ms();
    if (mask & R_HEAD)
        draw_head(now);
    if (mask & R_MID)
        draw_mid(now);
    if (mask & R_OLED) {
        cv_begin(240, 100, C_PANEL);
        panel_black(0, 100, 140);
        draw_low();
        cv_blit(0, 140);
    }
}

/* ---- die Eröffnung: die 16 Step-LEDs laufen durch (wie wenn der DB-01 startet), die Anzeige zählt hoch,
 *      dann steht der Name ---- */
#define INTRO_MS 2200u
static void intro_scene(float t, int32_t y0, int32_t h)
{
    int i;
    float name = sat01((t - 1.0f) / 0.5f), sub = sat01((t - 1.4f) / 0.4f);
    cv_oy = -y0;
    panel_black(y0, h, y0);
    /* die Anzeige oben: erst flackernde Segmente, dann "BA.55" */
    if (y0 < 70) {
        char d[8] = "8.8.8.8.";
        if (t > 0.35f && t < 1.0f) {
            int n = (int)(t * 40.0f) % 100;
            d[0] = ' ';
            d[1] = (char)('0' + (n / 10) % 10);
            d[2] = (char)('0' + n % 10);
            d[3] = ' ';
            d[4] = 0;
        } else if (t >= 1.0f) {
            d[0] = 'B'; d[1] = 'A'; d[2] = '.'; d[3] = '5'; d[4] = '5'; d[5] = 0;
        }
        seg_display(84, 34, d);
    }
    /* die Step-Tasten: ein Lauflicht hin und zurück */
    if (y0 + h > 150 && y0 < 190) {
        float pos = t < 1.6f ? fmod_pos(t * 18.0f, 30.0f) : -10.0f;
        int head = (int)(pos < 15.0f ? pos : 30.0f - pos);
        for (i = 0; i < 16; i++) {
            int32_t x = 8 + i * 14;
            float lit = t >= 1.6f ? sat01((t - 1.6f) * 3.0f) * 0.35f : (i == head ? 1.0f : 0.0f);
            rrect(x, 160, 12, 16, 2.0f, rgb3(46, 46, 48), rgb3(28, 28, 30), 1.0f);
            rrect(x + 3, 163, 6, 4, 1.0f, mix3(rgb3(40, 10, 16), c3(C_RED), lit), mix3(rgb3(40, 10, 16), c3(C_RED), lit), 1.0f);
            if (lit > 0.5f)
                bloom((float)x + 6.0f, 165.0f, 9.0f, c3(C_RED), 0.55f * lit);
        }
    }
    if (name > 0.0f) {
        uint16_t c = c565(mix3(c3(C_PANEL), c3(C_WHITE), name));
        ctext(120, 84, &APP_BIG_FONT, "BA-55", c);
        if (y0 < 130 && y0 + h > 118) {
            int32_t x;
            for (x = 40; x < 200; x++)
                px_blend(x, 122, c3(C_LINE), 0.75f * name);
        }
    }
    if (sub > 0.0f) {
        ctext(120, 130, &FONT_XS, "BASSLINE SYNTHESIZER", c565(mix3(c3(C_PANEL), c3(C_WHITE), sub)));
        ctext(120, 200, &FONT_XS, "by swtdr34mz", c565(mix3(c3(C_PANEL), c3(C_SUB), sub)));
    }
    cv_oy = 0;
}

static void intro_draw(float t)
{
    cv_begin(240, 110, C_PANEL);
    intro_scene(t, 0, 110);
    cv_blit(0, 0);
    cv_begin(240, 110, C_PANEL);
    intro_scene(t, 110, 110);
    cv_blit(0, 110);
    cv_begin(240, 20, C_PANEL);
    intro_scene(t, 220, 20);
    cv_blit(0, 220);
}

/* ------------------------------------------------------------ der Vertrag --- */
static int16_t fx_dl[BA_DLY_FRAMES] __attribute__((section(".pool")));
static int16_t fx_dr[BA_DLY_FRAMES] __attribute__((section(".pool")));
static float fx_cho[BA_CHO_FRAMES] __attribute__((section(".pool")));
static float fx_rev[BA_REV_FLOATS] __attribute__((section(".pool")));
static float fx_wide[BA_WIDE_FRAMES] __attribute__((section(".pool")));

void app_init(void)
{
    int i;
    bank_load(0);
    bank_load(1);
    settings_load();
    ba_init(&eng, SR, &edit);
    ba_fx_memory(&eng, fx_dl, fx_dr, fx_cho, fx_rev, fx_wide);
    eng.bend_range = (float)set.bend;
    for (i = 0; i < NKEYS; i++)
        ui.key_note[i] = -1;
    ui.copied = -1;
}

static void midi_in(void)                /* im Audio-Interrupt: Noten, Pitch Bend, Clock, Transport */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, ty = st & 0xF0u;
        switch (st) {
        case 0xF8u: ba_midi_clock(&eng); continue;
        case 0xFAu: ba_play(&eng, 1); ui.playing = 1; continue;
        case 0xFBu: ba_continue(&eng); ui.playing = 1; continue;
        case 0xFCu: ba_play(&eng, 0); ui.playing = 0; continue;
        default: break;
        }
        if (st >= 0xF0u || (set.mch && (st & 0x0Fu) != set.mch - 1u))
            continue;
        if (ty == 0x90u && d2)
            ba_note_on(&eng, (int)d1, (int)d2);
        else if (ty == 0x80u || ty == 0x90u)
            ba_note_off(&eng, (int)d1);
        else if (ty == 0xE0u)
            ba_bend(&eng, (int)(d2 << 7 | d1));
        else if (ty == 0xB0u && (d1 == 123u || d1 == 120u))
            ba_all_off(&eng);
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    static float bl[256], br[256];
    float gain = (float)master_q12 * (1.0f / 4096.0f) * 8300000.0f;
    uint32_t i;
    if (n > 256u)
        n = 256u;
    midi_in();
    ba_render(&eng, bl, br, (int)n);
    {
        uint32_t w = scope_w;
        float pk = out_peak * 0.92f;     /* der Pegel fürs Display: Spitze, langsam fallend */
        for (i = 0; i < n; i++) {
            float m = 0.5f * (bl[i] + br[i]), a = m < 0.0f ? -m : m;
            lr[2 * i] = (int32_t)(bl[i] * gain);
            lr[2 * i + 1] = (int32_t)(br[i] * gain);
            scope_ring[(w + i) % SCOPE_N] = m;
            if (a > pk)
                pk = a;
        }
        scope_w = w + n;
        out_peak = pk;
    }
}

void app_panic(void)
{
    ui.playing = 0;
    ba_play(&eng, 0);
    ba_all_off(&eng);
}

void app_splash(void)                    /* der Startbildschirm: das Ende der Eröffnung, still */
{
    lcd_fill(0, 0, 240, 240, C_PANEL);
    intro_draw(3.0f);
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_PANEL);
    touch_on(1);
    ui.step_drawn = -2;
    ui.intro = 1;
    ui.intro_at = plat_ms();
    ui.redraw = R_ALL;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    int32_t s, lv, m, pk;
    if (ui.intro) {                      /* die Eröffnung: bis sie durch ist, oder eine Taste / ein Knopf drückt */
        uint32_t t = now - ui.intro_at;
        if (t < INTRO_MS && !plat_keys() && !plat_buttons()) {
            intro_draw((float)t * 0.001f);
            plat_lights(0, 0, 0, 0, 0);
            return;
        }
        ui.intro = 0;
        ui.keys = plat_keys();           /* die Taste, die abgebrochen hat, spielt nicht */
        ui.btn = plat_buttons();
        ui.redraw = R_ALL;
    }
    input();
    lights();
    ui.frame++;
    if (ann_on() && ann_where()) {
        char t[48] = "BA-55. ";
        text_cat(t, PAGE_NAME[ui.page]);
        text_cat(t, ". ");
        text_cat(t, edit.name);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(now);
    if (ui.playing != (uint8_t)eng.running) {
        ui.playing = (uint8_t)eng.running;
        ui.redraw |= R_HEAD;
    }
    if (ui.saving && now > ui.save_until) {
        ui.saving = 0;
        ui.redraw |= R_OLED;
    }
    for (m = 0; m < 3; m++)              /* das Oszilloskop der Modulatoren */
        ui.scope[m][ui.scope_pos] = ba_mod_out(&eng, m);
    ui.scope_pos = (uint8_t)((ui.scope_pos + 1u) % 96u);
    if (view_animated() && !(ui.frame & 1u))   /* lebende Kurven: 30 Bilder pro Sekunde */
        ui.redraw |= R_OLED;
    pk = (int32_t)(fm_sqrtf(out_peak < 1.0f ? out_peak : 1.0f) * 8.0f);
    if (pk != ui.peak_drawn) {           /* der Pegel im Kopf */
        ui.peak_drawn = pk;
        ui.redraw |= R_HEAD;
    }
    s = ba_step_now(&eng);
    lv = (int32_t)(eng.level_now * 8.0f);
    if (s != ui.step_drawn) {            /* ein neuer Step: die Step-Tasten, und die RUN-LED im Kopf */
        ui.step_drawn = s;
        ui.redraw |= R_OLED | R_HEAD;
    }
    ui.lvl_drawn = lv;
    {   /* die 7-Segment-Anzeige zeigt die klingende Note */
        int nd = eng.gate ? eng.note : -1;
        if (nd != ui.note_drawn) {
            ui.note_drawn = nd;
            ui.redraw |= R_HEAD;
        }
    }
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw |= R_HEAD;
    }
    if (ui.big_until && now >= ui.big_until) {
        ui.big_until = 0;
        ui.redraw |= R_MID;
    }
    {   /* ein gedrehter Regler verlischt nach 0.7 s wieder */
        int k;
        for (k = 0; k < 4; k++)
            if (ui.knob_at[k] && now - ui.knob_at[k] >= 700u) {
                ui.knob_at[k] = 0;
                ui.redraw |= R_MID;
            }
    }
    if (now - ui.load_at >= 500u) {
        uint32_t l = plat_cpu_pct() | plat_xruns() << 8;
        ui.load_at = now;
        if (l != ui.load_shown)
            ui.load_shown = l;
        /* bleibt die Last eine Sekunde über 88 %, rechnet die Stimme ohne Oversampling (ECO) weiter */
        if (plat_cpu_pct() > 88u && !eng.eco) {
            if (++ui.hot >= 2) {
                eng.eco = 1;
                app_say("ECO MODE", "CPU HIGH");
            }
        } else {
            ui.hot = 0;
        }
    }
    /* gespeichert nur, wenn der Sequencer steht: Flash schreiben hält den Audio-Interrupt an */
    if (!ui.playing && set_dirty && now - dirty_at > 1500u)
        settings_save();
    if (ui.redraw) {
        uint32_t r = ui.redraw;
        ui.redraw = 0;
        draw(r);
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "playing"))
        *got = ui.playing;
    else if (!strcmp(what, "step"))
        *got = ba_step_now(&eng);
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "slot"))
        *got = set.slot;
    else if (!strcmp(what, "edited"))
        *got = set.edited;
    else if (!strcmp(what, "bpm"))
        *got = edit.bpm;
    else if (!strcmp(what, "len"))
        *got = ba_seq_len(&eng);
    else if (!strcmp(what, "rec"))
        *got = ui.rec;
    else if (!strcmp(what, "ssel"))
        *got = ui.ssel;
    else if (!strcmp(what, "note"))
        *got = eng.note;
    else if (!strcmp(what, "gate"))
        *got = eng.gate;
    else if (!strcmp(what, "hold"))
        *got = ui.hold;
    else if (!strcmp(what, "oct"))
        *got = set.oct;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "dest"))
        *got = ui.dest[0];
    else if (!strncmp(what, "seqfl", 5))
        *got = edit.seq[atoi(what + 5) - 1].fl;
    else if (!strncmp(what, "seqnote", 7))
        *got = edit.seq[atoi(what + 7) - 1].note;
    else if (!strncmp(what, "lvl", 3))   /* lvlMD: Modulator M, Ziel D */
        *got = edit.lvl[what[3] - '1'][atoi(what + 4)];
    else if (what[0] == 'p' && what[1] >= '0' && what[1] <= '9')   /* pN: Parameter N */
        *got = edit.p[atoi(what + 1)];
    else
        return 0;
    return 1;
}
#endif
