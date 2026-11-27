// MCI digital video (the "avivideo" device): the guest opens a movie file,
// places it in its window and plays it with MCI_NOTIFY, then waits for
// MM_MCINOTIFY. Decoding is mf::Media (FFmpeg); frames go to the window
// through host_display_present_window, paced by the wall clock, and the
// soundtrack is streamed to one host audio channel. One device at a time,
// which is what the games that use MCI for their intros open.
#include "com.h"
#include "dx.h"
#include "host_api.h"
#include "mf_media.h"
#include "../runtime/display_seam.h"
#include "../runtime/imports.h"
#include "../runtime/win32.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr uint32_t MCI_OPEN = 0x803, MCI_CLOSE = 0x804, MCI_PLAY = 0x806, MCI_SEEK = 0x807,
                   MCI_STOP = 0x808, MCI_PAUSE = 0x809, MCI_STATUS = 0x814, MCI_RESUME = 0x855,
                   MCI_WINDOW = 0x841, MCI_PUT = 0x842, MCI_WHERE = 0x843;
constexpr uint32_t MCI_NOTIFY = 0x1, MCI_OPEN_TYPE = 0x2000, MCI_OPEN_ELEMENT = 0x200,
                   MCI_STATUS_ITEM = 0x100, MCI_DGV_RECT = 0x10000;
constexpr uint32_t MCI_STATUS_LENGTH = 1, MCI_STATUS_POSITION = 2, MCI_STATUS_MODE = 4;
constexpr uint32_t MCI_MODE_STOP = 525, MCI_MODE_PLAY = 526, MCI_MODE_PAUSE = 529;
constexpr uint32_t MCIERR_INVALID_DEVICE_ID = 257, MCIERR_FILE_NOT_FOUND = 275,
                   MCIERR_UNSUPPORTED_FUNCTION = 274;
constexpr uint32_t MM_MCINOTIFY = 0x3b9, MCI_NOTIFY_SUCCESSFUL = 1, MCI_NOTIFY_ABORTED = 4;
constexpr uint32_t kDevice = 1;

double now_seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

struct Movie {
    bool open = false, playing = false, done = false;
    std::string path;
    std::unique_ptr<mf::Media> media;
    uint32_t notify_hwnd = 0;
    bool notify = false;
    int32_t rect[4] = {0, 0, 0, 0}; // x, y, width, height in the guest's display
    double position = 0, clock0 = 0;
    mf::VideoFrame cur, next;
    bool have_next = false, have_cur = false;
    int32_t channel = -1;
    bool audio_started = false;
    std::vector<uint32_t> screen;
};
Movie g;

void audio_stop() {
    if (g.channel >= 0) {
        host_audio_stop(g.channel);
        dx_free_audio_channel(g.channel);
        g.channel = -1;
    }
    g.audio_started = false;
}

bool load(const std::string &host) {
    g.media = std::make_unique<mf::Media>();
    if (!g.media->open(host) || !g.media->has_video()) {
        g.media.reset();
        return false;
    }
    g.have_next = g.have_cur = g.done = false;
    g.position = 0;
    return true;
}

void post_notify(uint32_t status) {
    if (!g.notify)
        return;
    g.notify = false;
    // MCI_PLAY's callback is a 16-bit window handle in the low word; the
    // guest's main window is the one it means when the full handle differs.
    uint32_t hwnd = g.notify_hwnd;
    const uint32_t main = host_main_window();
    if (main && (hwnd & 0xffffu) == (main & 0xffffu))
        hwnd = main;
    host_post_message(hwnd, MM_MCINOTIFY, status, kDevice);
}

void present(const mf::VideoFrame &f) {
    uint32_t mw = 0, mh = 0, bpp = 0;
    win32_display_mode(&mw, &mh, &bpp);
    if (!mw || !mh) {
        mw = (uint32_t)f.width;
        mh = (uint32_t)f.height;
    }
    int x = g.rect[0], y = g.rect[1], w = g.rect[2], h = g.rect[3];
    if (w <= 0 || h <= 0) { // no MCI_PUT: the film's own size, centred
        w = f.width;
        h = f.height;
        x = ((int)mw - w) / 2;
        y = ((int)mh - h) / 2;
    }
    g.screen.assign((size_t)mw * mh, 0xff000000u);
    for (int dy = 0; dy < h; ++dy) {
        const int oy = y + dy;
        if (oy < 0 || oy >= (int)mh)
            continue;
        const uint32_t *row = f.argb.data() + (size_t)(dy * f.height / h) * f.width;
        for (int dx = 0; dx < w; ++dx) {
            const int ox = x + dx;
            if (ox >= 0 && ox < (int)mw)
                g.screen[(size_t)oy * mw + ox] = 0xff000000u | row[dx * f.width / w];
        }
    }
    host_display_present_window(g.screen.data(), (int)mw, (int)mh);
}

void feed_audio() {
    if (!g.media->has_audio())
        return;
    const uint32_t ahead = (uint32_t)g.media->audio_rate() * g.media->audio_channels();
    if (g.audio_started && host_audio_queued_bytes(g.channel) >= ahead)
        return;
    std::vector<int16_t> pcm = g.media->take_audio();
    if (pcm.empty())
        return;
    if (!g.audio_started) {
        if (g.channel < 0)
            g.channel = dx_alloc_audio_channel();
        if (g.channel < 0)
            return;
        HostAudioPlay p{};
        p.channel = g.channel;
        p.pcm = pcm.data();
        p.bytes = (uint32_t)pcm.size() * 2;
        p.sample_rate = g.media->audio_rate();
        p.channels = g.media->audio_channels();
        p.bits = 16;
        host_audio_play(&p);
        g.audio_started = host_audio_stream(g.channel) >= 0;
    } else {
        host_audio_queue(g.channel, pcm.data(), (uint32_t)pcm.size() * 2);
    }
}

// Presents the newest frame that is due and holds the first that is not.
void advance() {
    if (!g.playing || !g.media)
        return;
    const double t = g.position + (now_seconds() - g.clock0);
    bool show = false;
    if (g.have_next && g.next.pts <= t) {
        g.cur = std::move(g.next);
        g.have_next = false;
        g.have_cur = show = true;
    }
    while (!g.have_next && !g.done) {
        mf::VideoFrame f;
        if (!g.media->next_video(&f)) {
            g.done = true;
            break;
        }
        if (f.pts <= t) {
            g.cur = std::move(f);
            g.have_cur = show = true;
        } else {
            g.next = std::move(f);
            g.have_next = true;
        }
    }
    feed_audio();
    if (show && g.have_cur)
        present(g.cur);
    if (g.done && !g.have_next &&
        (!g.audio_started || !host_audio_queued_bytes(g.channel))) {
        g.playing = false;
        post_notify(MCI_NOTIFY_SUCCESSFUL);
    }
}

void close_movie() {
    if (g.playing)
        post_notify(MCI_NOTIFY_ABORTED);
    audio_stop();
    g = Movie{};
}

uint32_t open_cmd(uint32_t flags, uint32_t p) {
    if (!p || !gm_valid(p, 16))
        return MCIERR_UNSUPPORTED_FUNCTION;
    if (!(flags & MCI_OPEN_ELEMENT)) {
        // MCI_OPEN_TYPE alone loads the driver: nothing to play yet.
        wr32(p + 4, kDevice);
        return 0;
    }
    const std::string name = gm_str(rd32(p + 12));
    const std::string host = win32_host_path_op(name, WIN32_FILE_READ);
    close_movie();
    if (host.empty() || !load(host)) {
        LOGW("mci: cannot play %s (%s)", name.c_str(), host.empty() ? "not found" : host.c_str());
        return MCIERR_FILE_NOT_FOUND;
    }
    g.open = true;
    g.path = host;
    wr32(p + 4, kDevice);
    LOGV("mci: opened %s, %dx%d", name.c_str(), g.media->width(), g.media->height());
    return 0;
}

void m_mciSendCommandA(X86 *c) {
    const uint32_t id = arg(c, 0), msg = arg(c, 1), flags = arg(c, 2), p = arg(c, 3);
    uint32_t rc = 0;
    if (msg == MCI_OPEN) {
        set_eax(c, open_cmd(flags, p));
        return;
    }
    if (id != kDevice || (!g.open && msg != MCI_CLOSE)) {
        set_eax(c, msg == MCI_CLOSE ? 0 : MCIERR_INVALID_DEVICE_ID);
        return;
    }
    switch (msg) {
    case MCI_CLOSE:
        close_movie();
        break;
    case MCI_PUT:
        if ((flags & MCI_DGV_RECT) && p && gm_valid(p, 20))
            for (int k = 0; k < 4; ++k)
                g.rect[k] = (int32_t)rd32(p + 4 + 4 * k); // x, y, width, height
        break;
    case MCI_PLAY:
    case MCI_RESUME:
        if ((flags & MCI_NOTIFY) && p && gm_valid(p, 4)) {
            g.notify = true;
            g.notify_hwnd = rd32(p);
        }
        if (!g.playing) {
            g.playing = true;
            g.clock0 = now_seconds();
        }
        break;
    case MCI_PAUSE:
    case MCI_STOP:
        if (g.playing) {
            g.position += now_seconds() - g.clock0;
            g.playing = false;
        }
        if (g.channel >= 0)
            audio_stop();
        break;
    case MCI_SEEK: // TOCA seeks only to the start; any seek restarts the film
        audio_stop();
        g.playing = false;
        if (!load(g.path))
            rc = MCIERR_FILE_NOT_FOUND;
        break;
    case MCI_STATUS:
        if ((flags & MCI_STATUS_ITEM) && p && gm_valid(p, 12)) {
            const uint32_t item = rd32(p + 8);
            uint32_t v = 0;
            if (item == MCI_STATUS_MODE)
                v = g.playing ? MCI_MODE_PLAY : g.done ? MCI_MODE_STOP : MCI_MODE_PAUSE;
            else if (item == MCI_STATUS_LENGTH)
                v = (uint32_t)(g.media ? g.media->duration() * 1000 : 0);
            else if (item == MCI_STATUS_POSITION)
                v = (uint32_t)((g.position + (g.playing ? now_seconds() - g.clock0 : 0)) * 1000);
            wr32(p + 4, v);
        }
        break;
    case MCI_WINDOW:
    case MCI_WHERE:
        break; // the film draws in the game's own window
    default:
        rc = MCIERR_UNSUPPORTED_FUNCTION;
    }
    set_eax(c, rc);
}

// mciGetDeviceIDA(name): the open device, whatever it is called.
void m_mciGetDeviceIDA(X86 *c) {
    set_eax(c, g.open ? kDevice : 0);
}

const ImportShim shims[] = {
    {"WINMM.dll", "mciSendCommandA", 4, m_mciSendCommandA},
    {"_INMM.dll", "mciSendCommandA", 4, m_mciSendCommandA},
    {"WINMM.dll", "mciGetDeviceIDA", 1, m_mciGetDeviceIDA},
};
} // namespace

void mci_frame_pump(X86 *c) {
    if (c)
        advance();
}
void mci_register() {
    imports_register(shims, std::size(shims));
}
