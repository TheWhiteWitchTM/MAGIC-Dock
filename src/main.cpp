#include "apps.hpp"
#include "backend.hpp"
#include "dock.hpp"
#include "settings.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>

#include <unistd.h>

#include <csignal>
#include <cstdio>

namespace {
volatile sig_atomic_t gDump = 0;
void onDump(int) { gDump = 1; }

// A pid file from the previous boot can name an unrelated process after reboot.
bool liveDock(int pid) {
    if (pid <= 0 || pid == static_cast<int>(::getpid())) return false;
    const QString dir = QStringLiteral("/proc/%1/").arg(pid);
    QFile comm(dir + QStringLiteral("comm"));
    if (!comm.open(QIODevice::ReadOnly)) return false;
    if (comm.readAll().trimmed() != "magic-dock") return false;
    QFile stat(dir + QStringLiteral("stat"));
    if (!stat.open(QIODevice::ReadOnly)) return false;
    const QByteArray text = stat.readAll();
    const int end = text.lastIndexOf(')');
    if (end < 0 || end + 2 >= text.size()) return false;
    return text.at(end + 2) != 'Z';
}

struct PidLock {
    QString path;
    bool held = false;
    ~PidLock() {
        if (held) QFile::remove(path);
    }
};
}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGUSR1, onDump);
    // The dock draws its own layer surface. Qt must not open a window.
    // These are unset after startup. launch() also strips them from children,
    // because a Qt app that inherits offscreen or this process's plugin path
    // starts with no window.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    QApplication app(argc, argv);
    qunsetenv("QT_QPA_PLATFORM");
    qunsetenv("QT_ENABLE_HIGHDPI_SCALING");
    QIcon::setThemeName(QStringLiteral("Cosmic"));
    QStringList paths = QIcon::themeSearchPaths();
    paths.prepend(QStringLiteral("/usr/share/icons"));
    paths.prepend(QDir::homePath() + QStringLiteral("/.local/share/icons"));
    paths.prepend(QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/icons"));
    QIcon::setThemeSearchPaths(paths);

    AppIndex apps;
    apps.scan();
    Dock dock(&apps);

    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--self-test"))) {
        int code = runSettingsSelfTest();
        if (runAppsSelfTest() != 0) code = 1;
        if (dock.selfTest() != 0) code = 1;
        return code;
    }
    if (args.contains(QStringLiteral("--preview"))) {
        const int index = args.indexOf(QStringLiteral("--preview"));
        const QString dir = index + 1 < args.size() ? args.at(index + 1) : QStringLiteral("build/preview");
        return dock.preview(dir);
    }

    // A session started from the source tree would otherwise hand that folder
    // to every app. Home is where a desktop launcher starts a program.
    QDir::setCurrent(QDir::homePath());

    const QString pidPath = QDir::homePath() + QStringLiteral("/.config/magic-dock/magic-dock.pid");
    QDir().mkpath(QFileInfo(pidPath).absolutePath());
    PidLock pidLock;
    pidLock.path = pidPath;
    {
        QFile existing(pidPath);
        if (existing.open(QIODevice::ReadOnly)) {
            bool ok = false;
            const int pid = QString::fromUtf8(existing.readAll()).trimmed().toInt(&ok);
            if (ok && liveDock(pid)) {
                std::fprintf(stderr, "magic-dock: already running (pid %d)\n", pid);
                return 0;
            }
        }
        QFile pidFile(pidPath);
        if (pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pidFile.write(QByteArray::number(static_cast<int>(::getpid())));
            pidLock.held = true;
        }
    }

    dock.load();
    Backend backend;
    dock.onActivate = [&backend](quint64 key) { backend.activate(key); };
    dock.onMinimize = [&backend](quint64 key) { backend.minimize(key); };
    dock.onUnminimize = [&backend](quint64 key) { backend.unminimize(key); };
    dock.onClose = [&backend](quint64 key) { backend.closeWindow(key); };
    dock.onRect = [&backend](quint64 key, int outputId, QRect rect) { backend.setRectangle(key, outputId, rect); };
    backend.onThumbnails = [&dock, &backend]() {
        for (quint64 key : dock.previewKeys()) {
            const Backend::Thumbnail thumb = backend.thumbnail(key);
            dock.noteThumbnail(key, thumb.image, thumb.serial);
        }
        backend.requestRender();
    };

    backend.paint = [&dock](int outputId, QPainter &painter, const OutputGeom &geom) {
        return dock.paint(outputId, painter, geom);
    };
    backend.onPointer = [&dock, &backend](const PointerEv &event) {
        dock.pointer(event);
        if (dock.tick()) backend.requestRender();
        else backend.requestRender();
    };
    backend.onKey = [&dock, &backend](const KeyEv &event) {
        dock.key(event);
        backend.requestRender();
    };
    backend.acceptsDrop = [&dock](int outputId, double x, double y) { return dock.acceptsFileDrop(outputId, x, y); };
    backend.onFileDrag = [&dock, &backend](int outputId, double x, double y, bool active) {
        dock.fileDrag(outputId, x, y, active);
        if (dock.tick()) backend.requestRender();
        else backend.requestRender();
    };
    backend.onFileDrop = [&dock, &backend](const QStringList &uris) {
        dock.dropFiles(uris);
        backend.requestRender();
    };
    backend.onWindows = [&dock, &backend]() {
        dock.setWindows(backend.windows());
        backend.requestRender();
    };

    if (!backend.init()) return 1;
    backend.applyShell(dock.shellSpec());
    dock.setWindows(backend.windows());
    backend.requestRender();

    bool keyboard = false;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&dock, &backend, &keyboard]() {
        const bool want = dock.wantsKeyboard();
        if (want != keyboard) {
            keyboard = want;
            backend.setKeyboardExclusive(want);
        }
        backend.watchThumbnails(dock.previewKeys());
        bool thumbs = false;
        for (quint64 key : dock.previewKeys()) {
            const Backend::Thumbnail thumb = backend.thumbnail(key);
            if (dock.noteThumbnail(key, thumb.image, thumb.serial)) thumbs = true;
        }
        if (dock.takeShellChange()) backend.applyShell(dock.shellSpec());
        if (dock.tick() || thumbs) backend.requestRender();
        if (gDump) {
            gDump = 0;
            const QImage frame = backend.lastFrame();
            if (!frame.isNull()) {
                frame.save(QStringLiteral("/tmp/magic-dock-frame.png"));
                std::fprintf(stderr, "magic-dock: wrote /tmp/magic-dock-frame.png\n");
            }
        }
    });
    timer.start(16);
    std::fprintf(stderr, "magic-dock: dock running\n");
    return app.exec();
}
