/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – der Klangkern. Reines C, keine Abhängigkeit von der FM-1-Plattform: dieselben Dateien laufen in der
 * Firmware (app.c) und später in einem VST-Plugin. Gebaut auf dem Kern von MONSUN.
 *
 * Eine Bassline nach dem Konzept der Erica Synths Bassline DB-01: ein VCO (Dreieck, Säge, Rechteck), ein
 * Sub-Oszillator eine Oktave tiefer, Rauschen, ein BBD-artiges DETUNE, ein aggressives 12-dB-Filter im
 * Polivoks-Stil (Tiefpass oder Bandpass), ein Overdrive auf einem Knopf, Hüllkurven für VCA, Filter und Tonhöhe,
 * ACCENT und SLIDE wie an der 303, drei Modulatoren, drei FX-Blöcke und ein 64-Step-Sequencer mit Accent, Slide
 * und einem Zufalls-Generator für Patterns.
 *
 * Ein Klang (ba_patch) besteht aus Bytes 0..127, genau so, wie die Firmware ihn speichert. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- die Parameter --- */
/* ein Modulator: 12 Parameter hintereinander (MO_* ist der Abstand zum Anfang) */
enum { MO_MODE, MO_SYNC, MO_LRATE, MO_LWAVE, MO_LFADE, MO_EA, MO_EDR, MO_ES, MO_RRATE, MO_RLAG, MO_RPROB, MO_SCOUNT,
       MO_N };

enum {
    /* der VCO (HOME) */
    P_WAVE, P_SUB, P_NOISE, P_DETUNE,
    /* das Filter (SEL) */
    P_CUTOFF, P_RESON, P_EGLVL, P_FMODE, P_ACCENT, P_TRACK,
    /* Hüllkurven: Filter, VCA, Tonhöhe */
    P_FA, P_FD, P_FS, P_FR, P_AA, P_AD, P_AS, P_AR, P_PENV, P_PDEC,
    /* SET */
    P_TUNE, P_SLIDE, P_GLIDE, P_VCALVL, P_LEGATO, P_GMODE, P_VELCUT, P_VELVCA,
    /* der Hochpass vor den FX */
    P_HPCUT, P_HPRES,
    /* FX1: Verzerrung */
    P_X1TYPE, P_X1GAIN, P_X1LVL, P_X1MIX,
    /* FX2: Chorus, Ensemble, Flanger */
    P_X2TYPE, P_X2DEPTH, P_X2SPD, P_X2WIDTH, P_X2FEED, P_X2MIX,
    /* FX3: Delay, Ping Pong, Reverbs */
    P_X3TYPE, P_X3P1, P_X3P2, P_X3FEED, P_X3DAMP, P_X3MIX, P_X3SYNC,
    /* Modulatoren */
    P_M1, P_M2 = P_M1 + MO_N, P_M3 = P_M2 + MO_N,
    /* Notensequencer */
    P_SLEN = P_M3 + MO_N, P_SDVS, P_SPROB, P_SSWING,
    /* DRIVE: der Overdrive auf ALGORITHM */
    P_DRIVE,
    BA_NP
};
#define P_MOD(m) (P_M1 + (m) * MO_N)

/* Art eines Parameters: Anzeige und Bedienung */
enum { MK_UNI, MK_BIP, MK_SW, MK_LIST };
typedef struct {
    const char *name;     /* lang (VST) */
    const char *label;    /* kurz, fürs Display */
    uint8_t def;          /* 0..127 */
    uint8_t kind;         /* MK_* */
    uint8_t n;            /* MK_LIST: Einträge */
    const char *const *names;   /* MK_LIST / MK_SW */
} ba_param_info;
extern const ba_param_info BA_PARAMS[BA_NP];

/* Listen */
enum { MODE_LFO, MODE_ENV, MODE_RND, MODE_STEP, BA_NMODES };
enum { SYNC_FREE, SYNC_KEY, SYNC_BPM, SYNC_BPMKEY };
enum { LW_TRI, LW_SQR, LW_PULSE, LW_TRAP, LW_CHAOS };
enum { X1_HARD, X1_FOLD, X1_SCURVE, X1_TRI, X1_SHRED, X1_CRUSH, BA_NX1 };
enum { X2_CHORUS, X2_ENSEMBLE, X2_FLANGER, BA_NX2 };
enum { X3_DELAY, X3_PING, X3_LARGE, X3_SOFT, BA_NX3 };
extern const char *const BA_MODE_NAME[BA_NMODES];
extern const char *const BA_X1_NAME[BA_NX1], *const BA_X2_NAME[BA_NX2], *const BA_X3_NAME[BA_NX3];

/* die Ziele der Modulatoren */
enum { D_CV, D_SUB, D_NOISE, D_PW, D_CUT, D_VCA, D_DRV, D_DET, D_FX2, D_FX3, D_RES, D_EGL, D_HPF, BA_NDEST };
extern const char *const BA_DEST_NAME[BA_NDEST];
extern const char *const BA_DEST_LONG[BA_NDEST];

/* Tempo-Teilungen: für die Modulatoren (BPM) und das Delay (SYNC) */
#define BA_NDIV 12
extern const char *const BA_DIV_NAME[BA_NDIV];
extern const float BA_DIV_BEATS[BA_NDIV];   /* Länge in Vierteln */
/* der Sequencer: Steps pro Takt */
#define BA_NDVS 10
extern const uint8_t BA_DVS[BA_NDVS];

/* die Wellen des VCO (ein Schalter wie am DB-01) */
#define BA_NWAVES 3
extern const char *const BA_WAVE_NAME[BA_NWAVES];

/* ---------------------------------------------------------------- ein Klang --- */
#define BA_STEPS 64
#define BA_MSTEPS 16
#define BA_REST 0        /* gate 0: Pause */
#define BA_TIE 127       /* gate 127: an den nächsten Step gebunden (gleiche Note: hält; SLIDE: gleitet) */
#define BA_ACC 1         /* fl: ACCENT */
#define BA_SLD 2         /* fl: SLIDE in den nächsten Step */
typedef struct {
    uint8_t note, gate, fl;    /* note: MIDI 24..96; fl: BA_ACC | BA_SLD */
} ba_step;

#define BA_NAMELEN 8
typedef struct {
    char name[BA_NAMELEN];               /* 7 Zeichen, mit 0 */
    uint16_t bpm;                        /* 30..300 */
    uint8_t p[BA_NP];
    uint8_t lvl[3][BA_NDEST];            /* der LVL jedes Modulators zu jedem Ziel: 64 = 0 */
    uint8_t mstep[3][BA_MSTEPS];         /* die Werte der Step-Modulatoren 0..127 */
    ba_step seq[BA_STEPS];
} ba_patch;

void ba_patch_init(ba_patch *pt);        /* der INITIAL PRESET */

/* --------------------------------------------------------------- die Bausteine --- */
typedef struct { float phase; } ba_osc;
typedef struct { float s1, s2; } ba_svf;   /* das Polivoks-artige Filter: zwei sättigende Integratoren */
typedef struct { float level; int stage; float a_inc, d_coef, r_coef, sus; } ba_adsr;   /* stage 0 aus, 1 A, 2 D/S, 3 R */

typedef struct {
    float ph, out, target, held, fade, fade_t, chaos;
    int stage, cycles;                   /* ENV: 0 aus, 1 A, 2 D/S, 3 R */
    int step, step_n;                    /* STEP: der Step, und wie oft schon gezählt */
    float step_to;
    uint32_t rs;
} ba_mod;

/* FX-Speicher: den gibt der Host (FM-1: der RAM-Pool) */
#define BA_DLY_FRAMES 44100              /* 1 s, wie bei MONSUN */
#define BA_CHO_FRAMES 2048               /* FX2 */
#define BA_REV_FLOATS 19200              /* die Reverbs: 12 Kämme, 8 Allpässe */
#define BA_WIDE_FRAMES 1024              /* DETUNE: die Stereo-Breite */

typedef struct {
    /* FX1 */
    float hp_l, hp_b;                    /* der Hochpass (SVF) */
    float cr_v;
    int cr_n;
    /* FX2 */
    float *cho;
    int cho_pos;
    float cho_ph, cho_fb;
    /* FX3 */
    int16_t *dl, *dr;
    int dpos;
    float dsl, dsr, dfl, dfr, dhl, dhr;
    float *comb[12], *ap[8], cf[12];     /* Kämme 0..5 links, 6..11 rechts; Allpässe 0..3 links, 4..7 rechts */
    int clen[12], cpos[12], alen[8], apos[8];
    float rdh, rdh2;
    /* DETUNE (Breite) */
    float *wide;
    int wpos;
    float wph;
    int ready;
} ba_fx;

/* Ereignisse von der Bedienung an den Audio-Teil (Ring, ein Schreiber / ein Leser) */
enum { MEV_ON, MEV_OFF, MEV_ALLOFF, MEV_PLAY, MEV_STOP, MEV_CONT, MEV_CLOCK, MEV_BEND, MEV_HOLD, MEV_ONF };
typedef struct { uint8_t type, a, b; } ba_ev;
#define BA_NEV 64

typedef struct {
    float sr;
    ba_patch *pt;                        /* der Klang, den die Bedienung bearbeitet */
    int dirty;                           /* der Klang hat sich geändert: neu umrechnen */
    /* Ereignisse */
    ba_ev ev[BA_NEV];
    volatile uint32_t ev_w, ev_r;
    /* Noten: der Stapel gehaltener Tasten (die letzte zählt) */
    uint8_t held[12];
    int nheld;
    int hold;                            /* HOLD: losgelassene Tasten klingen weiter */
    uint8_t latched[12];
    int nlatched;
    int note;                            /* die klingende Note, -1 keine */
    float vel, pitch, pitch_to, glide_coef, bend, bend_range;
    int gate;
    /* Stimme */
    ba_osc o1, o2, o3;                   /* o1 der VCO, o2 der Sub, o3 die DETUNE-Kopie */
    ba_svf vcf;
    ba_adsr fe, ae;
    float pe;                            /* die Tonhöhen-Hüllkurve (fällt von 1) */
    float pe_coef, acc, acc_env;         /* ACCENT: dieser Step, und sein eigener Filter-Stoß */
    float bbd_ph, bbd_wob;               /* DETUNE: das langsame Schwimmen der BBD-Kopie */
    float hb[46];
    int hb_pos;
    float dc_x, dc_y, drift1, drift2;
    uint32_t rs;
    int eco;
    /* Modulatoren */
    ba_mod mod[3];
    float md[BA_NDEST];                  /* die Summe zu jedem Ziel, jetzt */
    /* umgerechnet */
    float u_sub, u_noise, u_cut, u_res, u_egl, u_track, u_vca, u_velcut, u_velvca, u_tune, u_penv, u_acc;
    float u_glide, u_slide, u_det, u_drive, u_hp, u_hpq;
    int wave, bp, legato, auto_glide;
    float lvl[3][BA_NDEST];
    /* Sequencer und Takt */
    int running, cur, tie_hold, slide_next;
    float to_next, gate_left;
    int sw_odd;
    int transpose;
    int seq_note;                        /* die Note, die der Sequencer hält (-1 keine) */
    uint32_t counter, last_clock;
    float ext_bpm;
    int ext, ext_ticks, ext_running;
    int beat_tick;                       /* für die Step-Modulatoren */
    float step_clock;                    /* Samples bis zum nächsten Modulator-Step */
    /* FX */
    ba_fx fx;
    float mono[256];
    float level_now;                     /* für die Anzeige */
    float cut_hz;                        /* der Cutoff, wie er gerade steht (Anzeige) */
} ba_engine;

/* ------------------------------------------------------------- die Schnittstelle --- */
void ba_init(ba_engine *e, float sr, ba_patch *pt);
void ba_fx_memory(ba_engine *e, int16_t *dl, int16_t *dr, float *cho, float *rev, float *wide);
void ba_changed(ba_engine *e);           /* nach jeder Änderung am Klang */
void ba_render(ba_engine *e, float *l, float *r, int n);   /* n <= 256 */

/* aus der Bedienung (Hauptschleife): landen im Ring und wirken im nächsten Block */
void ba_note_on(ba_engine *e, int note, int vel);
void ba_note_off(ba_engine *e, int note);
void ba_note_on_play(ba_engine *e, int note, int vel);   /* spielt auch bei laufendem Sequencer (RECORD) */
void ba_all_off(ba_engine *e);
void ba_play(ba_engine *e, int on);
void ba_continue(ba_engine *e);
void ba_midi_clock(ba_engine *e);
void ba_bend(ba_engine *e, int v14);     /* 0..16383, 8192 Mitte */
void ba_hold(ba_engine *e, int on);

float ba_bpm(const ba_engine *e);
int ba_step_now(const ba_engine *e);     /* -1: gestoppt */
int ba_seq_len(const ba_engine *e);
int ba_dvs(const ba_engine *e);          /* Steps pro Takt */
float ba_mod_out(const ba_engine *e, int m);   /* für die Anzeige: -1..1 */
int ba_mod_step(const ba_engine *e, int m);

/* Umrechnungen für die Anzeige */
float ba_cutoff_hz(int v);
float ba_env_ms(int v, int attack);
float ba_lfo_hz(int v);
float ba_tune_semis(int v);
float ba_glide_ms(int v);
int ba_list_index(int id, int v);        /* MK_LIST: welcher Eintrag */
int ba_list_value(int id, int idx);      /* der Bytewert eines Eintrags */
int ba_wave_index(int v);
float ba_drive_db(int v);

/* der Zufalls-Generator (wie am DB-01): ein neues Pattern über die ersten LENG Steps, um den Grundton root,
 * Noten aus einer Moll-Pentatonik mit Oktavsprüngen, Pausen, Accents und Slides; seed macht jedes anders */
void ba_randomize(ba_patch *pt, int root, uint32_t seed);

/* Werks-Presets */
#define BA_NPRESETS 16
void ba_preset(ba_patch *pt, int i);
const char *ba_preset_name(int i);

#ifdef __cplusplus
}
#endif
