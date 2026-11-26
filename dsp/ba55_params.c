/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – die Parameterliste (FM-1 und VST lesen dieselbe) und die Umrechnungen. Teil von ba55_core.c */

static const char *const SW_ONOFF[2] = {"OFF", "ON"};
static const char *const FMODE_NAME[2] = {"LP", "BP"};
static const char *const LSYNC_NAME[2] = {"FREE", "BPM"};
static const char *const AOCT_NAME[3] = {"1", "2", "3"};
static const char *const AGATE_NAME[10] = {"10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"};

const char *const BA_WAVE_NAME[BA_NWAVES] = {"TRI", "SAW", "SQR"};
const char *const BA_SHAPE_NAME[BA_NSHAPES] = {"SINE", "TRI", "SAW", "RAMP", "SQUARE", "3 DEC SAW", "SAW+TRI",
                                               "3 RAMP", "S&H", "NOISE"};
const char *const BA_PMODE_NAME[BA_NPMODES] = {"FWD", "BWD", "RND", "PP", "PP2"};
const char *const BA_SCALE_NAME[BA_NSCALES] = {"CHROM", "MAJOR", "MINOR", "MAJ PEN", "MIN PEN", "WHOLE",
                                               "MAJ BLU", "MIN BLU"};
const uint16_t BA_SCALE_MASK[BA_NSCALES] = {
    0xFFF,                                              /* chromatisch */
    1 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 11,   /* Dur */
    1 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 10,   /* Moll */
    1 | 1 << 2 | 1 << 4 | 1 << 7 | 1 << 9,              /* Dur-Pentatonik */
    1 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 10,             /* Moll-Pentatonik */
    1 | 1 << 2 | 1 << 4 | 1 << 6 | 1 << 8 | 1 << 10,    /* Ganzton */
    1 | 1 << 2 | 1 << 3 | 1 << 4 | 1 << 7 | 1 << 9,     /* Dur-Blues */
    1 | 1 << 3 | 1 << 5 | 1 << 6 | 1 << 7 | 1 << 10,    /* Moll-Blues */
};
const char *const BA_NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char *const BA_RTGT_NAME[BA_NRTGT] = {"PATTERN", "NOTES", "GATES", "ACCENT", "SLIDE", "MOD", "PITCH ENV",
                                            "PROB", "STEP ON"};
const char *const BA_ADIR_NAME[BA_NADIR] = {"UP", "DOWN", "UP&DOWN", "RANDOM"};
const char *const BA_SYNC_NAME[BA_NSYNC] = {"8 BARS", "4 BARS", "2 BARS", "1 BAR", "1/2", "1/4", "1/4T", "1/8",
                                            "1/8T", "1/16", "1/16T", "1/32"};
const float BA_SYNC_BEATS[BA_NSYNC] = {32.0f, 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.6666667f, 0.5f, 0.3333333f, 0.25f,
                                       0.1666667f, 0.125f};
const char *const BA_PROB_NAME[BA_NPROB] = {"100%", "75%", "50%", "25%", "1/2", "1/3", "1/4"};

const ba_param_info BA_PARAMS[BA_NP] = {
    {"Wave", "WAVE", 1, MK_LIST, BA_NWAVES, BA_WAVE_NAME},
    {"Sub", "SUB", 40, MK_UNI, 0, 0},
    {"Noise", "NOISE", 0, MK_UNI, 0, 0},
    {"Detune", "DETUNE", 0, MK_UNI, 0, 0},
    {"Overdrive", "DRIVE", 30, MK_UNI, 0, 0},
    {"FM Level", "FM LVL", 0, MK_UNI, 0, 0},
    {"Cutoff", "CUTOFF", 50, MK_UNI, 0, 0},
    {"Resonance", "RESO", 70, MK_UNI, 0, 0},
    {"VCF Decay", "DECAY", 60, MK_UNI, 0, 0},
    {"VCF Env Depth", "DEPTH", 90, MK_UNI, 0, 0},
    {"VCF Mode", "MODE", 0, MK_SW, 2, FMODE_NAME},
    {"VCA Release", "RELEASE", 30, MK_UNI, 0, 0},
    {"LFO Rate", "RATE", 60, MK_UNI, 0, 0},
    {"LFO Level", "LFO LVL", 0, MK_UNI, 0, 0},
    {"LFO Shape", "SHAPE", LS_TRI, MK_LIST, BA_NSHAPES, BA_SHAPE_NAME},
    {"LFO Sync", "SYNC", 0, MK_SW, 2, LSYNC_NAME},
    {"LFO Reset", "RESET", 0, MK_SW, 2, SW_ONOFF},
    {"Glide", "GLIDE", 0, MK_UNI, 0, 0},
    {"Last Step", "LAST", 15, MK_LIST, BA_STEPS, 0},
    {"Play Mode", "PLAY", PM_FWD, MK_LIST, BA_NPMODES, BA_PMODE_NAME},
    {"Shuffle", "SHUFFLE", 0, MK_UNI, 0, 0},
    {"Scale", "SCALE", 4, MK_LIST, BA_NSCALES, BA_SCALE_NAME},
    {"Root", "ROOT", 0, MK_LIST, 12, BA_NOTE_NAME},
    {"Random Target", "TARGET", RT_PATTERN, MK_LIST, BA_NRTGT, BA_RTGT_NAME},
    {"Random Amount", "AMOUNT", 40, MK_UNI, 0, 0},
    {"Random Low", "LOW", 0, MK_UNI, 0, 0},
    {"Random High", "HIGH", 64, MK_UNI, 0, 0},
    {"Arp Direction", "DIR", AD_UP, MK_LIST, BA_NADIR, BA_ADIR_NAME},
    {"Arp Octaves", "OCT", 0, MK_LIST, 3, AOCT_NAME},
    {"Arp Gate", "GATE", 5, MK_LIST, 10, AGATE_NAME},
    {"Arp Latch", "LATCH", 0, MK_SW, 2, SW_ONOFF},
};

/* ------------------------------------------------------------- Umrechnungen --- */
static float nrm(int v) { return (float)v * (1.0f / 127.0f); }

float ba_cutoff_hz(int v) { return 20.0f * ba_exp2(nrm(v) * 10.0f); }                 /* 20 Hz .. 20 kHz */
float ba_fdecay_ms(int v) { return 10.0f * ba_exp2(nrm(v) * 8.2f); }                   /* 10 ms .. 3 s */
float ba_vrel_ms(int v) { return v >= 127 ? -1.0f : 5.0f * ba_exp2(nrm(v) * 9.6f); }   /* 5 ms .. 3.9 s, ganz: offen */
float ba_lfo_hz(int v) { return 0.05f * ba_exp2(nrm(v) * 14.0f); }                     /* 0.05 Hz .. 800 Hz (Audio) */
float ba_glide_ms(int v) { return v ? 5.0f * ba_exp2(nrm(v) * 8.6f) : 0.0f; }          /* bis 2 s */
float ba_pe_decay_ms(int d) { return 4.0f * ba_exp2((float)(d < 1 ? 1 : d > 9 ? 9 : d) * 0.75f - 0.75f); } /* 4..256 ms */

int ba_list_index(int id, int v)
{
    int n = BA_PARAMS[id].n, i;
    if (n < 2)
        return 0;
    i = (v * (n - 1) + 63) / 127;
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}
int ba_list_value(int id, int idx)
{
    int n = BA_PARAMS[id].n;
    if (n < 2)
        return 0;
    idx = idx < 0 ? 0 : idx >= n ? n - 1 : idx;
    return (idx * 127 + (n - 1) / 2) / (n - 1);
}
int ba_wave_index(int v) { return ba_list_index(P_WAVE, v); }

int ba_scale_snap(const ba_patch *pt, int note)
{
    uint16_t m = BA_SCALE_MASK[ba_list_index(P_SCALE, pt->p[P_SCALE])];
    int root = ba_list_index(P_ROOT, pt->p[P_ROOT]), d;
    for (d = 0; d < 12; d++) {           /* abwärts zuerst, dann aufwärts */
        int a = note - d, b = note + d;
        if (m >> (((a - root) % 12 + 12) % 12) & 1u)
            return a;
        if (m >> (((b - root) % 12 + 12) % 12) & 1u)
            return b;
    }
    return note;
}

void ba_patch_init(ba_patch *pt)
{
    int i;
    char *z = (char *)pt;
    for (i = 0; i < (int)sizeof *pt; i++)
        z[i] = 0;
    for (i = 0; i < BA_NP; i++)           /* bei Listen und Schaltern steht im Standardwert der Eintrag */
        pt->p[i] = BA_PARAMS[i].kind == MK_LIST || BA_PARAMS[i].kind == MK_SW
                       ? (uint8_t)(BA_PARAMS[i].kind == MK_SW ? (BA_PARAMS[i].def ? 127 : 0) : ba_list_value(i, BA_PARAMS[i].def))
                       : BA_PARAMS[i].def;
    for (i = 0; i < BA_STEPS; i++) {
        pt->seq[i].note = 36;
        pt->seq[i].gate = 5;
        pt->seq[i].mod = 64;
        pt->seq[i].pe = 5;               /* Decay 5, Depth 0 (aus) */
    }
    pt->bpm = 120;
    pt->name[0] = 'I';
    pt->name[1] = 'N';
    pt->name[2] = 'I';
    pt->name[3] = 'T';
}
