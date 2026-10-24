#pragma once

#include <QImage>
#include <QPainter>
#include <QRect>
#include <QRegion>
#include <QString>
#include <functional>
#include <vector>

struct WindowState {
    quint64 key = 0;
    QString appId;
    QString title;
    bool minimized = false;
    bool activated = false;
    bool fullscreen = false;
    bool maximized = false;
    bool onOutput = false;
    int outputId = -1;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool hasGeom = false;
};

struct PointerEv {
    enum class Type { Enter, Leave, Motion, Button };
    Type type = Type::Motion;
    int outputId = 0;
    double x = 0;
    double y = 0;
    quint32 button = 0;
    bool pressed = false;
    quint32 time = 0;
};

struct KeyEv {
    int outputId = 0;
    quint32 keycode = 0;  // Linux evdev code
    bool pressed = false;
    bool repeat = false;
};

struct OutputGeom {
    int id = 0;
    int logicalW = 0;
    int logicalH = 0;
    int outputW = 0;
    int outputH = 0;
    double scale = 1;
    QString name;
};

// Where the dock sits. Pop!_OS writes the same choice under the Dock anchor file.
struct ShellSpec {
    enum class Edge { Top, Bottom, Left, Right };
    Edge edge = Edge::Bottom;
    bool exclusive = false;
    int reserve = 0;
    QString output = QStringLiteral("All");  // All, Active, or a connector name
    int activeOutput = -1;
};

struct BarImpl;

class Backend {
public:
    using PaintFn = std::function<QRegion(int outputId, QPainter &, const OutputGeom &)>;

    bool init();
    void requestRender();
    void setKeyboardExclusive(bool exclusive);
    // Anchor, exclusive zone, and which monitor. Safe to call again when settings change.
    void applyShell(const ShellSpec &spec);

    const std::vector<WindowState> &windows() const { return windows_; }
    std::vector<OutputGeom> outputs() const;

    void activate(quint64 key);
    void minimize(quint64 key);
    void unminimize(quint64 key);
    void closeWindow(quint64 key);
    void setRectangle(quint64 key, int outputId, const QRect &rect);

    // Live window thumbnails for the multi-window picker. Empty when capture is unavailable.
    struct Thumbnail {
        QImage image;
        quint64 serial = 0;
    };
    void watchThumbnails(const std::vector<quint64> &keys);
    Thumbnail thumbnail(quint64 key) const;

    std::function<void()> onWindows;
    std::function<void()> onThumbnails;
    std::function<void(const PointerEv &)> onPointer;
    std::function<void(const KeyEv &)> onKey;
    std::function<void()> onOutputs;
    PaintFn paint;

    QImage lastFrame() const { return lastFrame_; }
    bool mapped() const { return mapped_; }
    ~Backend();

private:
    friend struct BarImpl;
    BarImpl *d = nullptr;
    std::vector<WindowState> windows_;
    QImage lastFrame_;
    bool mapped_ = false;
    friend struct Impl;
};
