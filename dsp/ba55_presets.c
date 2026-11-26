/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – die 16 Werks-Presets: Klang und Pattern. Teil von ba55_core.c
 *
 * Spickzettel (alle Werte 0..127):
 *   WAVE      0 TRI, 64 SAW, 127 SQR
 *   CUTOFF    20 Hz * 2^(v/12.7)       30 = 100 Hz, 40 = 180 Hz, 50 = 300 Hz, 60 = 530 Hz, 74 = 1 kHz
 *   DECAY     10 ms * 2^(v/15.5)       40 = 60 ms, 55 = 120 ms, 70 = 230 ms, 90 = 560 ms
 *   RELEASE   5 ms * 2^(v/13.2)        20 = 14 ms, 40 = 40 ms, 60 = 120 ms, 127 = offen (Drone)
 *   LFO RATE  0.05 Hz * 2^(v/9.1)      60 = 4.7 Hz, 80 = 21 Hz, 100 = 100 Hz, 115 = 300 Hz
 * Steps: notes 0 = aus; gates 1..10 (0: 5 = 50 %), 10 = verbunden; fl A = ACCENT, S = SLIDE;
 *        pe: Depth * 16 + Decay (0: aus); mods: 64 = nichts; probs: 0 = 100 % .. 6 = 1/4 */

typedef struct { uint8_t id, v; } pv_t;
typedef struct {
    const char *name;
    uint16_t bpm;
    const pv_t *pv;
    uint8_t npv;
    const uint8_t *notes, *gates, *fl, *pe, *mods, *probs;   /* je 16 Steps */
} preset_t;

#define N(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))
#define A BA_ACC
#define S BA_SLD
#define AS (BA_ACC | BA_SLD)
#define PE(d, k) (uint8_t)((d) << 4 | (k))

/* 1 BA-55: die Säge, Resonanz hoch, Accents und Slides */
static const pv_t PV_BA[] = {{P_WAVE, 64}, {P_SUB, 30}, {P_CUTOFF, 44}, {P_RESON, 98}, {P_FDECAY, 58},
                             {P_FDEPTH, 100}, {P_DRIVE, 40}, {P_VREL, 30}};
static const uint8_t NT_BA[16] = {33, 33, 45, 33, 0, 36, 33, 43, 33, 45, 0, 33, 48, 45, 36, 40};
static const uint8_t FL_BA[16] = {A, 0, S, 0, 0, A, 0, AS, 0, A, 0, 0, AS, 0, A, 0};

/* 2 ACIDLIN: die Resonanz fast am Pfeifen, viele Slides, kurze Gates */
static const pv_t PV_ACID[] = {{P_WAVE, 64}, {P_CUTOFF, 36}, {P_RESON, 114}, {P_FDECAY, 52}, {P_FDEPTH, 110},
                               {P_DRIVE, 55}, {P_VREL, 24}, {P_SHUFFLE, 20}};
static const uint8_t NT_ACID[16] = {36, 48, 36, 39, 36, 0, 51, 36, 46, 36, 48, 0, 36, 43, 36, 41};
static const uint8_t GT_ACID[16] = {4, 3, 4, 3, 4, 0, 5, 3, 4, 3, 6, 0, 3, 5, 3, 4};
static const uint8_t FL_ACID[16] = {A, S, 0, A, S, 0, AS, 0, A, S, A, 0, 0, AS, 0, A};

/* 3 SQRACID: das Rechteck, OVERDRIVE weit auf */
static const pv_t PV_SQR[] = {{P_WAVE, 127}, {P_SUB, 20}, {P_CUTOFF, 42}, {P_RESON, 100}, {P_FDECAY, 62},
                              {P_FDEPTH, 96}, {P_DRIVE, 92}, {P_VREL, 30}};
static const uint8_t NT_SQR[16] = {31, 31, 0, 43, 31, 34, 0, 31, 38, 31, 0, 43, 31, 46, 43, 38};
static const uint8_t FL_SQR[16] = {A, 0, 0, AS, 0, A, 0, 0, S, 0, 0, A, 0, AS, 0, 0};

/* 4 DRONE: RELEASE ganz rechts, der VCA bleibt offen; DETUNE und ein langsamer LFO auf dem Filter */
static const pv_t PV_DRONE[] = {{P_WAVE, 64}, {P_SUB, 90}, {P_DETUNE, 60}, {P_CUTOFF, 52}, {P_RESON, 92},
                                {P_FDECAY, 100}, {P_FDEPTH, 40}, {P_VREL, 127}, {P_LRATE, 24}, {P_LFOLVL, 60},
                                {P_LSHAPE, 0}, {P_DRIVE, 50},
    {P_RVMIX, 60}, {P_RVSIZE, 120}, {P_RVDECAY, 110}, {P_RVTONE, 50}};
static const uint8_t NT_DRONE[16] = {28, 0, 0, 0, 0, 0, 0, 0, 31, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t GT_DRONE[16] = {10, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t FL_DRONE[16] = {S, 0, 0, 0, 0, 0, 0, 0, S, 0, 0, 0, 0, 0, 0, 0};

/* 5 BPZAP: der Bandpass, eine Pitch-Hüllkurve auf jedem Accent */
static const pv_t PV_ZAP[] = {{P_WAVE, 64}, {P_FMODE, 127}, {P_CUTOFF, 58}, {P_RESON, 104}, {P_FDECAY, 48},
                              {P_FDEPTH, 90}, {P_DRIVE, 50}, {P_VREL, 20},
    {P_FBMIX, 100}, {P_FBSHAPE, 81}, {P_FBRESO, 108}, {P_FBMOD, 85}, {P_FBRATE, 102}, {P_FBDEPTH, 90}, {P_FB1 + 0, 40}, {P_FB1 + 1, 70}, {P_FB1 + 2, 90}, {P_FB1 + 3, 110}, {P_FB1 + 4, 124}, {P_FB1 + 5, 124}, {P_FB1 + 6, 60}, {P_FB1 + 7, 30}, {P_FB1 + 8, 20}, {P_FB1 + 9, 10}, {P_RVMIX, 25}};
static const uint8_t NT_ZAP[16] = {40, 0, 40, 52, 0, 40, 0, 47, 40, 0, 52, 40, 0, 45, 40, 0};
static const uint8_t FL_ZAP[16] = {A, 0, 0, A, 0, 0, 0, A, 0, 0, A, 0, 0, A, 0, 0};
static const uint8_t PE_ZAP[16] = {PE(6, 3), 0, 0, PE(8, 2), 0, 0, 0, PE(5, 4), 0, 0, PE(9, 2), 0, 0, PE(7, 3), 0, 0};

/* 6 DIRTY: OVERDRIVE ganz, Säge und Sub */
static const pv_t PV_DIRTY[] = {{P_WAVE, 64}, {P_SUB, 70}, {P_CUTOFF, 46}, {P_RESON, 88}, {P_FDECAY, 64},
                                {P_FDEPTH, 100}, {P_DRIVE, 127}, {P_VREL, 34}};
static const uint8_t NT_DIRTY[16] = {29, 29, 41, 29, 0, 29, 32, 29, 0, 29, 41, 39, 29, 0, 36, 29};
static const uint8_t FL_DIRTY[16] = {A, 0, S, 0, 0, A, 0, 0, 0, A, AS, 0, 0, 0, A, 0};

/* 7 ROLLER: Techno-Offbeat, Rechteck und Sub, kurz */
static const pv_t PV_ROLL[] = {{P_WAVE, 127}, {P_SUB, 90}, {P_CUTOFF, 50}, {P_RESON, 40}, {P_FDECAY, 46},
                               {P_FDEPTH, 80}, {P_DRIVE, 50}, {P_VREL, 18}};
static const uint8_t NT_ROLL[16] = {0, 0, 33, 0, 0, 0, 33, 0, 0, 0, 33, 0, 0, 0, 36, 0};
static const uint8_t GT_ROLL[16] = {0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 3, 0};
static const uint8_t FL_ROLL[16] = {0, 0, A, 0, 0, 0, 0, 0, 0, 0, A, 0, 0, 0, 0, 0};

/* 8 SWARM: DETUNE weit auf, der Schwarm */
static const pv_t PV_SWARM[] = {{P_WAVE, 64}, {P_DETUNE, 70}, {P_SUB, 60}, {P_CUTOFF, 54}, {P_RESON, 70},
                                {P_FDECAY, 80}, {P_FDEPTH, 84}, {P_VREL, 64},
    {P_RVMIX, 40}, {P_RVSIZE, 90}, {P_RVDECAY, 80}};
static const uint8_t NT_SWARM[16] = {33, 0, 0, 33, 0, 0, 40, 0, 38, 0, 0, 36, 0, 0, 31, 0};
static const uint8_t GT_SWARM[16] = {10, 0, 0, 8, 0, 0, 10, 0, 10, 0, 0, 8, 0, 0, 7, 0};
static const uint8_t FL_SWARM[16] = {0, 0, 0, S, 0, 0, 0, 0, 0, 0, 0, S, 0, 0, 0, 0};

/* 9 PSYROLL: drei Sechzehntel nach jedem Kick */
static const pv_t PV_PSY[] = {{P_WAVE, 64}, {P_SUB, 30}, {P_CUTOFF, 50}, {P_RESON, 60}, {P_FDECAY, 44},
                              {P_FDEPTH, 92}, {P_DRIVE, 55}, {P_VREL, 16}};
static const uint8_t NT_PSY[16] = {0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 31, 28};
static const uint8_t GT_PSY[16] = {0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4};
static const uint8_t FL_PSY[16] = {0, A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0, A, 0, 0};

/* 10 BOOMKCK: das Dreieck, eine tiefe Note und eine große Pitch-Hüllkurve: eine Bassdrum */
static const pv_t PV_KICK[] = {{P_WAVE, 0}, {P_SUB, 0}, {P_CUTOFF, 74}, {P_RESON, 10}, {P_FDECAY, 40},
                               {P_FDEPTH, 30}, {P_DRIVE, 70}, {P_VREL, 56}};
static const uint8_t NT_KICK[16] = {24, 0, 0, 0, 24, 0, 0, 0, 24, 0, 0, 0, 24, 0, 0, 24};
static const uint8_t GT_KICK[16] = {3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 2};
static const uint8_t FL_KICK[16] = {A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0};
static const uint8_t PE_KICK[16] = {PE(9, 4), 0, 0, 0, PE(9, 4), 0, 0, 0, PE(9, 4), 0, 0, 0, PE(9, 4), 0, 0, PE(7, 3)};

/* 11 NOIZHAT: das Dreieck tief, NOISE ganz, der Bandpass hoch: Hi-Hats (ein Trick aus dem Handbuch) */
static const pv_t PV_HAT[] = {{P_WAVE, 0}, {P_SUB, 0}, {P_NOISE, 127}, {P_FMODE, 127}, {P_CUTOFF, 112},
                              {P_RESON, 70}, {P_FDECAY, 30}, {P_FDEPTH, 20}, {P_VREL, 10}};
static const uint8_t NT_HAT[16] = {0, 0, 12, 0, 0, 0, 12, 12, 0, 0, 12, 0, 0, 0, 12, 12};
static const uint8_t GT_HAT[16] = {0, 0, 2, 0, 0, 0, 2, 1, 0, 0, 2, 0, 0, 0, 3, 1};
static const uint8_t FL_HAT[16] = {0, 0, A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0, A, 0};

/* 12 FMGRIT: der LFO im Audiobereich auf der Tonhöhe (FM LVL), Rechteck, OVERDRIVE */
static const pv_t PV_FM[] = {{P_WAVE, 127}, {P_SUB, 50}, {P_CUTOFF, 52}, {P_RESON, 80}, {P_FDECAY, 60},
                             {P_FDEPTH, 96}, {P_DRIVE, 90}, {P_VREL, 28}, {P_FMLVL, 46}, {P_LRATE, 104},
                             {P_LSHAPE, 14},
    {P_FBMIX, 90}, {P_FBSHAPE, 58}, {P_FBRESO, 100}, {P_FBDRIVE, 60}, {P_FBMOD, 42}, {P_FBRATE, 40}, {P_FBDEPTH, 70}, {P_FB1 + 0, 120}, {P_FB1 + 1, 0}, {P_FB1 + 2, 120}, {P_FB1 + 3, 0}, {P_FB1 + 4, 120}, {P_FB1 + 5, 0}, {P_FB1 + 6, 120}, {P_FB1 + 7, 0}, {P_FB1 + 8, 120}, {P_FB1 + 9, 0}};
static const uint8_t NT_FM[16] = {36, 0, 36, 36, 0, 39, 36, 0, 36, 0, 43, 36, 0, 41, 36, 34};
static const uint8_t FL_FM[16] = {A, 0, 0, S, 0, A, 0, 0, A, 0, AS, 0, 0, A, 0, 0};

/* 13 WOBBLER: der LFO im Tempo (1/8) auf dem Cutoff, lange Noten */
static const pv_t PV_WOB[] = {{P_WAVE, 64}, {P_SUB, 100}, {P_CUTOFF, 42}, {P_RESON, 74}, {P_FDECAY, 70},
                              {P_FDEPTH, 50}, {P_DRIVE, 80}, {P_VREL, 40}, {P_LSYNC, 127}, {P_LRATE, 81},
                              {P_LFOLVL, 70}, {P_LRESET, 127}};
static const uint8_t NT_WOB[16] = {26, 0, 0, 0, 26, 0, 0, 0, 29, 0, 0, 0, 24, 0, 0, 0};
static const uint8_t GT_WOB[16] = {10, 0, 0, 0, 10, 0, 0, 0, 10, 0, 0, 0, 10, 0, 0, 0};
static const uint8_t FL_WOB[16] = {S, 0, 0, 0, 0, 0, 0, 0, S, 0, 0, 0, 0, 0, 0, 0};

/* 14 MODTRAK: die Mod-Spur öffnet und schließt das Filter Step für Step */
static const pv_t PV_MOD[] = {{P_WAVE, 64}, {P_SUB, 40}, {P_CUTOFF, 46}, {P_RESON, 96}, {P_FDECAY, 54},
                              {P_FDEPTH, 80}, {P_DRIVE, 60}, {P_VREL, 24}};
static const uint8_t NT_MOD[16] = {36, 36, 48, 36, 36, 46, 36, 36, 36, 36, 48, 36, 51, 36, 46, 43};
static const uint8_t FL_MOD[16] = {A, 0, 0, 0, A, S, 0, 0, A, 0, 0, 0, AS, 0, 0, 0};
static const uint8_t MD_MOD[16] = {40, 48, 56, 64, 72, 80, 88, 96, 104, 96, 88, 80, 72, 64, 56, 48};

/* 15 RANDOM: RND PATT hat das Pattern geschrieben (REC auf der Seite RND PATT macht ein neues) */
static const pv_t PV_RND[] = {{P_WAVE, 64}, {P_CUTOFF, 42}, {P_RESON, 104}, {P_FDECAY, 56}, {P_FDEPTH, 104},
                              {P_DRIVE, 60}, {P_VREL, 26}, {P_RHI, 70}, {P_RAMT, 50}};

/* 16 PROBPAT: Wahrscheinlichkeiten pro Step, PLAY MODE PP: das Pattern spielt nie ganz gleich */
static const pv_t PV_PROB[] = {{P_WAVE, 127}, {P_SUB, 50}, {P_CUTOFF, 48}, {P_RESON, 90}, {P_FDECAY, 58},
                               {P_FDEPTH, 96}, {P_DRIVE, 64}, {P_VREL, 24}, {P_PMODE, 95}};
static const uint8_t NT_PROB[16] = {33, 45, 33, 40, 33, 43, 45, 33, 36, 33, 48, 33, 40, 33, 45, 38};
static const uint8_t FL_PROB[16] = {A, 0, S, 0, A, 0, 0, S, A, 0, 0, 0, AS, 0, 0, 0};
static const uint8_t PR_PROB[16] = {0, 2, 0, 1, 0, 3, 4, 0, 0, 2, 5, 0, 0, 1, 6, 2};

static const preset_t PRESETS[BA_NPRESETS] = {
    {"BA-55", 126, PV_BA, N(PV_BA), NT_BA, 0, FL_BA, 0, 0, 0},
    {"ACIDLIN", 130, PV_ACID, N(PV_ACID), NT_ACID, GT_ACID, FL_ACID, 0, 0, 0},
    {"SQRACID", 128, PV_SQR, N(PV_SQR), NT_SQR, 0, FL_SQR, 0, 0, 0},
    {"DRONE", 90, PV_DRONE, N(PV_DRONE), NT_DRONE, GT_DRONE, FL_DRONE, 0, 0, 0},
    {"BPZAP", 128, PV_ZAP, N(PV_ZAP), NT_ZAP, 0, FL_ZAP, PE_ZAP, 0, 0},
    {"DIRTY", 132, PV_DIRTY, N(PV_DIRTY), NT_DIRTY, 0, FL_DIRTY, 0, 0, 0},
    {"ROLLER", 130, PV_ROLL, N(PV_ROLL), NT_ROLL, GT_ROLL, FL_ROLL, 0, 0, 0},
    {"SWARM", 112, PV_SWARM, N(PV_SWARM), NT_SWARM, GT_SWARM, FL_SWARM, 0, 0, 0},
    {"PSYROLL", 142, PV_PSY, N(PV_PSY), NT_PSY, GT_PSY, FL_PSY, 0, 0, 0},
    {"BOOMKCK", 128, PV_KICK, N(PV_KICK), NT_KICK, GT_KICK, FL_KICK, PE_KICK, 0, 0},
    {"NOIZHAT", 128, PV_HAT, N(PV_HAT), NT_HAT, GT_HAT, FL_HAT, 0, 0, 0},
    {"FMGRIT", 126, PV_FM, N(PV_FM), NT_FM, 0, FL_FM, 0, 0, 0},
    {"WOBBLER", 140, PV_WOB, N(PV_WOB), NT_WOB, GT_WOB, FL_WOB, 0, 0, 0},
    {"MODTRAK", 128, PV_MOD, N(PV_MOD), NT_MOD, 0, FL_MOD, 0, MD_MOD, 0},
    {"RANDOM", 128, PV_RND, N(PV_RND), 0, 0, 0, 0, 0, 0},
    {"PROBPAT", 124, PV_PROB, N(PV_PROB), NT_PROB, 0, FL_PROB, 0, 0, PR_PROB},
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
    if (!pr->notes) {                    /* ohne Pattern: RND PATT, immer derselbe Samen; Grundton A */
        pt->p[P_ROOT] = (uint8_t)ba_list_value(P_ROOT, 9);
        ba_randomize(pt, 0xBA55u + (uint32_t)idx);
        for (i = 16; i < BA_STEPS; i++)
            pt->seq[i] = pt->seq[i & 15];
        return;
    }
    for (i = 0; i < BA_STEPS; i++) {     /* 16 Steps, viermal (LAST zeigt die ersten 16) */
        int s = i & 15;
        ba_step *st = &pt->seq[i];
        st->note = pr->notes[s] ? pr->notes[s] : 36;
        st->gate = !pr->notes[s] ? 0 : pr->gates && pr->gates[s] ? pr->gates[s] : 5;
        st->fl = pr->notes[s] && pr->fl ? pr->fl[s] : 0;
        st->pe = pr->pe && pr->pe[s] ? pr->pe[s] : 5;
        st->mod = pr->mods ? pr->mods[s] : 64;
        st->prob = pr->probs ? pr->probs[s] : 0;
    }
}
