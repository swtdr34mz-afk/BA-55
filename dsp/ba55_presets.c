/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – die 16 Werks-Presets: Klang, Modulatoren und Pattern. Teil von ba55_core.c
 *
 * Spickzettel (alle Werte 0..127):
 *   WAVE      0 TRI, 64 SAW, 127 SQR
 *   CUTOFF    20 Hz * 2^(v/12.7)       30 = 100 Hz, 40 = 180 Hz, 50 = 300 Hz, 60 = 530 Hz, 74 = 1 kHz
 *   ENV MOD   64 = 0, 127 = +7 Oktaven
 *   DECAY     50 = 70 ms, 58 = 120 ms, 66 = 210 ms, 74 = 360 ms, 88 = 1 s
 *   P.DECAY   30 = 17 ms, 40 = 33 ms, 50 = 70 ms
 *   LFO RATE  26 = 0.1 Hz, 44 = 0.3 Hz, 64 = 1 Hz, 89 = 5 Hz, 110 = 18 Hz
 *   LVL       64 = 0, darüber +, darunter -
 * Steps: notes 0 = Pause; gates 0 = 50 %, 127 = TIE; fl: A = ACCENT, S = SLIDE */

typedef struct { uint8_t id, v; } pv_t;
typedef struct { uint8_t m, dest, v; } lv_t;
typedef struct {
    const char *name;
    uint16_t bpm;
    const pv_t *pv;
    uint8_t npv;
    const lv_t *lv;
    uint8_t nlv;
    const uint8_t *notes;                /* 16 Steps (0: Pause); ohne: der Zufalls-Generator */
    const uint8_t *gates;                /* 0 = 50 %, 127 = TIE */
    const uint8_t *fl;                   /* A, S */
} preset_t;

#define M(m, o) (uint8_t)(P_MOD(m) + (o))
#define N(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))
#define A BA_ACC
#define S BA_SLD
#define AS (BA_ACC | BA_SLD)

/* 1 BA-55: die Säge, Resonanz hoch, Accents und Slides, ein wenig DRIVE und ein Ping Pong */
static const pv_t PV_BA[] = {
    {P_WAVE, 64}, {P_SUB, 30}, {P_CUTOFF, 44}, {P_RESON, 96}, {P_EGLVL, 108}, {P_FD, 60}, {P_ACCENT, 95},
    {P_DRIVE, 40}, {P_X3TYPE, 42}, {P_X3P1, 75}, {P_X3FEED, 40}, {P_X3MIX, 22}, {P_X3DAMP, 30}, {P_SDVS, 99}};
static const uint8_t NT_BA[16] = {33, 33, 45, 33, 0, 36, 33, 43, 33, 45, 0, 33, 48, 45, 36, 40};
static const uint8_t GT_BA[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t FL_BA[16] = {A, 0, S, 0, 0, A, 0, AS, 0, A, 0, 0, AS, 0, A, 0};

/* 2 ACIDLIN: Resonanz fast am Pfeifen, viele Slides, das Delay im punktierten Achtel */
static const pv_t PV_ACID[] = {
    {P_WAVE, 64}, {P_CUTOFF, 38}, {P_RESON, 112}, {P_EGLVL, 114}, {P_FD, 56}, {P_ACCENT, 110}, {P_SLIDE, 40},
    {P_DRIVE, 55}, {P_X3TYPE, 0}, {P_X3P1, 70}, {P_X3P2, 82}, {P_X3FEED, 50}, {P_X3MIX, 28}, {P_SDVS, 99},
    {P_SSWING, 20}};
static const uint8_t NT_ACID[16] = {36, 48, 36, 39, 36, 0, 51, 36, 46, 36, 48, 0, 36, 43, 36, 41};
static const uint8_t GT_ACID[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t FL_ACID[16] = {A, S, 0, A, S, 0, AS, 0, A, S, A, 0, 0, AS, 0, A};

/* 3 SQRACID: das Rechteck, DRIVE weit auf */
static const pv_t PV_SQR[] = {
    {P_WAVE, 127}, {P_CUTOFF, 42}, {P_RESON, 100}, {P_EGLVL, 104}, {P_FD, 62}, {P_ACCENT, 90}, {P_DRIVE, 78},
    {P_X2TYPE, 0}, {P_X2MIX, 20}, {P_SDVS, 99}};
static const uint8_t NT_SQR[16] = {31, 31, 0, 43, 31, 34, 0, 31, 38, 31, 0, 43, 31, 46, 43, 38};
static const uint8_t FL_SQR[16] = {A, 0, 0, AS, 0, A, 0, 0, S, 0, 0, A, 0, AS, 0, 0};

/* 4 SUBDROP: Dreieck und Sub, ein tiefer Fall der Tonhöhe bei jedem Anschlag, lange Noten */
static const pv_t PV_SUBD[] = {
    {P_WAVE, 0}, {P_SUB, 110}, {P_CUTOFF, 58}, {P_RESON, 20}, {P_EGLVL, 80}, {P_FD, 74}, {P_PENV, 40},
    {P_PDEC, 62}, {P_AD, 90}, {P_AS, 110}, {P_DRIVE, 30}, {P_SDVS, 99}};
static const uint8_t NT_SUBD[16] = {28, 0, 0, 28, 0, 0, 28, 0, 31, 0, 0, 26, 0, 0, 28, 0};
static const uint8_t GT_SUBD[16] = {127, 0, 0, 100, 0, 0, 90, 0, 127, 0, 0, 100, 0, 0, 80, 0};

/* 5 BPZAP: der Bandpass, Resonanz hoch, die Tonhöhe springt kurz hoch: Zap */
static const pv_t PV_ZAP[] = {
    {P_WAVE, 64}, {P_FMODE, 127}, {P_CUTOFF, 56}, {P_RESON, 110}, {P_EGLVL, 100}, {P_FD, 52}, {P_PENV, 100},
    {P_PDEC, 34}, {P_AD, 60}, {P_AS, 0}, {P_DRIVE, 50}, {P_X3TYPE, 42}, {P_X3P1, 60}, {P_X3MIX, 25},
    {P_SDVS, 99}};
static const uint8_t NT_ZAP[16] = {40, 0, 40, 52, 0, 40, 0, 47, 40, 0, 52, 40, 0, 45, 40, 0};
static const uint8_t FL_ZAP[16] = {A, 0, 0, A, 0, 0, 0, A, 0, 0, A, 0, 0, A, 0, 0};

/* 6 DIRTY: alles zerreißt: DRIVE und S-Curve hintereinander */
static const pv_t PV_DIRTY[] = {
    {P_WAVE, 64}, {P_SUB, 50}, {P_CUTOFF, 48}, {P_RESON, 84}, {P_EGLVL, 100}, {P_FD, 66}, {P_ACCENT, 100},
    {P_DRIVE, 112}, {P_X1TYPE, 51}, {P_X1GAIN, 70}, {P_X1LVL, 70}, {P_X1MIX, 60}, {P_SDVS, 99}};
static const uint8_t NT_DIRTY[16] = {29, 29, 41, 29, 0, 29, 32, 29, 0, 29, 41, 39, 29, 0, 36, 29};
static const uint8_t FL_DIRTY[16] = {A, 0, S, 0, 0, A, 0, 0, 0, A, AS, 0, 0, 0, A, 0};

/* 7 ROLLER: Techno-Offbeat, Rechteck und Sub, kurz und trocken */
static const pv_t PV_ROLL[] = {
    {P_WAVE, 127}, {P_SUB, 80}, {P_CUTOFF, 50}, {P_RESON, 40}, {P_EGLVL, 90}, {P_FD, 54}, {P_AD, 58},
    {P_AS, 0}, {P_AR, 20}, {P_DRIVE, 45}, {P_SDVS, 99}};
static const uint8_t NT_ROLL[16] = {0, 0, 33, 0, 0, 0, 33, 0, 0, 0, 33, 0, 0, 0, 36, 0};
static const uint8_t FL_ROLL[16] = {0, 0, A, 0, 0, 0, 0, 0, 0, 0, A, 0, 0, 0, 0, 0};

/* 8 DETROIT: Säge mit DETUNE, Chorus, warm */
static const pv_t PV_DET[] = {
    {P_WAVE, 64}, {P_DETUNE, 70}, {P_SUB, 40}, {P_CUTOFF, 56}, {P_RESON, 60}, {P_EGLVL, 92}, {P_FD, 70},
    {P_DRIVE, 25}, {P_X2TYPE, 0}, {P_X2MIX, 40}, {P_X3TYPE, 42}, {P_X3P1, 60}, {P_X3MIX, 22}, {P_SDVS, 99}};
static const uint8_t NT_DET[16] = {36, 0, 48, 0, 46, 0, 36, 43, 0, 36, 48, 0, 51, 0, 46, 43};
static const uint8_t FL_DET[16] = {A, 0, 0, 0, S, 0, 0, A, 0, 0, A, 0, AS, 0, 0, 0};

/* 9 SWARM: DETUNE ganz, Ensemble, ein langsamer LFO im Filter */
static const pv_t PV_SWARM[] = {
    {P_WAVE, 64}, {P_DETUNE, 127}, {P_SUB, 60}, {P_CUTOFF, 52}, {P_RESON, 70}, {P_EGLVL, 84}, {P_FD, 84},
    {P_AD, 90}, {P_AS, 110}, {P_AR, 60}, {P_X2TYPE, 64}, {P_X2MIX, 50}, {P_X3TYPE, 85}, {P_X3P2, 100},
    {P_X3FEED, 80}, {P_X3MIX, 30}, {P_SDVS, 71}, {M(0, MO_LRATE), 40}};
static const lv_t LV_SWARM[] = {{0, D_CUT, 82}};
static const uint8_t NT_SWARM[16] = {33, 0, 0, 33, 0, 0, 40, 0, 38, 0, 0, 36, 0, 0, 31, 0};
static const uint8_t GT_SWARM[16] = {127, 0, 0, 110, 0, 0, 127, 0, 127, 0, 0, 110, 0, 0, 100, 0};
static const uint8_t FL_SWARM[16] = {0, 0, 0, S, 0, 0, 0, 0, 0, 0, 0, S, 0, 0, 0, 0};

/* 10 PSYROLL: Psytrance-Bass, drei Sechzehntel nach jedem Kick */
static const pv_t PV_PSY[] = {
    {P_WAVE, 64}, {P_SUB, 20}, {P_CUTOFF, 52}, {P_RESON, 50}, {P_EGLVL, 96}, {P_FD, 50}, {P_AD, 52},
    {P_AS, 0}, {P_AR, 18}, {P_DRIVE, 50}, {P_SDVS, 99}};
static const uint8_t NT_PSY[16] = {0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 31, 28};
static const uint8_t FL_PSY[16] = {0, A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0, A, 0, 0};

/* 11 BOOMKCK: Dreieck mit großer Tonhöhen-Hüllkurve: eine Bassdrum (der DB-01 kann das auch) */
static const pv_t PV_KICK[] = {
    {P_WAVE, 0}, {P_SUB, 0}, {P_CUTOFF, 70}, {P_RESON, 10}, {P_EGLVL, 64}, {P_PENV, 127}, {P_PDEC, 36},
    {P_AD, 70}, {P_AS, 0}, {P_AR, 40}, {P_DRIVE, 60}, {P_VELVCA, 0}, {P_SDVS, 99}};
static const uint8_t NT_KICK[16] = {29, 0, 0, 0, 29, 0, 0, 0, 29, 0, 0, 0, 29, 0, 0, 29};
static const uint8_t FL_KICK[16] = {A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0};

/* 12 NOIZHAT: nur Rauschen durch den Bandpass, kurz: ein Hi-Hat auf den Offbeats */
static const pv_t PV_HAT[] = {
    {P_WAVE, 0}, {P_SUB, 0}, {P_NOISE, 127}, {P_FMODE, 127}, {P_CUTOFF, 112}, {P_RESON, 60}, {P_EGLVL, 74},
    {P_FD, 40}, {P_TRACK, 127}, {P_AD, 42}, {P_AS, 0}, {P_AR, 10}, {P_HPCUT, 90}, {P_X3TYPE, 85},
    {P_X3MIX, 15}, {P_SDVS, 99}};
static const uint8_t NT_HAT[16] = {0, 0, 72, 0, 0, 0, 72, 72, 0, 0, 72, 0, 0, 0, 72, 79};
static const uint8_t FL_HAT[16] = {0, 0, A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0, A, 0};

/* 13 WOBBLER: der LFO im Achtel auf dem Filter */
static const pv_t PV_WOB[] = {
    {P_WAVE, 64}, {P_SUB, 90}, {P_CUTOFF, 46}, {P_RESON, 66}, {P_EGLVL, 70}, {P_FD, 80}, {P_AD, 90},
    {P_AS, 120}, {P_DRIVE, 70}, {P_SDVS, 71},
    {M(0, MO_SYNC), 85}, {M(0, MO_LRATE), 76}};
static const lv_t LV_WOB[] = {{0, D_CUT, 104}};
static const uint8_t NT_WOB[16] = {26, 0, 0, 0, 26, 0, 0, 0, 29, 0, 0, 0, 24, 0, 0, 0};
static const uint8_t GT_WOB[16] = {127, 0, 0, 0, 127, 0, 0, 0, 127, 0, 0, 0, 127, 0, 0, 0};
static const uint8_t FL_WOB[16] = {S, 0, 0, 0, 0, 0, 0, 0, S, 0, 0, 0, 0, 0, 0, 0};

/* 14 FMGRIT: ein schneller LFO auf der Tonhöhe (der FM-Regler des DB-01), Rechteck, DRIVE */
static const pv_t PV_FM[] = {
    {P_WAVE, 127}, {P_SUB, 50}, {P_CUTOFF, 54}, {P_RESON, 80}, {P_EGLVL, 96}, {P_FD, 62}, {P_DRIVE, 90},
    {P_X3TYPE, 42}, {P_X3P1, 50}, {P_X3MIX, 20}, {P_SDVS, 99},
    {M(0, MO_LRATE), 116}, {M(0, MO_LWAVE), 32}};
static const lv_t LV_FM[] = {{0, D_CV, 90}};
static const uint8_t NT_FM[16] = {36, 0, 36, 36, 0, 39, 36, 0, 36, 0, 43, 36, 0, 41, 36, 34};
static const uint8_t FL_FM[16] = {A, 0, 0, S, 0, A, 0, 0, A, 0, AS, 0, 0, A, 0, 0};

/* 15 RANDOM: der Zufalls-Generator hat das Pattern geschrieben (ARP auf SEQ SETUP macht ein neues) */
static const pv_t PV_RND[] = {
    {P_WAVE, 64}, {P_CUTOFF, 42}, {P_RESON, 104}, {P_EGLVL, 110}, {P_FD, 58}, {P_ACCENT, 100}, {P_DRIVE, 60},
    {P_X3TYPE, 42}, {P_X3P1, 75}, {P_X3MIX, 25}, {P_SDVS, 99}};

/* 16 DRONE01: der Bandpass, DETUNE, zwei langsame LFOs, großer Hall */
static const pv_t PV_DRONE[] = {
    {P_WAVE, 64}, {P_FMODE, 127}, {P_DETUNE, 100}, {P_SUB, 70}, {P_NOISE, 20}, {P_CUTOFF, 62}, {P_RESON, 90},
    {P_EGLVL, 64}, {P_AA, 80}, {P_AS, 127}, {P_AR, 100}, {P_X3TYPE, 85}, {P_X3P1, 70}, {P_X3P2, 127},
    {P_X3FEED, 110}, {P_X3MIX, 60}, {P_SDVS, 15}, {M(0, MO_LRATE), 22}, {M(1, MO_LRATE), 30},
    {M(1, MO_LWAVE), 95}};
static const lv_t LV_DRONE[] = {{0, D_CUT, 90}, {1, D_RES, 80}};
static const uint8_t NT_DRONE[16] = {33, 0, 0, 0, 36, 0, 0, 0, 31, 0, 0, 0, 28, 0, 0, 0};
static const uint8_t GT_DRONE[16] = {127, 0, 0, 0, 127, 0, 0, 0, 127, 0, 0, 0, 127, 0, 0, 0};
static const uint8_t FL_DRONE[16] = {S, 0, 0, 0, S, 0, 0, 0, S, 0, 0, 0, S, 0, 0, 0};

static const preset_t PRESETS[BA_NPRESETS] = {
    {"BA-55", 126, PV_BA, N(PV_BA), 0, 0, NT_BA, GT_BA, FL_BA},
    {"ACIDLIN", 130, PV_ACID, N(PV_ACID), 0, 0, NT_ACID, GT_ACID, FL_ACID},
    {"SQRACID", 128, PV_SQR, N(PV_SQR), 0, 0, NT_SQR, 0, FL_SQR},
    {"SUBDROP", 122, PV_SUBD, N(PV_SUBD), 0, 0, NT_SUBD, GT_SUBD, 0},
    {"BPZAP", 128, PV_ZAP, N(PV_ZAP), 0, 0, NT_ZAP, 0, FL_ZAP},
    {"DIRTY", 132, PV_DIRTY, N(PV_DIRTY), 0, 0, NT_DIRTY, 0, FL_DIRTY},
    {"ROLLER", 130, PV_ROLL, N(PV_ROLL), 0, 0, NT_ROLL, 0, FL_ROLL},
    {"DETROIT", 124, PV_DET, N(PV_DET), 0, 0, NT_DET, 0, FL_DET},
    {"SWARM", 110, PV_SWARM, N(PV_SWARM), LV_SWARM, N(LV_SWARM), NT_SWARM, GT_SWARM, FL_SWARM},
    {"PSYROLL", 142, PV_PSY, N(PV_PSY), 0, 0, NT_PSY, 0, FL_PSY},
    {"BOOMKCK", 128, PV_KICK, N(PV_KICK), 0, 0, NT_KICK, 0, FL_KICK},
    {"NOIZHAT", 128, PV_HAT, N(PV_HAT), 0, 0, NT_HAT, 0, FL_HAT},
    {"WOBBLER", 140, PV_WOB, N(PV_WOB), LV_WOB, N(LV_WOB), NT_WOB, GT_WOB, FL_WOB},
    {"FMGRIT", 126, PV_FM, N(PV_FM), LV_FM, N(LV_FM), NT_FM, 0, FL_FM},
    {"RANDOM", 128, PV_RND, N(PV_RND), 0, 0, 0, 0, 0},
    {"DRONE01", 72, PV_DRONE, N(PV_DRONE), LV_DRONE, N(LV_DRONE), NT_DRONE, GT_DRONE, FL_DRONE},
};

const char *ba_preset_name(int i) { return i >= 0 && i < BA_NPRESETS ? PRESETS[i].name : ""; }

void ba_preset(ba_patch *pt, int idx)
{
    const preset_t *pr;
    int i;
    if (idx < 0 || idx >= BA_NPRESETS)
        return;
    pr = &PRESETS[idx];
    ba_patch_init(pt);
    for (i = 0; i < BA_NAMELEN; i++)
        pt->name[i] = 0;
    for (i = 0; i < BA_NAMELEN - 1 && pr->name[i]; i++)
        pt->name[i] = pr->name[i];
    pt->bpm = pr->bpm;
    for (i = 0; i < pr->npv; i++)
        pt->p[pr->pv[i].id] = pr->pv[i].v;
    for (i = 0; i < pr->nlv; i++)
        pt->lvl[pr->lv[i].m][pr->lv[i].dest] = pr->lv[i].v;
    if (!pr->notes) {                    /* ohne Sequenz: der Zufalls-Generator, immer derselbe Samen */
        ba_randomize(pt, 33, 0xBA55u + (uint32_t)idx);
        for (i = 16; i < BA_STEPS; i++)
            pt->seq[i] = pt->seq[i & 15];
        return;
    }
    for (i = 0; i < BA_STEPS; i++) {     /* 16 Steps, viermal (die Länge zeigt die ersten 16) */
        int s = i & 15;
        ba_step *st = &pt->seq[i];
        st->note = pr->notes[s] ? pr->notes[s] : 36;
        st->gate = !pr->notes[s] ? BA_REST : pr->gates && pr->gates[s] ? pr->gates[s] : 64;
        st->fl = pr->notes[s] && pr->fl ? pr->fl[s] : 0;
    }
}
