/*
 * Usefulity - Stereo utility audio effect
 *
 * Ableton Utility clone: channel select, stereo width, mono sum,
 * bass mono crossover, gain, balance/pan, phase invert, mute, DC filter.
 *
 * Signal flow:
 *   Input -> DC Filter -> Phase Invert -> Channel Select -> Width ->
 *   Mono -> Bass Mono -> Gain -> Balance -> Mute -> Output
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "audio_fx_api_v2.h"

#define SAMPLE_RATE 44100
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Channel modes */
enum {
    CH_STEREO = 0,
    CH_LEFT,
    CH_RIGHT,
    CH_SWAP
};

/* Biquad filter state */
typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} biquad_t;

/* One-pole smoother for parameter changes */
typedef struct {
    float current;
    float target;
    float coeff;
} smoother_t;

typedef struct usefulity_instance {
    char module_dir[512];

    /* Parameters */
    int channel_mode;
    float width;            /* 0.0-2.0 (0%=mono, 1.0=normal, 2.0=wide) */
    int mono;
    int bass_mono;
    int bass_freq;          /* 50-500 Hz */
    float gain_db;          /* -100 to +35 dB (-100 = -inf) */
    float pan;              /* -1.0 (left) to +1.0 (right) */
    int mute;
    int phase_l;
    int phase_r;
    int dc_filter;
    int bass_audition;

    /* DSP state */
    biquad_t lpf_l, lpf_r;     /* bass mono lowpass */
    biquad_t dc_l, dc_r;       /* DC offset highpass */
    int bass_freq_last;
    smoother_t gain_smooth;
    smoother_t pan_smooth;

} usefulity_instance_t;

static const host_api_v1_t *g_host = NULL;

static void util_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[USEFULITY] %s", msg);
        g_host->log(buf);
    }
}

/* --- Tiny JSON helpers --- */

static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    *out = (float)atof(p);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

/* --- DSP helpers --- */

static inline float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static float db_to_linear(float db) {
    if (db <= -100.0f) return 0.0f;
    return powf(10.0f, db / 20.0f);
}

static void smoother_init(smoother_t *s, float initial, float time_ms) {
    s->current = initial;
    s->target = initial;
    if (time_ms > 0.0f) {
        s->coeff = 1.0f - expf(-1.0f / (SAMPLE_RATE * time_ms / 1000.0f));
    } else {
        s->coeff = 1.0f;
    }
}

static inline float smoother_next(smoother_t *s) {
    s->current += s->coeff * (s->target - s->current);
    return s->current;
}

/* 2nd-order Butterworth lowpass */
static void biquad_lowpass(biquad_t *bq, float freq_hz) {
    float w0 = 2.0f * (float)M_PI * freq_hz / SAMPLE_RATE;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * 0.7071067811865476f); /* Q = 1/sqrt(2) */

    float a0 = 1.0f + alpha;
    bq->b0 = ((1.0f - cos_w0) / 2.0f) / a0;
    bq->b1 = (1.0f - cos_w0) / a0;
    bq->b2 = ((1.0f - cos_w0) / 2.0f) / a0;
    bq->a1 = (-2.0f * cos_w0) / a0;
    bq->a2 = (1.0f - alpha) / a0;
}

/* 2nd-order Butterworth highpass for DC removal (~5 Hz) */
static void biquad_dc_highpass(biquad_t *bq) {
    float freq_hz = 5.0f;
    float w0 = 2.0f * (float)M_PI * freq_hz / SAMPLE_RATE;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * 0.7071067811865476f);

    float a0 = 1.0f + alpha;
    bq->b0 = ((1.0f + cos_w0) / 2.0f) / a0;
    bq->b1 = (-(1.0f + cos_w0)) / a0;
    bq->b2 = ((1.0f + cos_w0) / 2.0f) / a0;
    bq->a1 = (-2.0f * cos_w0) / a0;
    bq->a2 = (1.0f - alpha) / a0;
}

static inline float biquad_process(biquad_t *bq, float in) {
    float out = bq->b0 * in + bq->b1 * bq->x1 + bq->b2 * bq->x2
              - bq->a1 * bq->y1 - bq->a2 * bq->y2;
    bq->x2 = bq->x1;
    bq->x1 = in;
    bq->y2 = bq->y1;
    bq->y1 = out;
    return out;
}

static void biquad_reset(biquad_t *bq) {
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0f;
}

/* --- Audio FX API v2 --- */

static void* v2_create_instance(const char *module_dir, const char *config_json) {
    util_log("Creating instance");

    usefulity_instance_t *inst = (usefulity_instance_t *)calloc(1, sizeof(usefulity_instance_t));
    if (!inst) return NULL;

    if (module_dir) {
        strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    }

    /* Defaults */
    inst->channel_mode = CH_STEREO;
    inst->width = 1.0f;
    inst->mono = 0;
    inst->bass_mono = 0;
    inst->bass_freq = 120;
    inst->gain_db = 0.0f;
    inst->pan = 0.0f;
    inst->mute = 0;
    inst->phase_l = 0;
    inst->phase_r = 0;
    inst->dc_filter = 0;
    inst->bass_audition = 0;

    /* Init filters */
    biquad_lowpass(&inst->lpf_l, 120.0f);
    biquad_lowpass(&inst->lpf_r, 120.0f);
    biquad_reset(&inst->lpf_l);
    biquad_reset(&inst->lpf_r);
    biquad_dc_highpass(&inst->dc_l);
    biquad_dc_highpass(&inst->dc_r);
    biquad_reset(&inst->dc_l);
    biquad_reset(&inst->dc_r);
    inst->bass_freq_last = 120;

    /* Init smoothers (5ms smoothing time) */
    smoother_init(&inst->gain_smooth, 1.0f, 5.0f);
    smoother_init(&inst->pan_smooth, 0.0f, 5.0f);

    util_log("Instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    usefulity_instance_t *inst = (usefulity_instance_t *)instance;
    if (!inst) return;
    util_log("Destroying instance");
    free(inst);
}

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    usefulity_instance_t *inst = (usefulity_instance_t *)instance;
    if (!inst) return;

    /* Update bass mono filter coefficients if frequency changed */
    if (inst->bass_freq != inst->bass_freq_last) {
        biquad_lowpass(&inst->lpf_l, (float)inst->bass_freq);
        biquad_lowpass(&inst->lpf_r, (float)inst->bass_freq);
        inst->bass_freq_last = inst->bass_freq;
    }

    /* Update smoother targets */
    inst->gain_smooth.target = db_to_linear(inst->gain_db);
    inst->pan_smooth.target = inst->pan;

    float phase_l_mul = inst->phase_l ? -1.0f : 1.0f;
    float phase_r_mul = inst->phase_r ? -1.0f : 1.0f;

    for (int i = 0; i < frames; i++) {
        float l = (float)audio_inout[i * 2];
        float r = (float)audio_inout[i * 2 + 1];

        /* 1. DC filter */
        if (inst->dc_filter) {
            l = biquad_process(&inst->dc_l, l);
            r = biquad_process(&inst->dc_r, r);
        }

        /* 2. Phase invert */
        l *= phase_l_mul;
        r *= phase_r_mul;

        /* 3. Channel select */
        switch (inst->channel_mode) {
        case CH_LEFT:
            r = l;
            break;
        case CH_RIGHT:
            l = r;
            break;
        case CH_SWAP: {
            float tmp = l;
            l = r;
            r = tmp;
            break;
        }
        case CH_STEREO:
        default:
            break;
        }

        /* 4. Stereo width (mid/side) */
        if (inst->channel_mode == CH_STEREO || inst->channel_mode == CH_SWAP) {
            float mid  = (l + r) * 0.5f;
            float side = (l - r) * 0.5f;
            l = mid + side * inst->width;
            r = mid - side * inst->width;
        }

        /* 5. Mono switch */
        if (inst->mono) {
            float m = (l + r) * 0.5f;
            l = m;
            r = m;
        }

        /* 6. Bass mono */
        if (inst->bass_mono || inst->bass_audition) {
            float low_l = biquad_process(&inst->lpf_l, l);
            float low_r = biquad_process(&inst->lpf_r, r);
            float high_l = l - low_l;
            float high_r = r - low_r;
            float bass_mono_sum = (low_l + low_r) * 0.5f;

            if (inst->bass_audition) {
                l = bass_mono_sum;
                r = bass_mono_sum;
            } else {
                l = bass_mono_sum + high_l;
                r = bass_mono_sum + high_r;
            }
        }

        /* 7. Gain (smoothed) */
        float gain = smoother_next(&inst->gain_smooth);
        l *= gain;
        r *= gain;

        /* 8. Balance/Pan (smoothed) */
        float p = smoother_next(&inst->pan_smooth);
        if (p < 0.0f) {
            r *= (1.0f + p);
        } else if (p > 0.0f) {
            l *= (1.0f - p);
        }

        /* 9. Mute */
        if (inst->mute) {
            l = 0.0f;
            r = 0.0f;
        }

        /* Clamp to int16 */
        if (l > 32767.0f) l = 32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r > 32767.0f) r = 32767.0f;
        if (r < -32768.0f) r = -32768.0f;

        audio_inout[i * 2] = (int16_t)l;
        audio_inout[i * 2 + 1] = (int16_t)r;
    }
}

/* --- Parameter parsing --- */

static int parse_channel_mode(const char *val) {
    if (strcmp(val, "Stereo") == 0) return CH_STEREO;
    if (strcmp(val, "Left") == 0)   return CH_LEFT;
    if (strcmp(val, "Right") == 0)  return CH_RIGHT;
    if (strcmp(val, "Swap") == 0)   return CH_SWAP;
    int idx = (int)(atof(val) * 3.0f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    return idx;
}

static int parse_onoff(const char *val) {
    if (strcmp(val, "On") == 0) return 1;
    if (strcmp(val, "Off") == 0) return 0;
    return (atof(val) > 0.5f) ? 1 : 0;
}

static int parse_phase(const char *val) {
    if (strcmp(val, "Invert") == 0) return 1;
    if (strcmp(val, "Normal") == 0) return 0;
    return (atof(val) > 0.5f) ? 1 : 0;
}

static const char *channel_mode_name(int mode) {
    static const char *names[] = { "Stereo", "Left", "Right", "Swap" };
    if (mode < 0 || mode > 3) return "Stereo";
    return names[mode];
}

static const char *onoff_name(int val) {
    return val ? "On" : "Off";
}

static const char *phase_name(int val) {
    return val ? "Invert" : "Normal";
}

/* --- set_param --- */

static void v2_set_param(void *instance, const char *key, const char *val) {
    usefulity_instance_t *inst = (usefulity_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "channel_mode") == 0) {
        inst->channel_mode = parse_channel_mode(val);
    } else if (strcmp(key, "width") == 0) {
        inst->width = clampf((float)atof(val), 0.0f, 2.0f);
    } else if (strcmp(key, "mono") == 0) {
        inst->mono = parse_onoff(val);
    } else if (strcmp(key, "bass_mono") == 0) {
        inst->bass_mono = parse_onoff(val);
    } else if (strcmp(key, "bass_freq") == 0) {
        inst->bass_freq = (int)clampf((float)atof(val), 50.0f, 500.0f);
    } else if (strcmp(key, "gain_db") == 0) {
        inst->gain_db = clampf((float)atof(val), -100.0f, 35.0f);
    } else if (strcmp(key, "pan") == 0) {
        inst->pan = clampf((float)atof(val), -1.0f, 1.0f);
    } else if (strcmp(key, "mute") == 0) {
        inst->mute = parse_onoff(val);
    } else if (strcmp(key, "phase_l") == 0) {
        inst->phase_l = parse_phase(val);
    } else if (strcmp(key, "phase_r") == 0) {
        inst->phase_r = parse_phase(val);
    } else if (strcmp(key, "dc_filter") == 0) {
        inst->dc_filter = parse_onoff(val);
    } else if (strcmp(key, "bass_audition") == 0) {
        inst->bass_audition = parse_onoff(val);
    } else if (strcmp(key, "state") == 0) {
        float fval;
        char sval[32];

        if (json_get_string(val, "channel_mode", sval, sizeof(sval)) == 0) {
            inst->channel_mode = parse_channel_mode(sval);
        } else if (json_get_number(val, "channel_mode", &fval) == 0) {
            inst->channel_mode = (int)clampf(fval, 0.0f, 3.0f);
        }
        if (json_get_number(val, "width", &fval) == 0) {
            inst->width = clampf(fval, 0.0f, 2.0f);
        }
        if (json_get_number(val, "mono", &fval) == 0) {
            inst->mono = fval > 0.5f ? 1 : 0;
        }
        if (json_get_number(val, "bass_mono", &fval) == 0) {
            inst->bass_mono = fval > 0.5f ? 1 : 0;
        }
        if (json_get_number(val, "bass_freq", &fval) == 0) {
            inst->bass_freq = (int)clampf(fval, 50.0f, 500.0f);
        }
        if (json_get_number(val, "gain_db", &fval) == 0) {
            inst->gain_db = clampf(fval, -100.0f, 35.0f);
        }
        if (json_get_number(val, "pan", &fval) == 0) {
            inst->pan = clampf(fval, -1.0f, 1.0f);
        }
        if (json_get_number(val, "mute", &fval) == 0) {
            inst->mute = fval > 0.5f ? 1 : 0;
        }
        if (json_get_number(val, "phase_l", &fval) == 0) {
            inst->phase_l = fval > 0.5f ? 1 : 0;
        }
        if (json_get_number(val, "phase_r", &fval) == 0) {
            inst->phase_r = fval > 0.5f ? 1 : 0;
        }
        if (json_get_number(val, "dc_filter", &fval) == 0) {
            inst->dc_filter = fval > 0.5f ? 1 : 0;
        }
        if (json_get_number(val, "bass_audition", &fval) == 0) {
            inst->bass_audition = fval > 0.5f ? 1 : 0;
        }
    }
}

/* --- get_param --- */

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    usefulity_instance_t *inst = (usefulity_instance_t *)instance;
    if (!inst) return -1;

    if (strcmp(key, "channel_mode") == 0) return snprintf(buf, buf_len, "%s", channel_mode_name(inst->channel_mode));
    if (strcmp(key, "width") == 0) return snprintf(buf, buf_len, "%.2f", inst->width);
    if (strcmp(key, "mono") == 0) return snprintf(buf, buf_len, "%s", onoff_name(inst->mono));
    if (strcmp(key, "bass_mono") == 0) return snprintf(buf, buf_len, "%s", onoff_name(inst->bass_mono));
    if (strcmp(key, "bass_freq") == 0) return snprintf(buf, buf_len, "%d", inst->bass_freq);
    if (strcmp(key, "gain_db") == 0) return snprintf(buf, buf_len, "%.1f", inst->gain_db);
    if (strcmp(key, "pan") == 0) return snprintf(buf, buf_len, "%.2f", inst->pan);
    if (strcmp(key, "mute") == 0) return snprintf(buf, buf_len, "%s", onoff_name(inst->mute));
    if (strcmp(key, "phase_l") == 0) return snprintf(buf, buf_len, "%s", phase_name(inst->phase_l));
    if (strcmp(key, "phase_r") == 0) return snprintf(buf, buf_len, "%s", phase_name(inst->phase_r));
    if (strcmp(key, "dc_filter") == 0) return snprintf(buf, buf_len, "%s", onoff_name(inst->dc_filter));
    if (strcmp(key, "bass_audition") == 0) return snprintf(buf, buf_len, "%s", onoff_name(inst->bass_audition));
    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "USEFULITY");

    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"channel_mode\":%d,\"width\":%.3f,\"mono\":%d,"
            "\"bass_mono\":%d,\"bass_freq\":%d,\"gain_db\":%.1f,"
            "\"pan\":%.3f,\"mute\":%d,\"phase_l\":%d,\"phase_r\":%d,"
            "\"dc_filter\":%d,\"bass_audition\":%d}",
            inst->channel_mode, inst->width, inst->mono,
            inst->bass_mono, inst->bass_freq, inst->gain_db,
            inst->pan, inst->mute, inst->phase_l, inst->phase_r,
            inst->dc_filter, inst->bass_audition);
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"channel_mode\",\"width\",\"mono\",\"bass_mono\",\"bass_freq\",\"gain_db\",\"pan\",\"mute\"],"
                    "\"params\":[\"channel_mode\",\"width\",\"mono\",\"bass_mono\",\"bass_freq\",\"gain_db\",\"pan\",\"mute\",\"phase_l\",\"phase_r\",\"dc_filter\",\"bass_audition\"]"
                "}"
            "}"
        "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    if (strcmp(key, "chain_params") == 0) {
        const char *params_json = "["
            "{\"key\":\"channel_mode\",\"name\":\"Channel\",\"type\":\"enum\",\"options\":[\"Stereo\",\"Left\",\"Right\",\"Swap\"],\"default\":\"Stereo\"},"
            "{\"key\":\"width\",\"name\":\"Width\",\"type\":\"float\",\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
            "{\"key\":\"mono\",\"name\":\"Mono\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":\"Off\"},"
            "{\"key\":\"bass_mono\",\"name\":\"Bass Mono\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":\"Off\"},"
            "{\"key\":\"bass_freq\",\"name\":\"Bass Freq\",\"type\":\"int\",\"min\":50,\"max\":500,\"default\":120,\"step\":1},"
            "{\"key\":\"gain_db\",\"name\":\"Gain\",\"type\":\"float\",\"min\":-100,\"max\":35,\"default\":0,\"step\":0.5},"
            "{\"key\":\"pan\",\"name\":\"Balance\",\"type\":\"float\",\"min\":-1,\"max\":1,\"default\":0,\"step\":0.01},"
            "{\"key\":\"mute\",\"name\":\"Mute\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":\"Off\"},"
            "{\"key\":\"phase_l\",\"name\":\"Phase L\",\"type\":\"enum\",\"options\":[\"Normal\",\"Invert\"],\"default\":\"Normal\"},"
            "{\"key\":\"phase_r\",\"name\":\"Phase R\",\"type\":\"enum\",\"options\":[\"Normal\",\"Invert\"],\"default\":\"Normal\"},"
            "{\"key\":\"dc_filter\",\"name\":\"DC Filter\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":\"Off\"},"
            "{\"key\":\"bass_audition\",\"name\":\"Bass Audition\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":\"Off\"}"
        "]";
        int len = strlen(params_json);
        if (len < buf_len) {
            strcpy(buf, params_json);
            return len;
        }
        return -1;
    }

    return -1;
}

/* --- API exports --- */

static audio_fx_api_v2_t g_fx_api_v2;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block = v2_process_block;
    g_fx_api_v2.set_param = v2_set_param;
    g_fx_api_v2.get_param = v2_get_param;

    util_log("USEFULITY v2 plugin initialized");

    return &g_fx_api_v2;
}
