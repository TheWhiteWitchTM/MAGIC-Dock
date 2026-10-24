#include "backend.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QSocketNotifier>
#include <QTimer>

#include <QHash>
#include <QList>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

extern "C" {
struct ext_workspace_handle_v1;
struct zcosmic_workspace_handle_v1;
#include "wlr-layer-shell-unstable-v1-client.h"
#include "ext-foreign-toplevel-list-v1-client.h"
#include "cosmic-toplevel-info-unstable-v1-client.h"
#include "cosmic-toplevel-management-unstable-v1-client.h"
#include "fractional-scale-v1-client.h"
#include "viewporter-client.h"
#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"
}

struct Out;
struct BarImpl;

struct Buffer {
    wl_buffer *buf = nullptr;
    void *data = nullptr;
    size_t bytes = 0;
    int w = 0;
    int h = 0;
    int stride = 0;
    bool busy = false;
    bool retired = false;
    Out *owner = nullptr;
};

struct Out {
    BarImpl *impl = nullptr;
    int id = 0;
    wl_output *output = nullptr;
    wl_surface *surface = nullptr;
    zwlr_layer_surface_v1 *layer = nullptr;
    wp_viewport *viewport = nullptr;
    wp_fractional_scale_v1 *frac = nullptr;
    int modeW = 0;
    int modeH = 0;
    int pixelW = 0;
    int pixelH = 0;
    int intScale = 1;
    int transform = 0;
    double prefScale = 0;
    int logicalW = 0;
    int logicalH = 0;
    bool configured = false;
    bool currentMode = false;
    QString name;
    std::vector<Buffer *> buffers;
    bool renderQueued = false;
};

struct Win {
    BarImpl *impl = nullptr;
    quint64 key = 0;
    ext_foreign_toplevel_handle_v1 *foreign = nullptr;
    zcosmic_toplevel_handle_v1 *cosmic = nullptr;
    QString appId;
    QString title;
    bool minimized = false;
    bool activated = false;
    bool fullscreen = false;
    bool maximized = false;
    QHash<int, QRect> geom;
    QList<int> outputs;
    bool alive = true;
};

struct Shot;

struct BarImpl {
    Backend *self = nullptr;
    ~BarImpl();
    wl_display *dpy = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    wl_seat *seat = nullptr;
    wl_pointer *pointer = nullptr;
    wl_keyboard *keyboard = nullptr;
    zwlr_layer_shell_v1 *layerShell = nullptr;
    ext_foreign_toplevel_list_v1 *toplevelList = nullptr;
    zcosmic_toplevel_info_v1 *info = nullptr;
    zcosmic_toplevel_manager_v1 *manager = nullptr;
    wp_fractional_scale_manager_v1 *fracMgr = nullptr;
    wp_viewporter *viewporter = nullptr;
    ext_image_copy_capture_manager_v1 *captureMgr = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1 *sourceMgr = nullptr;
    wl_cursor_theme *cursorTheme = nullptr;
    wl_surface *cursorSurface = nullptr;
    int cursorPx = 0;
    QString cursorKey;
    ShellSpec shell;
    bool hasArgb = false;
    bool keyboardExclusive = false;
    int nextId = 1;
    bool inRender = false;
    std::vector<std::unique_ptr<Out>> outputs;
    std::vector<std::unique_ptr<Win>> wins;
    std::vector<std::unique_ptr<Shot>> shots;
    bool keepFrames = false;

    int idFor(wl_output *output) const {
        for (const auto &o : outputs) {
            if (o->output == output) return o->id;
        }
        return -1;
    }

    Out *outById(int id) const {
        for (const auto &o : outputs) {
            if (o->id == id) return o.get();
        }
        return nullptr;
    }

    Win *winByKey(quint64 key) const {
        for (const auto &w : wins) {
            if (w->key == key) return w.get();
        }
        return nullptr;
    }

    void publish();
    void ensureSurface(Out *out);
    void render(Out *out);
    void setCursor(quint32 serial, int scale);
    void configureShell(Out *out);
    void destroyWin(Win *win);
    Buffer *acquire(Out *out, int w, int h);
    Shot *shotByKey(quint64 key) const;
    void ensureShot(Win *win);
    void dropShot(Shot *shot);
    void allocShotBuffer(Shot *shot, uint32_t format);
    void beginFrame(Shot *shot);
};

struct Shot {
    BarImpl *impl = nullptr;
    quint64 key = 0;
    ext_image_capture_source_v1 *source = nullptr;
    ext_image_copy_capture_session_v1 *session = nullptr;
    ext_image_copy_capture_frame_v1 *frame = nullptr;
    wl_buffer *buffer = nullptr;
    void *data = nullptr;
    size_t bytes = 0;
    int width = 0;
    int height = 0;
    uint32_t format = 0;
    bool haveSize = false;
    bool capturing = false;
    bool stopped = false;
    int fails = 0;
    qint64 retryAt = 0;
    QImage image;
    quint64 serial = 0;
    std::vector<uint32_t> formats;

    ~Shot() {
        if (frame) ext_image_copy_capture_frame_v1_destroy(frame);
        if (session) ext_image_copy_capture_session_v1_destroy(session);
        if (source) ext_image_capture_source_v1_destroy(source);
        if (buffer) wl_buffer_destroy(buffer);
        if (data && bytes) munmap(data, bytes);
    }
};

BarImpl::~BarImpl() = default;

void bufferRelease(void *data, wl_buffer *) {
    auto *buf = static_cast<Buffer *>(data);
    buf->busy = false;
    if (buf->retired) {
        if (buf->data) munmap(buf->data, buf->bytes);
        delete buf;
        return;
    }
    if (buf->owner && buf->owner->renderQueued && buf->owner->impl && !buf->owner->impl->inRender) {
        buf->owner->renderQueued = false;
        buf->owner->impl->render(buf->owner);
    }
}

const wl_buffer_listener &bufferListener() {
    static wl_buffer_listener listener{};
    static bool init = false;
    if (!init) {
        listener.release = bufferRelease;
        init = true;
    }
    return listener;
}

void retireBuffers(Out *out) {
    for (Buffer *b : out->buffers) b->retired = true;
    std::vector<Buffer *> keep;
    for (Buffer *b : out->buffers) {
        if (!b->busy) {
            if (b->buf) wl_buffer_destroy(b->buf);
            if (b->data) munmap(b->data, b->bytes);
            delete b;
        } else {
            keep.push_back(b);
        }
    }
    out->buffers.swap(keep);
}

Buffer *BarImpl::acquire(Out *out, int w, int h) {
    for (Buffer *b : out->buffers) {
        if (!b->retired && !b->busy && b->w == w && b->h == h) return b;
    }
    int live = 0;
    for (Buffer *b : out->buffers) {
        if (!b->retired && b->w == w && b->h == h) live++;
    }
    if (live >= 2) {
        out->renderQueued = true;
        return nullptr;
    }
    const int stride = w * 4;
    const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
    const int fd = memfd_create("cosmic-bar", MFD_CLOEXEC);
    if (fd < 0) {
        std::perror("magic-dock: memfd_create");
        return nullptr;
    }
    if (ftruncate(fd, static_cast<off_t>(bytes)) < 0) {
        close(fd);
        return nullptr;
    }
    void *map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    wl_shm_pool *pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(bytes));
    wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    auto *buf = new Buffer();
    buf->buf = buffer;
    buf->data = map;
    buf->bytes = bytes;
    buf->w = w;
    buf->h = h;
    buf->stride = stride;
    buf->owner = out;
    wl_buffer_add_listener(buffer, &bufferListener(), buf);
    out->buffers.push_back(buf);
    return buf;
}

void BarImpl::publish() {
    self->windows_.clear();
    for (const auto &win : wins) {
        if (!win->alive) continue;
        if (win->outputs.isEmpty()) {
            WindowState state;
            state.key = win->key;
            state.appId = win->appId;
            state.title = win->title;
            state.minimized = win->minimized;
            state.activated = win->activated;
            state.fullscreen = win->fullscreen;
            state.maximized = win->maximized;
            self->windows_.push_back(state);
            continue;
        }
        for (int id : win->outputs) {
            WindowState state;
            state.key = win->key;
            state.appId = win->appId;
            state.title = win->title;
            state.minimized = win->minimized;
            state.activated = win->activated;
            state.fullscreen = win->fullscreen;
            state.maximized = win->maximized;
            state.onOutput = true;
            state.outputId = id;
            if (win->geom.contains(id)) {
                const QRect r = win->geom.value(id);
                state.x = r.x();
                state.y = r.y();
                state.w = r.width();
                state.h = r.height();
                state.hasGeom = r.width() > 0 && r.height() > 0;
            }
            self->windows_.push_back(state);
        }
    }
    if (self->onWindows) self->onWindows();
}

void BarImpl::destroyWin(Win *win) {
    win->alive = false;
    std::vector<Shot *> gone;
    for (const auto &shot : shots) {
        if (shot->key == win->key) gone.push_back(shot.get());
    }
    for (Shot *shot : gone) dropShot(shot);
    if (win->cosmic) {
        zcosmic_toplevel_handle_v1_destroy(win->cosmic);
        win->cosmic = nullptr;
    }
    if (win->foreign) {
        ext_foreign_toplevel_handle_v1_destroy(win->foreign);
        win->foreign = nullptr;
    }
    wins.erase(std::remove_if(wins.begin(), wins.end(), [&](const std::unique_ptr<Win> &w) { return w.get() == win; }),
               wins.end());
    publish();
}

namespace {

qint64 shotNow() { return QDateTime::currentMSecsSinceEpoch(); }

uint32_t pickShmFormat(const std::vector<uint32_t> &formats) {
    const uint32_t prefer[] = {WL_SHM_FORMAT_ARGB8888, WL_SHM_FORMAT_XRGB8888, WL_SHM_FORMAT_ABGR8888,
                               WL_SHM_FORMAT_XBGR8888};
    for (uint32_t want : prefer) {
        if (std::find(formats.begin(), formats.end(), want) != formats.end()) return want;
    }
    return 0;
}

QImage::Format shotImageFormat(uint32_t format) {
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888:
        return QImage::Format_ARGB32;
    case WL_SHM_FORMAT_XRGB8888:
        return QImage::Format_RGB32;
    case WL_SHM_FORMAT_ABGR8888:
        return QImage::Format_RGBA8888;
    case WL_SHM_FORMAT_XBGR8888:
        return QImage::Format_RGBX8888;
    default:
        return QImage::Format_Invalid;
    }
}

}  // namespace

Shot *BarImpl::shotByKey(quint64 key) const {
    for (const auto &shot : shots) {
        if (shot->key == key) return shot.get();
    }
    return nullptr;
}

void BarImpl::dropShot(Shot *shot) {
    shots.erase(std::remove_if(shots.begin(), shots.end(),
                               [&](const std::unique_ptr<Shot> &item) { return item.get() == shot; }),
                shots.end());
}

void BarImpl::allocShotBuffer(Shot *shot, uint32_t format) {
    if (shot->buffer) wl_buffer_destroy(shot->buffer);
    shot->buffer = nullptr;
    if (shot->data && shot->bytes) munmap(shot->data, shot->bytes);
    shot->data = nullptr;
    shot->bytes = 0;
    shot->format = 0;
    if (shot->width < 1 || shot->height < 1 || shot->width > 8192 || shot->height > 8192) return;
    const int stride = shot->width * 4;
    const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(shot->height);
    const int fd = memfd_create("cosmic-bar-thumb", MFD_CLOEXEC);
    if (fd < 0) return;
    if (ftruncate(fd, static_cast<off_t>(bytes)) < 0) {
        close(fd);
        return;
    }
    void *map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    wl_shm_pool *pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(bytes));
    wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, shot->width, shot->height, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    shot->buffer = buffer;
    shot->data = map;
    shot->bytes = bytes;
    shot->format = format;
}

void BarImpl::beginFrame(Shot *shot) {
    if (!shot->session || !shot->buffer || shot->capturing || shot->stopped || shot->frame) return;
    shot->frame = ext_image_copy_capture_session_v1_create_frame(shot->session);
    if (!shot->frame) return;
    static ext_image_copy_capture_frame_v1_listener listener{};
    static bool init = false;
    if (!init) {
        listener.transform = [](void *, ext_image_copy_capture_frame_v1 *, uint32_t) {};
        listener.damage = [](void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t, int32_t) {};
        listener.presentation_time = [](void *, ext_image_copy_capture_frame_v1 *, uint32_t, uint32_t, uint32_t) {};
        listener.ready = [](void *data, ext_image_copy_capture_frame_v1 *) {
            auto *shot = static_cast<Shot *>(data);
            const QImage::Format format = shotImageFormat(shot->format);
            if (format != QImage::Format_Invalid && shot->data && shot->width > 0 && shot->height > 0) {
                QImage wrapped(static_cast<const uchar *>(shot->data), shot->width, shot->height, shot->width * 4, format);
                const int edge = std::max(shot->width, shot->height);
                QImage scaled = wrapped;
                if (edge > 280) {
                    scaled = wrapped.scaled(std::max(1, (shot->width * 280) / edge), std::max(1, (shot->height * 280) / edge),
                                            Qt::KeepAspectRatio, Qt::SmoothTransformation);
                } else {
                    scaled = wrapped.copy();
                }
                shot->image = scaled.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                shot->serial++;
                shot->fails = 0;
                if (shot->impl->self->onThumbnails) shot->impl->self->onThumbnails();
            }
            if (shot->frame) {
                ext_image_copy_capture_frame_v1_destroy(shot->frame);
                shot->frame = nullptr;
            }
            shot->capturing = false;
            if (!shot->stopped) shot->impl->beginFrame(shot);
        };
        listener.failed = [](void *data, ext_image_copy_capture_frame_v1 *, uint32_t reason) {
            auto *shot = static_cast<Shot *>(data);
            if (shot->frame) {
                ext_image_copy_capture_frame_v1_destroy(shot->frame);
                shot->frame = nullptr;
            }
            shot->capturing = false;
            shot->fails++;
            if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
                if (shot->buffer) wl_buffer_destroy(shot->buffer);
                shot->buffer = nullptr;
                if (shot->data && shot->bytes) munmap(shot->data, shot->bytes);
                shot->data = nullptr;
                shot->bytes = 0;
                shot->format = 0;
            }
            const bool giveUp = reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED || shot->fails > 3;
            shot->stopped = giveUp;
            shot->retryAt = shotNow() + (giveUp ? std::min(4000, 500 * shot->fails) : 300);
            if (!giveUp) {
                const uint32_t format = shot->format ? shot->format : pickShmFormat(shot->formats);
                if (!shot->buffer && format != 0) shot->impl->allocShotBuffer(shot, format);
                if (shot->buffer) shot->impl->beginFrame(shot);
                else shot->stopped = true;
            }
        };
        init = true;
    }
    ext_image_copy_capture_frame_v1_add_listener(shot->frame, &listener, shot);
    ext_image_copy_capture_frame_v1_attach_buffer(shot->frame, shot->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(shot->frame, 0, 0, shot->width, shot->height);
    ext_image_copy_capture_frame_v1_capture(shot->frame);
    shot->capturing = true;
}

void BarImpl::ensureShot(Win *win) {
    if (!captureMgr || !sourceMgr || !shm || !win->foreign) return;
    auto shot = std::make_unique<Shot>();
    shot->impl = this;
    shot->key = win->key;
    shot->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(sourceMgr, win->foreign);
    if (!shot->source) return;
    shot->session = ext_image_copy_capture_manager_v1_create_session(captureMgr, shot->source, 0);
    if (!shot->session) return;
    Shot *raw = shot.get();
    static ext_image_copy_capture_session_v1_listener listener{};
    static bool init = false;
    if (!init) {
        listener.buffer_size = [](void *data, ext_image_copy_capture_session_v1 *, uint32_t width, uint32_t height) {
            auto *shot = static_cast<Shot *>(data);
            const int w = static_cast<int>(width);
            const int h = static_cast<int>(height);
            if (shot->width != w || shot->height != h) {
                shot->haveSize = false;
                if (!shot->capturing && shot->buffer) {
                    wl_buffer_destroy(shot->buffer);
                    shot->buffer = nullptr;
                    if (shot->data && shot->bytes) munmap(shot->data, shot->bytes);
                    shot->data = nullptr;
                    shot->bytes = 0;
                    shot->format = 0;
                }
            }
            shot->width = w;
            shot->height = h;
            shot->haveSize = w > 0 && h > 0;
        };
        listener.shm_format = [](void *data, ext_image_copy_capture_session_v1 *, uint32_t format) {
            auto *shot = static_cast<Shot *>(data);
            if (std::find(shot->formats.begin(), shot->formats.end(), format) == shot->formats.end())
                shot->formats.push_back(format);
        };
        listener.dmabuf_device = [](void *, ext_image_copy_capture_session_v1 *, wl_array *) {};
        listener.dmabuf_format = [](void *, ext_image_copy_capture_session_v1 *, uint32_t, wl_array *) {};
        listener.done = [](void *data, ext_image_copy_capture_session_v1 *) {
            auto *shot = static_cast<Shot *>(data);
            if (shot->capturing || shot->stopped) return;
            const uint32_t format = pickShmFormat(shot->formats);
            if (!shot->haveSize || format == 0) {
                shot->fails++;
                shot->retryAt = shotNow() + 800;
                if (shot->fails > 3) shot->stopped = true;
                return;
            }
            if (!shot->buffer || shot->format != format) shot->impl->allocShotBuffer(shot, format);
            if (!shot->buffer) {
                shot->stopped = true;
                shot->retryAt = shotNow() + 1000;
                return;
            }
            shot->impl->beginFrame(shot);
        };
        listener.stopped = [](void *data, ext_image_copy_capture_session_v1 *) {
            auto *shot = static_cast<Shot *>(data);
            shot->stopped = true;
            shot->capturing = false;
            shot->retryAt = shotNow() + 1000;
        };
        init = true;
    }
    ext_image_copy_capture_session_v1_add_listener(raw->session, &listener, raw);
    shots.push_back(std::move(shot));
}

void BarImpl::setCursor(quint32 serial, int scale) {
    if (!pointer || !shm || !compositor) return;
    // Match the session cursor. A hardcoded 24px theme swaps the pointer the
    // moment it enters the dock (this session uses XCURSOR_SIZE=48).
    scale = std::max(1, scale);
    const char *theme = std::getenv("XCURSOR_THEME");
    int size = 24;
    if (const char *raw = std::getenv("XCURSOR_SIZE")) {
        const int parsed = std::atoi(raw);
        if (parsed >= 8 && parsed <= 128) size = parsed;
    }
    const int px = size * scale;
    const QString key = QString::fromUtf8(theme ? theme : "") + QLatin1Char('|') + QString::number(px);
    if (!cursorTheme || cursorKey != key) {
        if (cursorTheme) wl_cursor_theme_destroy(cursorTheme);
        cursorTheme = wl_cursor_theme_load(theme && theme[0] ? theme : nullptr, px, shm);
        cursorPx = px;
        cursorKey = key;
    }
    if (!cursorTheme) return;
    wl_cursor *cursor = wl_cursor_theme_get_cursor(cursorTheme, "left_ptr");
    if (!cursor) cursor = wl_cursor_theme_get_cursor(cursorTheme, "default");
    if (!cursor || cursor->image_count < 1) return;
    wl_cursor_image *image = cursor->images[0];
    wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;
    if (!cursorSurface) cursorSurface = wl_compositor_create_surface(compositor);
    wl_surface_set_buffer_scale(cursorSurface, scale);
    wl_surface_attach(cursorSurface, buffer, 0, 0);
    wl_surface_damage_buffer(cursorSurface, 0, 0, static_cast<int32_t>(image->width), static_cast<int32_t>(image->height));
    wl_surface_commit(cursorSurface);
    wl_pointer_set_cursor(pointer, serial, cursorSurface, static_cast<int32_t>(image->hotspot_x / scale),
                          static_cast<int32_t>(image->hotspot_y / scale));
}

void BarImpl::configureShell(Out *out) {
    if (!out || !out->layer) return;
    const int scale = std::max(1, out->intScale);
    int logicalW = out->pixelW / scale;
    int logicalH = out->pixelH / scale;
    if (logicalW < 1) logicalW = std::max(1, out->pixelW);
    if (logicalH < 1) logicalH = std::max(1, out->pixelH);
    uint32_t anchor = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    switch (shell.edge) {
    case ShellSpec::Edge::Top:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        height = static_cast<uint32_t>(logicalH);
        break;
    case ShellSpec::Edge::Left:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        width = static_cast<uint32_t>(logicalW);
        break;
    case ShellSpec::Edge::Right:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        width = static_cast<uint32_t>(logicalW);
        break;
    case ShellSpec::Edge::Bottom:
    default:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        height = static_cast<uint32_t>(logicalH);
        break;
    }
    bool show = true;
    if (shell.output == QLatin1String("Active")) {
        show = shell.activeOutput < 0 || out->id == shell.activeOutput;
    } else if (!shell.output.isEmpty() && shell.output != QLatin1String("All")) {
        show = out->name == shell.output;
    }
    const int zone = !show ? 0 : (shell.exclusive ? std::max(1, shell.reserve) : -1);
    zwlr_layer_surface_v1_set_anchor(out->layer, anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(out->layer, zone);
    zwlr_layer_surface_v1_set_margin(out->layer, 0, 0, 0, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(out->layer, keyboardExclusive
                                                                    ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                                                                    : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_set_size(out->layer, width, height);
    wl_surface_commit(out->surface);
}

void BarImpl::render(Out *out) {
    if (!out->configured || !out->surface || inRender) return;
    if (out->logicalW < 1 || out->logicalH < 1) return;
    const double scale = out->prefScale > 0.1 ? out->prefScale : std::max(1, out->intScale);
    const int bw = std::max(1, static_cast<int>(std::lround(out->logicalW * scale)));
    const int bh = std::max(1, static_cast<int>(std::lround(out->logicalH * scale)));
    Buffer *buf = acquire(out, bw, bh);
    if (!buf) return;
    inRender = true;
    std::memset(buf->data, 0, buf->bytes);
    QImage image(static_cast<uchar *>(buf->data), bw, bh, buf->stride, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    QRegion input;
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        OutputGeom geom;
        geom.id = out->id;
        geom.logicalW = out->logicalW;
        geom.logicalH = out->logicalH;
        geom.outputW = out->logicalW;
        geom.outputH = out->logicalH;
        geom.scale = scale;
        geom.name = out->name;
        if (self->paint) input = self->paint(out->id, painter, geom);
    }
    if (out->viewport) wp_viewport_set_destination(out->viewport, out->logicalW, out->logicalH);
    wl_surface_set_buffer_scale(out->surface, 1);
    wl_surface_attach(out->surface, buf->buf, 0, 0);
    wl_surface_damage_buffer(out->surface, 0, 0, bw, bh);
    wl_region *region = wl_compositor_create_region(compositor);
    for (const QRect &rect : input) {
        if (rect.width() > 0 && rect.height() > 0) {
            wl_region_add(region, rect.x(), rect.y(), rect.width(), rect.height());
        }
    }
    wl_surface_set_input_region(out->surface, region);
    wl_region_destroy(region);
    wl_surface_commit(out->surface);
    buf->busy = true;
    self->mapped_ = true;
    self->lastFrame_ = image.copy();
    if (keepFrames) {
        static bool saved = false;
        if (!saved && !self->lastFrame_.isNull()) {
            saved = self->lastFrame_.save(QStringLiteral("/tmp/magic-dock-frame.png"));
            if (saved) std::fprintf(stderr, "magic-dock: wrote /tmp/magic-dock-frame.png\n");
        }
    }
    inRender = false;
    wl_display_flush(dpy);
}

void BarImpl::ensureSurface(Out *out) {
    if (out->surface || !layerShell || !compositor || out->pixelW <= 0 || out->pixelH <= 0) return;
    const int scale = std::max(1, out->intScale);
    out->surface = wl_compositor_create_surface(compositor);
    wl_surface_set_user_data(out->surface, out);
    if (viewporter) out->viewport = wp_viewporter_get_viewport(viewporter, out->surface);
    if (fracMgr) {
        out->frac = wp_fractional_scale_manager_v1_get_fractional_scale(fracMgr, out->surface);
        static wp_fractional_scale_v1_listener fracListener{};
        static bool fracInit = false;
        if (!fracInit) {
            fracListener.preferred_scale = [](void *data, wp_fractional_scale_v1 *, uint32_t scale120) {
                auto *surface = static_cast<Out *>(data);
                const double next = static_cast<double>(scale120) / 120.0;
                if (std::abs(next - surface->prefScale) < 0.01) return;
                surface->prefScale = next;
                std::fprintf(stderr, "magic-dock: output %s fractional scale %.2f\n",
                             qPrintable(surface->name), next);
                if (surface->configured) surface->impl->render(surface);
            };
            fracInit = true;
        }
        wp_fractional_scale_v1_add_listener(out->frac, &fracListener, out);
    }
    out->layer = zwlr_layer_shell_v1_get_layer_surface(layerShell, out->surface, out->output,
                                                       ZWLR_LAYER_SHELL_V1_LAYER_TOP, "magic-dock");
    static zwlr_layer_surface_v1_listener layerListener{};
    static bool layerInit = false;
    if (!layerInit) {
        layerListener.configure = [](void *data, zwlr_layer_surface_v1 *layer, uint32_t serial, uint32_t width,
                                     uint32_t height) {
            auto *surface = static_cast<Out *>(data);
            zwlr_layer_surface_v1_ack_configure(layer, serial);
            if (width > 0) surface->logicalW = static_cast<int>(width);
            if (height > 0) surface->logicalH = static_cast<int>(height);
            surface->configured = true;
            std::fprintf(stderr, "magic-dock: configured %s %dx%d\n", qPrintable(surface->name), surface->logicalW,
                         surface->logicalH);
            surface->impl->render(surface);
            if (surface->impl->self->onOutputs) surface->impl->self->onOutputs();
        };
        layerListener.closed = [](void *data, zwlr_layer_surface_v1 *) {
            auto *surface = static_cast<Out *>(data);
            std::fprintf(stderr, "magic-dock: layer surface closed on %s\n", qPrintable(surface->name));
            retireBuffers(surface);
            if (surface->layer) zwlr_layer_surface_v1_destroy(surface->layer);
            if (surface->frac) wp_fractional_scale_v1_destroy(surface->frac);
            if (surface->viewport) wp_viewport_destroy(surface->viewport);
            if (surface->surface) wl_surface_destroy(surface->surface);
            surface->layer = nullptr;
            surface->frac = nullptr;
            surface->viewport = nullptr;
            surface->surface = nullptr;
            surface->configured = false;
            surface->impl->ensureSurface(surface);
        };
        layerInit = true;
    }
    zwlr_layer_surface_v1_add_listener(out->layer, &layerListener, out);
    configureShell(out);
    std::fprintf(stderr, "magic-dock: layer surface on %s (mode %dx%d scale %d edge %d)\n",
                 qPrintable(out->name.isEmpty() ? QStringLiteral("output") : out->name), out->pixelW, out->pixelH, scale,
                 static_cast<int>(shell.edge));
}

// --- toplevels -----------------------------------------------------------------

void foreignClosed(void *data, ext_foreign_toplevel_handle_v1 *) {
    auto *win = static_cast<Win *>(data);
    if (!win->alive) return;
    win->impl->destroyWin(win);
}

void foreignDone(void *data, ext_foreign_toplevel_handle_v1 *) {
    auto *win = static_cast<Win *>(data);
    if (win->alive) win->impl->publish();
}

void foreignTitle(void *data, ext_foreign_toplevel_handle_v1 *, const char *title) {
    auto *win = static_cast<Win *>(data);
    win->title = QString::fromUtf8(title ? title : "");
}

void foreignAppId(void *data, ext_foreign_toplevel_handle_v1 *, const char *appId) {
    auto *win = static_cast<Win *>(data);
    win->appId = QString::fromUtf8(appId ? appId : "");
}

void foreignIdentifier(void *, ext_foreign_toplevel_handle_v1 *, const char *) {}

const ext_foreign_toplevel_handle_v1_listener &foreignListener() {
    static ext_foreign_toplevel_handle_v1_listener listener{};
    static bool init = false;
    if (!init) {
        listener.closed = foreignClosed;
        listener.done = foreignDone;
        listener.title = foreignTitle;
        listener.app_id = foreignAppId;
        listener.identifier = foreignIdentifier;
        init = true;
    }
    return listener;
}

void cosmicClosed(void *, zcosmic_toplevel_handle_v1 *) {}
void cosmicDone(void *, zcosmic_toplevel_handle_v1 *) {}
void cosmicTitle(void *, zcosmic_toplevel_handle_v1 *, const char *) {}
void cosmicAppId(void *, zcosmic_toplevel_handle_v1 *, const char *) {}

void cosmicOutputEnter(void *data, zcosmic_toplevel_handle_v1 *, wl_output *output) {
    auto *win = static_cast<Win *>(data);
    const int id = win->impl->idFor(output);
    if (id >= 0 && !win->outputs.contains(id)) win->outputs.append(id);
}

void cosmicOutputLeave(void *data, zcosmic_toplevel_handle_v1 *, wl_output *output) {
    auto *win = static_cast<Win *>(data);
    const int id = win->impl->idFor(output);
    win->outputs.removeAll(id);
    win->geom.remove(id);
}

void cosmicWorkspaceEnter(void *, zcosmic_toplevel_handle_v1 *, zcosmic_workspace_handle_v1 *) {}
void cosmicWorkspaceLeave(void *, zcosmic_toplevel_handle_v1 *, zcosmic_workspace_handle_v1 *) {}

void cosmicState(void *data, zcosmic_toplevel_handle_v1 *, wl_array *state) {
    auto *win = static_cast<Win *>(data);
    win->minimized = false;
    win->activated = false;
    win->fullscreen = false;
    win->maximized = false;
    auto *entry = static_cast<uint32_t *>(state->data);
    const auto *end = entry + state->size / sizeof(uint32_t);
    for (; entry < end; ++entry) {
        switch (*entry) {
        case ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
            win->minimized = true;
            break;
        case ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
            win->activated = true;
            break;
        case ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
            win->fullscreen = true;
            break;
        case ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
            win->maximized = true;
            break;
        default:
            break;
        }
    }
    if (win->alive) win->impl->publish();
}

void cosmicGeometry(void *data, zcosmic_toplevel_handle_v1 *, wl_output *output, int32_t x, int32_t y, int32_t w,
                    int32_t h) {
    auto *win = static_cast<Win *>(data);
    const int id = win->impl->idFor(output);
    if (id < 0) return;
    if (!win->outputs.contains(id)) win->outputs.append(id);
    const QRect next(x, y, w, h);
    if (win->geom.value(id) == next) return;
    win->geom.insert(id, next);
    if (win->alive) win->impl->publish();
}

void cosmicExtWorkspaceEnter(void *, zcosmic_toplevel_handle_v1 *, ext_workspace_handle_v1 *) {}
void cosmicExtWorkspaceLeave(void *, zcosmic_toplevel_handle_v1 *, ext_workspace_handle_v1 *) {}

const zcosmic_toplevel_handle_v1_listener &cosmicListener() {
    static zcosmic_toplevel_handle_v1_listener listener{};
    static bool init = false;
    if (!init) {
        listener.closed = cosmicClosed;
        listener.done = cosmicDone;
        listener.title = cosmicTitle;
        listener.app_id = cosmicAppId;
        listener.output_enter = cosmicOutputEnter;
        listener.output_leave = cosmicOutputLeave;
        listener.workspace_enter = cosmicWorkspaceEnter;
        listener.workspace_leave = cosmicWorkspaceLeave;
        listener.state = cosmicState;
        listener.geometry = cosmicGeometry;
        listener.ext_workspace_enter = cosmicExtWorkspaceEnter;
        listener.ext_workspace_leave = cosmicExtWorkspaceLeave;
        init = true;
    }
    return listener;
}

void listToplevel(void *data, ext_foreign_toplevel_list_v1 *, ext_foreign_toplevel_handle_v1 *handle) {
    auto *impl = static_cast<BarImpl *>(data);
    auto win = std::make_unique<Win>();
    win->impl = impl;
    win->foreign = handle;
    win->key = static_cast<quint64>(reinterpret_cast<uintptr_t>(handle));
    Win *raw = win.get();
    ext_foreign_toplevel_handle_v1_add_listener(handle, &foreignListener(), raw);
    if (impl->info) {
        raw->cosmic = zcosmic_toplevel_info_v1_get_cosmic_toplevel(impl->info, handle);
        zcosmic_toplevel_handle_v1_add_listener(raw->cosmic, &cosmicListener(), raw);
    }
    impl->wins.push_back(std::move(win));
}

void listFinished(void *, ext_foreign_toplevel_list_v1 *) {}

const ext_foreign_toplevel_list_v1_listener &listListener() {
    static ext_foreign_toplevel_list_v1_listener listener{};
    static bool init = false;
    if (!init) {
        listener.toplevel = listToplevel;
        listener.finished = listFinished;
        init = true;
    }
    return listener;
}

void managerCapabilities(void *, zcosmic_toplevel_manager_v1 *, wl_array *caps) {
    QStringList names;
    auto *entry = static_cast<uint32_t *>(caps->data);
    const auto *end = entry + caps->size / sizeof(uint32_t);
    for (; entry < end; ++entry) {
        switch (*entry) {
        case ZCOSMIC_TOPLEVEL_MANAGER_V1_ZCOSMIC_TOPLELEVEL_MANAGEMENT_CAPABILITIES_V1_CLOSE:
            names << QStringLiteral("close");
            break;
        case ZCOSMIC_TOPLEVEL_MANAGER_V1_ZCOSMIC_TOPLELEVEL_MANAGEMENT_CAPABILITIES_V1_ACTIVATE:
            names << QStringLiteral("activate");
            break;
        case ZCOSMIC_TOPLEVEL_MANAGER_V1_ZCOSMIC_TOPLELEVEL_MANAGEMENT_CAPABILITIES_V1_MINIMIZE:
            names << QStringLiteral("minimize");
            break;
        default:
            break;
        }
    }
    std::fprintf(stderr, "magic-dock: toplevel caps %s\n", qPrintable(names.join(", ")));
}

// --- pointer / keyboard --------------------------------------------------------

void pointerEnter(void *data, wl_pointer *pointer, uint32_t serial, wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    auto *impl = static_cast<BarImpl *>(data);
    auto *out = static_cast<Out *>(wl_surface_get_user_data(surface));
    if (!out) return;
    const int scale = out->prefScale > 0.1 ? static_cast<int>(std::lround(out->prefScale)) : std::max(1, out->intScale);
    impl->setCursor(serial, scale);
    if (!impl->self->onPointer) return;
    PointerEv ev;
    ev.type = PointerEv::Type::Enter;
    ev.outputId = out->id;
    ev.x = wl_fixed_to_double(x);
    ev.y = wl_fixed_to_double(y);
    impl->self->onPointer(ev);
}

void pointerLeave(void *data, wl_pointer *, uint32_t, wl_surface *surface) {
    auto *impl = static_cast<BarImpl *>(data);
    auto *out = surface ? static_cast<Out *>(wl_surface_get_user_data(surface)) : nullptr;
    if (!out || !impl->self->onPointer) return;
    PointerEv ev;
    ev.type = PointerEv::Type::Leave;
    ev.outputId = out->id;
    impl->self->onPointer(ev);
}

void pointerMotion(void *data, wl_pointer *, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    auto *impl = static_cast<BarImpl *>(data);
    if (!impl->self->onPointer || impl->outputs.empty()) return;
    // Motion has no surface. The last entered output is tracked by the dock via Enter.
    PointerEv ev;
    ev.type = PointerEv::Type::Motion;
    ev.outputId = -1;
    ev.x = wl_fixed_to_double(x);
    ev.y = wl_fixed_to_double(y);
    ev.time = time;
    impl->self->onPointer(ev);
}

void pointerButton(void *data, wl_pointer *, uint32_t, uint32_t time, uint32_t button, uint32_t state) {
    auto *impl = static_cast<BarImpl *>(data);
    if (!impl->self->onPointer) return;
    PointerEv ev;
    ev.type = PointerEv::Type::Button;
    ev.outputId = -1;
    ev.button = button;
    ev.pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    ev.time = time;
    impl->self->onPointer(ev);
}

void pointerAxis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
void pointerFrame(void *, wl_pointer *) {}
void pointerAxisSource(void *, wl_pointer *, uint32_t) {}
void pointerAxisStop(void *, wl_pointer *, uint32_t, uint32_t) {}
void pointerAxisDiscrete(void *, wl_pointer *, uint32_t, int32_t) {}

void keyMap(void *, wl_keyboard *, uint32_t, int32_t fd, uint32_t) {
    if (fd >= 0) close(fd);
}
void keyEnter(void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {}
void keyLeave(void *, wl_keyboard *, uint32_t, wl_surface *) {}
void keyKey(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    auto *impl = static_cast<BarImpl *>(data);
    if (!impl->self->onKey) return;
    KeyEv ev;
    ev.keycode = key;
    ev.pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (!impl->outputs.empty()) ev.outputId = impl->outputs.front()->id;
    impl->self->onKey(ev);
}
void keyModifiers(void *, wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void keyRepeat(void *, wl_keyboard *, int32_t, int32_t) {}

// --- outputs / registry --------------------------------------------------------

void outputGeometry(void *data, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *,
                    int32_t transform) {
    static_cast<Out *>(data)->transform = transform;
}

void outputMode(void *data, wl_output *, uint32_t flags, int32_t width, int32_t height, int32_t) {
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    auto *out = static_cast<Out *>(data);
    out->modeW = width;
    out->modeH = height;
    out->currentMode = true;
}

void outputDone(void *data, wl_output *) {
    auto *out = static_cast<Out *>(data);
    out->pixelW = out->modeW;
    out->pixelH = out->modeH;
    if ((out->transform & 1) && out->pixelW > 0) std::swap(out->pixelW, out->pixelH);
    out->impl->ensureSurface(out);
}

void outputScale(void *data, wl_output *, int32_t scale) {
    auto *out = static_cast<Out *>(data);
    out->intScale = std::max(1, scale);
}

void outputName(void *data, wl_output *, const char *name) {
    static_cast<Out *>(data)->name = QString::fromUtf8(name ? name : "");
}

void outputDescription(void *, wl_output *, const char *) {}

const wl_output_listener &outputListener() {
    static wl_output_listener listener{};
    static bool init = false;
    if (!init) {
        listener.geometry = outputGeometry;
        listener.mode = outputMode;
        listener.done = outputDone;
        listener.scale = outputScale;
        listener.name = outputName;
        listener.description = outputDescription;
        init = true;
    }
    return listener;
}

void shmFormat(void *data, wl_shm *, uint32_t format) {
    if (format == WL_SHM_FORMAT_ARGB8888) static_cast<BarImpl *>(data)->hasArgb = true;
}

void seatCapabilities(void *data, wl_seat *seat, uint32_t caps) {
    auto *impl = static_cast<BarImpl *>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !impl->pointer) {
        impl->pointer = wl_seat_get_pointer(seat);
        static wl_pointer_listener listener{};
        static bool init = false;
        if (!init) {
            listener.enter = pointerEnter;
            listener.leave = pointerLeave;
            listener.motion = pointerMotion;
            listener.button = pointerButton;
            listener.axis = pointerAxis;
            listener.frame = pointerFrame;
            listener.axis_source = pointerAxisSource;
            listener.axis_stop = pointerAxisStop;
            listener.axis_discrete = pointerAxisDiscrete;
            init = true;
        }
        wl_pointer_add_listener(impl->pointer, &listener, impl);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !impl->keyboard) {
        impl->keyboard = wl_seat_get_keyboard(seat);
        static wl_keyboard_listener listener{};
        static bool init = false;
        if (!init) {
            listener.keymap = keyMap;
            listener.enter = keyEnter;
            listener.leave = keyLeave;
            listener.key = keyKey;
            listener.modifiers = keyModifiers;
            listener.repeat_info = keyRepeat;
            init = true;
        }
        wl_keyboard_add_listener(impl->keyboard, &listener, impl);
    }
}

void seatName(void *, wl_seat *, const char *) {}

void registryGlobal(void *data, wl_registry *registry, uint32_t name, const char *iface, uint32_t version) {
    auto *impl = static_cast<BarImpl *>(data);
    const QString interface = QString::fromUtf8(iface);
    auto bind = [&](const wl_interface *def, uint32_t ver) {
        return wl_registry_bind(registry, name, def, std::min(version, ver));
    };
    if (interface == QLatin1String("wl_compositor")) {
        impl->compositor = static_cast<wl_compositor *>(bind(&wl_compositor_interface, 4));
    } else if (interface == QLatin1String("wl_shm")) {
        impl->shm = static_cast<wl_shm *>(bind(&wl_shm_interface, 1));
        static wl_shm_listener listener{};
        static bool init = false;
        if (!init) {
            listener.format = shmFormat;
            init = true;
        }
        wl_shm_add_listener(impl->shm, &listener, impl);
    } else if (interface == QLatin1String("wl_seat")) {
        impl->seat = static_cast<wl_seat *>(bind(&wl_seat_interface, 7));
        static wl_seat_listener listener{};
        static bool init = false;
        if (!init) {
            listener.capabilities = seatCapabilities;
            listener.name = seatName;
            init = true;
        }
        wl_seat_add_listener(impl->seat, &listener, impl);
    } else if (interface == QLatin1String("wl_output")) {
        auto out = std::make_unique<Out>();
        out->impl = impl;
        out->id = impl->nextId++;
        out->output = static_cast<wl_output *>(bind(&wl_output_interface, 4));
        wl_output_add_listener(out->output, &outputListener(), out.get());
        impl->outputs.push_back(std::move(out));
    } else if (interface == QLatin1String("zwlr_layer_shell_v1")) {
        impl->layerShell = static_cast<zwlr_layer_shell_v1 *>(bind(&zwlr_layer_shell_v1_interface, 4));
    } else if (interface == QLatin1String("ext_foreign_toplevel_list_v1")) {
        impl->toplevelList =
            static_cast<ext_foreign_toplevel_list_v1 *>(bind(&ext_foreign_toplevel_list_v1_interface, 1));
        ext_foreign_toplevel_list_v1_add_listener(impl->toplevelList, &listListener(), impl);
    } else if (interface == QLatin1String("zcosmic_toplevel_info_v1")) {
        impl->info = static_cast<zcosmic_toplevel_info_v1 *>(bind(&zcosmic_toplevel_info_v1_interface, 3));
    } else if (interface == QLatin1String("zcosmic_toplevel_manager_v1")) {
        impl->manager = static_cast<zcosmic_toplevel_manager_v1 *>(bind(&zcosmic_toplevel_manager_v1_interface, 4));
        static zcosmic_toplevel_manager_v1_listener listener{};
        static bool init = false;
        if (!init) {
            listener.capabilities = managerCapabilities;
            init = true;
        }
        zcosmic_toplevel_manager_v1_add_listener(impl->manager, &listener, impl);
    } else if (interface == QLatin1String("wp_fractional_scale_manager_v1")) {
        impl->fracMgr =
            static_cast<wp_fractional_scale_manager_v1 *>(bind(&wp_fractional_scale_manager_v1_interface, 1));
    } else if (interface == QLatin1String("wp_viewporter")) {
        impl->viewporter = static_cast<wp_viewporter *>(bind(&wp_viewporter_interface, 1));
    } else if (interface == QLatin1String("ext_image_copy_capture_manager_v1")) {
        impl->captureMgr =
            static_cast<ext_image_copy_capture_manager_v1 *>(bind(&ext_image_copy_capture_manager_v1_interface, 1));
    } else if (interface == QLatin1String("ext_foreign_toplevel_image_capture_source_manager_v1")) {
        impl->sourceMgr = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1 *>(
            bind(&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
    }
}

void registryRemove(void *, wl_registry *, uint32_t) {}

bool Backend::init() {
    d = new BarImpl();
    d->self = this;
    d->keepFrames = qEnvironmentVariableIsSet("COSMIC_BAR_KEEP_FRAME");
    d->dpy = wl_display_connect(nullptr);
    if (!d->dpy) {
        std::fprintf(stderr, "magic-dock: could not connect to Wayland (%s)\n",
                     qgetenv("WAYLAND_DISPLAY").constData());
        return false;
    }
    d->registry = wl_display_get_registry(d->dpy);
    static wl_registry_listener listener{};
    static bool regInit = false;
    if (!regInit) {
        listener.global = registryGlobal;
        listener.global_remove = registryRemove;
        regInit = true;
    }
    wl_registry_add_listener(d->registry, &listener, d);
    wl_display_roundtrip(d->dpy);
    wl_display_roundtrip(d->dpy);
    if (!d->compositor || !d->shm || !d->layerShell) {
        std::fprintf(stderr, "magic-dock: compositor is missing compositor, shm, or layer-shell\n");
        return false;
    }
    if (!d->hasArgb) std::fprintf(stderr, "magic-dock: ARGB8888 not advertised, trying it anyway\n");
    if (!d->toplevelList) {
        std::fprintf(stderr, "magic-dock: no ext_foreign_toplevel_list_v1; running apps will not show\n");
    }
    if (!d->info) std::fprintf(stderr, "magic-dock: no zcosmic_toplevel_info_v1\n");
    if (!d->manager) std::fprintf(stderr, "magic-dock: no zcosmic_toplevel_manager_v1; focus and minimize disabled\n");
    if (!d->captureMgr || !d->sourceMgr) {
        std::fprintf(stderr, "magic-dock: no image-copy capture; window previews will use icons\n");
    }
    for (auto &out : d->outputs) d->ensureSurface(out.get());
    wl_display_roundtrip(d->dpy);
    wl_display_flush(d->dpy);

    const int fd = wl_display_get_fd(d->dpy);
    auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, QCoreApplication::instance());
    QObject::connect(notifier, &QSocketNotifier::activated, QCoreApplication::instance(), [this](int) {
        if (wl_display_dispatch(d->dpy) < 0) {
            std::fprintf(stderr, "magic-dock: Wayland connection closed\n");
            QCoreApplication::quit();
        }
    });
    auto *flusher = new QTimer(QCoreApplication::instance());
    QObject::connect(flusher, &QTimer::timeout, QCoreApplication::instance(), [this] {
        if (d && d->dpy) wl_display_flush(d->dpy);
    });
    flusher->start(32);
    std::fprintf(stderr, "magic-dock: ready, %zu output(s)\n", d->outputs.size());
    return true;
}

Backend::~Backend() {
    if (!d) return;
    d->shots.clear();
    if (d->dpy) wl_display_disconnect(d->dpy);
    d->dpy = nullptr;
    delete d;
    d = nullptr;
}

void Backend::requestRender() {
    if (!d) return;
    for (auto &out : d->outputs) d->render(out.get());
}

void Backend::applyShell(const ShellSpec &spec) {
    if (!d) return;
    d->shell = spec;
    for (auto &out : d->outputs) {
        if (out->layer) d->configureShell(out.get());
    }
    wl_display_flush(d->dpy);
}

void Backend::setKeyboardExclusive(bool exclusive) {
    if (!d || d->keyboardExclusive == exclusive) return;
    d->keyboardExclusive = exclusive;
    for (auto &out : d->outputs) {
        if (!out->layer) continue;
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            out->layer, exclusive ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                                  : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    }
    requestRender();
}

std::vector<OutputGeom> Backend::outputs() const {
    std::vector<OutputGeom> list;
    if (!d) return list;
    for (const auto &out : d->outputs) {
        OutputGeom geom;
        geom.id = out->id;
        geom.logicalW = out->logicalW;
        geom.logicalH = out->logicalH;
        geom.outputW = out->logicalW;
        geom.outputH = out->logicalH;
        geom.scale = out->prefScale > 0.1 ? out->prefScale : std::max(1, out->intScale);
        geom.name = out->name;
        list.push_back(geom);
    }
    return list;
}

void Backend::activate(quint64 key) {
    if (!d || !d->manager || !d->seat) return;
    Win *win = d->winByKey(key);
    if (!win || !win->cosmic) return;
    zcosmic_toplevel_manager_v1_activate(d->manager, win->cosmic, d->seat);
    wl_display_flush(d->dpy);
}

void Backend::minimize(quint64 key) {
    if (!d || !d->manager) return;
    Win *win = d->winByKey(key);
    if (!win || !win->cosmic) return;
    zcosmic_toplevel_manager_v1_set_minimized(d->manager, win->cosmic);
    wl_display_flush(d->dpy);
}

void Backend::unminimize(quint64 key) {
    if (!d || !d->manager || !d->seat) return;
    Win *win = d->winByKey(key);
    if (!win || !win->cosmic) return;
    zcosmic_toplevel_manager_v1_unset_minimized(d->manager, win->cosmic);
    zcosmic_toplevel_manager_v1_activate(d->manager, win->cosmic, d->seat);
    wl_display_flush(d->dpy);
}

void Backend::closeWindow(quint64 key) {
    if (!d || !d->manager) return;
    Win *win = d->winByKey(key);
    if (!win || !win->cosmic) return;
    zcosmic_toplevel_manager_v1_close(d->manager, win->cosmic);
    wl_display_flush(d->dpy);
}

void Backend::watchThumbnails(const std::vector<quint64> &keys) {
    if (!d) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    std::vector<Shot *> drop;
    for (const auto &shot : d->shots) {
        const bool want = std::find(keys.begin(), keys.end(), shot->key) != keys.end();
        Win *win = d->winByKey(shot->key);
        if (!want || !win || !win->alive || win->minimized) drop.push_back(shot.get());
        else if (shot->stopped && now >= shot->retryAt) drop.push_back(shot.get());
    }
    for (Shot *shot : drop) d->dropShot(shot);
    if (d->captureMgr && d->sourceMgr) {
        for (quint64 key : keys) {
            Win *win = d->winByKey(key);
            if (!win || !win->alive || !win->foreign || win->minimized) continue;
            Shot *shot = d->shotByKey(key);
            if (!shot) d->ensureShot(win);
            else if (!shot->capturing && !shot->frame && shot->buffer && !shot->stopped && now >= shot->retryAt)
                d->beginFrame(shot);
        }
    }
    if (d->dpy) wl_display_flush(d->dpy);
}

Backend::Thumbnail Backend::thumbnail(quint64 key) const {
    Thumbnail out;
    if (!d) return out;
    const Shot *shot = d->shotByKey(key);
    if (!shot) return out;
    out.image = shot->image;
    out.serial = shot->serial;
    return out;
}

void Backend::setRectangle(quint64 key, int outputId, const QRect &rect) {
    if (!d || !d->manager) return;
    Win *win = d->winByKey(key);
    Out *out = d->outById(outputId);
    if (!win || !win->cosmic || !out || !out->surface) return;
    zcosmic_toplevel_manager_v1_set_rectangle(d->manager, win->cosmic, out->surface, rect.x(), rect.y(), rect.width(),
                                              rect.height());
}
