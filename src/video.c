/* Video wallpapers: frames are decoded by an ffmpeg child process into a pipe
 * (raw RGBA, already scaled to the monitor), paced by a reader thread. Optional
 * sound is played by a separate ffplay process. */
#include "we.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct Video {
    pid_t           pid, snd_pid;
    int             fd;
    int             w, h, fps;
    size_t          fsz;
    unsigned char  *front, *back;
    bool            fresh;
    bool            paused;
    volatile bool   quit;
    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
};

static pid_t spawn(char *const argv[], int out_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(out_fd >= 0 ? out_fd : null, 1);
        if (!cfg.verbose) dup2(null, 2);
        setpgid(0, 0);
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static bool read_full(int fd, unsigned char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

static void *reader(void *arg)
{
    Video *v = arg;
    double period = 1.0 / v->fps, next = now_sec();
    while (!v->quit) {
        pthread_mutex_lock(&v->mu);
        while (v->paused && !v->quit) {
            pthread_cond_wait(&v->cv, &v->mu);
            next = now_sec();
        }
        pthread_mutex_unlock(&v->mu);
        if (v->quit || !read_full(v->fd, v->back, v->fsz)) break;

        pthread_mutex_lock(&v->mu);
        unsigned char *t = v->front;
        v->front = v->back;
        v->back  = t;
        v->fresh = true;
        pthread_mutex_unlock(&v->mu);

        next += period;
        double wait = next - now_sec();
        if (wait > 0) {
            struct timespec ts = { (time_t)wait, (long)((wait - (time_t)wait) * 1e9) };
            nanosleep(&ts, NULL);
        } else if (wait < -0.5) {
            next = now_sec(); /* fell far behind, resync */
        }
    }
    return NULL;
}

Video *video_open(const char *path, int w, int h, FitMode fit, int fps)
{
    if (!in_path("ffmpeg")) {
        logmsg("video wallpapers need ffmpeg installed (sudo apt install ffmpeg)");
        return NULL;
    }
    if (cfg.video_max_width > 0 && w > cfg.video_max_width) {
        h = h * cfg.video_max_width / w;
        w = cfg.video_max_width;
    }
    w &= ~1;
    h &= ~1;
    if (fps <= 0 || fps > 60) fps = 30;

    char vf[512];
    switch (fit) {
    case FIT_STRETCH:
        snprintf(vf, sizeof vf, "scale=%d:%d,fps=%d", w, h, fps);
        break;
    case FIT_CONTAIN:
    case FIT_CENTER:
        snprintf(vf, sizeof vf,
                 "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,fps=%d",
                 w, h, w, h, fps);
        break;
    default:
        snprintf(vf, sizeof vf,
                 "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d,fps=%d", w, h, w, h, fps);
    }

    int p[2];
    if (pipe(p) < 0) return NULL;
    char *argv[] = { "ffmpeg", "-nostdin", "-loglevel", "error", "-stream_loop", "-1",
                     "-i", (char *)path, "-an", "-vf", vf, "-pix_fmt", "rgba",
                     "-f", "rawvideo", "pipe:1", NULL };
    pid_t pid = spawn(argv, p[1]);
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return NULL;
    }

    Video *v = calloc(1, sizeof *v);
    v->pid = pid;
    v->fd  = p[0];
    v->w = w; v->h = h; v->fps = fps;
    v->fsz   = (size_t)w * h * 4;
    v->front = calloc(1, v->fsz);
    v->back  = calloc(1, v->fsz);
    v->snd_pid = -1;
    pthread_mutex_init(&v->mu, NULL);
    pthread_cond_init(&v->cv, NULL);
    pthread_create(&v->th, NULL, reader, v);

    if (cfg.video_sound) {
        if (in_path("ffplay")) {
            char vol[16];
            snprintf(vol, sizeof vol, "%d", cfg.volume);
            char *sargv[] = { "ffplay", "-nodisp", "-loop", "0", "-loglevel", "quiet",
                              "-volume", vol, (char *)path, NULL };
            v->snd_pid = spawn(sargv, -1);
        } else {
            logmsg("video sound needs ffplay (part of ffmpeg)");
        }
    }
    return v;
}

void video_size(Video *v, int *w, int *h) { *w = v->w; *h = v->h; }

bool video_update(Video *v, GLuint tex)
{
    bool up = false;
    pthread_mutex_lock(&v->mu);
    if (v->fresh) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, v->w, v->h, GL_RGBA, GL_UNSIGNED_BYTE, v->front);
        v->fresh = false;
        up = true;
    }
    pthread_mutex_unlock(&v->mu);
    return up;
}

void video_pause(Video *v, bool paused)
{
    pthread_mutex_lock(&v->mu);
    if (v->paused != paused) {
        v->paused = paused;
        pthread_cond_signal(&v->cv);
        /* ffmpeg blocks on the full pipe by itself; just freeze the sound. */
        if (v->snd_pid > 0) kill(v->snd_pid, paused ? SIGSTOP : SIGCONT);
    }
    pthread_mutex_unlock(&v->mu);
}

static void reap(pid_t pid)
{
    if (pid <= 0) return;
    kill(pid, SIGCONT);
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

void video_close(Video *v)
{
    if (!v) return;
    pthread_mutex_lock(&v->mu);
    v->quit = true;
    v->paused = false;
    pthread_cond_signal(&v->cv);
    pthread_mutex_unlock(&v->mu);
    reap(v->pid);          /* closes the write side, unblocking read() */
    pthread_join(v->th, NULL);
    close(v->fd);
    reap(v->snd_pid);
    pthread_mutex_destroy(&v->mu);
    pthread_cond_destroy(&v->cv);
    free(v->front);
    free(v->back);
    free(v);
}
