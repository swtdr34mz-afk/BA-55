/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – der Klangkern. Reines C, keine Abhängigkeit von der FM-1-Plattform: dieselben Dateien laufen in der
 * Firmware (app.c) und später in einem VST-Plugin.
 *
 * Eine Bassline streng nach der Erica Synths Bassline DB-01, nur mit dem, was das Gerät selbst hat:
 *   VCO        WAVE (Dreieck, Säge, Rechteck), SUB (Transistor-Teiler, eine Oktave tiefer), NOISE (ins Filter),
 *              DETUNE (eine BBD-verschobene Kopie; weiter aufgedreht läuft ihr LFO schneller, ganz rechts: FM),
 *              OVERDRIVE (eine Überblendung vom trockenen Klang zur Übersteuerungsstufe), FM LVL (LFO auf die Tonhöhe)
 *   VCF        CUTOFF, RESONANCE (ganz rechts: Selbstoszillation), DECAY und DEPTH der Filterhüllkurve,
 *              MODE Tiefpass / Bandpass (ein sättigendes 12-dB-Filter im Geist von Polivoks / Acidbox)
 *   VCA        RELEASE einer Sustain-Release-Hüllkurve, die der Gate-Länge folgt; ganz rechts bleibt der VCA offen
 *   LFO        RATE, LFO LVL (auf den Cutoff), SHAPE (10 Formen), SYNC (frei / Tempo), RESET (bei jedem Gate)
 *   ACCENT     pro Step: öffnet VCA und VCF ein wenig       SLIDE   pro Step: gleitet zur nächsten Note
 *   GLIDE      für das Spiel von Tasten / MIDI          TUNE    die Stimmung 430..450 Hz (Einstellungen)
 *   Sequencer  64 Steps (4 Takte zu 16): Note, Gate 10..100 % (100 % verbindet), Accent, Slide, MOD (Cutoff),
 *              PITCH ENV (Depth / Decay 1..9), Wahrscheinlichkeit (% oder 1/N); LAST STEP, PLAY MODE
 *              (FWD, BWD, RND, PP, PP2), SHUFFLE, Transponieren; RND PATT mit Grenzen; Skalen; Arpeggiator.
 *
 * Ein Klang mit seinem Pattern (ba_patch) besteht aus Bytes, genau so, wie die Firmware ihn speichert. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- die Parameter --- */
enum {
    /* VCO */
    P_WAVE, P_SUB, P_NOISE, P_DETUNE, P_DRIVE, P_FMLVL,
    /* VCF und VCA */
    P_CUTOFF, P_RESON, P_FDECAY, P_FDEPTH, P_FMODE, P_VREL,
    /* LFO */
    P_LRATE, P_LFOLVL, P_LSHAPE, P_LSYNC, P_LRESET,
    /* das Spiel */
    P_GLIDE,
    /* das Pattern */
    P_LAST, P_PMODE, P_SHUFFLE, P_SCALE, P_ROOT,
    /* RND PATT */
    P_RTGT, P_RAMT, P_RLO, P_RHI,
    /* der Arpeggiator */
    P_ADIR, P_AOCT, P_AGATE, P_ALATCH,
    /* FX: die Filterbank (nach der Erica Synths Resonant Filterbank) und ein Reverb */
    P_FBSHAPE, P_FBRESO, P_FBDRIVE, P_FBMIX, P_FBSPREAD, P_FBMOD, P_FBRATE, P_FBDEPTH,
    P_FB1, P_FB10 = P_FB1 + 9,
    P_RVSIZE, P_RVDECAY, P_RVTONE, P_RVMIX,
    BA_NP
};

/* Art eines Parameters: Anzeige und Bedienung */
enum { MK_UNI, MK_BIP, MK_SW, MK_LIST };
typedef struct {
    const char *name;     /* lang (VST) */
    const char *label;    /* kurz, fürs Display */
    uint8_t def;          /* 0..127; bei Listen: der Eintrag */
    uint8_t kind;         /* MK_* */
    uint8_t n;            /* MK_LIST / MK_SW: Einträge */
    const char *const *names;
} ba_param_info;
extern const ba_param_info BA_PARAMS[BA_NP];

/* Listen */
#define BA_NWAVES 3
extern const char *const BA_WAVE_NAME[BA_NWAVES];
enum { LS_SIN, LS_TRI, LS_SAW, LS_RAMP, LS_SQR, LS_3DEC, LS_SAWTRI, LS_3RAMP, LS_SH, LS_NOISE, BA_NSHAPES };
extern const char *const BA_SHAPE_NAME[BA_NSHAPES];
enum { PM_FWD, PM_BWD, PM_RND, PM_PP, PM_PP2, BA_NPMODES };
extern const char *const BA_PMODE_NAME[BA_NPMODES];
#define BA_NSCALES 8
extern const char *const BA_SCALE_NAME[BA_NSCALES];
extern const uint16_t BA_SCALE_MASK[BA_NSCALES];   /* Bit n: Halbton n über dem Grundton gehört dazu */
extern const char *const BA_NOTE_NAME[12];
enum { RT_PATTERN, RT_NOTES, RT_GATES, RT_ACC, RT_SLIDE, RT_MOD, RT_PENV, RT_PROB, RT_ON, BA_NRTGT };
extern const char *const BA_RTGT_NAME[BA_NRTGT];
enum { AD_UP, AD_DOWN, AD_UPDN, AD_RND, BA_NADIR };
extern const char *const BA_ADIR_NAME[BA_NADIR];
#define BA_NSYNC 12
extern const char *const BA_SYNC_NAME[BA_NSYNC];   /* LFO im Tempo: die Länge eines Zyklus */
extern const float BA_SYNC_BEATS[BA_NSYNC];
/* FX: die zehn Bänder der Filterbank (fest, wie am Original), ihre Formen, die Modulation */
#define BA_NBANDS 10
extern const float BA_BAND_HZ[BA_NBANDS];
#define BA_NFBSHAPES 12
extern const char *const BA_FBSHAPE_NAME[BA_NFBSHAPES];
extern const uint8_t BA_FBSHAPE[BA_NFBSHAPES][BA_NBANDS];   /* die Pegel der Bänder je Form (P_FB1..P_FB10) */
enum { FM_OFF, FM_LFO, FM_CLOCK, FM_ENV, BA_NFBMODS };
extern const char *const BA_FBMOD_NAME[BA_NFBMODS];
#define BA_NPROB 7
extern const char *const BA_PROB_NAME[BA_NPROB];   /* 100 %, 75 %, 50 %, 25 %, 1/2, 1/3, 1/4 */

/* ---------------------------------------------------------------- ein Pattern --- */
#define BA_STEPS 64
#define BA_ACC 1         /* fl: ACCENT */
#define BA_SLD 2         /* fl: SLIDE zur nächsten Note */
typedef struct {
    uint8_t note;        /* MIDI 12..108 */
    uint8_t gate;        /* 0: der Step ist aus; 1..10: 10 % .. 100 % (100 %: verbunden mit dem nächsten) */
    uint8_t fl;          /* BA_ACC | BA_SLD */
    uint8_t mod;         /* die Mod-Spur: zum Cutoff, 64 = nichts */
    uint8_t pe;          /* PITCH ENV: Depth (obere 4 Bit, 0 = aus, 1..9), Decay (untere 4 Bit, 1..9) */
    uint8_t prob;        /* 0..BA_NPROB-1 */
} ba_step;
#define BA_PE_DEPTH(pe) ((pe) >> 4)
#define BA_PE_DECAY(pe) ((pe) & 15)

#define BA_NAMELEN 8
typedef struct {
    char name[BA_NAMELEN];               /* 7 Zeichen, mit 0 */
    uint16_t bpm;                        /* 30..300 */
    uint8_t p[BA_NP];
    ba_step seq[BA_STEPS];
} ba_patch;

void ba_patch_init(ba_patch *pt);

/* --------------------------------------------------------------- die Bausteine --- */
typedef struct { float phase; } ba_osc;
typedef struct { float s1, s2; } ba_svf;
#define BA_BBD_N 2048                    /* DETUNE: die Eimerkette (bei 88.2 kHz: 23 ms) */

/* Ereignisse von der Bedienung an den Audio-Teil (Ring, ein Schreiber / ein Leser) */
enum { MEV_ON, MEV_OFF, MEV_ALLOFF, MEV_PLAY, MEV_STOP, MEV_CONT, MEV_CLOCK, MEV_BEND, MEV_ARP, MEV_ONF };
typedef struct { uint8_t type, a, b; } ba_ev;
#define BA_NEV 64

/* ---- GROOVE: Kick, Drums und eine pumpende Sidechain (wie in MONSUN) ---- */
#define BA_NSTYLES 10
extern const char *const BA_STYLE_NAME[BA_NSTYLES];
typedef struct {
    uint8_t on;                          /* 1: läuft mit PLAY */
    uint8_t style;                       /* 0..BA_NSTYLES-1 */
    uint8_t kick, drums, pump;           /* 0..127: KICK (kurz .. boomig), DRUMS (Lautstärke), PUMP (Tiefe) */
    uint8_t rel, hats, tune, var;        /* 0..127: RELEASE der Pumpe, HATS, TUNE (64 = 0), VAR (Variation) */
} ba_groove;
void ba_groove_init(ba_groove *g);

typedef struct { int16_t off; uint8_t step; } ba_gtrig;
typedef struct {
    float kph, kenv, kt, kfreq, kdec, kclick, kdrive, kpitch;   /* Kick */
    float cenv, ct, cbp1, cbp2;          /* Clap / Snare (Rauschen im Bandpass) */
    float sph, senv;                     /* Snare: der Ton */
    float henv, oenv, hlp1, hlp2;        /* Hats: geschlossen, offen */
    float pph, pfreq, penv;              /* Perc */
    float sc_t;                          /* Samples seit dem letzten Kick (Sidechain) */
    float lvl_k, lvl_c, lvl_h, lvl_o, lvl_p;
    uint32_t rs;
} ba_drums;

typedef struct {
    float sr;
    ba_patch *pt;                        /* der Klang, den die Bedienung bearbeitet */
    int dirty;
    float tune_hz;                       /* 430..450 (Einstellungen) */
    /* Ereignisse */
    ba_ev ev[BA_NEV];
    volatile uint32_t ev_w, ev_r;
    /* Tasten: der Stapel gehaltener Tasten (die letzte zählt) */
    uint8_t held[12];
    int nheld;
    int note;                            /* die klingende Note, -1 keine */
    float pitch, pitch_to, glide_coef, bend, bend_range;
    int gate;
    /* die Stimme */
    ba_osc o1, o2;
    ba_svf vcf;
    float fenv, venv;                    /* Filterhüllkurve (fällt von 1), VCA */
    float acc;                           /* ACCENT dieses Steps: 0 oder 1 */
    float pe, pe_amt, pe_coef;           /* PITCH ENV dieses Steps */
    float mod_cut;                       /* MOD dieses Steps, in Oktaven */
    float hb[46];
    int hb_pos;
    float dc_x, dc_y, drift;
    uint32_t rs;
    int eco;
    /* DETUNE */
    float bbd[BA_BBD_N];
    int bbd_w;
    float bbd_ph, bbd_lp;
    /* LFO */
    float lfo_ph, lfo_out, lfo_sh, lfo_tgt;
    /* umgerechnet */
    int wave, bp;
    float u_sub, u_noise, u_det, u_drive, u_fm, u_cut, u_res, u_fdec, u_fdep, u_vrel, u_lfolvl, u_glide;
    float fdec_coef, vrel_coef;
    /* Sequencer und Takt */
    int running, cur, pp_dir, loops, sw_odd;
    float to_next, gate_left;
    int tie_hold, slide_next;
    int transpose;
    int seq_note;
    uint32_t counter, last_clock;
    float ext_bpm;
    int ext, ext_ticks;
    /* Arpeggiator */
    int arp, arp_i, arp_note;
    uint8_t latched[12];
    int nlatched;
    float arp_next;
    /* GROOVE */
    const ba_groove *gv;
    int g_step;
    float g_to_next;
    ba_gtrig gq[16];
    int gqn;
    ba_drums dr;
    float sc_now;                        /* die Sidechain jetzt: 1 offen, 0 ganz geduckt (Anzeige) */
    uint32_t kick_count;
    /* FX: die Filterbank */
    float fb_s1[BA_NBANDS], fb_s2[BA_NBANDS], fb_y[BA_NBANDS], fb_lvl[BA_NBANDS], fb_g[BA_NBANDS];
    float fb_ph, fb_env;
    /* FX: der Reverb (sechs Kämme und vier Allpässe je Seite, davor ein Pre-Delay) */
    float *rv;
    float *rv_comb[12], *rv_ap[8], *rv_pre, rv_cf[12];
    int rv_clen[12], rv_cpos[12], rv_alen[8], rv_apos[8], rv_pre_len, rv_pre_pos;
    float rv_lvl;
    /* Ausgang */
    float mono[256];
    float level_now, cut_hz;
} ba_engine;

/* FX: der Speicher des Reverbs (den gibt der Host: auf dem FM-1 der RAM-Pool); ohne ihn läuft nur die Filterbank */
#define BA_RV_FLOATS 19400

/* ------------------------------------------------------------- die Schnittstelle --- */
void ba_init(ba_engine *e, float sr, ba_patch *pt);
void ba_changed(ba_engine *e);
void ba_groove_use(ba_engine *e, const ba_groove *g);   /* die GROOVE-Einstellungen (gehören der Bedienung) */
int ba_groove_step(const ba_engine *e);  /* 0..31, -1: steht */
int ba_groove_hit(const ba_engine *e, int lane, int step);
void ba_render(ba_engine *e, float *l, float *r, int n);   /* n <= 256 */
void ba_fx_memory(ba_engine *e, float *rv);   /* BA_RV_FLOATS floats für den Reverb (oder 0: ohne Reverb) */
float ba_band_level(const ba_engine *e, int band);   /* für die Anzeige: was im Band gerade klingt, 0..1 */
float ba_band_gain(const ba_engine *e, int band);    /* und wie weit es gerade offen ist (mit der Modulation) */
float ba_reverb_level(const ba_engine *e);

void ba_note_on(ba_engine *e, int note, int vel);
void ba_note_off(ba_engine *e, int note);
void ba_note_on_play(ba_engine *e, int note, int vel);   /* spielt auch bei laufendem Sequencer (RECORD) */
void ba_all_off(ba_engine *e);
void ba_play(ba_engine *e, int on);
void ba_continue(ba_engine *e);
void ba_midi_clock(ba_engine *e);
void ba_bend(ba_engine *e, int v14);
void ba_arp(ba_engine *e, int on);

float ba_bpm(const ba_engine *e);
int ba_step_now(const ba_engine *e);     /* -1: gestoppt */
int ba_seq_len(const ba_engine *e);
float ba_lfo_out(const ba_engine *e);    /* -1..1 */

/* Umrechnungen für die Anzeige */
float ba_cutoff_hz(int v);
float ba_fdecay_ms(int v);
float ba_vrel_ms(int v);                 /* < 0: der VCA bleibt offen */
float ba_lfo_hz(int v);
float ba_glide_ms(int v);
float ba_pe_decay_ms(int d);             /* 1..9 */
int ba_list_index(int id, int v);
int ba_list_value(int id, int idx);
int ba_wave_index(int v);

/* RND PATT: die Steps 0..LAST-1 neu, nach dem Ziel, der Menge und den Grenzen des Klangs (P_RT*) */
void ba_randomize(ba_patch *pt, uint32_t seed);
/* eine Note der Skala am nächsten (für die Zufallsnoten und den Arpeggiator) */
int ba_scale_snap(const ba_patch *pt, int note);

/* Werks-Presets */
#define BA_NPRESETS 16
void ba_preset(ba_patch *pt, int i);
const char *ba_preset_name(int i);

#ifdef __cplusplus
}
#endif
