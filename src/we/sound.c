/* Scene sounds: decoded in-process (Ogg Vorbis, MP3, WAV, FLAC), mixed in a
 * thread and played through pw-cat / pacat / aplay. Auto-mute watches PipeWire
 * for other applications playing audio. */
#include "internal.h"

#define DR_MP3_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "../third_party/dr_flac.h"
#include "../third_party/dr_mp3.h"
#include "../third_party/dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define RATE 44100
#define CHUNK 1024

typedef struct {
    int      id;
    short   *pcm;      /* stereo interleaved */
    size_t   frames, pos;
    bool     loop, paused, done;
    float    volume;
} Voice;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       thread;
static bool            running;
static VEC(Voice)      voices;
static int             next_id = 1;
static pid_t           out_pid = -1;
static int             out_fd = -1;
static float           master = 15 / 128.0f;
static bool            muted, auto_muted, all_paused;

/* ------------------------------------------------------------------ decoding */
static short *to_stereo44(const short *in, size_t frames, int ch, int rate, size_t *out_frames)
{
    if (ch < 1) ch = 1;
    size_t of = rate == RATE ? frames : (size_t)((double)frames * RATE / rate);
    short *out = malloc(sizeof(short) * 2 * (of ? of : 1));
    for (size_t i = 0; i < of; i++) {
        double src = rate == RATE ? i : (double)i * rate / RATE;
        size_t a = (size_t)src;
        double f = src - a;
        size_t b = a + 1 < frames ? a + 1 : a;
        for (int c = 0; c < 2; c++) {
            int sc = ch == 1 ? 0 : c;
            double v = in[a * ch + sc] * (1 - f) + in[b * ch + sc] * f;
            out[i * 2 + c] = (short)v;
        }
    }
    *out_frames = of;
    return out;
}

static short *decode(const unsigned char *d, size_t len, size_t *frames)
{
    short *raw = NULL, *out = NULL;
    int ch = 0, rate = 0;
    size_t n = 0;
    if (len >= 4 && !memcmp(d, "OggS", 4)) {
        int r = stb_vorbis_decode_memory(d, (int)len, &ch, &rate, &raw);
        if (r > 0) n = r;
    } else if (len >= 4 && !memcmp(d, "RIFF", 4)) {
        unsigned int c, sr;
        drwav_uint64 fc;
        raw = drwav_open_memory_and_read_pcm_frames_s16(d, len, &c, &sr, &fc, NULL);
        ch = c;
        rate = sr;
        n = fc;
    } else if (len >= 4 && !memcmp(d, "fLaC", 4)) {
        unsigned int c, sr;
        drflac_uint64 fc;
        raw = drflac_open_memory_and_read_pcm_frames_s16(d, len, &c, &sr, &fc, NULL);
        ch = c;
        rate = sr;
        n = fc;
    } else {
        drmp3_config cfg3;
        drmp3_uint64 fc;
        raw = drmp3_open_memory_and_read_pcm_frames_s16(d, len, &cfg3, &fc, NULL);
        ch = cfg3.channels;
        rate = cfg3.sampleRate;
        n = fc;
    }
    if (!raw || !n || rate <= 0) {
        free(raw);
        return NULL;
    }
    out = to_stereo44(raw, n, ch, rate, frames);
    free(raw);
    return out;
}

/* ------------------------------------------------------------------ output process */
static bool have(const char *p) { return in_path(p); }

static bool spawn_output(void)
{
    char *argv[16];
    if (have("pw-cat")) {
        char *a[] = { "pw-cat", "--playback", "-a", "--format", "s16", "--rate", "44100", "--channels", "2",
                      "--latency", "60ms", "-P", "{ application.name = wallengine media.role = Music }", "-", NULL };
        memcpy(argv, a, sizeof a);
    } else if (have("pacat")) {
        char *a[] = { "pacat", "--raw", "--format=s16le", "--rate=44100", "--channels=2", "--latency-msec=60",
                      "--client-name=wallengine", NULL };
        memcpy(argv, a, sizeof a);
    } else if (have("aplay")) {
        char *a[] = { "aplay", "-q", "-f", "S16_LE", "-r", "44100", "-c", "2", NULL };
        memcpy(argv, a, sizeof a);
    } else {
        we_err("scene sounds need pw-cat, pacat or aplay");
        return false;
    }
    int p[2];
    if (pipe(p) < 0) return false;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(p[0], 0);
        dup2(null, 1);
        if (!cfg.verbose) dup2(null, 2);
        close(p[1]);
        setpgid(0, 0);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[0]);
    if (pid < 0) {
        close(p[1]);
        return false;
    }
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    out_pid = pid;
    out_fd = p[1];
    return true;
}

static void stop_output(void)
{
    if (out_fd >= 0) close(out_fd);
    out_fd = -1;
    if (out_pid > 0) {
        kill(out_pid, SIGTERM);
        waitpid(out_pid, NULL, 0);
    }
    out_pid = -1;
}

static void *mixer(void *arg)
{
    (void)arg;
    short buf[CHUNK * 2];
    int mix[CHUNK * 2];
    while (running) {
        pthread_mutex_lock(&mu);
        bool any = false;
        memset(mix, 0, sizeof mix);
        float gain = (muted || auto_muted) ? 0 : master;
        for (int i = 0; i < voices.n; i++) {
            Voice *v = &voices.v[i];
            if (v->done) continue;
            any = true;
            if (v->paused || all_paused) continue;
            for (int f = 0; f < CHUNK; f++) {
                if (v->pos >= v->frames) {
                    if (v->loop && v->frames) v->pos = 0;
                    else {
                        v->done = true;
                        break;
                    }
                }
                mix[f * 2] += v->pcm[v->pos * 2] * v->volume * gain;
                mix[f * 2 + 1] += v->pcm[v->pos * 2 + 1] * v->volume * gain;
                v->pos++;
            }
        }
        bool paused = all_paused;
        pthread_mutex_unlock(&mu);
        if (!any || paused) {
            if (!any && out_fd >= 0) stop_output();
            usleep(20000);
            continue;
        }
        if (out_fd < 0 && !spawn_output()) {
            usleep(500000);
            continue;
        }
        for (int i = 0; i < CHUNK * 2; i++) buf[i] = mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i];
        size_t off = 0, total = sizeof buf;
        while (off < total) {
            ssize_t w = write(out_fd, (char *)buf + off, total - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) {
                stop_output();
                break;
            }
            off += w;
        }
    }
    return NULL;
}

void sound_system_init(void)
{
    if (running) return;
    signal(SIGPIPE, SIG_IGN);
    master = we_settings.volume / 128.0f;
    muted = we_settings.silent;
    running = true;
    pthread_create(&thread, NULL, mixer, NULL);
}

void sound_system_shutdown(void)
{
    if (!running) return;
    running = false;
    pthread_join(thread, NULL);
    stop_output();
    for (int i = 0; i < voices.n; i++) free(voices.v[i].pcm);
    VFREE(voices);
}

int sound_play(const unsigned char *data, size_t len, const char *name, bool loop, float volume)
{
    size_t frames = 0;
    short *pcm = decode(data, len, &frames);
    if (!pcm) {
        we_err("cannot decode sound %s", name);
        return 0;
    }
    pthread_mutex_lock(&mu);
    Voice v = { .id = next_id++, .pcm = pcm, .frames = frames, .loop = loop, .volume = volume };
    VPUSH(voices, v);
    pthread_mutex_unlock(&mu);
    we_dbg("playing sound %s (%.1fs%s)", name, frames / (double)RATE, loop ? ", looped" : "");
    return v.id;
}

void sound_stop(int id)
{
    pthread_mutex_lock(&mu);
    for (int i = 0; i < voices.n; i++)
        if (voices.v[i].id == id) {
            free(voices.v[i].pcm);
            voices.v[i] = voices.v[--voices.n];
            break;
        }
    pthread_mutex_unlock(&mu);
}

void sound_pause(int id, bool paused)
{
    pthread_mutex_lock(&mu);
    for (int i = 0; i < voices.n; i++)
        if (voices.v[i].id == id) voices.v[i].paused = paused;
    pthread_mutex_unlock(&mu);
}

void sound_pause_all(bool paused)
{
    pthread_mutex_lock(&mu);
    all_paused = paused;
    pthread_mutex_unlock(&mu);
}

void sound_set_master(float volume, bool m)
{
    pthread_mutex_lock(&mu);
    master = volume;
    muted = m;
    pthread_mutex_unlock(&mu);
}

/* ------------------------------------------------------------------ auto-mute */
static pthread_t       am_thread;
static bool            am_running, am_busy;
static volatile bool   am_result;

static bool other_audio_playing(void)
{
    FILE *p = popen("pw-dump 2>/dev/null", "r");
    if (!p) return false;
    size_t cap = 1 << 20, n = 0;
    char *buf = malloc(cap);
    size_t r;
    while ((r = fread(buf + n, 1, cap - n - 1, p)) > 0) {
        n += r;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    buf[n] = 0;
    pclose(p);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    bool playing = false;
    const cJSON *node;
    cJSON_ArrayForEach(node, root)
    {
        const cJSON *info = js_get(node, "info");
        const cJSON *props = js_get(info, "props");
        if (strcmp(js_str(props, "media.class", ""), "Stream/Output/Audio")) continue;
        if (strcmp(js_str(info, "state", ""), "running")) continue;
        const cJSON *pidj = js_get(props, "application.process.id");
        long pid = pidj ? (cJSON_IsNumber(pidj) ? (long)pidj->valuedouble : atol(pidj->valuestring ? pidj->valuestring : "0")) : 0;
        if (pid == getpid() || (out_pid > 0 && pid == out_pid)) continue;
        if (!strcmp(js_str(props, "application.name", ""), "wallengine")) continue;
        /* ignore our own video sound players (ffplay children) */
        char path[64], comm[64] = "";
        snprintf(path, sizeof path, "/proc/%ld/stat", pid);
        FILE *f = fopen(path, "r");
        if (f) {
            long ppid = 0;
            if (fscanf(f, "%*d (%63[^)]) %*c %ld", comm, &ppid) == 2 && ppid == getpid()) {
                fclose(f);
                continue;
            }
            fclose(f);
        }
        playing = true;
        break;
    }
    cJSON_Delete(root);
    return playing;
}

static void *am_worker(void *arg)
{
    (void)arg;
    am_result = other_audio_playing();
    am_busy = false;
    return NULL;
}

void sound_automute_tick(void)
{
    static double last;
    double now = now_sec();
    if (!we_settings.automute || !in_path("pw-dump")) {
        auto_muted = false;
        return;
    }
    if (am_running && !am_busy) {
        pthread_join(am_thread, NULL);
        am_running = false;
        if (auto_muted != am_result) {
            auto_muted = am_result;
            we_dbg("auto-mute %s", auto_muted ? "on (another application is playing audio)" : "off");
        }
    }
    if (now - last < 2.0 || am_running || voices.n == 0) return;
    last = now;
    am_busy = true;
    am_running = true;
    pthread_create(&am_thread, NULL, am_worker, NULL);
}

bool sound_automuted(void) { return auto_muted; }

/* ------------------------------------------------------------------ sound objects */
typedef struct { VEC(int) ids; } SoundObj;

static void sound_destroy(SceneObj *o)
{
    SoundObj *so = o->impl;
    for (int i = 0; i < so->ids.n; i++) sound_stop(so->ids.v[i]);
    VFREE(so->ids);
    free(so);
}

bool sound_create(WeScene *s, SceneObj *o)
{
    const cJSON *j = o->json;
    SoundObj *so = calloc(1, sizeof *so);
    o->impl = so;
    o->destroy = sound_destroy;
    const char *mode = js_str(j, "playbackmode", NULL);
    bool loop = mode && !strcmp(mode, "loop");
    float vol = js_num(j, "volume", 1.0);
    const cJSON *it;
    cJSON_ArrayForEach(it, js_get(j, "sound"))
    {
        if (!cJSON_IsString(it)) continue;
        size_t len;
        unsigned char *d = vfs_read(s->project->vfs, it->valuestring, &len);
        if (!d) {
            we_err("cannot find sound %s", it->valuestring);
            continue;
        }
        int id = sound_play(d, len, it->valuestring, loop, vol);
        free(d);
        if (id) {
            VPUSH(so->ids, id);
            VPUSH(s->sound_ids, id);
        }
    }
    return true;
}
