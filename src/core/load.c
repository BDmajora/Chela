/* Model load progress. The bar and the ETA come from what the inference computer
 * reports: bytes of weights in memory, the bytes bound for the GPU and which stage
 * the server's log has reached. Between reports the estimate advances at the measured
 * rate, and a previous load of the same model fills in what has not been measured
 * yet. It never invents movement: with no report and no history the bar stays empty. */
#include "chela.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE_WEIGHT 0.4      /* of each new reading in the smoothed rate */
#define ETA_WEIGHT 0.6       /* of each new estimate in the smoothed ETA */
#define STALL_SECONDS 20.0   /* with no change for this long, the ETA is unknown */
#define START_SHARE 0.02     /* of the bar: the server starting up */
#define UPLOAD_SHARE 0.7     /* of what follows reading: copying weights to the GPU */
#define FINISH_LIMIT 0.99    /* only the server saying "loaded" completes the bar */
#define UPLOAD_MIB_PER_S 2048.0
#define SETUP_SECONDS 3.0    /* context buffers and the warmup run */

static double seconds(int64_t us) {
    return (double)us / 1e6;
}
static double tail_estimate(const ChLoadTrack *t) {
    return t->previous_tail > 0 ? t->previous_tail : SETUP_SECONDS + t->last.gpu_mib / UPLOAD_MIB_PER_S;
}
static double read_share(const ChLoadTrack *t) {
    if (t->previous_seconds > 0 && t->previous_tail > 0 && t->previous_tail < t->previous_seconds)
        return CH_CLAMP(1 - t->previous_tail / t->previous_seconds, 0.3, 0.95);
    return 0.8;
}
void ch_load_begin(ChLoadTrack *t, int64_t now, double previous_seconds, double previous_tail, double previous_mapped) {
    memset(t, 0, sizeof(*t));
    t->started_us = now;
    t->previous_seconds = previous_seconds > 0 && isfinite(previous_seconds) ? previous_seconds : 0;
    t->previous_tail = previous_tail > 0 && isfinite(previous_tail) ? previous_tail : 0;
    t->previous_mapped = previous_mapped > 0 && isfinite(previous_mapped) ? previous_mapped : 0;
    t->last.upload_percent = -1;
    t->eta = -1;
}
static double target_of(const ChLoadTrack *t) {
    double target = t->last.target_mib;
    /* The last load shows how much of the file a load really reads: lazily mapped shards
     * never become resident. It only ever lowers the target, and never below this load's
     * own progress. */
    if (t->previous_mapped > 0 && (target <= 0 || t->previous_mapped < target))
        target = CH_MAX(t->previous_mapped, t->last.mapped_mib);
    return target;
}
static double reading_remaining(const ChLoadTrack *t) {
    double target = target_of(t);
    if (target <= 0 || t->rate_mib <= 0)
        return -1;
    return CH_MAX(target - t->last.mapped_mib, 0) / t->rate_mib;
}
/* Seconds left as of the latest report, or -1 when nothing supports a figure. */
static double measured_eta(const ChLoadTrack *t, int64_t now) {
    double tail = tail_estimate(t), elapsed = seconds(now - t->started_us);
    double history = t->previous_seconds > 0 ? CH_MAX(t->previous_seconds - elapsed, 0) : -1;
    switch (t->last.phase) {
    case CH_LOAD_STARTING:
        return history;
    case CH_LOAD_READING: {
        double left = reading_remaining(t);
        return left >= 0 ? left + tail : history;
    }
    case CH_LOAD_UPLOADING: {
        double since = seconds(now - t->reading_end_us);
        int percent = t->last.upload_percent;
        if (percent > 0 && percent <= 100 && since > 0)
            return since * (100 - percent) / percent + SETUP_SECONDS;
        return CH_MAX(tail - since, 0);
    }
    default:
        return CH_MAX(tail - seconds(now - t->reading_end_us), 0);
    }
}
void ch_load_sample(ChLoadTrack *t, const ChLoadSample *sample, int64_t now) {
    if (now < t->sample_us)
        now = t->sample_us;
    ChLoadSample s = *sample;
    /* A report that went backwards is stale: it says nothing about the current stage. */
    if (t->measured && s.phase < t->last.phase)
        s.phase = t->last.phase;
    if (t->measured && s.mapped_mib < t->last.mapped_mib)
        s.mapped_mib = t->last.mapped_mib;
    if (s.phase != CH_LOAD_UPLOADING)
        s.upload_percent = -1;
    if (t->measured && s.upload_percent < t->last.upload_percent)
        s.upload_percent = t->last.upload_percent;
    double dt = t->measured ? seconds(now - t->sample_us) : 0;
    if (dt > 0 && s.phase == CH_LOAD_READING) {
        double rate = (s.mapped_mib - t->last.mapped_mib) / dt;
        t->rate_mib = t->rate_mib > 0 ? t->rate_mib + RATE_WEIGHT * (rate - t->rate_mib) : rate;
    }
    bool moved = !t->measured || s.phase != t->last.phase || s.mapped_mib > t->last.mapped_mib ||
                 s.upload_percent != t->last.upload_percent;
    if (moved)
        t->moved_us = now;
    if (s.phase > CH_LOAD_READING && !t->reading_end_us) {
        t->reading_end_us = now;
        t->mapped_peak = CH_MAX(t->last.mapped_mib, s.mapped_mib);
    }
    t->last = s;
    t->sample_us = now;
    t->measured = true;
    double eta = measured_eta(t, now);
    double before = t->eta >= 0 ? CH_MAX(t->eta - dt, 0) : -1;
    t->eta = eta >= 0 && before >= 0 ? ETA_WEIGHT * eta + (1 - ETA_WEIGHT) * before : eta;
}
static double measured_fraction(const ChLoadTrack *t, int64_t now, double dt) {
    double share = read_share(t), target = target_of(t);
    switch (t->last.phase) {
    case CH_LOAD_STARTING:
        return 0;
    case CH_LOAD_READING: {
        /* Between reports, reading continues at the measured rate. */
        double mapped = t->last.mapped_mib + CH_MAX(t->rate_mib, 0) * dt;
        double part = target > 0 ? CH_CLAMP(mapped / target, 0, 0.999) : 0;
        return START_SHARE + (share - START_SHARE) * part;
    }
    case CH_LOAD_UPLOADING: {
        double part;
        if (t->last.upload_percent >= 0)
            part = t->last.upload_percent / 100.0;
        else {
            double planned = CH_MAX(tail_estimate(t) - SETUP_SECONDS, 0.5);
            part = CH_MIN(seconds(now - t->reading_end_us) / planned, 0.95);
        }
        return share + (1 - share) * UPLOAD_SHARE * part;
    }
    case CH_LOAD_CONTEXT:
        return share + (1 - share) * (UPLOAD_SHARE + (1 - UPLOAD_SHARE) * 0.3);
    default:
        return share + (1 - share) * (UPLOAD_SHARE + (1 - UPLOAD_SHARE) * 0.7);
    }
}
ChLoadEstimate ch_load_estimate(ChLoadTrack *t, int64_t now) {
    if (now < t->sample_us)
        now = t->sample_us;
    double elapsed = seconds(now - t->started_us);
    ChLoadEstimate e = {.fraction = 0, .eta = -1, .rate_mib = 0, .measured = t->measured};
    if (t->measured) {
        double dt = seconds(now - t->sample_us);
        e.fraction = measured_fraction(t, now, dt);
        e.eta = t->eta >= 0 ? CH_MAX(t->eta - dt, 0) : -1;
        if (t->last.phase == CH_LOAD_READING)
            e.rate_mib = CH_MAX(t->rate_mib, 0);
        e.stalled = seconds(now - t->moved_us) >= STALL_SECONDS;
        if (e.stalled)
            e.eta = -1;
    } else if (t->previous_seconds > 0) {
        e.fraction = CH_MIN(elapsed / t->previous_seconds, 0.97);
        e.eta = CH_MAX(t->previous_seconds - elapsed, 0);
    }
    e.overdue = e.eta == 0;
    /* The bar only moves forward, and the server's report ends it. */
    t->shown = CH_MAX(t->shown, CH_CLAMP(e.fraction, 0, FINISH_LIMIT));
    e.fraction = t->shown;
    return e;
}
double ch_load_tail_seconds(const ChLoadTrack *t, int64_t now) {
    return t->reading_end_us && now > t->reading_end_us ? seconds(now - t->reading_end_us) : 0;
}
bool ch_load_sample_parse(json_object *load, ChLoadSample *sample) {
    static const struct {
        const char *name;
        ChLoadPhase phase;
    } phases[] = {{"starting", CH_LOAD_STARTING}, {"reading", CH_LOAD_READING}, {"uploading", CH_LOAD_UPLOADING},
                  {"context", CH_LOAD_CONTEXT},   {"warming", CH_LOAD_WARMING}};
    if (!load || !json_object_is_type(load, json_type_object))
        return false;
    const char *phase = ch_json_str(load, "phase");
    bool known = false;
    memset(sample, 0, sizeof(*sample));
    for (size_t i = 0; i < CH_N_ELEMENTS(phases); i++)
        if (!strcmp(phase, phases[i].name)) {
            sample->phase = phases[i].phase;
            known = true;
        }
    if (!known)
        return false;
    double *fields[] = {&sample->target_mib, &sample->mapped_mib, &sample->gpu_mib};
    const char *names[] = {"target_mib", "mapped_mib", "gpu_mib"};
    for (size_t i = 0; i < CH_N_ELEMENTS(fields); i++) {
        double value = json_object_get_double(ch_json_get(load, names[i]));
        *fields[i] = isfinite(value) && value > 0 ? value : 0;
    }
    json_object *percent = ch_json_get(load, "upload_percent");
    sample->upload_percent = percent ? CH_CLAMP(json_object_get_int(percent), 0, 100) : -1;
    return true;
}
char *ch_load_eta_text(double eta) {
    if (eta < 0 || !isfinite(eta))
        return NULL;
    long left = lround(ceil(eta));
    if (left < 1)
        return ch_strdup("Almost done");
    if (left < 60)
        return ch_strdup_printf("%lds left", left);
    if (left < 3600)
        return ch_strdup_printf("%ldm %02lds left", left / 60, left % 60);
    return ch_strdup_printf("%ldh %02ldm left", left / 3600, left / 60 % 60);
}
static char *size_text(double mib) {
    return mib >= 1024 ? ch_strdup_printf("%.1f GiB", mib / 1024) : ch_strdup_printf("%.0f MiB", mib);
}
char *ch_load_phase_text(const ChLoadTrack *t, const ChLoadEstimate *e) {
    const ChLoadSample *s = &t->last;
    if (!t->measured)
        return ch_strdup(t->previous_seconds > 0 ? "Estimating from the last load" : "Waiting for the server");
    switch (s->phase) {
    case CH_LOAD_STARTING:
        return ch_strdup("Starting the model server");
    case CH_LOAD_READING: {
        double target = target_of(t);
        if (target <= 0)
            return ch_strdup("Reading weights");
        char *done = size_text(CH_MIN(s->mapped_mib, target)), *total = size_text(target);
        char *text = e->rate_mib >= 1 ? ch_strdup_printf("Reading weights · %s of %s · %.0f MiB/s", done, total, e->rate_mib)
                                      : ch_strdup_printf("Reading weights · %s of %s", done, total);
        free(done);
        free(total);
        return text;
    }
    case CH_LOAD_UPLOADING: {
        if (s->gpu_mib <= 0)
            return ch_strdup("Placing weights");
        char *size = size_text(s->gpu_mib);
        char *text = s->upload_percent >= 0 ? ch_strdup_printf("Copying %s to the GPU · %d%%", size, s->upload_percent)
                                            : ch_strdup_printf("Copying %s to the GPU", size);
        free(size);
        return text;
    }
    case CH_LOAD_CONTEXT:
        return ch_strdup("Preparing the context");
    default:
        return ch_strdup("Warming up");
    }
}
