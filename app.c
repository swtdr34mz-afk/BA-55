/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55: Bedienung, Seiten, Presets, Sequencer-Editor, Bildschirm. Teil des Unity-Builds (device/unity.c,
 * host/plat_host.c), nach der Plattform: gfx.c und announce.c sind sichtbar. Der Klang ist dsp/ba55_core.c.
 *
 * Bedienung: die Bedienelemente der Erica Synths Bassline DB-01, auf die Knöpfe des FM-1 verteilt
 *   K1..K4     die vier Regler der Seite
 *   HOME       VCO: WAVE, SUB, NOISE, DETUNE; nochmal: GROOVE (STYLE, KICK, DRUMS, PUMP);
 *              nochmal: GROOVE SETUP (RELEASE, HATS, TUNE, VAR)
 *   SEL        VCF: CUTOFF, RESO, MODE (LP / BP), DRIVE
 *   ENV        ENV: DECAY, DEPTH (die Filterhüllkurve), RELEASE (VCA; ganz rechts offen), GLIDE
 *   LFO        LFO: RATE, FM LVL, LFO LVL, SHAPE; nochmal: SYNC, RESET
 *   SEQ        STEP: STEP, NOTE, GATE, ACC/SLIDE; nochmal: STEP, MOD, PROB; nochmal: STEP, PITCH ENV DEPTH, DECAY
 *   GLO        PATTERN: LAST STEP, PLAY MODE, SHUFFLE, TEMPO; nochmal: SETTINGS (TUNE, MIDI CH, BEND)
 *   EDIT       RND PATT: TARGET, AMOUNT, LOW, HIGH; nochmal: SCALE, ROOT
 *   FX         (frei: der DB-01 hat keine Effekte)
 *   SELECT     CUTOFF; REC gehalten und SELECT gedreht, während das Pattern läuft: die Mod-Spur des Steps
 *   ALGORITHM  DRIVE                  PRESET   die 16 Plätze
 *   PLAY       Start / Stop           REC   STEP-Seiten: Noten einspielen; RND PATT: ein neues Pattern;
 *                                           GROOVE-Seiten: der Groove an / aus;
 *                                           sonst 1 s halten: das Werks-Preset
 *   SAVE       speichern: PRESET wählt den Platz, SAVE nochmal speichert
 *   ARP        der Arpeggiator an / aus, dazu seine Seite (DIR, OCT, GATE, LATCH); auf den STEP-Seiten: Step kopieren / einfügen
 *   OCT- OCT+  die Oktave der Tasten; auf den STEP-Seiten: die Oktave des Steps
 *   Tasten     spielen; läuft das Pattern, transponieren sie es (C3 = unverändert) */
#include "fastmath.h"
#include "ba55_core.h"

#define SR 44100.0f

static ba_engine eng;
static ba_patch edit;                    /* der Klang in Arbeit (den die Engine spielt) */

/* ------------------------------------------------------------ Speicher --- */
#define SET_MAGIC 0x33354142u            /* "BA53" (mit GROOVE) */
#define BANK_MAGIC 0x43354142u           /* "BA5C" */
#define NSLOTS 16
typedef struct {
    uint32_t magic;
    uint8_t slot, edited, oct, bend, mch, tune, rsv[2];   /* tune: 0..20 = 430..450 Hz */
    ba_groove grv;                       /* GROOVE: global, damit ein Presetwechsel den Beat nicht ändert */
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
    int i;
    for (i = 0; i < BA_NP; i++)
        if (p->p[i] > 127u)
            return 0;
    for (i = 0; i < BA_STEPS; i++) {
        const ba_step *s = &p->seq[i];
        if (s->note < 12u || s->note > 108u || s->gate > 10u || s->fl > 3u || s->mod > 127u
            || BA_PE_DEPTH(s->pe) > 9u || BA_PE_DECAY(s->pe) > 9u || s->prob >= BA_NPROB)
            return 0;
    }
    return p->bpm >= 30u && p->bpm <= 300u && p->name[BA_NAMELEN - 1] == 0;
}

static ba_patch *slot_patch(int s) { return &bank[s >= 8].p[s & 7]; }

/* ein Klang aus v0.1 (vor den FX: 31 Parameter, 426 Bytes) in die neue Form: alles bleibt, die FX kommen aus
 * (MIX 0), mit ihren Standardwerten. old darf im selben Speicher liegen (es wird zuerst kopiert) */
#define V1_NP 31
#define V1_PATCH 426
static void patch_from_v1(const uint8_t *old, ba_patch *np)
{
    uint8_t o[V1_PATCH];
    memcpy(o, old, V1_PATCH);
    ba_patch_init(np);
    memcpy(np->name, o, BA_NAMELEN);
    memcpy(&np->bpm, o + BA_NAMELEN, 2);
    memcpy(np->p, o + BA_NAMELEN + 2, V1_NP);
    memcpy(np->seq, o + BA_NAMELEN + 2 + V1_NP, sizeof np->seq);
}

static void bank_load(int b)
{
    int i, got = plat_store_load(OBJ_BANK_A + (uint32_t)b, &bank[b], sizeof bank[b]), ok;
    if (got == 4 + 8 * V1_PATCH && bank[b].magic == BANK_MAGIC)   /* eine Bank aus v0.1: von hinten nach vorn umsetzen */
        for (i = 7; i >= 0; i--)
            patch_from_v1((const uint8_t *)&bank[b] + 4 + i * V1_PATCH, &bank[b].p[i]);
    ok = (got == (int)sizeof bank[b] || got == 4 + 8 * V1_PATCH) && bank[b].magic == BANK_MAGIC;
    for (i = 0; ok && i < 8; i++)
        ok = patch_ok(&bank[b].p[i]);
    if (!ok) {
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

static void edited(void)
{
    set.edited = 1;
    ba_changed(&eng);
    dirty();
}

static void settings_load(void)
{
    int off = (int)__builtin_offsetof(settings_t, edit), got = plat_store_load(OBJ_SETTINGS, &set, sizeof set);
    if (got == ((off + V1_PATCH + 3) & ~3) && set.magic == SET_MAGIC) {   /* aus v0.1: der Klang in Arbeit bleibt */
        patch_from_v1((const uint8_t *)&set + off, &set.edit);
        got = (int)sizeof set;
    }
    if (got != (int)sizeof set || set.magic != SET_MAGIC
        || set.slot >= NSLOTS || !patch_ok(&set.edit) || set.oct > 6u || set.bend < 1u || set.bend > 12u
        || set.mch > 16u || set.tune > 20u || set.grv.style >= BA_NSTYLES || set.grv.kick > 127u
        || set.grv.drums > 127u || set.grv.pump > 127u || set.grv.rel > 127u || set.grv.hats > 127u
        || set.grv.tune > 127u || set.grv.var > 127u) {
        memset(&set, 0, sizeof set);
        set.magic = SET_MAGIC;
        set.oct = 3;
        set.bend = 2;
        set.tune = 10;                   /* 440 Hz */
        ba_groove_init(&set.grv);
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
static void e_on(int note, int force)
{
    uint32_t f = plat_lock();
    if (force)
        ba_note_on_play(&eng, note, 100);
    else
        ba_note_on(&eng, note, 100);
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
static void e_arp(int on)
{
    uint32_t f = plat_lock();
    ba_arp(&eng, on);
    plat_unlock(f);
}

/* ------------------------------------------------------------------ Seiten --- */
enum { PG_VCO, PG_VCF, PG_ENV, PG_LFO, PG_LFO2, PG_STEP, PG_STEP2, PG_STEP3, PG_PATT, PG_SETUP, PG_RND, PG_SCALE,
       PG_ARP, PG_GRV, PG_GSET, PG_FX, NPAGES };
static const char *const PAGE_NAME[NPAGES] = {"VCO", "VCF", "ENV", "LFO", "LFO", "STEP", "STEP MOD", "STEP PITCH ENV",
                                              "PATTERN", "SETTINGS", "RND PATT", "SCALE", "ARPEGGIATOR", "GROOVE",
                                              "GROOVE SETUP", "FX"};

enum { K_NONE, K_PARAM, K_SSEL, K_SNOTE, K_SGATE, K_SFL, K_SMOD, K_SPROB, K_SPED, K_SPEK, K_TEMPO, K_TUNE, K_MCH,
       K_BEND, K_GRV };
enum { GV_STYLE, GV_KICK, GV_DRUMS, GV_PUMP, GV_REL, GV_HATS, GV_TUNE, GV_VAR };
typedef struct { uint8_t kind, id; } kdef;

static const kdef PAGE_K[NPAGES][4] = {
    {{K_PARAM, P_WAVE}, {K_PARAM, P_SUB}, {K_PARAM, P_NOISE}, {K_PARAM, P_DETUNE}},
    {{K_PARAM, P_CUTOFF}, {K_PARAM, P_RESON}, {K_PARAM, P_FMODE}, {K_PARAM, P_DRIVE}},
    {{K_PARAM, P_FDECAY}, {K_PARAM, P_FDEPTH}, {K_PARAM, P_VREL}, {K_PARAM, P_GLIDE}},
    {{K_PARAM, P_LRATE}, {K_PARAM, P_FMLVL}, {K_PARAM, P_LFOLVL}, {K_PARAM, P_LSHAPE}},
    {{K_PARAM, P_LSYNC}, {K_PARAM, P_LRESET}, {K_NONE, 0}, {K_NONE, 0}},
    {{K_SSEL, 0}, {K_SNOTE, 0}, {K_SGATE, 0}, {K_SFL, 0}},
    {{K_SSEL, 0}, {K_SMOD, 0}, {K_SPROB, 0}, {K_NONE, 0}},
    {{K_SSEL, 0}, {K_SPED, 0}, {K_SPEK, 0}, {K_NONE, 0}},
    {{K_PARAM, P_LAST}, {K_PARAM, P_PMODE}, {K_PARAM, P_SHUFFLE}, {K_TEMPO, 0}},
    {{K_TUNE, 0}, {K_MCH, 0}, {K_BEND, 0}, {K_NONE, 0}},
    {{K_PARAM, P_RTGT}, {K_PARAM, P_RAMT}, {K_PARAM, P_RLO}, {K_PARAM, P_RHI}},
    {{K_PARAM, P_SCALE}, {K_PARAM, P_ROOT}, {K_NONE, 0}, {K_NONE, 0}},
    {{K_PARAM, P_ADIR}, {K_PARAM, P_AOCT}, {K_PARAM, P_AGATE}, {K_PARAM, P_ALATCH}},
    {{K_GRV, GV_STYLE}, {K_GRV, GV_KICK}, {K_GRV, GV_DRUMS}, {K_GRV, GV_PUMP}},
    {{K_GRV, GV_REL}, {K_GRV, GV_HATS}, {K_GRV, GV_TUNE}, {K_GRV, GV_VAR}},
    {{K_PARAM, P_FX}, {K_PARAM, P_FXAMT}, {K_PARAM, P_FXMOT}, {K_PARAM, P_RVMIX}},
};

static uint8_t *grv_field(int f)
{
    uint8_t *g[8] = {&set.grv.style, &set.grv.kick, &set.grv.drums, &set.grv.pump, &set.grv.rel, &set.grv.hats,
                     &set.grv.tune, &set.grv.var};
    return g[f & 7];
}

enum { R_HEAD = 1, R_MID = 2, R_LOW = 4, R_ALL = 7 };

static struct {
    char msg[2][24];
    uint32_t msg_until;
    char big[2][20];
    uint32_t big_until;
    char seg[8];                         /* was die 7-Segment-Anzeige gerade zeigt (ein Wert beim Drehen) */
    uint32_t seg_until;
    uint32_t knob_at[4], enc_at[NE];
    uint32_t keys, btn;
    uint8_t page, playing, arp, redraw;
    uint8_t intro;
    uint32_t intro_at, frame;
    uint8_t ssel;                        /* der Step im Editor */
    uint8_t rec;                         /* Noten einspielen */
    int8_t copied;
    ba_step clip;
    uint8_t saving, save_slot;
    uint32_t save_until;
    uint32_t rec_t;                      /* REC gedrückt seit */
    uint8_t rec_used;                    /* REC wurde mit SELECT benutzt (die Mod-Spur) */
    int8_t key_note[NKEYS];
    int32_t step_drawn, note_drawn;
    int16_t batt_drawn;                  /* der Akku, wie er gezeichnet ist (Prozent, USB, Blinken mit Halbsekunde) */
    int8_t batt_pct;                     /* die angezeigten Prozent (-1: noch keine), mit etwas Hysterese */
    uint8_t batt_test;
    int8_t batt_tpct;                    /* (Tests: vorgegebene Prozent, -1 keine) */
    uint32_t load_at;
    uint8_t hot;
    float lfo_scope[96];
    uint8_t lfo_pos;
    uint32_t kick_seen, flash_until;     /* GROOVE: der Kick-Blitz (ARP-LED) */
} ui;

static float fabsf_(float x) { return x < 0.0f ? -x : x; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int is_step_page(int pg) { return pg == PG_STEP || pg == PG_STEP2 || pg == PG_STEP3; }

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
static void dec1_text(float v, char *b)
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
    t[0] = 0;
    text_cat(t, BA_NOTE_NAME[n % 12]);
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

/* die 7-Segment-Anzeige zeigt beim Drehen kurz den Wert, wie am Gerät */
static void seg_show(const char *s)
{
    uint32_t i;
    for (i = 0; i < 7u && s[i]; i++)
        ui.seg[i] = s[i];
    ui.seg[i] = 0;
    ui.seg_until = plat_ms() + 900u;
    ui.redraw |= R_HEAD;
}

/* ---------------------------------------------------------------- die Regler --- */
static kdef kdef_of(int k) { return PAGE_K[ui.page][k]; }

static const char *knob_label(kdef d)
{
    switch (d.kind) {
    case K_PARAM:
        if (d.id == P_RLO || d.id == P_RHI || d.id == P_RAMT) {   /* RND PATT: was die Grenzen bedeuten */
            int t = ba_list_index(P_RTGT, edit.p[P_RTGT]);
            if (d.id == P_RAMT)
                return t == RT_ON ? "STEPS ON" : "AMOUNT";
            if (t == RT_ACC || t == RT_SLIDE || t == RT_PROB || t == RT_ON)
                return "-";
        }
        return BA_PARAMS[d.id].label;
    case K_SSEL: return "STEP";
    case K_SNOTE: return "NOTE";
    case K_SGATE: return "GATE";
    case K_SFL: return "ACC/SLD";
    case K_SMOD: return "MOD";
    case K_SPROB: return "PROB";
    case K_SPED: return "DEPTH";
    case K_SPEK: return "DECAY";
    case K_TEMPO: return "TEMPO";
    case K_TUNE: return "TUNE";
    case K_MCH: return "MIDI CH";
    case K_BEND: return "BEND";
    case K_GRV: {
        static const char *const N[8] = {"STYLE", "KICK", "DRUMS", "PUMP", "RELEASE", "HATS", "TUNE", "VAR"};
        return N[d.id & 7];
    }
    default: return "";
    }
}

static int knob_get(kdef d)
{
    const ba_step *s = &edit.seq[ui.ssel];
    switch (d.kind) {
    case K_PARAM: return edit.p[d.id];
    case K_SSEL: return ui.ssel * 127 / (BA_STEPS - 1);
    case K_SNOTE: return (s->note - 12) * 127 / 96;
    case K_SGATE: return s->gate * 127 / 10;
    case K_SFL: return s->fl * 127 / 3;
    case K_SMOD: return s->mod;
    case K_SPROB: return s->prob * 127 / (BA_NPROB - 1);
    case K_SPED: return BA_PE_DEPTH(s->pe) * 127 / 9;
    case K_SPEK: return BA_PE_DECAY(s->pe) * 127 / 9;
    case K_TEMPO: return ((int)edit.bpm - 30) * 127 / 270;
    case K_TUNE: return set.tune * 127 / 20;
    case K_MCH: return set.mch * 127 / 16;
    case K_BEND: return (set.bend - 1) * 127 / 11;
    case K_GRV: return d.id == GV_STYLE ? set.grv.style * 127 / (BA_NSTYLES - 1) : *grv_field(d.id);
    default: return -1;
    }
}

static int knob_bip(kdef d) { return d.kind == K_SMOD || (d.kind == K_GRV && d.id == GV_TUNE); }

static void val_text(kdef d, char *t)
{
    int v = knob_get(d);
    const ba_step *s = &edit.seq[ui.ssel];
    t[0] = 0;
    switch (d.kind) {
    case K_PARAM: {
        const ba_param_info *pi = &BA_PARAMS[d.id];
        int id = d.id, tg = ba_list_index(P_RTGT, edit.p[P_RTGT]);
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
        switch (id) {
        case P_CUTOFF: hz_text(ba_cutoff_hz(v), t); return;
        case P_FDECAY: ms_text(ba_fdecay_ms(v), t); return;
        case P_VREL:
            if (ba_vrel_ms(v) < 0.0f)
                text_cat(t, "OPEN");
            else
                ms_text(ba_vrel_ms(v), t);
            return;
        case P_GLIDE:
            if (!v)
                text_cat(t, "OFF");
            else
                ms_text(ba_glide_ms(v), t);
            return;
        case P_LRATE:
            if (edit.p[P_LSYNC] >= 64)
                text_cat(t, BA_SYNC_NAME[(v * (BA_NSYNC - 1) + 63) / 127]);
            else
                hz_text(ba_lfo_hz(v), t);
            return;
        case P_SHUFFLE: int_text(50 + v * 25 / 127, t); text_cat(t, "%"); return;
        case P_RLO:
        case P_RHI:
            switch (tg) {
            case RT_PATTERN:
            case RT_NOTES: text_cat(t, "+"); int_text(v * 24 / 127, t + 1); text_cat(t, "ST"); return;
            case RT_GATES: int_text(10 + v * 9 / 127 * 10, t); text_cat(t, "%"); return;
            case RT_PENV: int_text(1 + v * 8 / 127, t); return;
            case RT_MOD: int_text((v - 64) * 100 / 63, t); return;
            default: text_cat(t, "-"); return;
            }
        case P_RAMT: int_text(v * 100 / 127, t); text_cat(t, "%"); return;
        case P_FXAMT: int_text(v * 100 / 127, t); text_cat(t, "%"); return;
        case P_RVMIX: int_text(v * 100 / 127, t); text_cat(t, "%"); return;
        case P_FXMOT:                    /* je nach Effekt: Hz, eine Teilung im Takt, oder wie schnell */
            if (v < 2) {
                text_cat(t, "STILL");
                return;
            }
            switch (BA_FXPRESET[ba_list_index(P_FX, edit.p[P_FX])].mod) {
            case FM_CLOCK: {
                static const char *const D[6] = {"1 BAR", "1/2", "1/4", "1/8", "1/16", "1/32"};
                text_cat(t, D[(v * 5 + 63) / 127]);
                return;
            }
            case FM_LFO: hz_text(0.1f * fm_exp2f((float)v / 127.0f * 6.6f), t); return;
            default: int_text(v * 100 / 127, t); text_cat(t, "%"); return;
            }
        default: break;
        }
        int_text(v * 100 / 127, t);
        return;
    }
    case K_SSEL: int_text(ui.ssel + 1, t); return;
    case K_SNOTE: note_text(s->note, t); return;
    case K_SGATE:
        if (!s->gate)
            text_cat(t, "OFF");
        else if (s->gate >= 10)
            text_cat(t, "TIE");
        else {
            int_text(s->gate * 10, t);
            text_cat(t, "%");
        }
        return;
    case K_SFL: {
        static const char *const F[4] = {"-", "ACCENT", "SLIDE", "ACC+SLD"};
        text_cat(t, F[s->fl & 3u]);
        return;
    }
    case K_SMOD: {
        int b = (s->mod - 64) * 100 / 63;
        if (b > 0)
            text_cat(t, "+");
        int_text(b, t + txt_len(t));
        return;
    }
    case K_SPROB: text_cat(t, BA_PROB_NAME[s->prob]); return;
    case K_SPED:
        if (!BA_PE_DEPTH(s->pe))
            text_cat(t, "OFF");
        else
            int_text(BA_PE_DEPTH(s->pe), t);
        return;
    case K_SPEK: int_text(BA_PE_DECAY(s->pe) ? BA_PE_DECAY(s->pe) : 1, t); return;
    case K_TEMPO: int_text(edit.bpm, t); return;
    case K_TUNE: int_text(430 + set.tune, t); text_cat(t, "HZ"); return;
    case K_MCH:
        if (!set.mch)
            text_cat(t, "ALL");
        else
            int_text(set.mch, t);
        return;
    case K_BEND: int_text(set.bend, t); text_cat(t, " ST"); return;
    case K_GRV:
        switch (d.id) {
        case GV_STYLE: text_cat(t, BA_STYLE_NAME[set.grv.style]); return;
        case GV_REL: int_text(40 + set.grv.rel * 560 / 127, t); text_cat(t, "MS"); return;
        case GV_TUNE: {
            int st = (set.grv.tune - 64) * 12 / 64;
            if (st > 0)
                text_cat(t, "+");
            int_text(st, t + txt_len(t));
            text_cat(t, " ST");
            return;
        }
        default: int_text(v * 100 / 127, t); return;
        }
    default: return;
    }
}

/* der Wert für die 7-Segment-Anzeige: vier Stellen */
static void seg_value(kdef d)
{
    char t[12] = "";
    int v = knob_get(d);
    const ba_step *s = &edit.seq[ui.ssel];
    switch (d.kind) {
    case K_SNOTE: {
        static const char L[12] = {'C', 'C', 'D', 'D', 'E', 'F', 'F', 'G', 'G', 'A', 'A', 'B'};
        int sh = (0x54Au >> (s->note % 12)) & 1u, o = s->note / 12 - 2;
        t[0] = ' ';
        t[1] = L[s->note % 12];
        t[2] = sh ? '.' : 0;
        t[sh ? 3 : 2] = o < 0 ? '-' : (char)('0' + o);
        t[sh ? 4 : 3] = 0;
        break;
    }
    case K_SGATE:
        if (!s->gate)
            text_cat(t, " OFF");
        else if (s->gate >= 10)
            text_cat(t, " tIE");
        else
            int_text(s->gate * 10, t);
        break;
    case K_SFL: {
        static const char *const F[4] = {"  --", " ACC", " SLd", "A.S.  "};
        text_cat(t, F[s->fl & 3u]);
        break;
    }
    case K_SSEL: int_text(ui.ssel + 1, t); break;
    case K_SMOD: int_text((s->mod - 64) * 99 / 63, t); break;
    case K_SPED: int_text(BA_PE_DEPTH(s->pe), t); break;
    case K_SPEK: int_text(BA_PE_DECAY(s->pe), t); break;
    case K_SPROB: {
        static const char *const P[BA_NPROB] = {"100", " 75", " 50", " 25", "1-2", "1-3", "1-4"};
        text_cat(t, P[s->prob]);
        break;
    }
    case K_TEMPO: int_text(edit.bpm, t); break;
    case K_TUNE: int_text(430 + set.tune, t); break;
    case K_MCH: int_text(set.mch, t); break;
    case K_BEND: int_text(set.bend, t); break;
    case K_PARAM:
        if (BA_PARAMS[d.id].kind == MK_LIST || BA_PARAMS[d.id].kind == MK_SW) {
            if (d.id == P_FMODE) {
                text_cat(t, v >= 64 ? "  BP" : "  LP");
                break;
            }
            int_text(BA_PARAMS[d.id].kind == MK_SW ? (v >= 64) : ba_list_index(d.id, v) + 1, t);
            break;
        }
        if (d.id == P_VREL && v >= 127) {
            text_cat(t, "OPEN");
            break;
        }
        int_text(v * 99 / 127, t);
        break;
    default: return;
    }
    {   /* rechtsbündig in vier Stellen (Punkte zählen nicht) */
        char o[12] = "";
        int n = 0, i;
        for (i = 0; t[i]; i++)
            n += t[i] != '.';
        for (i = n; i < 4; i++)
            text_cat(o, " ");
        text_cat(o, t);
        seg_show(o);
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

/* wie weit ein Regler pro Raste geht: VCO, VCF und ENV schnell, der Rest fein */
static int32_t knob_step(int k, int32_t e)
{
    switch (ui.page) {
    case PG_VCO:
    case PG_VCF:
    case PG_ENV: return accel(EN_K1 + k, e, 2, 4) * 3;
    default: return accel(EN_K1 + k, e, 3, 6);
    }
}

static int coarse(int k, int32_t e);
static void knob_turn(int k, int32_t e)
{
    kdef d = kdef_of(k);
    int dir = e > 0 ? 1 : -1;
    ba_step *s = &edit.seq[ui.ssel];
    if (d.kind == K_NONE)
        return;
    ui.knob_at[k] = plat_ms();
    ui.redraw |= R_ALL;
    switch (d.kind) {
    case K_PARAM: {
        const ba_param_info *pi = &BA_PARAMS[d.id];
        int v = edit.p[d.id];
        if (pi->kind == MK_LIST)
            v = ba_list_value(d.id, ba_list_index(d.id, v) + dir);
        else if (pi->kind == MK_SW)
            v = dir > 0 ? 127 : 0;
        else if (d.id == P_RVMIX)        /* REVERB: grob wie SELECT, 2,5 pro Raste, damit man live schnell hinkommt */
            v = iclamp(v + coarse(EN_K1 + k, e), 0, 127);
        else
            v = iclamp(v + knob_step(k, e), 0, 127);
        if (v != edit.p[d.id]) {
            edit.p[d.id] = (uint8_t)v;
            if (d.id == P_LAST && ui.ssel >= ba_seq_len(&eng))
                ui.ssel = (uint8_t)(ba_seq_len(&eng) - 1);
            edited();
        }
        break;
    }
    case K_SSEL: ui.ssel = (uint8_t)iclamp(ui.ssel + dir, 0, BA_STEPS - 1); break;
    case K_SNOTE:
        s->note = (uint8_t)iclamp(s->note + accel(EN_K1 + k, e, 1, 3), 12, 108);
        if (!s->gate)                    /* eine Note auf einem leeren Step: er spielt wieder */
            s->gate = 5;
        edited();
        break;
    case K_SGATE: s->gate = (uint8_t)iclamp(s->gate + dir, 0, 10); edited(); break;
    case K_SFL: s->fl = (uint8_t)iclamp(s->fl + dir, 0, 3); edited(); break;
    case K_SMOD: s->mod = (uint8_t)iclamp(s->mod + accel(EN_K1 + k, e, 3, 6), 0, 127); edited(); break;
    case K_SPROB: s->prob = (uint8_t)iclamp(s->prob + dir, 0, BA_NPROB - 1); edited(); break;
    case K_SPED:
        s->pe = (uint8_t)(iclamp(BA_PE_DEPTH(s->pe) + dir, 0, 9) << 4 | (BA_PE_DECAY(s->pe) ? BA_PE_DECAY(s->pe) : 5));
        edited();
        break;
    case K_SPEK: s->pe = (uint8_t)(BA_PE_DEPTH(s->pe) << 4 | iclamp(BA_PE_DECAY(s->pe) + dir, 1, 9)); edited(); break;
    case K_TEMPO: edit.bpm = (uint16_t)iclamp((int)edit.bpm + accel(EN_K1 + k, e, 2, 5), 30, 300); edited(); break;
    case K_TUNE:
        set.tune = (uint8_t)iclamp(set.tune + dir, 0, 20);
        eng.tune_hz = 430.0f + (float)set.tune;
        dirty();
        break;
    case K_MCH: set.mch = (uint8_t)iclamp(set.mch + dir, 0, 16); dirty(); break;
    case K_BEND:
        set.bend = (uint8_t)iclamp(set.bend + dir, 1, 12);
        eng.bend_range = (float)set.bend;
        dirty();
        break;
    case K_GRV:
        if (d.id == GV_STYLE)
            set.grv.style = (uint8_t)iclamp(set.grv.style + dir, 0, BA_NSTYLES - 1);
        else
            *grv_field(d.id) = (uint8_t)iclamp(*grv_field(d.id) + accel(EN_K1 + k, e, 3, 6), 0, 127);
        dirty();
        return;
    default: return;
    }
    seg_value(d);
}

static void page_set(int pg)
{
    int i;
    for (i = 0; i < 4; i++)
        ui.knob_at[i] = 0;
    ui.big_until = 0;
    if (ui.rec && !is_step_page(pg))     /* die STEP-Seiten verlassen: das Einspielen endet */
        ui.rec = 0;
    ui.page = (uint8_t)pg;
    ui.redraw |= R_ALL;
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

static void factory(void)
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

static void rec_key(int note)            /* Noten einspielen: die Taste schreibt den Step und geht weiter */
{
    ba_step *s = &edit.seq[ui.ssel];
    int len = ba_seq_len(&eng);
    s->note = (uint8_t)iclamp(note, 12, 108);
    if (!s->gate)
        s->gate = 5;
    if (ui.ssel + 1 >= len && len < BA_STEPS)
        edit.p[P_LAST] = (uint8_t)ba_list_value(P_LAST, len);   /* hinter dem letzten Step: das Pattern wächst */
    if (ui.ssel + 1 >= BA_STEPS) {
        ui.rec = 0;
        app_say("RECORD", "64 STEPS");
    } else {
        ui.ssel++;
    }
    edited();
}

static void random_pattern(void)
{
    ba_randomize(&edit, plat_ms() * 2654435761u + ui.frame * 40503u + 1u);
    edited();
    app_say("RND PATT", BA_RTGT_NAME[ba_list_index(P_RTGT, edit.p[P_RTGT])]);
}

/* ------------------------------------------------------------------ Eingabe --- */
static int key_base(void) { return 53 + 12 * ((int)set.oct - 3); }   /* Taste 0 = F3 (MIDI 53) */

static void keys_input(uint32_t keys)
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit)) {
            int note = key_base() + (int)k;
            if (note < 12 || note > 115)
                continue;
            ui.key_note[k] = (int8_t)note;
            if (ui.rec) {
                e_on(note, 1);
                rec_key(note);
            } else {
                e_on(note, 0);
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
    int stp = is_step_page(ui.page);
    if (ui.saving && i != B_SAVE) {
        ui.saving = 0;
        app_say("SAVE", "CANCELLED");
        return;
    }
    switch (i) {
    case B_HOME: page_set(ui.page == PG_VCO ? PG_GRV : ui.page == PG_GRV ? PG_GSET : PG_VCO); break;   /* VCO, GROOVE, GROOVE SETUP */
    case B_SEL: page_set(PG_VCF); break;
    case B_ENV: page_set(PG_ENV); break;
    case B_LFO: page_set(ui.page == PG_LFO ? PG_LFO2 : PG_LFO); break;
    case B_SEQ: page_set(ui.page == PG_STEP ? PG_STEP2 : ui.page == PG_STEP2 ? PG_STEP3 : PG_STEP); break;
    case B_GLO: page_set(ui.page == PG_PATT ? PG_SETUP : PG_PATT); break;
    case B_EDIT: page_set(ui.page == PG_RND ? PG_SCALE : PG_RND); break;
    case B_FX: page_set(PG_FX); break;   /* FX: eine Seite, vier Regler */
    case B_PLAY: play_toggle(); break;
    case B_REC:
        ui.rec_t = plat_ms();            /* was REC tut, entscheidet das Loslassen (oder 1 s Halten) */
        ui.rec_used = 0;
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
        if (stp) {                       /* Step kopieren, dann einfügen */
            if (ui.copied < 0) {
                ui.clip = edit.seq[ui.ssel];
                ui.copied = (int8_t)ui.ssel;
                app_say("COPY", "STEP");
            } else {
                edit.seq[ui.ssel] = ui.clip;
                ui.copied = -1;
                edited();
                app_say("PASTE", "");
            }
        } else {                         /* der Arpeggiator an / aus, dazu seine Seite */
            ui.arp = (uint8_t)!ui.arp;
            e_arp(ui.arp);
            page_set(PG_ARP);
            app_say("ARP", ui.arp ? "ON" : "OFF");
        }
        break;
    case B_OCTDN:
    case B_OCTUP: {
        char t[8] = "";
        int up = i == B_OCTUP;
        if (stp) {                       /* die Oktave des Steps */
            ba_step *s = &edit.seq[ui.ssel];
            s->note = (uint8_t)iclamp(s->note + (up ? 12 : -12), 12, 108);
            edited();
            seg_value(PAGE_K[PG_STEP][1]);
            break;
        }
        set.oct = (uint8_t)iclamp((int)set.oct + (up ? 1 : -1), 0, 6);
        dirty();
        if (set.oct > 3)
            text_cat(t, "+");
        int_text((int)set.oct - 3, t + txt_len(t));
        app_say("OCTAVE", t);
        break;
    }
    default: break;
    }
}

static void rec_release(void)            /* REC losgelassen (vor 1 s, ohne SELECT): je nach Seite */
{
    if (is_step_page(ui.page)) {
        ui.rec = (uint8_t)!ui.rec;
        ui.redraw |= R_ALL;
        app_say(ui.rec ? "RECORD" : "RECORD OFF", ui.rec ? "PLAY KEYS" : "");
    } else if (ui.page == PG_RND) {
        random_pattern();
    } else if (ui.page == PG_GRV || ui.page == PG_GSET) {   /* auf den GROOVE-Seiten: der Groove an / aus */
        set.grv.on = (uint8_t)!set.grv.on;
        dirty();
        ui.redraw |= R_ALL;
        app_say("GROOVE", set.grv.on ? BA_STYLE_NAME[set.grv.style] : "OFF");
    }
}

static void held_buttons(uint32_t btn)
{
    if (!ui.rec_t)
        return;
    if (!(btn >> B_REC & 1u)) {
        if (!ui.rec_used && plat_ms() - ui.rec_t < 1000u)
            rec_release();
        ui.rec_t = 0;
    } else if (!ui.rec_used && plat_ms() - ui.rec_t >= 1000u && !is_step_page(ui.page) && ui.page != PG_RND
               && ui.page != PG_GRV && ui.page != PG_GSET) {
        ui.rec_t = 0;
        factory();
    }
}

/* SELECT und ALGORITHM: grob, für live: 2,5 pro Raste (abwechselnd 2 und 3), schnell gedreht bis 7,5;
 * der ganze Weg in ~50 Rasten, schnell in ~17. Der halbe Schritt wird je Regler aufgehoben. */
static int coarse(int k, int32_t e)
{
    static int rem[NE];
    int n = (int)accel(k, e, 2, 3) * 5 + rem[k], q = n / 2;
    rem[k] = n - q * 2;
    return q;
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
    if ((e = plat_enc(EN_SELECT)) != 0) {
        char t[12];
        int cur = ba_step_now(&eng);
        if (ui.rec_t && (btn >> B_REC & 1u) && cur >= 0) {   /* REC + CUTOFF: die Mod-Spur aufnehmen, wie am Gerät */
            ba_step *s = &edit.seq[cur];
            s->mod = (uint8_t)iclamp(s->mod + coarse(EN_SELECT, e), 0, 127);
            ui.rec_used = 1;
            edited();
            int_text((s->mod - 64) * 99 / 63, t);
            say_big(t, "MOD REC");
        } else {
            edit.p[P_CUTOFF] = (uint8_t)iclamp(edit.p[P_CUTOFF] + coarse(EN_SELECT, e), 0, 127);
            edited();
            hz_text(ba_cutoff_hz(edit.p[P_CUTOFF]), t);
            say_big(t, "CUTOFF");
            seg_value(PAGE_K[PG_VCF][0]);
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {      /* OVERDRIVE */
        char t[8];
        edit.p[P_DRIVE] = (uint8_t)iclamp(edit.p[P_DRIVE] + coarse(EN_ALGO, e), 0, 127);
        edited();
        int_text(edit.p[P_DRIVE] * 100 / 127, t);
        say_big(t, edit.p[P_DRIVE] ? "DRIVE" : "DRIVE OFF");
        seg_value(PAGE_K[PG_VCF][3]);
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
    static const uint8_t PAGE_BTN[NPAGES] = {B_HOME, B_SEL, B_ENV, B_LFO, B_LFO, B_SEQ, B_SEQ, B_SEQ, B_GLO, B_GLO,
                                             B_EDIT, B_EDIT, B_ARP, B_HOME, B_HOME, B_FX};
    uint32_t btn = 1u << PAGE_BTN[ui.page], dimb, lit = 0, k;
    if (ui.playing)
        btn |= LED_PLAY_GREEN;
    if (ui.rec)
        btn |= 1u << B_REC;
    if (ui.arp)
        btn |= 1u << B_ARP;
    if (set.grv.on && ui.playing && plat_ms() < ui.flash_until)   /* GROOVE: HOME blitzt mit jeder Kick */
        btn ^= 1u << B_HOME;
    if (ui.saving && (plat_ms() / 250u) & 1u)
        btn |= 1u << B_SAVE;
    if (set.oct != 3)
        btn |= 1u << (set.oct > 3 ? B_OCTUP : B_OCTDN);
    for (k = 0; k < NKEYS; k++)
        if ((key_base() + (int)k) % 12 == 0)
            lit |= 1u << k;
    dimb = (1u << NB) - 1u;
    plat_lights(btn | (ui.btn & ~(1u << B_PLAY)), ui.keys, dimb & ~(1u << B_PLAY), lit, 1);
}

/* ----------------------------------------------------------------- Zeichnen --- */
/* Die Optik der Bassline DB-01, so hochwertig wie möglich: ein mattschwarzes, sandgestrahltes Aluminium-Panel, auf
 * das Licht von links oben fällt; weiße Siebdruck-Beschriftung mit Sperrung und haarfeine weiße Linien mit runden
 * Ecken um die Sektionen; schwarze Regler im Moog-Stil (fein gerändelter Rock mit Fase, flache Kappe mit weichem
 * Glanz, weißer Strich) über einer gedruckten Skala, ihr Wert als rote LED-Punkte; oben das rote 7-Segment-Fenster
 * hinter Rauchglas (es zeigt die klingende Note, beim Drehen den Wert); unten die Reihe der 16 Gummitasten, deren
 * LEDs pink-rot durchscheinen und Licht aufs Panel werfen; die Kurven in einer LED-Punktmatrix hinter rotem
 * Diffusor. Alles kantengeglättet (ui_gfx.h). */
#undef C_PANEL
#define C_PANEL RGB(14, 14, 15)
#undef C_WHITE
#define C_WHITE RGB(238, 238, 234)
#undef C_SUB
#define C_SUB RGB(122, 122, 120)
#undef C_RED
#define C_RED RGB(255, 36, 74)
#undef C_LINE
#define C_LINE RGB(214, 214, 210)

#include "ui_gfx.h"

#define SCOPE_N 1024u
static float scope_ring[SCOPE_N];
static volatile uint32_t scope_w;
static volatile float out_peak;

static col3 red3(void) { return c3(C_RED); }

/* Text mit Sperrung (Siebdruck-Typografie): Abstand t zwischen den Zeichen */
static int32_t ttext_w(const felucca_font_t *f, const char *s, int32_t t)
{
    int32_t w = 0;
    char c[2] = {0, 0};
    for (; *s; s++) {
        c[0] = *s;
        w += text_w(f, c) + (s[1] ? t : 0);
    }
    return w;
}
static int32_t ttext(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t col, int32_t t)
{
    char c[2] = {0, 0};
    for (; *s; s++) {
        c[0] = *s;
        cv_text(x, y, f, c, col);
        x += text_w(f, c) + t;
    }
    return x - t;
}
static void ctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void tctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t col, int32_t t)
{
    ttext(cx - ttext_w(f, s, t) / 2, y, f, s, col, t);
}

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

/* der Rand eines abgerundeten Rechtecks, 1 px, kantengeglättet */
static void rframe(float x0, float y0, float x1, float y1, float r, col3 c, float a)
{
    aa_line(x0 + r, y0, x1 - r, y0, c, a);
    aa_line(x0 + r, y1, x1 - r, y1, c, a);
    aa_line(x0, y0 + r, x0, y1 - r, c, a);
    aa_line(x1, y0 + r, x1, y1 - r, c, a);
    ring_arc(x0 + r, y0 + r, r, 1.0f, -90.0f, 0.0f, c, a, 0.0f);
    ring_arc(x1 - r, y0 + r, r, 1.0f, 0.0f, 90.0f, c, a, 0.0f);
    ring_arc(x1 - r, y1 - r, r, 1.0f, 90.0f, 180.0f, c, a, 0.0f);
    ring_arc(x0 + r, y1 - r, r, 1.0f, 180.0f, 270.0f, c, a, 0.0f);
}

/* das Panel: sandgestrahltes, schwarz eloxiertes Aluminium; Licht von links oben, die Ränder etwas dunkler */
static void panel_black(int32_t ly0, int32_t h, int32_t screen_y0)
{
    int32_t x, j;
    for (j = 0; j < h; j++) {
        int32_t sy = j + screen_y0, y = ly0 + j;
        float vy = ((float)sy - 120.0f) / 120.0f;
        for (x = 0; x < (int32_t)cv_w; x++) {
            uint32_t n = (uint32_t)(x * 73856093) ^ (uint32_t)(sy * 19349663);
            float vx = ((float)x - 120.0f) / 120.0f, g, light;
            n = (n ^ (n >> 13)) * 1274126177u;
            light = 1.0f - 0.22f * (vx * vx + vy * vy) + 0.10f * (-vx - vy) * 0.5f;
            g = 14.5f * light + (float)((n >> 24) & 7u) * 0.38f - 1.3f;
            if (px_ok(x, y))
                px_put(x, y, rgb3(g, g, g + 0.9f));
        }
    }
}

/* eine Sektion wie auf dem Panel: ein feiner weißer Rahmen mit runden Ecken, oben links unterbrochen für den
 * gesperrten Namen; extra: ein grauer Zusatz daneben */
static void section(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const char *name, const char *extra)
{
    col3 lc = c3(C_LINE);
    int32_t tw = name ? ttext_w(&FONT_XS, name, 1) : 0, gx;
    if (extra && extra[0])
        tw += 7 + text_w(&FONT_XS, extra);
    gx = x0 + 6 + tw + 12;
    rframe((float)x0 + 0.5f, (float)y0 + 0.5f, (float)x1 + 0.5f, (float)y1 + 0.5f, 3.0f, lc, 0.62f);
    /* die Unterbrechung oben für den Namen */
    {
        int32_t x, yy;
        for (yy = y0 - 1; yy <= y0 + 1; yy++)
            for (x = x0 + 6; x < gx; x++)
                if (px_ok(x, yy)) {
                    uint32_t n = (uint32_t)(x * 73856093) ^ (uint32_t)((yy + cv_oy) * 19349663);
                    float g;
                    n = (n ^ (n >> 13)) * 1274126177u;
                    g = 13.5f + (float)((n >> 24) & 7u) * 0.38f - 1.3f;
                    px_put(x, yy, rgb3(g, g, g + 0.9f));
                }
    }
    if (name) {
        int32_t x = ttext(x0 + 12, y0 - 5, &FONT_XS, name, C_WHITE, 1);
        if (extra && extra[0])
            cv_text(x + 7, y0 - 5, &FONT_XS, extra, C_SUB);
    }
}

/* ---- die rote 7-Segment-Anzeige ---- */
static uint8_t seg_bits(char c)
{
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
    case 'N': return 55;
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

/* ein Segment: ein schmales Sechseck mit spitzen Enden; an: ein heißer Kern und ein Schein */
static void seg_bar(float x0, float y0, float x1, float y1, float w, int on)
{
    float dx = x1 - x0, dy = y1 - y0, len = fm_sqrtf(dx * dx + dy * dy), t;
    col3 off = rgb3(38, 9, 15), hot = mix3(red3(), rgb3(255, 210, 220), 0.25f);
    for (t = 0.0f; t <= len; t += 0.4f) {
        float k = t / len, e = t < len - t ? t : len - t, ww = w * sat01(e / (w * 0.55f));
        aa_disc(x0 + dx * k, y0 + dy * k, 0.35f + ww * 0.5f, on ? red3() : off, 1.0f);
        if (on && ww > w * 0.5f)
            aa_disc(x0 + dx * k, y0 + dy * k, ww * 0.22f, hot, 0.6f);
    }
    if (on)
        bloom((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, len * 0.85f, red3(), 0.13f);
}

static void seg_digit(float x, float y, float w, float h, char ch, int dp)
{
    uint8_t b = seg_bits(ch);
    float sk = 0.10f, g = 1.4f, hh = h * 0.5f, sw = w * 0.26f;
#define SX(px, py) ((px) + sk * (h - ((py) - y)))
    seg_bar(SX(x + g, y), y, SX(x + w - g, y), y, sw, b & 1);
    seg_bar(SX(x + w, y + g), y + g, SX(x + w, y + hh - g), y + hh - g, sw, b & 2);
    seg_bar(SX(x + w, y + hh + g), y + hh + g, SX(x + w, y + h - g), y + h - g, sw, b & 4);
    seg_bar(SX(x + g, y + h), y + h, SX(x + w - g, y + h), y + h, sw, b & 8);
    seg_bar(SX(x, y + hh + g), y + hh + g, SX(x, y + h - g), y + h - g, sw, b & 16);
    seg_bar(SX(x, y + g), y + g, SX(x, y + hh - g), y + hh - g, sw, b & 32);
    seg_bar(SX(x + g, y + hh), y + hh, SX(x + w - g, y + hh), y + hh, sw, b & 64);
    aa_disc(SX(x + w + 3.2f, y + h), y + h, 1.5f, dp ? red3() : rgb3(38, 9, 15), 1.0f);
    if (dp)
        bloom(SX(x + w + 3.2f, y + h), y + h, 4.5f, red3(), 0.5f);
#undef SX
}

/* vier Stellen hinter Rauchglas: ein gefaster Rahmen, das dunkelrote Glas, oben ein Lichtreflex */
static void seg_display(int32_t x, int32_t y, const char *s)
{
    char d[4] = {' ', ' ', ' ', ' '};
    int p[4] = {0, 0, 0, 0}, n = 0, i;
    int32_t xx, yy;
    for (i = 0; s[i] && n <= 4; i++) {
        if (s[i] == '.') {
            if (n > 0)
                p[n - 1] = 1;
        } else if (n < 4) {
            d[n++] = s[i];
        }
    }
    rrect(x - 2, y - 2, 78, 28, 4.0f, rgb3(4, 4, 5), rgb3(30, 30, 32), 1.0f);    /* die Fase: unten Licht */
    rrect(x, y, 74, 24, 2.5f, rgb3(26, 5, 10), rgb3(12, 2, 5), 1.0f);            /* das Rauchglas */
    for (i = 0; i < 4; i++)
        seg_digit((float)(x + 7 + i * 16), (float)(y + 4), 8.5f, 15.0f, d[i], p[i]);
    for (yy = y + 1; yy < y + 9; yy++)  /* der Reflex: ein schräges, sehr blasses Lichtband */
        for (xx = x + 2; xx < x + 72; xx++) {
            float u = (float)(xx - x) - (float)(yy - y) * 1.6f;
            if (u > 6.0f && u < 30.0f)
                px_glow(xx, yy, rgb3(255, 255, 255), 0.035f * (1.0f - (float)(yy - y) / 9.0f));
        }
}

/* ---- die LED-Punktmatrix: 74 x 21 Punkte hinter einem roten Diffusor ---- */
#define MX_W 74
#define MX_H 19
#define MX_X 10
static uint8_t mx[MX_H][MX_W];

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
static void mx_fill_below(int x, int y, int v)
{
    for (y = y < 0 ? 0 : y; y < MX_H; y++)
        mx_dot(x, y, v);
}
static int mx_y(float v) { int y = (int)(10.5f - v * 9.6f); return y < 0 ? 0 : y > MX_H - 1 ? MX_H - 1 : y; }

static void mx_draw(int32_t oy)
{
    int x, y;
    col3 r = red3();
    /* das Fenster: eine dunkelrote Scheibe mit Fase */
    rrect(MX_X - 5, oy - 4, MX_W * 3 + 8, MX_H * 3 + 6, 3.0f, rgb3(5, 2, 3), rgb3(24, 22, 23), 1.0f);
    rrect(MX_X - 4, oy - 3, MX_W * 3 + 6, MX_H * 3 + 4, 2.5f, rgb3(20, 4, 8), rgb3(11, 2, 5), 1.0f);
    for (y = 0; y < MX_H; y++)
        for (x = 0; x < MX_W; x++) {
            float cx = (float)(MX_X + x * 3) + 1.0f, cy = (float)(oy + y * 3) + 1.0f, a = (float)mx[y][x] / 255.0f;
            if (a <= 0.0f) {
                aa_disc(cx, cy, 0.85f, rgb3(46, 11, 18), 1.0f);
            } else {
                col3 c = mix3(rgb3(80, 10, 26), r, a);
                if (a > 0.86f)
                    c = mix3(c, rgb3(255, 200, 210), (a - 0.86f) * 2.2f);
                aa_disc(cx, cy, 1.05f, c, 1.0f);
                if (a > 0.4f)
                    bloom(cx, cy, 3.4f, r, 0.10f * a);
            }
        }
    {   /* ein schwacher Glanz über dem Diffusor */
        int32_t xx, yy;
        for (yy = oy - 2; yy < oy + 6; yy++)
            for (xx = MX_X - 2; xx < MX_X + MX_W * 3; xx++)
                px_glow(xx, yy, rgb3(255, 255, 255), 0.018f * (1.0f - (float)(yy - oy + 2) / 8.0f));
    }
}

/* ---- der Regler im Moog-Stil über seiner gedruckten Skala; der Wert als rote LED-Punkte ---- */
static void knob_moog(float cx, float cy, float r, float v, int hot, int bip)
{
    float va = -135.0f + 270.0f * sat01(v), a0 = bip ? 0.0f : -135.0f, lo = va < a0 ? va : a0, hi = va < a0 ? a0 : va;
    float ux = fm_sinf(va * 0.0174533f), uy = -fm_cosf(va * 0.0174533f), rt = r * 0.68f;
    int32_t x, y, R = (int32_t)(r + 4.0f), i;
    col3 rd = red3();
    /* die Skala: 11 Striche, die bei 0, 5 und 10 länger */
    for (i = 0; i <= 10; i++) {
        float an = (-135.0f + 27.0f * (float)i) * 0.0174533f, sx = fm_sinf(an), sy = -fm_cosf(an);
        float r0 = r + 5.5f, r1 = r + (i % 5 == 0 ? 9.5f : 7.5f);
        aa_line(cx + sx * r0, cy + sy * r0, cx + sx * r1, cy + sy * r1, c3(C_LINE), i % 5 == 0 ? 0.9f : 0.55f);
    }
    /* der Wert: 25 LED-Punkte im Ring zwischen Regler und Skala */
    for (i = 0; i <= 24; i++) {
        float an = -135.0f + 11.25f * (float)i, rad = an * 0.0174533f;
        float px = cx + fm_sinf(rad) * (r + 2.8f), py = cy - fm_cosf(rad) * (r + 2.8f);
        int on = an >= lo - 0.5f && an <= hi + 0.5f && fabsf_(va - a0) > 0.8f;
        aa_disc(px, py, 0.9f, on ? rd : rgb3(42, 10, 16), on ? (hot ? 1.0f : 0.85f) : 1.0f);
        if (on && hot)
            bloom(px, py, 3.2f, rd, 0.28f);
    }
    for (y = (int32_t)cy - R; y <= (int32_t)cy + R; y++)
        for (x = (int32_t)cx - R; x <= (int32_t)cx + R; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = fm_sqrtf(dx * dx + dy * dy);
            float sdx = dx - 1.2f, sdy = dy - 2.2f, sd = fm_sqrtf(sdx * sdx + sdy * sdy), cov, an, lam, proj, perp;
            col3 c;
            if (!px_ok(x, y))
                continue;
            if (sd < r + 2.5f)               /* der Schatten nach rechts unten, weich */
                px_blend(x, y, rgb3(0, 0, 0), sat01((r + 2.5f - sd) / 4.0f) * 0.75f);
            if (d > r + 0.5f)
                continue;
            cov = sat01(r - d + 0.5f);
            an = ang_deg(dx, dy);
            lam = d > 0.5f ? -(dx + dy) * 0.7071f / d : 0.0f;
            if (d > rt + 1.2f) {             /* der Rock: fein gerändelt, die Rippen fangen das Licht */
                float kn = fm_cosf(an * 0.0174533f * 40.0f), rim = sat01((d - (r - 1.2f)) / 1.2f);
                float g = 15.0f + 9.0f * lam + 6.0f * kn * (0.6f + 0.4f * sat01(lam + 0.5f)) - 6.0f * rim;
                c = rgb3(g, g, g + 1.0f);
            } else if (d > rt) {             /* die Fase zwischen Rock und Kappe: ein heller Lichtkranz oben links */
                float g = 26.0f + 46.0f * sat01(lam) * sat01(lam) + 6.0f * lam;
                c = rgb3(g, g, g + 1.0f);
            } else {                         /* die Kappe: flach, seidenmatt, ein weicher Glanz oben links */
                float ox = dx + rt * 0.38f, oy = dy + rt * 0.42f;
                float gl = sat01(1.0f - fm_sqrtf(ox * ox + oy * oy) / (rt * 0.95f));
                float g = 20.0f + 30.0f * gl * gl + 3.0f * lam;
                c = rgb3(g, g, g + 1.5f);
            }
            proj = dx * ux + dy * uy;        /* der Zeiger: ein weißer Strich über Kappe und Rock */
            perp = fabsf_(dx * uy - dy * ux);
            if (proj > r * 0.22f && proj < r - 0.8f) {
                float pc = sat01(1.1f - perp);
                if (pc > 0.0f)
                    c = mix3(c, rgb3(246, 246, 242), pc);
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
    tctext(cx, y, &FONT_XS, knob_label(d), C_WHITE, 1);
    knob_moog((float)cx, (float)(y + 36), 13.5f, (float)v / 127.0f, hot, knob_bip(d));
    val_text(d, t);
    tctext(cx, y + 61, &FONT_XS, t, hot ? C_RED : C_SUB, hot ? 1 : 0);
    if (hot)
        bloom((float)cx, (float)y + 66.0f, 15.0f, red3(), 0.08f);
}

/* ---- die Seitengruppen: kleine LEDs ---- */
static void group_of(int pg, int *i, int *n)
{
    static const uint8_t G0[NPAGES] = {0, 0, 0, 0, 1, 0, 1, 2, 0, 1, 0, 1, 0, 1, 2, 0};
    static const uint8_t GN[NPAGES] = {3, 1, 1, 2, 2, 3, 3, 3, 2, 2, 2, 2, 1, 3, 3, 1};
    *i = G0[pg];
    *n = GN[pg];
}

/* ---- die Anzeigen in der Punktmatrix ---- */
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
    for (x = 0; x < 222; x++) {
        float v = fabsf_(scope_ring[(best + (uint32_t)x) % SCOPE_N]);
        if (v > pk)
            pk = v;
    }
    if (pk < 0.002f) {                   /* still: die Form von Welle und Sub, gedimmt */
        for (x = 0; x < MX_W; x++) {
            float ph = (float)x / 37.0f, p = ph - (float)(int)ph, v;
            v = wi == 0 ? 1.0f - 4.0f * fabsf_(p - 0.5f) : wi == 1 ? 2.0f * p - 1.0f : p < 0.5f ? 1.0f : -1.0f;
            v += (float)edit.p[P_SUB] / 127.0f * (fmod_pos(ph * 0.5f, 1.0f) < 0.5f ? 0.45f : -0.45f);
            v *= 0.68f;
            if (x)
                mx_line(x - 1, py, x, mx_y(v), 110);
            py = mx_y(v);
        }
        return;
    }
    g = 0.95f / (pk < 0.08f ? 0.08f : pk);
    for (x = 0; x < MX_W; x++) {
        int y = mx_y(scope_ring[(best + (uint32_t)x * 3u) % SCOPE_N] * g);
        if (x)
            mx_line(x - 1, py, x, y, 255);
        py = y;
    }
}

static void show_filter(void)            /* die Filterkurve, wie sie gerade wirklich steht; LP oder BP */
{
    float knob = (float)edit.p[P_CUTOFF] / 127.0f, q = (float)edit.p[P_RESON] / 127.0f, fc;
    int x, py = 0, bp = edit.p[P_FMODE] >= 64;
    fc = eng.cut_hz > 0.0f && eng.venv > 0.01f ? sat01(fm_log2f(eng.cut_hz / 20.0f) / 10.0f) : knob;
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
        mx_fill_below(x, y + 1, 34);
        py = y;
    }
    for (x = 0; x < MX_H; x += 2)
        mx_dot((int)(knob * (float)(MX_W - 1)), x, 85);
}

static void show_env(void)               /* die Filterhüllkurve (DECAY, DEPTH) und der VCA (Gate, RELEASE) */
{
    float dec = ba_fdecay_ms(edit.p[P_FDECAY]), rel = ba_vrel_ms(edit.p[P_VREL]), dep = (float)edit.p[P_FDEPTH] / 127.0f;
    float wf = dec * 1.6f < 120.0f ? 120.0f : dec * 1.6f, wr = rel < 0.0f ? 1000.0f : 250.0f + (rel * 1.4f < 150.0f ? 150.0f : rel * 1.4f);
    int x, py = 0, pv = 0, split = 36;
    for (x = 0; x < split; x++) {        /* links: die Filterhüllkurve (das Fenster wächst mit DECAY) */
        float t = (float)x / (float)(split - 1) * wf, v = dep * fm_exp2f(-t / dec * 9.95f);
        int y = MX_H - 1 - (int)(v * (float)(MX_H - 2));
        if (x)
            mx_line(x - 1, py, x, y, 230);
        mx_fill_below(x, y + 1, 30);
        py = y;
    }
    for (x = 0; x < MX_H; x += 2)
        mx_dot(split + 1, x, 60);
    for (x = 0; x < MX_W - split - 3; x++) {   /* rechts: der VCA, ein Gate von 250 ms, dann RELEASE */
        float t = (float)x / (float)(MX_W - split - 4) * wr, v;
        int y;
        v = t < 250.0f ? 1.0f : rel < 0.0f ? 1.0f : fm_exp2f(-(t - 250.0f) / rel * 9.95f);
        y = MX_H - 1 - (int)(v * (float)(MX_H - 2));
        if (x)
            mx_line(split + 2 + x, pv, split + 3 + x, y, 230);
        mx_fill_below(split + 3 + x, y + 1, 30);
        pv = y;
    }
    if (eng.venv > 0.01f) {             /* wie hoch beide gerade stehen: ein heller Balken je Seite */
        int hf = (int)(eng.fenv * dep * (float)(MX_H - 2)), hv = (int)(eng.venv * (float)(MX_H - 2)), y;
        for (y = 0; y <= hf; y++)
            mx_dot(0, MX_H - 1 - y, 255);
        for (y = 0; y <= hv; y++)
            mx_dot(MX_W - 1, MX_H - 1 - y, 255);
    }
}

static void show_lfo(void)               /* der LFO, wie er gerade läuft */
{
    int x, py = 0;
    for (x = 0; x < MX_W; x++) {
        float v = ui.lfo_scope[(ui.lfo_pos + (uint32_t)(x * 96 / MX_W)) % 96u];
        int y = mx_y(v * 0.95f);
        if (x)
            mx_line(x - 1, py, x, y, x >= MX_W - 2 ? 255 : 190);
        py = y;
    }
    for (x = 0; x < MX_W; x += 3)
        mx_dot(x, 10, 40);
}

/* das Pattern der Seite als Pianorolle: Note als Höhe, Gate als Länge, ACCENT dick, SLIDE verbunden;
 * lane: 0 die Noten; 1 die MOD-Spur als Balken; 2 die PITCH ENV als Zacken */
static void show_seq(int lane)
{
    int pg = ui.ssel / 16, i, cur = ba_step_now(&eng), lo = 127, hi = 0;
    for (i = 0; i < 16; i++) {
        const ba_step *s = &edit.seq[pg * 16 + i];
        if (s->gate) {
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
    for (i = 0; i < 16; i++) {
        const ba_step *s = &edit.seq[pg * 16 + i];
        int x0 = 2 + i * 4 + (i / 4) * 2, y, len, c, br, st = pg * 16 + i;
        if (st == cur)
            for (y = 0; y < MX_H; y += 2)
                mx_dot(x0 + 1, y, 70);
        if (st == ui.ssel)
            for (c = 0; c < 3; c++)
                mx_dot(x0 + c, MX_H - 1, 200);
        else
            mx_dot(x0 + 1, MX_H - 1, 40);
        if (lane == 1) {                 /* MOD: von der Mitte nach oben / unten */
            int h = (s->mod - 64) * 9 / 63, k;
            for (k = 0; k != h; k += h > 0 ? 1 : -1)
                for (c = 0; c < 3; c++)
                    mx_dot(x0 + c, 9 - k - (h > 0 ? 1 : -1) + (h > 0 ? 1 : 0), st == cur ? 255 : st == ui.ssel ? 220 : 130);
            for (c = 0; c < 3; c++)
                mx_dot(x0 + c, 9, 60);
            continue;
        }
        if (lane == 2) {                 /* PITCH ENV: ein Zacken, so hoch wie DEPTH, so lang wie DECAY */
            int dpt = BA_PE_DEPTH(s->pe), dk = BA_PE_DECAY(s->pe), k;
            if (!s->gate || !dpt)
                continue;
            for (k = 0; k < 4; k++) {
                int h = dpt * 2 - k * (11 - dk) / 3;
                if (h > 0)
                    mx_line(x0 + k, MX_H - 2, x0 + k, MX_H - 2 - h, st == cur ? 255 : st == ui.ssel ? 220 : 140);
            }
            continue;
        }
        if (!s->gate)
            continue;
        y = MX_H - 3 - (s->note - lo) * (MX_H - 5) / (hi - lo);
        len = s->gate >= 10 || (s->fl & BA_SLD) ? 4 : 1 + s->gate * 3 / 10;
        br = st == cur ? 255 : s->fl & BA_ACC ? 235 : st == ui.ssel ? 200 : 130;
        if (s->prob)                     /* mit Wahrscheinlichkeit: gepunktet */
            br = br * 3 / 4;
        for (c = 0; c < len; c++) {
            if (!(s->prob && (c & 1)))
                mx_dot(x0 + c, y, br);
            if (s->fl & BA_ACC)
                mx_dot(x0 + c, y - 1, br);
        }
        if ((s->fl & BA_SLD) && i < 15) {
            const ba_step *n = &edit.seq[pg * 16 + i + 1];
            if (n->gate)
                mx_line(x0 + 3, y, 2 + (i + 1) * 4 + ((i + 1) / 4) * 2, MX_H - 3 - (n->note - lo) * (MX_H - 5) / (hi - lo), 70);
        }
    }
}

static void show_pattern(void)           /* LAST STEP und PLAY MODE: die 64 Steps als 4 Takte, der Weg als Pfeile */
{
    int len = ba_seq_len(&eng), cur = ba_step_now(&eng), i, pm = ba_list_index(P_PMODE, edit.p[P_PMODE]);
    for (i = 0; i < BA_STEPS; i++) {
        int x = (i % 16) * 4 + (i % 16 / 4) * 2 + 2, y = 1 + (i / 16) * 3, on = i < len, br;
        br = i == cur ? 255 : on ? (edit.seq[i].gate ? 150 : 60) : 14;
        mx_dot(x, y, br);
        mx_dot(x + 1, y, br);
        mx_dot(x, y + 1, br);
        mx_dot(x + 1, y + 1, br);
    }
    /* der Weg: FWD →, BWD ←, RND ?, PP ↔, PP2 ↔↔ */
    {
        int y = 16, x;
        for (x = 6; x < 68; x++)
            mx_dot(x, y, pm == PM_RND ? ((x * 7) % 5 == 0 ? 200 : 0) : 150);
        if (pm == PM_FWD || pm >= PM_PP) {
            mx_line(64, 14, 68, 16, 220);
            mx_line(64, 18, 68, 16, 220);
        }
        if (pm == PM_BWD || pm >= PM_PP) {
            mx_line(10, 14, 6, 16, 220);
            mx_line(10, 18, 6, 16, 220);
        }
        if (pm == PM_PP2) {
            mx_line(60, 14, 64, 16, 220);
            mx_line(60, 18, 64, 16, 220);
            mx_line(14, 14, 10, 16, 220);
            mx_line(14, 18, 10, 16, 220);
        }
    }
}

static void show_scale(void)             /* SCALE: eine Oktave Klaviatur, die Töne der Skala leuchten */
{
    static const uint8_t BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    uint16_t m = BA_SCALE_MASK[ba_list_index(P_SCALE, edit.p[P_SCALE])];
    int root = ba_list_index(P_ROOT, edit.p[P_ROOT]), k, wx = 0;
    for (k = 0; k < 12; k++) {
        int in = m >> ((k - root + 12) % 12) & 1u, isr = k == root, x, y;
        if (BLACK[k])
            continue;
        for (y = 2; y < MX_H - 1; y++)
            for (x = 0; x < 8; x++)
                mx_dot(3 + wx * 10 + x, y, (x == 0 || x == 7 || y == 2 || y == MX_H - 2) ? 70 : in ? (isr ? 255 : 170) : 0);
        wx++;
    }
    wx = 0;
    for (k = 0; k < 12; k++) {
        int in = m >> ((k - root + 12) % 12) & 1u, isr = k == root, x, y;
        if (!BLACK[k]) {
            wx++;
            continue;
        }
        for (y = 2; y < 12; y++)
            for (x = 0; x < 6; x++)
                mx_dot(3 + wx * 10 - 4 + x, y, in ? (isr ? 255 : 200) : 35);
    }
}

static void show_rnd(void)               /* RND PATT: die Grenzen als Band, und das Pattern, wie es jetzt ist */
{
    int tg = ba_list_index(P_RTGT, edit.p[P_RTGT]), lo = edit.p[P_RLO], hi = edit.p[P_RHI], x, y;
    if (tg == RT_PATTERN || tg == RT_NOTES || tg == RT_GATES || tg == RT_MOD || tg == RT_PENV) {
        int y0 = MX_H - 2 - lo * (MX_H - 3) / 127, y1 = MX_H - 2 - hi * (MX_H - 3) / 127;
        if (y1 > y0) {
            int t = y0;
            y0 = y1;
            y1 = t;
        }
        for (x = 0; x < MX_W; x += 2) {
            mx_dot(x, y0, 120);
            mx_dot(x, y1, 120);
            for (y = y1 + 1; y < y0; y++)
                if (((x + y) & 3) == 0)
                    mx_dot(x, y, 40);
        }
    } else {                             /* eine Menge: ein Balken */
        int n = edit.p[P_RAMT] * MX_W / 127;
        for (x = 0; x < MX_W; x++)
            for (y = 8; y < 13; y++)
                mx_dot(x, y, x < n ? 200 : 25);
    }
}

static void show_arp(void)               /* ARP: die Richtung als Treppe über die Oktaven */
{
    int dir = ba_list_index(P_ADIR, edit.p[P_ADIR]), oct = ba_list_index(P_AOCT, edit.p[P_AOCT]) + 1;
    int gate = ba_list_index(P_AGATE, edit.p[P_AGATE]) + 1, n = 3 * oct, steps = 16, i, c;
    uint32_t s = 12345u;
    for (i = 0; i < steps; i++) {
        int k, y, w = 1 + gate * 3 / 10;
        switch (dir) {
        case AD_DOWN: k = n - 1 - i % n; break;
        case AD_UPDN: {
            int per = n > 1 ? 2 * n - 2 : 1, q = i % per;
            k = q < n ? q : per - q;
            break;
        }
        case AD_RND: s = s * 1103515245u + 12345u; k = (int)((s >> 16) % (uint32_t)n); break;
        default: k = i % n; break;
        }
        y = MX_H - 2 - k * (MX_H - 3) / (n > 1 ? n - 1 : 1);
        for (c = 0; c < (w > 4 ? 4 : w); c++)
            mx_dot(2 + i * 4 + (i / 4) * 2 + c, y, eng.arp && eng.nlatched ? 230 : 150);
    }
}

/* FX: links die zehn Bänder als Fader-Säulen (wie weit offen, mit der Bewegung), darüber als heller Punkt, was
 * gerade darin klingt (ein kleiner Spektrum-Analysator); rechts der Hall als roter Nebel, so dicht wie REVERB und
 * so hoch, wie er gerade klingt */
static void show_fx(void)
{
    int b, y, c, x;
    float rv = (float)edit.p[P_RVMIX] / 127.0f, rl = sat01(ba_reverb_level(&eng) * 4.0f);
    uint32_t r = 2463534242u + (plat_ms() / 120u) * 2654435761u;
    for (b = 0; b < BA_NBANDS; b++) {
        int x0 = 1 + b * 6, h = (int)(sat01(ba_band_gain(&eng, b) * 0.5f) * (float)(MX_H - 3) + 0.5f);
        int lv = (int)(fm_sqrtf(ba_band_level(&eng, b)) * (float)(MX_H - 2) + 0.5f);
        int on = edit.p[P_FXAMT] >= 2;
        for (y = 0; y < h; y++)
            for (c = 0; c < 4; c++)
                mx_dot(x0 + c, MX_H - 1 - y, on ? 95 : 35);
        if (on && lv > 0)
            for (c = 0; c < 4; c++)
                mx_dot(x0 + c, MX_H - 1 - (lv < MX_H - 1 ? lv : MX_H - 1), 255);
    }
    for (y = 0; y < MX_H; y += 2)
        mx_dot(61, y, 50);
    for (x = 63; x < MX_W; x++)          /* der Hall */
        for (y = 0; y < MX_H; y++) {
            float a = (float)(MX_H - 1 - y) / (float)MX_H, top = 0.35f + 0.6f * rl;
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            if (rv > 0.01f && a < top && (float)(r & 255u) < (80.0f + 175.0f * rv) * (1.0f - a / top))
                mx_dot(x, y, 60 + (int)(170.0f * (1.0f - a / top) * (0.4f + 0.6f * rl)));
        }
}

static void show_groove(void)            /* GROOVE: die fünf Spuren über zwei Takte, der laufende Step hell; unten die Pumpe */
{
    int lane, i, cur = ba_groove_step(&eng), x;
    float pump = (float)set.grv.pump / 127.0f, depth = 0.95f * pump * (1.3f - 0.3f * pump);
    float rel = 0.04f + 0.56f * (float)set.grv.rel / 127.0f, beat = 60.0f / ba_bpm(&eng);
    for (lane = 0; lane < 5; lane++)
        for (i = 0; i < 32; i++) {
            int v = ba_groove_hit(&eng, lane, i), xx = 5 + i * 2 + (i / 16) * 2;
            int b = v ? (v == 1 ? 90 : v == 2 ? 170 : 230) : (i % 4 ? 0 : 18);
            if (!set.grv.on && v)
                b = b * 2 / 3;
            if (i == cur && v)
                b = 255;
            mx_dot(xx, lane * 2, b);
        }
    if (cur >= 0)                        /* der Abspielkopf */
        for (lane = 0; lane < 10; lane++)
            mx_dot(5 + cur * 2 + (cur / 16) * 2 + 1, lane, 60);
    for (x = 0; x < MX_W; x++) {         /* die Pumpe: eine Viertel lang */
        float t = (float)x / (float)(MX_W - 1) * beat, g;
        int y;
        if (t < 0.004f)
            g = 1.0f - depth * t / 0.004f;
        else if (t < 0.004f + rel) {
            float u = 1.0f - (t - 0.004f) / rel;
            g = 1.0f - depth * u * u * (3.0f - 2.0f * u);
        } else
            g = 1.0f;
        y = 18 - (int)(g * 6.5f + 0.5f);
        mx_dot(x, y, set.grv.on ? 230 : 120);
        mx_fill_below(x, y + 1, set.grv.on ? 40 : 20);
    }
    if (set.grv.on && ui.playing) {      /* wo die Pumpe gerade steht: ein heller Punkt links */
        int y = 18 - (int)(eng.sc_now * 6.5f + 0.5f);
        mx_dot(0, y, 255);
        mx_dot(1, y, 255);
    }
}

static void show_glo(void)               /* SETTINGS: das Tempo im Takt, die Last als Balken */
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

static int view_animated(void)
{
    return ui.page == PG_VCO || ui.page == PG_VCF || ui.page == PG_ENV || ui.page == PG_LFO || ui.page == PG_LFO2
           || ui.page == PG_SETUP || (ui.page == PG_ARP && ui.arp) || ui.page == PG_GRV || ui.page == PG_GSET
           || ui.page == PG_FX;
}

static const char *view_title(char *t)
{
    char n[8];
    t[0] = 0;
    if (ui.saving)
        return "SAVE";
    switch (ui.page) {
    case PG_VCO: text_cat(t, "OUT "); text_cat(t, BA_WAVE_NAME[ba_wave_index(edit.p[P_WAVE])]); return t;
    case PG_VCF:
        text_cat(t, edit.p[P_FMODE] >= 64 ? "BP " : "LP ");
        hz_text(eng.cut_hz > 0.0f && eng.venv > 0.01f ? eng.cut_hz : ba_cutoff_hz(edit.p[P_CUTOFF]), t + txt_len(t));
        return t;
    case PG_ENV: return "VCF   VCA";
    case PG_LFO:
    case PG_LFO2: return BA_SHAPE_NAME[ba_list_index(P_LSHAPE, edit.p[P_LSHAPE])];
    case PG_STEP:
    case PG_STEP2:
    case PG_STEP3:
        text_cat(t, "BAR ");
        int_text(ui.ssel / 16 + 1, n);
        text_cat(t, n);
        text_cat(t, "/");
        int_text((ba_seq_len(&eng) + 15) / 16, n);
        text_cat(t, n);
        return t;
    case PG_PATT: return BA_PMODE_NAME[ba_list_index(P_PMODE, edit.p[P_PMODE])];
    case PG_RND: return "REC: GENERATE";
    case PG_SCALE: {
        static const char *const FULL[BA_NSCALES] = {"CHROMATIC", "MAJOR", "MINOR", "MAJOR PENTA", "MINOR PENTA",
                                                     "WHOLE TONE", "MAJOR BLUES", "MINOR BLUES"};
        text_cat(t, BA_NOTE_NAME[ba_list_index(P_ROOT, edit.p[P_ROOT])]);
        text_cat(t, " ");
        text_cat(t, FULL[ba_list_index(P_SCALE, edit.p[P_SCALE])]);
        return t;
    }
    case PG_ARP: return ui.arp ? "ARP ON" : "ARP OFF";
    case PG_GRV:
    case PG_GSET:
        text_cat(t, set.grv.on ? BA_STYLE_NAME[set.grv.style] : "GROOVE OFF");
        return t;
    case PG_FX: {                        /* welches der 12, oder aus */
        char n[8];
        if (edit.p[P_FXAMT] < 2 && edit.p[P_RVMIX] < 2)
            return "FX OFF";
        int_text(ba_list_index(P_FX, edit.p[P_FX]) + 1, n);
        text_cat(t, n);
        text_cat(t, "/12");
        return t;
    }
    default: text_cat(t, eng.ext ? "MIDI CLOCK" : "TEMPO"); return t;
    }
}

/* ---- der Akku: unter dem Namen, immer zu sehen. plat_battery: 0..3 Balken, 4 Strom über USB, -1 nicht gelesen
 *      (die Browser-Version hat keinen Akku: dann nichts). Wie eine Gerätebeschriftung: ein weißer Rahmen, darin ein
 *      roter LED-Balken, der sich stufenlos füllt, daneben die Prozent; unter 15 % rot, unter 8 % blinkt er; an USB
 *      voll mit einem kleinen Blitz. Die Prozent kommen aus der Messung der Zellspannung (plat_battery_raw): die drei
 *      Balken der Plattform liegen bei 531 / 561 / 591, eine volle Zelle um 615 ---- */
static int batt_level(void)
{
#ifdef PLAT_HOST
    if (ui.batt_test)
        return ui.batt_test - 2;         /* (Tests: 1 = nicht gelesen, 2..6 = 0..4) */
#endif
    return plat_battery();
}

static int batt_raw_pct(void)            /* aus der Messung: 0..100, -1 keine Messung */
{
    int32_t raw;
#ifdef PLAT_HOST
    if (ui.batt_tpct >= 0)
        return ui.batt_tpct;
#endif
    raw = plat_battery_raw();
    if (raw <= 0)
        return -1;
    raw = (raw - 515) * 100 / 100;       /* 515 leer .. 615 voll */
    return raw < 0 ? 0 : raw > 100 ? 100 : (int)raw;
}

static int batt_percent(int lvl)         /* die angezeigten Prozent, ohne Zappeln: erst ab 2 Punkten Unterschied */
{
    int p = batt_raw_pct();
    if (p < 0)                           /* (noch keine Messung: aus den Balken) */
        p = lvl >= 4 ? 100 : lvl * 33;
    if (ui.batt_pct < 0 || p >= ui.batt_pct + 2 || p <= ui.batt_pct - 2 || p == 0 || p == 100)
        ui.batt_pct = (int8_t)p;
    return ui.batt_pct;
}

static void batt_icon(float x, float y, int lvl, uint32_t now)
{
    int usb = lvl >= 4, pct = usb ? 100 : batt_percent(lvl), low = !usb && pct < 15;
    int blink = !usb && pct < 8 && ((now / 500u) & 1u);
    float w = 14.0f * (float)pct / 100.0f, full;
    col3 off = rgb3(48, 11, 18);
    rframe(x + 0.5f, y + 0.5f, x + 16.5f, y + 8.5f, 1.5f, c3(C_LINE), 0.85f);
    rrect((int32_t)x + 17, (int32_t)y + 3, 2, 3, 0.5f, c3(C_LINE), c3(C_LINE), 0.85f);
    rrect((int32_t)x + 2, (int32_t)y + 2, 14, 5, 0.8f, off, off, 1.0f);
    if (blink)
        w = 0.0f;
    else if (w < 1.0f && pct > 0)
        w = 1.0f;
    full = (float)(int)w;
    if (full >= 1.0f)                    /* die ganzen Pixel, dann der angebrochene als Halbton: stufenlos */
        rrect((int32_t)x + 2, (int32_t)y + 2, (int32_t)full, 5, 0.8f, red3(), red3(), 1.0f);
    if (w - full > 0.05f)
        rrect((int32_t)x + 2 + (int32_t)full, (int32_t)y + 2, 1, 5, 0.0f, red3(), red3(), w - full);
    if (w > 0.0f)
        bloom(x + 2.0f + w * 0.5f, y + 4.5f, 3.0f + w * 0.5f, red3(), 0.16f);
    if (usb) {                           /* Strom über USB: ein kleiner Blitz rechts neben dem Akku */
        aa_line(x + 24.0f, y + 0.5f, x + 22.0f, y + 4.5f, c3(C_WHITE), 0.95f);
        aa_line(x + 22.0f, y + 4.5f, x + 25.0f, y + 4.5f, c3(C_WHITE), 0.95f);
        aa_line(x + 25.0f, y + 4.5f, x + 23.0f, y + 8.5f, c3(C_WHITE), 0.95f);
    } else {
        char n[8];
        int_text(pct, n);
        text_cat(n, "%");
        cv_text((int32_t)x + 22, (int32_t)y - 1, &FONT_XS, n, low ? C_RED : C_SUB);
    }
}

/* ---- die 16 Step-Tasten: Gummi, gewölbt, die LED scheint durch und wirft Licht aufs Panel ---- */
static void step_key(int32_t x, int32_t y, float lit, int dim, int play)
{
    col3 rd = red3();
    float a = dim ? 0.45f : 1.0f;
    if (lit > 0.0f)                      /* das Licht, das neben der Taste aufs Panel fällt */
        bloom((float)x + 6.0f, (float)y + 8.0f, play ? 14.0f : 10.0f, rd, (play ? 0.22f : 0.10f) * lit);
    rrect(x - 1, y - 1, 14, 19, 3.0f, rgb3(4, 4, 5), rgb3(6, 6, 7), a);           /* die Fuge im Panel */
    rrect(x, y, 12, 17, 2.5f, rgb3(58, 58, 61), rgb3(30, 30, 32), a);             /* der Gummi */
    rrect(x + 1, y + 1, 10, 6, 2.0f, rgb3(70, 70, 73), rgb3(56, 56, 59), a);       /* die Wölbung fängt Licht */
    {   /* das LED-Fenster */
        col3 c = lit > 0.0f ? mix3(rgb3(90, 12, 30), rd, lit) : rgb3(46, 12, 20);
        rrect(x + 3, y + 3, 6, 4, 1.2f, c, mix3(c, rgb3(0, 0, 0), 0.25f), a);
        if (lit > 0.6f)
            rrect(x + 4, y + 3, 4, 2, 1.0f, rgb3(255, 190, 205), rgb3(255, 120, 150), (lit - 0.6f) * 2.0f);
    }
    if (play)
        bloom((float)x + 6.0f, (float)y + 5.0f, 8.0f, rd, 0.6f);
}

static void draw_steps(int32_t oy)
{
    int len = ba_seq_len(&eng), cur = ba_step_now(&eng), stp = is_step_page(ui.page);
    int pg = (cur >= 0 && !stp) ? cur / 16 : ui.ssel / 16, i;
    for (i = 0; i < 16; i++) {
        int st = pg * 16 + i;
        const ba_step *s = &edit.seq[st];
        int32_t x = 9 + i * 14, y = oy;
        int in = st < len, note = in && s->gate, play = st == cur;
        float lit = play ? 1.0f : note ? (s->fl & BA_ACC ? 0.62f : 0.34f) : 0.0f;
        step_key(x, y, lit, !in, play);
        if (note && (s->fl & BA_SLD) && i < 15)   /* SLIDE: ein feiner Strich hinüber zur nächsten Taste */
            aa_line((float)x + 11.0f, (float)y + 13.5f, (float)x + 16.0f, (float)y + 13.5f, c3(C_LINE), 0.75f);
        if (stp && st == ui.ssel)        /* der gewählte Step: ein weißer Rahmen */
            rframe((float)x - 2.5f, (float)y - 2.5f, (float)x + 13.5f, (float)y + 18.5f, 3.0f, c3(C_WHITE), 0.95f);
    }
    {   /* die vier Takte: kleine LEDs in der Kopflinie der Sektion, hinter SEQUENCER */
        int nb = (len + 15) / 16;
        for (i = 0; i < 4; i++) {
            float x = 88.0f + (float)i * 8.0f;
            aa_disc(x, 5.5f, 1.9f, i == pg ? red3() : i < nb ? rgb3(110, 20, 36) : rgb3(40, 10, 16), 1.0f);
            if (i == pg)
                bloom(x, 5.5f, 5.0f, red3(), 0.5f);
        }
    }
}

static void draw_save(void)
{
    char t[24] = "TO ";
    const ba_patch *p = slot_patch(ui.save_slot);
    t[3] = (char)('0' + (ui.save_slot + 1) / 10);
    t[4] = (char)('0' + (ui.save_slot + 1) % 10);
    t[5] = 0;
    tctext(120, 14, &FONT_B, "SAVE", C_WHITE, 2);
    ctext(120, 30, &APP_BIG_FONT, t, C_RED);
    tctext(120, 58, &FONT_XS, p->name, C_SUB, 1);
}

static void draw_low(void)
{
    char t[32];
    const char *title = view_title(t);
    section(3, 5, 236, 98, "SEQUENCER", "      ");
    {
        int32_t tw = ttext_w(&FONT_XS, title, 1);
        rrect(236 - tw - 15, 1, tw + 10, 9, 0.0f, rgb3(13, 13, 14), rgb3(13, 13, 14), 1.0f);
        ttext(236 - tw - 10, 0, &FONT_XS, title, C_WHITE, 1);
    }
    if (ui.saving) {
        draw_save();
    } else {
        mx_clear();
        switch (ui.page) {
        case PG_VCO: show_scope(); break;
        case PG_VCF: show_filter(); break;
        case PG_ENV: show_env(); break;
        case PG_LFO:
        case PG_LFO2: show_lfo(); break;
        case PG_STEP: show_seq(0); break;
        case PG_STEP2: show_seq(1); break;
        case PG_STEP3: show_seq(2); break;
        case PG_PATT: show_pattern(); break;
        case PG_RND: show_rnd(); break;
        case PG_SCALE: show_scale(); break;
        case PG_ARP: show_arp(); break;
        case PG_GRV:
        case PG_GSET: show_groove(); break;
        case PG_FX: show_fx(); break;
        default: show_glo(); break;
        }
        mx_draw(15);
    }
    if (ui.rec) {                        /* RECORD: eine blinkende LED links oben */
        int on = (plat_ms() / 400u) & 1u;
        aa_disc(126.0f, 5.5f, 2.3f, on ? red3() : rgb3(70, 12, 24), 1.0f);
        if (on)
            bloom(126.0f, 5.5f, 6.0f, red3(), 0.6f);
        cv_text(131, 0, &FONT_XS, "REC", C_RED);
    }
    draw_steps(78);
}

/* ---- der Kopf: BA-55 in Weiß, die RUN-LED mit dem Tempo, das 7-Segment-Fenster ---- */
static void head_display(char *t)
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
    if (ui.seg_until && plat_ms() < ui.seg_until) {
        text_cat(t, ui.seg);
        return;
    }
    if (eng.gate && eng.note >= 0) {     /* die klingende Note: Buchstabe, Punkt = Kreuz, Oktave */
        static const char L[12] = {'C', 'C', 'D', 'D', 'E', 'F', 'F', 'G', 'G', 'A', 'A', 'B'};
        int n = eng.note, o = n / 12 - 2, sh = (0x54Au >> (n % 12)) & 1u;
        t[0] = ' ';
        t[1] = ' ';
        t[2] = L[n % 12];
        t[3] = sh ? '.' : 0;
        t[sh ? 4 : 3] = o < 0 ? '-' : (char)('0' + (o > 9 ? 9 : o));
        t[sh ? 5 : 4] = 0;
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
    cv_begin(240, 32, C_PANEL);
    panel_black(0, 32, 0);
    if (now < ui.msg_until) {            /* eine Meldung: etwas höher, damit der Akku darunter frei bleibt */
        x = ttext(9, 5, &FONT_B, ui.msg[0], C_WHITE, 1);
        cv_text(x + 8, 5, &FONT_B, ui.msg[1], C_RED);
    } else {
        x = ttext(9, 6, &FONT_B, "BA-55", C_WHITE, 2);
        ttext(x + 9, 5, &FONT_XS, "BASSLINE", C_WHITE, 1);
        ttext(x + 9, 15, &FONT_XS, "SYNTH", C_SUB, 1);
    }
    /* RUN: eine LED, die auf jeder Zählzeit aufblitzt; das Tempo darunter (nicht, solange eine Meldung steht) */
    if (now >= ui.msg_until) {
        int run = ui.playing, s = ba_step_now(&eng), beat = run && s >= 0 && s % 4 == 0;
        aa_disc(146.0f, 8.0f, 2.4f, run ? (beat ? mix3(red3(), rgb3(255, 200, 210), 0.3f) : red3()) : rgb3(50, 12, 20), 1.0f);
        if (run)
            bloom(146.0f, 8.0f, beat ? 8.0f : 5.0f, red3(), beat ? 0.7f : 0.35f);
        int_text((int32_t)(ba_bpm(&eng) + 0.5f), t);
        tctext(146, 15, &FONT_XS, t, eng.ext ? C_RED : C_SUB, 0);
    }
    {
        int lvl = batt_level();
        if (lvl >= 0)
            batt_icon(9.0f, 21.0f, lvl, now);
    }
    head_display(t);
    seg_display(159, 4, t);
    cv_blit(0, 0);
}

/* ---- die Mitte: die Sektion der Seite und ihre vier Regler ---- */
static void draw_mid(uint32_t now)
{
    int gi, gn, i;
    cv_begin(240, 108, C_PANEL);
    panel_black(0, 108, 32);
    section(3, 6, 236, 104, PAGE_NAME[ui.page], edit.name);
    group_of(ui.page, &gi, &gn);
    for (i = 0; i < gn && gn > 1; i++) {
        float x = 227.0f - (float)(gn - 1 - i) * 8.0f;
        aa_disc(x, 6.0f, 2.1f, i == gi ? red3() : rgb3(52, 12, 20), 1.0f);
        if (i == gi)
            bloom(x, 6.0f, 5.0f, red3(), 0.5f);
    }
    if (now < ui.big_until) {
        tctext(120, 26, &FONT_XS, ui.big[1], C_WHITE, 2);
        ctext(120, 44, &APP_BIG_FONT, ui.big[0], C_RED);
        bloom(120.0f, 54.0f, 48.0f, red3(), 0.07f);
    } else {
        for (i = 0; i < 4; i++)
            draw_knob(i, 33 + i * 58, 17, now);
    }
    cv_blit(0, 32);
}

static void draw(uint32_t mask)
{
    uint32_t now = plat_ms();
    if (mask & R_HEAD)
        draw_head(now);
    if (mask & R_MID)
        draw_mid(now);
    if (mask & R_LOW) {
        cv_begin(240, 100, C_PANEL);
        panel_black(0, 100, 140);
        draw_low();
        cv_blit(0, 140);
    }
}

/* ---- die Eröffnung: die Anzeige testet alle Segmente, ein Lauflicht mit Nachleuchten über die 16 Tasten,
 *      dann "bASS" und der Name ---- */
#define INTRO_MS 2400u
static void intro_scene(float t, int32_t y0, int32_t h)
{
    int i;
    float name = sat01((t - 1.15f) / 0.45f), sub = sat01((t - 1.55f) / 0.4f), wipe = sat01((t - 1.2f) / 0.5f);
    cv_oy = -y0;
    panel_black(y0, h, y0);
    if (y0 < 70) {
        char d[10] = "8.8.8.8.";
        if (t > 0.4f && t < 1.15f) {
            int n = (int)((t - 0.4f) * 70.0f);
            d[0] = ' ';
            d[1] = (char)('0' + (n / 10) % 10);
            d[2] = (char)('0' + n % 10);
            d[3] = ' ';
            d[4] = 0;
        } else if (t >= 1.15f) {
            d[0] = 'b'; d[1] = 'A'; d[2] = 'S'; d[3] = 'S'; d[4] = 0;
        }
        seg_display(83, 30, d);
    }
    if (y0 + h > 150 && y0 < 190) {     /* das Lauflicht: hin, zurück, mit Nachleuchten */
        float pos = t < 1.7f ? fmod_pos(t * 20.0f, 30.0f) : -10.0f;
        float head = pos < 15.0f ? pos : 30.0f - pos;
        for (i = 0; i < 16; i++) {
            float dd = fabsf_((float)i - head), lit = t >= 1.7f ? sat01((t - 1.7f) * 2.5f) * ((i % 4 == 0) ? 0.62f : 0.3f)
                                                               : sat01(1.0f - dd / 2.2f);
            step_key(9 + i * 14, 160, lit, 0, lit > 0.9f);
        }
    }
    if (name > 0.0f) {
        uint16_t c = c565(mix3(c3(C_PANEL), c3(C_WHITE), name));
        tctext(120, 82, &APP_BIG_FONT, "BA-55", c, 3);
        if (y0 < 130 && y0 + h > 118) {
            int32_t x;
            for (x = 120 - (int32_t)(80.0f * wipe); x < 120 + (int32_t)(80.0f * wipe); x++)
                px_blend(x, 121, c3(C_LINE), 0.8f * name * (1.0f - fabsf_((float)x - 120.0f) / 90.0f));
        }
    }
    if (sub > 0.0f) {
        tctext(120, 129, &FONT_XS, "BASSLINE SYNTHESIZER", c565(mix3(c3(C_PANEL), c3(C_WHITE), sub)), 2);
        tctext(120, 203, &FONT_XS, "by swtdr34mz", c565(mix3(c3(C_PANEL), c3(C_SUB), sub)), 1);
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
static float fx_rv[BA_RV_FLOATS] __attribute__((section(".pool")));

void app_init(void)
{
    int i;
    bank_load(0);
    bank_load(1);
    settings_load();
    ba_init(&eng, SR, &edit);
    eng.bend_range = (float)set.bend;
    eng.tune_hz = 430.0f + (float)set.tune;
    ba_groove_use(&eng, &set.grv);
    ba_fx_memory(&eng, fx_rv);           /* der Reverb: sein Speicher im RAM-Pool */
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
        float pk = out_peak * 0.92f;
        for (i = 0; i < n; i++) {
            float m = bl[i], a = m < 0.0f ? -m : m;
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

void app_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_PANEL);
    intro_draw(3.0f);
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_PANEL);
    touch_on(1);
    ui.step_drawn = -2;
    ui.note_drawn = -2;
    ui.batt_drawn = -9;
    ui.batt_pct = -1;
    ui.batt_tpct = -1;
    ui.intro = 1;
    ui.intro_at = plat_ms();
    ui.redraw = R_ALL;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    int32_t s;
    if (ui.intro) {
        uint32_t t = now - ui.intro_at;
        if (t < INTRO_MS && !plat_keys() && !plat_buttons()) {
            intro_draw((float)t * 0.001f);
            plat_lights(0, 0, 0, 0, 0);
            return;
        }
        ui.intro = 0;
        ui.keys = plat_keys();
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
        ui.redraw |= R_LOW | R_HEAD;
    }
    ui.lfo_scope[ui.lfo_pos] = ba_lfo_out(&eng);
    ui.lfo_pos = (uint8_t)((ui.lfo_pos + 1u) % 96u);
    if (view_animated() && !(ui.frame & 1u))
        ui.redraw |= R_LOW;
    if (eng.kick_count != ui.kick_seen) {   /* GROOVE: eine Kick, die ARP-LED blitzt */
        ui.kick_seen = eng.kick_count;
        ui.flash_until = now + 70u;
    }
    s = ba_step_now(&eng);
    if (s != ui.step_drawn) {            /* ein neuer Step: die Tasten und die RUN-LED */
        ui.step_drawn = s;
        ui.redraw |= R_LOW | R_HEAD;
    }
    {
        int nd = eng.gate ? eng.note : -1;
        if (nd != ui.note_drawn) {       /* die 7-Segment-Anzeige zeigt die klingende Note */
            ui.note_drawn = nd;
            ui.redraw |= R_HEAD;
        }
    }
    {   /* der Akku: neu zeichnen, wenn er sich ändert; leer blinkt er im Halbsekundentakt */
        int lvl = batt_level(), p = lvl < 0 ? -1 : lvl >= 4 ? 200 : batt_percent(lvl);
        int key = lvl < 0 ? -1 : p + (p < 8 ? 300 * (int)((now / 500u) & 1u) : 0);
        if (key != ui.batt_drawn) {
            ui.batt_drawn = (int8_t)key;
            ui.redraw |= R_HEAD;
        }
    }
    if (ui.seg_until && now >= ui.seg_until) {
        ui.seg_until = 0;
        ui.redraw |= R_HEAD;
    }
    if (ui.rec)
        ui.redraw |= R_LOW;
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw |= R_HEAD;
    }
    if (ui.big_until && now >= ui.big_until) {
        ui.big_until = 0;
        ui.redraw |= R_MID;
    }
    {
        int k;
        for (k = 0; k < 4; k++)
            if (ui.knob_at[k] && now - ui.knob_at[k] >= 700u) {
                ui.knob_at[k] = 0;
                ui.redraw |= R_MID;
            }
    }
    if (now - ui.load_at >= 500u) {      /* bleibt die Last eine Sekunde über 88 %: ohne Oversampling (ECO) */
        ui.load_at = now;
        if (plat_cpu_pct() > 88u && !eng.eco) {
            if (++ui.hot >= 2) {
                eng.eco = 1;
                app_say("ECO MODE", "CPU HIGH");
            }
        } else {
            ui.hot = 0;
        }
    }
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
    if (!strncmp(what, "batt", 4) && what[4] >= '0' && what[4] <= '9') {   /* battN: den Akku vorgeben (Tests) */
        ui.batt_test = (uint8_t)(what[4] - '0');
        *got = 0;
        return 1;
    }
    if (!strncmp(what, "bpct", 4)) {     /* bpctN: die Prozent vorgeben (Tests) */
        ui.batt_tpct = (int8_t)atoi(what + 4);
        ui.batt_pct = -1;
        *got = 0;
        return 1;
    }
    if (!strcmp(what, "reverb")) {      /* reverb: 1 wenn der Hall seinen Speicher hat; reverb_lvl: wie laut er klingt (x1000) */
        *got = eng.rv != 0;
        return 1;
    }
    if (!strncmp(what, "reverb_ge", 9)) {   /* reverb_geN: der Hall klingt mit mindestens N/1000 (1) */
        *got = (int)(ba_reverb_level(&eng) * 1000.0f) >= atoi(what + 9);
        return 1;
    }
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
    else if (!strcmp(what, "arp"))
        *got = ui.arp;
    else if (!strcmp(what, "oct"))
        *got = set.oct;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "groove"))
        *got = set.grv.on;
    else if (!strcmp(what, "gstyle"))
        *got = set.grv.style;
    else if (!strncmp(what, "kicks_ge", 8))   /* kicks_geN: mindestens N Kicks gespielt (1) */
        *got = (int)eng.kick_count >= atoi(what + 8);
    else if (!strcmp(what, "tune"))
        *got = set.tune;
    else if (!strncmp(what, "seqfl", 5))
        *got = edit.seq[atoi(what + 5) - 1].fl;
    else if (!strncmp(what, "seqmod", 6))
        *got = edit.seq[atoi(what + 6) - 1].mod;
    else if (!strncmp(what, "seqnote", 7))
        *got = edit.seq[atoi(what + 7) - 1].note;
    else if (what[0] == 'p' && what[1] >= '0' && what[1] <= '9')
        *got = edit.p[atoi(what + 1)];
    else
        return 0;
    return 1;
}
#endif
