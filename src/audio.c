/* Audio-reactive input: captures what the system is playing (PipeWire or
 * PulseAudio monitor), runs an FFT and exposes a Shadertoy-style 512x2 texture:
 * row 0 = log-scaled spectrum, row 1 = waveform. */
#include "we.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define RATE  44100
#define N     1024
#define RING  8192

static pid_t           pid = -1;
static int             fd = -1;
static pthread_t       th;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static float           ring[RING];
static unsigned        wpos;
static volatile bool   running;

static GLuint        tex;
static float         smooth[AUDIO_BINS];
static unsigned char texdata[2][AUDIO_BINS];
static float         bass, level;

static void *reader(void *arg)
{
    (void)arg;
    short buf[512];
    while (running) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        int n = r / 2;
        pthread_mutex_lock(&mu);
        for (int i = 0; i < n; i++) ring[(wpos + i) % RING] = buf[i] / 32768.0f;
        wpos = (wpos + n) % RING;
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static const char *default_cmd(void)
{
    if (in_path("pw-record"))
        return "pw-record -a -P '{ stream.capture.sink=true }' --format s16 --rate 44100 "
               "--channels 1 --latency 20ms -";
    if (in_path("parec"))
        return "parec -d @DEFAULT_MONITOR@ --format=s16le --rate=44100 --channels=1 --latency-msec=20";
    return NULL;
}

bool audio_start(const char *cmd)
{
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, AUDIO_BINS, 2, 0, GL_RED, GL_UNSIGNED_BYTE, texdata);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    memset(texdata[1], 128, AUDIO_BINS);

    if (!cmd || !*cmd) cmd = default_cmd();
    if (!cmd) {
        logmsg("audio: neither pw-record nor parec found; audio-reactive effects disabled");
        return false;
    }
    int p[2];
    if (pipe(p) < 0) return false;
    pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(p[1], 1);
        if (!cfg.verbose) dup2(null, 2);
        close(p[0]);
        setpgid(0, 0);
        char full[1200];
        snprintf(full, sizeof full, "exec %s", cmd);
        execl("/bin/sh", "sh", "-c", full, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return false;
    }
    fd = p[0];
    running = true;
    pthread_create(&th, NULL, reader, NULL);
    dbg("audio capture: %s", cmd);
    return true;
}

static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

static float to_unit(float mag)
{
    float db = 20.0f * log10f(mag + 1e-9f);
    float v = (db + 70.0f) / 60.0f;
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

void audio_update(void)
{
    if (!running) return;
    float re[N], im[N], wave[N];
    pthread_mutex_lock(&mu);
    for (int i = 0; i < N; i++) wave[i] = ring[(wpos + RING - N + i) % RING];
    pthread_mutex_unlock(&mu);

    float rms = 0;
    for (int i = 0; i < N; i++) {
        float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (N - 1));
        re[i] = wave[i] * w;
        im[i] = 0;
        rms += wave[i] * wave[i];
    }
    level = level * 0.7f + 0.3f * fminf(1.0f, sqrtf(rms / N) * 4.0f);
    fft(re, im, N);

    float mag[N / 2];
    for (int k = 0; k < N / 2; k++) mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]) * 4.0f / N;

    /* Map 512 output bins logarithmically over ~43 Hz .. 22 kHz. */
    for (int i = 0; i < AUDIO_BINS; i++) {
        float f0 = powf(N / 2 - 1, (float)i / AUDIO_BINS);
        float f1 = powf(N / 2 - 1, (float)(i + 1) / AUDIO_BINS);
        int k0 = (int)f0, k1 = (int)f1;
        if (k1 < k0) k1 = k0;
        float m = 0;
        for (int k = k0; k <= k1 && k < N / 2; k++) m = fmaxf(m, mag[k]);
        float v = to_unit(m);
        smooth[i] = v > smooth[i] ? v : smooth[i] * 0.85f + v * 0.15f;
        texdata[0][i] = (unsigned char)(smooth[i] * 255.0f);
        float s = wave[N - AUDIO_BINS + i] * 0.5f + 0.5f;
        texdata[1][i] = (unsigned char)(fminf(fmaxf(s, 0), 1) * 255.0f);
    }
    float b = 0;
    for (int k = 1; k <= 4; k++) b = fmaxf(b, mag[k]);   /* ~43..172 Hz */
    b = to_unit(b);
    bass = b > bass ? b : bass * 0.88f + b * 0.12f;

    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, AUDIO_BINS, 2, GL_RED, GL_UNSIGNED_BYTE, texdata);
}

/* latest n samples (mono, -1..1), oldest first; returns n or 0 when not capturing */
int audio_samples(float *out, int n)
{
    if (!running || n > RING) return 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) out[i] = ring[(wpos + RING - n + i) % RING];
    pthread_mutex_unlock(&mu);
    return n;
}

/* power spectrum (re^2 + im^2) of the latest N samples without windowing, as
 * linux-wallpaperengine computes it; returns the number of bins (N/2) or 0 */
int audio_power(float *power)
{
    if (!running) return 0;
    float re[N], im[N];
    pthread_mutex_lock(&mu);
    for (int i = 0; i < N; i++) {
        re[i] = ring[(wpos + RING - N + i) % RING];
        im[i] = 0;
    }
    pthread_mutex_unlock(&mu);
    fft(re, im, N);
    for (int k = 0; k < N / 2; k++) power[k] = re[k] * re[k] + im[k] * im[k];
    return N / 2;
}

GLuint audio_texture(void) { return tex; }
float  audio_bass(void) { return bass; }
float  audio_level(void) { return level; }

void audio_stop(void)
{
    if (!running) return;
    running = false;
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
    pthread_join(th, NULL);
    close(fd);
}
