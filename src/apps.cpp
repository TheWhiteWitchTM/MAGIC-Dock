#include "apps.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <cstdio>

namespace {

QString unescapeDesktop(QString value) {
    QString out;
    out.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const QChar n = value[++i];
            if (n == 's') out += ' ';
            else if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == 'r') out += '\r';
            else out += n;
        } else {
            out += value[i];
        }
    }
    return out;
}

void addRoot(QStringList &roots, const QString &path) {
    if (path.isEmpty() || roots.contains(path)) return;
    roots << path;
}

QStringList iconSearchRoots(const QString &extra) {
    QStringList roots;
    const QString home = QDir::homePath();
    // User hicolor comes first. Chrome, Edge, and Brave drop web-app icons there
    // by name. COSMIC misses them until gtk-update-icon-cache is run by hand.
    // This lookup reads the files, so a stale icon-theme.cache does not matter.
    addRoot(roots, extra);
    addRoot(roots, home + QStringLiteral("/.local/share/icons/hicolor"));
    addRoot(roots, home + QStringLiteral("/.local/share/icons"));
    addRoot(roots, home + QStringLiteral("/.local/share/pixmaps"));
    addRoot(roots, QStringLiteral("/usr/share/icons/Cosmic"));
    addRoot(roots, QStringLiteral("/usr/share/icons/Pop"));
    addRoot(roots, QStringLiteral("/usr/share/icons/hicolor"));
    addRoot(roots, QStringLiteral("/usr/share/pixmaps"));
    addRoot(roots, home + QStringLiteral("/.local/share/flatpak/exports/share/icons/hicolor"));
    addRoot(roots, QStringLiteral("/var/lib/flatpak/exports/share/icons/hicolor"));
    const QStringList xdg = QString::fromLocal8Bit(qgetenv("XDG_DATA_DIRS")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &dir : xdg) {
        addRoot(roots, dir + QStringLiteral("/icons/hicolor"));
        addRoot(roots, dir + QStringLiteral("/pixmaps"));
    }
    const QStringList browsers = {QStringLiteral("com.google.Chrome"), QStringLiteral("org.chromium.Chromium"),
                                  QStringLiteral("com.brave.Browser"), QStringLiteral("com.microsoft.Edge")};
    for (const QString &id : browsers) {
        addRoot(roots, home + QStringLiteral("/.var/app/") + id + QStringLiteral("/data/icons/hicolor"));
        addRoot(roots, home + QStringLiteral("/.var/app/") + id + QStringLiteral("/.local/share/icons/hicolor"));
    }
    return roots;
}

QString webToken(const QString &text) {
    static const QRegularExpression re(QStringLiteral("(?:chrome|chromium|msedge|brave|crx)[_-]([a-z]{16,32})"));
    const auto match = re.match(text.toLower());
    if (!match.hasMatch()) return {};
    return match.captured(1);
}

QString chromeProfileIcon(const QString &name) {
    const QString token = webToken(name);
    if (token.isEmpty()) return {};
    const QString home = QDir::homePath();
    const QStringList profiles = {home + QStringLiteral("/.config/google-chrome"),
                                  home + QStringLiteral("/.config/chromium"),
                                  home + QStringLiteral("/.config/BraveSoftware/Brave-Browser"),
                                  home + QStringLiteral("/.config/microsoft-edge")};
    QString best;
    int bestArea = 0;
    for (const QString &profile : profiles) {
        QDir root(profile);
        if (!root.exists()) continue;
        const auto subs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &sub : subs) {
            const QString icons = profile + QLatin1Char('/') + sub +
                                  QStringLiteral("/Web Applications/Manifest Resources/") + token +
                                  QStringLiteral("/Icons");
            QDir iconDir(icons);
            if (!iconDir.exists()) continue;
            const auto files = iconDir.entryInfoList({QStringLiteral("*.png"), QStringLiteral("*.webp"), QStringLiteral("*.ico")},
                                                     QDir::Files);
            for (const QFileInfo &fi : files) {
                const QImage image(fi.absoluteFilePath());
                const int area = image.width() * image.height();
                if (area > bestArea) {
                    bestArea = area;
                    best = fi.absoluteFilePath();
                }
            }
        }
    }
    return best;
}

const char *kContexts[] = {"apps", "places", "devices", "categories", "status", "actions", "mimetypes"};
const int kSizes[] = {512, 256, 192, 180, 144, 128, 96, 72, 64, 48, 32, 24, 22, 16};

QStringList launchNames(const QString &appId) {
    QStringList names;
    auto add = [&](const QString &name) {
        const QString trimmed = name.trimmed();
        if (trimmed.isEmpty() || names.contains(trimmed)) return;
        names << trimmed;
    };
    add(appId);
    add(appId.section(QLatin1Char('.'), -1));
    add(appId.section(QLatin1Char('-'), -1));
    return names;
}

QString discoverProcess(const QStringList &names) {
    QDir proc(QStringLiteral("/proc"));
    const auto entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    const QString self = QString::number(QCoreApplication::applicationPid());
    for (const QString &pid : entries) {
        bool ok = false;
        pid.toInt(&ok);
        if (!ok || pid == self) continue;
        QFile command(QStringLiteral("/proc/") + pid + QStringLiteral("/cmdline"));
        if (!command.open(QIODevice::ReadOnly)) continue;
        const QByteArray raw = command.readAll();
        QStringList tokens;
        QByteArray cur;
        for (char byte : raw) {
            if (byte == '\0') {
                if (!cur.isEmpty()) tokens << QString::fromUtf8(cur);
                cur.clear();
            } else {
                cur.append(byte);
            }
        }
        if (!cur.isEmpty()) tokens << QString::fromUtf8(cur);
        const QString link = QFile::symLinkTarget(QStringLiteral("/proc/") + pid + QStringLiteral("/exe"));
        QString chosen;
        for (const QString &token : tokens) {
            const QString base = QFileInfo(token).fileName();
            if (!names.contains(base) && !names.contains(token)) continue;
            if (base == QLatin1String("bash") || base == QLatin1String("sh") || base == QLatin1String("dash") ||
                base == QLatin1String("gio") || base == QLatin1String("gtk-launch") || base == QLatin1String("flatpak") ||
                base == QLatin1String("bwrap")) {
                continue;
            }
            if (QFileInfo(token).isExecutable()) {
                chosen = token;
                break;
            }
        }
        if (chosen.isEmpty() && names.contains(QFileInfo(link).fileName()) && QFileInfo(link).isExecutable()) chosen = link;
        if (!chosen.isEmpty()) return chosen;
    }
    return {};
}

QString discoverProject(const QString &base) {
    if (base.isEmpty() || base.contains(QLatin1Char('/'))) return {};
    const QString home = QDir::homePath();
    const QStringList candidates = {home + QStringLiteral("/Projects/") + base + QLatin1Char('/') + base,
                                    home + QStringLiteral("/Projects/") + base + QStringLiteral("/dist/") + base,
                                    home + QStringLiteral("/Projects/") + base + QStringLiteral("/dist/bin/") + base,
                                    home + QStringLiteral("/.local/bin/") + base};
    for (const QString &path : candidates) {
        if (QFileInfo(path).isExecutable()) return path;
    }
    return {};
}

QString launchDirectory(const QString &requested) {
    if (!requested.isEmpty() && QFileInfo(requested).isDir()) return QFileInfo(requested).absoluteFilePath();
    return QDir::homePath();
}

// An absolute path must exist. A bare name must be on PATH. gio cannot load a
// desktop file whose command is missing, and it exits before any window opens.
QString resolveProgram(const QString &program) {
    if (program.isEmpty()) return {};
    if (program.contains(QLatin1Char('/'))) {
        const QFileInfo info(program);
        return info.isExecutable() ? info.absoluteFilePath() : QString();
    }
    return QStandardPaths::findExecutable(program);
}

bool isCosmicTerm(const QString &program) {
    return QFileInfo(program).fileName() == QLatin1String("cosmic-term");
}

QStringList commandArgs(const QString &program, const QStringList &args, const QString &dir) {
    if (!isCosmicTerm(program)) return args;
    for (const QString &arg : args) {
        if (arg == QLatin1String("-w") || arg == QLatin1String("--working-directory")) return args;
    }
    QStringList out = args;
    out << QStringLiteral("--working-directory") << dir;
    return out;
}

}  // namespace

bool startDetachedApp(const QString &program, const QStringList &args, const QString &workdir, const char *how) {
    if (program.isEmpty()) return false;
    // The dock process is often started from its source tree. An empty Path
    // must not inherit that directory, or a terminal opens in the dock folder.
    const QString dir = launchDirectory(workdir);
    QProcess process;
    process.setProcessEnvironment(launchEnvironment());
    process.setProgram(program);
    process.setArguments(commandArgs(program, args, dir));
    process.setWorkingDirectory(dir);
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
        std::fprintf(stderr, "magic-dock: could not start %s (%s)\n", qPrintable(program), how);
        return false;
    }
    std::fprintf(stderr, "magic-dock: started %s (pid %lld, %s)\n", qPrintable(program), static_cast<long long>(pid), how);
    return true;
}

void AppIndex::scan(const QStringList &extraDirs) {
    apps_.clear();
    lower_.clear();
    byClass_.clear();
    byExec_.clear();
    byWeb_.clear();
    iconCache_.clear();
    iconFileCache_.clear();
    extraDirs_ = extraDirs;
    const QString home = QDir::homePath();
    // Later directories lose to earlier ones, so user installs win.
    QStringList dirs = extraDirs;
    dirs << home + QStringLiteral("/.local/share/applications")
         << home + QStringLiteral("/.local/share/flatpak/exports/share/applications")
         << QStringLiteral("/var/lib/flatpak/exports/share/applications")
         << QStringLiteral("/var/lib/snapd/desktop/applications")
         << QStringLiteral("/usr/local/share/applications")
         << QStringLiteral("/usr/share/applications");
    for (const QString &dir : dirs) scanDir(dir);
    stamp_ = treeStamp();
}

qint64 AppIndex::treeStamp() const {
    qint64 stamp = 0;
    const QString home = QDir::homePath();
    const QStringList dirs = {home + QStringLiteral("/.local/share/applications"),
                              home + QStringLiteral("/.local/share/icons/hicolor"),
                              home + QStringLiteral("/.local/share/flatpak/exports/share/applications"),
                              QStringLiteral("/usr/share/applications")};
    for (const QString &dir : dirs) {
        QFileInfo info(dir);
        if (!info.exists()) continue;
        stamp ^= info.lastModified().toMSecsSinceEpoch() + info.size();
        QDir folder(dir);
        const auto files = folder.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        int counted = 0;
        for (const QFileInfo &file : files) {
            stamp ^= file.lastModified().toMSecsSinceEpoch() + file.size();
            if (++counted > 400) break;
        }
    }
    return stamp;
}

bool AppIndex::rescanIfChanged() {
    const qint64 stamp = treeStamp();
    if (stamp == stamp_) return false;
    const QStringList extra = extraDirs_;
    const QString root = iconRoot_;
    scan(extra);
    iconRoot_ = root;
    return true;
}

void AppIndex::adoptDesktop(const QString &filePath) {
    if (!QFileInfo::exists(filePath)) return;
    parseDesktop(filePath);
}

QString AppIndex::adoptProject(const QString &appId) {
    const QString already = resolve(appId);
    if (!already.isEmpty()) return already;
    const QString base = appId.section(QLatin1Char('.'), -1);
    if (base.isEmpty() || base.contains(QLatin1Char('/'))) return {};
    const QString home = QDir::homePath();
    const QStringList desks = {home + QStringLiteral("/Projects/") + base + QStringLiteral("/share/") + base +
                                   QStringLiteral(".desktop"),
                               home + QStringLiteral("/Projects/") + base + QLatin1Char('/') + base + QStringLiteral(".desktop")};
    for (const QString &path : desks) {
        if (QFileInfo::exists(path)) adoptDesktop(path);
    }
    return resolve(appId);
}

void AppIndex::setIconRoot(const QString &root) { iconRoot_ = root; }

void AppIndex::scanDir(const QString &dir) {
    QDir d(dir);
    if (!d.exists()) return;
    const auto entries = d.entryInfoList(QStringList() << QStringLiteral("*.desktop"), QDir::Files);
    for (const QFileInfo &fi : entries) {
        if (apps_.contains(fi.completeBaseName())) continue;
        parseDesktop(fi.absoluteFilePath());
    }
}

void AppIndex::parseDesktop(const QString &filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&file);
    enum class Section { None, Main, Action };
    Section section = Section::None;
    DesktopApp app;
    app.path = filePath;
    app.id = QFileInfo(filePath).completeBaseName();
    DesktopAction action;
    bool mainName = false;
    bool actionName = false;
    bool hidden = false;

    auto commitAction = [&]() {
        if (!action.id.isEmpty() && !action.exec.isEmpty()) app.actions.push_back(action);
        action = DesktopAction{};
        actionName = false;
    };

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('[')) {
            if (section == Section::Action) commitAction();
            if (line == QLatin1String("[Desktop Entry]")) {
                section = Section::Main;
            } else if (line.startsWith(QLatin1String("[Desktop Action ")) && line.endsWith(']')) {
                section = Section::Action;
                action.id = line.mid(16, line.size() - 17);
            } else {
                section = Section::None;
            }
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString key = line.left(eq);
        const QString value = unescapeDesktop(line.mid(eq + 1));
        if (section == Section::Main) {
            if (key == QLatin1String("Name") && !mainName) {
                app.name = value;
                mainName = true;
            } else if (key == QLatin1String("Exec")) {
                app.exec = value;
            } else if (key == QLatin1String("Icon")) {
                app.icon = value;
            } else if (key == QLatin1String("StartupWMClass")) {
                app.wmClass = value;
            } else if (key == QLatin1String("Path")) {
                app.workdir = value;
            } else if (key == QLatin1String("NoDisplay")) {
                app.noDisplay = value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
            } else if (key == QLatin1String("Hidden")) {
                hidden = value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
            }
        } else if (section == Section::Action) {
            if (key == QLatin1String("Name") && !actionName) {
                action.name = value;
                actionName = true;
            } else if (key == QLatin1String("Exec")) {
                action.exec = value;
            }
        }
    }
    if (section == Section::Action) commitAction();
    if (hidden || app.name.isEmpty() || app.exec.isEmpty()) return;
    if (app.wmClass.isEmpty()) app.wmClass = app.id;
    apps_.insert(app.id, app);
    if (!lower_.contains(app.id.toLower())) lower_.insert(app.id.toLower(), app.id);
    const QString wm = app.wmClass.toLower();
    if (!wm.isEmpty() && !byClass_.contains(wm)) byClass_.insert(wm, app.id);
    const QStringList argv = splitExec(app.exec, &app);
    if (!argv.isEmpty()) {
        const QString base = QFileInfo(argv.first()).fileName().toLower();
        if (!base.isEmpty() && !byExec_.contains(base)) byExec_.insert(base, app.id);
    }
    const QString fromId = webToken(app.id);
    const QString fromClass = webToken(app.wmClass);
    if (!fromId.isEmpty() && !byWeb_.contains(fromId)) byWeb_.insert(fromId, app.id);
    if (!fromClass.isEmpty() && !byWeb_.contains(fromClass)) byWeb_.insert(fromClass, app.id);
}

const DesktopApp *AppIndex::byId(const QString &id) const {
    auto it = apps_.constFind(id);
    if (it == apps_.constEnd()) return nullptr;
    return &(*it);
}

QString AppIndex::resolve(const QString &appId) const {
    if (appId.isEmpty()) return {};
    QString id = appId;
    if (id.endsWith(QLatin1String(".desktop"))) id.chop(8);
    if (apps_.contains(id)) return id;
    const QString lower = id.toLower();
    if (lower_.contains(lower)) return lower_.value(lower);
    if (byClass_.contains(lower)) return byClass_.value(lower);
    const QString token = webToken(lower);
    if (!token.isEmpty() && byWeb_.contains(token)) return byWeb_.value(token);
    if (lower == QLatin1String("google-chrome") || lower == QLatin1String("chrome")) {
        if (apps_.contains(QLatin1String("com.google.Chrome"))) return QStringLiteral("com.google.Chrome");
        if (apps_.contains(QLatin1String("google-chrome"))) return QStringLiteral("google-chrome");
    }
    if (lower == QLatin1String("discord")) {
        if (apps_.contains(QLatin1String("com.discordapp.Discord"))) return QStringLiteral("com.discordapp.Discord");
    }
    // Qt builds the Wayland app id as the reversed organization domain plus the
    // executable name. cinder.local + cinder becomes local.cinder.cinder, while
    // the desktop file is still just "cinder".
    const QString last = id.section(QLatin1Char('.'), -1);
    if (!last.isEmpty() && last.compare(id, Qt::CaseInsensitive) != 0) {
        const QString lastLower = last.toLower();
        if (lower_.contains(lastLower)) return lower_.value(lastLower);
        if (byClass_.contains(lastLower)) return byClass_.value(lastLower);
        if (byExec_.contains(lastLower)) return byExec_.value(lastLower);
    }
    if (byExec_.contains(lower)) return byExec_.value(lower);
    QString suffix;
    for (auto it = apps_.cbegin(); it != apps_.cend(); ++it) {
        const QString key = it.key();
        if (key.size() <= id.size() || !key.endsWith(id, Qt::CaseInsensitive)) continue;
        const QChar prev = key.at(key.size() - id.size() - 1);
        if (prev != QLatin1Char('.') && prev != QLatin1Char('-') && prev != QLatin1Char('_')) continue;
        if (suffix.isEmpty() || key.size() < suffix.size()) suffix = key;
    }
    if (!suffix.isEmpty()) return suffix;
    return {};
}

QStringList AppIndex::cosmicFavorites() const {
    QStringList out;
    QFile file(QDir::homePath() + QStringLiteral("/.config/cosmic/com.system76.CosmicAppList/v1/favorites"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    const QString text = QString::fromUtf8(file.readAll());
    static const QRegularExpression re(QStringLiteral("\"([^\"]+)\""));
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QString raw = it.next().captured(1);
        const QString id = resolve(raw);
        const QString use = id.isEmpty() ? raw : id;
        if (!out.contains(use)) out << use;
    }
    return out;
}

QString AppIndex::findIconFile(const QString &name) const {
    if (name.isEmpty()) return {};
    auto cached = iconFileCache_.constFind(name);
    if (cached != iconFileCache_.constEnd()) return cached.value();
    QString found;
    const QFileInfo direct(name);
    if (direct.isAbsolute() && direct.exists()) {
        found = direct.absoluteFilePath();
    } else {
        const QStringList roots = iconSearchRoots(iconRoot_);
        const QStringList exts = {QStringLiteral(".svg"), QStringLiteral(".png"), QStringLiteral(".xpm"),
                                  QStringLiteral(".ico"), QStringLiteral(".webp")};
        for (const QString &root : roots) {
            for (int size : kSizes) {
                for (const char *ctx : kContexts) {
                    for (const QString &ext : exts) {
                        const QString path = QStringLiteral("%1/%2x%2/%3/%4%5").arg(root).arg(size).arg(QLatin1String(ctx), name, ext);
                        if (QFileInfo::exists(path)) {
                            found = path;
                            break;
                        }
                    }
                    if (!found.isEmpty()) break;
                }
                if (!found.isEmpty()) break;
            }
            if (!found.isEmpty()) break;
            for (const char *ctx : kContexts) {
                for (const QString &ext : exts) {
                    const QString path = QStringLiteral("%1/scalable/%2/%3%4").arg(root, QLatin1String(ctx), name, ext);
                    if (QFileInfo::exists(path)) {
                        found = path;
                        break;
                    }
                }
                if (!found.isEmpty()) break;
            }
            if (!found.isEmpty()) break;
        }
        if (found.isEmpty()) {
            for (const QString &root : roots) {
                for (const QString &ext : exts) {
                    const QString flat = root + QLatin1Char('/') + name + ext;
                    if (QFileInfo::exists(flat)) {
                        found = flat;
                        break;
                    }
                }
                if (!found.isEmpty()) break;
            }
        }
        if (found.isEmpty()) found = chromeProfileIcon(name);
    }
    iconFileCache_.insert(name, found);
    return found;
}

QPixmap AppIndex::iconNamed(const QString &nameOrPath, int devicePx) const {
    if (devicePx < 8) devicePx = 8;
    const QString key = nameOrPath + QLatin1Char('|') + QString::number(devicePx);
    auto cached = iconCache_.constFind(key);
    if (cached != iconCache_.constEnd()) return cached.value();

    QPixmap pix;
    const QString file = findIconFile(nameOrPath);
    if (!file.isEmpty()) {
        QIcon icon(file);
        pix = icon.pixmap(devicePx, devicePx);
        if (pix.isNull()) pix = QPixmap(file);
        if (!pix.isNull() && (pix.width() != devicePx || pix.height() != devicePx)) {
            pix = pix.scaled(devicePx, devicePx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }
    if (pix.isNull()) {
        pix = QIcon::fromTheme(nameOrPath).pixmap(devicePx, devicePx);
    }
    iconCache_.insert(key, pix);
    return pix;
}

QPixmap AppIndex::iconFor(const QString &desktopId, int devicePx) const {
    const DesktopApp *app = byId(desktopId);
    if (!app) return iconNamed(desktopId, devicePx);
    QPixmap pix = iconNamed(app->icon.isEmpty() ? app->id : app->icon, devicePx);
    if (pix.isNull()) pix = iconNamed(app->id, devicePx);
    return pix;
}

QStringList splitExec(const QString &exec, const DesktopApp *app) {
    QStringList tokens;
    QString cur;
    bool quote = false;
    for (int i = 0; i < exec.size(); ++i) {
        const QChar c = exec[i];
        if (c == '"') {
            quote = !quote;
            continue;
        }
        if (!quote && c.isSpace()) {
            if (!cur.isEmpty()) {
                tokens << cur;
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.isEmpty()) tokens << cur;

    QStringList out;
    for (QString tok : tokens) {
        if (tok == QLatin1String("%f") || tok == QLatin1String("%F") || tok == QLatin1String("%u") ||
            tok == QLatin1String("%U") || tok == QLatin1String("%d") || tok == QLatin1String("%D")) {
            continue;
        }
        if (tok == QLatin1String("%i")) {
            if (app && !app->icon.isEmpty()) {
                out << QStringLiteral("--icon") << app->icon;
            }
            continue;
        }
        if (tok == QLatin1String("%c")) {
            out << (app ? app->name : QString());
            continue;
        }
        if (tok == QLatin1String("%k")) {
            out << (app ? app->path : QString());
            continue;
        }
        tok.replace(QLatin1String("%%"), QLatin1String("%"));
        if (tok.contains(QLatin1Char('%'))) {
            tok.remove(QLatin1String("%f"));
            tok.remove(QLatin1String("%F"));
            tok.remove(QLatin1String("%u"));
            tok.remove(QLatin1String("%U"));
        }
        if (!tok.isEmpty()) out << tok;
    }
    return out;
}

QProcessEnvironment launchEnvironment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // The bundled dock points Qt at a plugin directory that only contains the
    // offscreen platform. A Qt app that inherits that path (Vesper, Cinder)
    // starts with no window. The session's own Qt path is a different directory
    // and has to stay, or those apps cannot find their platform plugin.
    const QString bundleRoot = QFileInfo(QCoreApplication::applicationDirPath() + QStringLiteral("/..")).canonicalFilePath();
    auto bundled = [&](const QString &path) {
        if (bundleRoot.isEmpty() || path.isEmpty()) return false;
        const QString canon = QFileInfo(path).canonicalFilePath();
        const QString check = canon.isEmpty() ? QDir(path).absolutePath() : canon;
        return check == bundleRoot || check.startsWith(bundleRoot + QLatin1Char('/'));
    };
    auto stripBundled = [&](const QString &key) {
        if (!env.contains(key)) return;
        QStringList keep;
        const QStringList parts = env.value(key).split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            if (!bundled(part)) keep << part;
        }
        if (keep.isEmpty()) env.remove(key);
        else env.insert(key, keep.join(QLatin1Char(':')));
    };
    if (env.value(QStringLiteral("QT_QPA_PLATFORM")) == QLatin1String("offscreen"))
        env.remove(QStringLiteral("QT_QPA_PLATFORM"));
    stripBundled(QStringLiteral("QT_PLUGIN_PATH"));
    stripBundled(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"));
    stripBundled(QStringLiteral("QT_QML_IMPORT_PATH"));
    stripBundled(QStringLiteral("QML2_IMPORT_PATH"));
    stripBundled(QStringLiteral("LD_LIBRARY_PATH"));
    return env;
}

QString AppIndex::programFor(const QString &appId) const {
    const QString resolved = resolve(appId);
    if (const DesktopApp *app = byId(resolved.isEmpty() ? appId : resolved)) {
        const QStringList args = splitExec(app->exec, app);
        if (!args.isEmpty() && args.first().startsWith(QLatin1Char('/')) && QFileInfo(args.first()).exists()) return args.first();
    }
    const QStringList names = launchNames(resolved.isEmpty() ? appId : resolved) + launchNames(appId);
    const QString running = discoverProcess(names);
    if (!running.isEmpty()) return running;
    for (const QString &name : names) {
        const QString project = discoverProject(name);
        if (!project.isEmpty()) return project;
    }
    return {};
}

std::vector<AppHit> AppIndex::search(const QString &query, int limit) const {
    std::vector<AppHit> hits;
    const QString needle = query.trimmed().toLower();
    if (needle.isEmpty() || limit < 1) return hits;
    struct Ranked {
        int rank = 2;
        QString name;
        QString id;
    };
    std::vector<Ranked> ranked;
    for (auto it = apps_.cbegin(); it != apps_.cend(); ++it) {
        if (it->noDisplay || it->name.isEmpty()) continue;
        const QString name = it->name.toLower();
        const QString id = it.key().toLower();
        int rank = 2;
        if (name.startsWith(needle)) rank = 0;
        else if (name.contains(needle)) rank = 1;
        else if (id.contains(needle)) rank = 2;
        else continue;
        ranked.push_back(Ranked{rank, it->name, it.key()});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked &a, const Ranked &b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    if (static_cast<int>(ranked.size()) > limit) ranked.resize(static_cast<size_t>(limit));
    hits.reserve(ranked.size());
    for (const Ranked &row : ranked) hits.push_back(AppHit{row.id, row.name});
    return hits;
}

bool AppIndex::launch(const QString &appId, const QString &fallbackExec, const QString &fallbackDir) {
    const QString resolved = adoptProject(appId);
    const DesktopApp *app = byId(resolved.isEmpty() ? appId : resolved);
    const QString desktopDir = launchDirectory(app ? app->workdir : QString());
    const QStringList args = (app && !app->exec.isEmpty()) ? splitExec(app->exec, app) : QStringList();
    // gio launch would start cosmic-term in the dock's directory. Pass the
    // directory on the command line, and use home when the desktop file has no Path.
    if (!args.isEmpty() && isCosmicTerm(args.first())) {
        if (startDetachedApp(args.first(), args.mid(1), desktopDir, "terminal")) return true;
    }
    // Run the desktop command ourselves when it is a real program. gio exits
    // at once when that command is missing, and startDetached would still
    // report the failed gio process as a successful launch.
    const QString direct = args.isEmpty() ? QString() : resolveProgram(args.first());
    if (!direct.isEmpty()) {
        if (startDetachedApp(direct, args.mid(1), desktopDir, "exec line")) return true;
        if (app && !app->path.isEmpty() &&
            startDetachedApp(QStringLiteral("gio"), {QStringLiteral("launch"), app->path}, desktopDir, "desktop file"))
            return true;
    }
    const QString savedRaw = !fallbackExec.isEmpty() ? fallbackExec : programFor(appId);
    const QString saved = resolveProgram(savedRaw);
    if (!saved.isEmpty()) {
        const bool terminal = isCosmicTerm(saved);
        const QString dir = terminal ? launchDirectory(app && !app->workdir.isEmpty() ? app->workdir : QString())
                                     : launchDirectory(!fallbackDir.isEmpty() ? fallbackDir : QFileInfo(saved).absolutePath());
        if (startDetachedApp(saved, {}, dir, "saved program")) return true;
    }
    const QString id = resolved.isEmpty() ? appId : resolved;
    if (direct.isEmpty() && !id.isEmpty() &&
        startDetachedApp(QStringLiteral("gtk-launch"), {id}, QDir::homePath(), "gtk-launch id"))
        return true;
    std::fprintf(stderr, "magic-dock: no way to open \"%s\"\n", qPrintable(appId));
    return false;
}

int runAppsSelfTest() {
    const QString root = QDir::tempPath() + QStringLiteral("/magic-dock-apps-test");
    QDir(root).removeRecursively();
    const QString apps = root + QStringLiteral("/applications");
    const QString icons = root + QStringLiteral("/hicolor/128x128/apps");
    QDir().mkpath(apps);
    QDir().mkpath(icons);
    QImage icon(16, 16, QImage::Format_ARGB32_Premultiplied);
    icon.fill(QColor(QStringLiteral("#3b82f6")));
    const QString iconName = QStringLiteral("chrome-abcdefghijklmnop-Default");
    if (!icon.save(icons + QLatin1Char('/') + iconName + QStringLiteral(".png"))) {
        std::fprintf(stderr, "magic-dock: could not write the web-app icon fixture\n");
        return 1;
    }
    QFile desktop(apps + QStringLiteral("/chrome-abcdefghijklmnop-Default.desktop"));
    if (!desktop.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
    desktop.write("[Desktop Entry]\n"
                  "Type=Application\n"
                  "Name=Example\n"
                  "Exec=/usr/bin/true\n"
                  "Icon=chrome-abcdefghijklmnop-Default\n"
                  "StartupWMClass=crx_abcdefghijklmnop\n");
    desktop.close();
    QImage cinderIcon(8, 8, QImage::Format_ARGB32_Premultiplied);
    cinderIcon.fill(QColor(QStringLiteral("#ffb454")));
    const QString cinderPng = root + QStringLiteral("/cinder.png");
    cinderIcon.save(cinderPng);
    QFile cinder(apps + QStringLiteral("/cinder.desktop"));
    if (!cinder.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
    cinder.write(("[Desktop Entry]\n"
                  "Type=Application\n"
                  "Name=Cinder\n"
                  "Exec=/home/andreas/Projects/cinder/cinder %F\n"
                  "Icon=" +
                  cinderPng +
                  "\n"
                  "StartupWMClass=cinder\n")
                     .toUtf8());
    cinder.close();

    AppIndex index;
    index.setIconRoot(root + QStringLiteral("/hicolor"));
    index.scan({apps});
    const QString web = index.resolve(QStringLiteral("crx_abcdefghijklmnop"));
    const QString qt = index.resolve(QStringLiteral("local.cinder.cinder"));
    const QPixmap webIcon = index.iconFor(web, 16);
    const QPixmap qtIcon = index.iconFor(qt, 16);
    const QString bundlePlugins = QDir(QCoreApplication::applicationDirPath() + QStringLiteral("/../plugins")).absolutePath();
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_PLUGIN_PATH", (bundlePlugins + QStringLiteral(":/tmp/user-qt-plugins")).toUtf8());
    qputenv("QT_QML_IMPORT_PATH", "/tmp/user-qml");
    const QProcessEnvironment env = launchEnvironment();
    qunsetenv("QT_QPA_PLATFORM");
    qunsetenv("QT_PLUGIN_PATH");
    qunsetenv("QT_QML_IMPORT_PATH");
    const QString marker = root + QStringLiteral("/launched");
    QFile markerScript(root + QStringLiteral("/mark.sh"));
    if (!markerScript.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
    markerScript.write(("#!/bin/sh\ntouch '" + marker + "'\n").toUtf8());
    markerScript.close();
    markerScript.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                QFileDevice::ExeOther);
    QFile missingCmd(apps + QStringLiteral("/no-such-blogy.desktop"));
    if (!missingCmd.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
    missingCmd.write("[Desktop Entry]\n"
                     "Type=Application\n"
                     "Name=Missing Blogy\n"
                     "Exec=blogy-not-on-path %F\n");
    missingCmd.close();
    AppIndex launchIndex;
    launchIndex.scan({apps});
    const bool fellThrough = launchIndex.launch(QStringLiteral("no-such-blogy"), markerScript.fileName(), QDir::tempPath());
    for (int i = 0; i < 50 && !QFile::exists(marker); ++i) QThread::msleep(20);
    const bool markerOk = QFile::exists(marker);
    const QString foundTrue = resolveProgram(QStringLiteral("true"));
    const QString missingName = resolveProgram(QStringLiteral("blogy-not-on-path"));
    QDir(root).removeRecursively();
    const bool envOk = !env.contains(QStringLiteral("QT_QPA_PLATFORM")) &&
                       env.value(QStringLiteral("QT_PLUGIN_PATH")) == QLatin1String("/tmp/user-qt-plugins") &&
                       env.value(QStringLiteral("QT_QML_IMPORT_PATH")) == QLatin1String("/tmp/user-qml");
    const QString launchedHome = launchDirectory({});
    const QString launchedKept = launchDirectory(QDir::tempPath());
    const QStringList termArgs = commandArgs(QStringLiteral("/usr/bin/cosmic-term"), {}, launchedHome);
    const QStringList plainArgs = commandArgs(QStringLiteral("/usr/bin/true"), {QStringLiteral("--version")}, launchedHome);
    const QStringList keptFlag = commandArgs(QStringLiteral("cosmic-term"), {QStringLiteral("--working-directory"), QStringLiteral("/tmp")},
                                             launchedHome);
    const bool dirOk = QDir(launchedHome) == QDir::home() && QDir(launchedKept) == QDir(QDir::tempPath()) &&
                       termArgs.size() == 2 && termArgs.at(0) == QLatin1String("--working-directory") &&
                       QDir(termArgs.at(1)) == QDir::home() && plainArgs.size() == 1 && keptFlag.size() == 2;
    if (web != QLatin1String("chrome-abcdefghijklmnop-Default") || qt != QLatin1String("cinder") || webIcon.isNull() ||
        qtIcon.isNull() || !envOk || !dirOk || !fellThrough || !markerOk || foundTrue.isEmpty() ||
        !missingName.isEmpty()) {
        std::fprintf(stderr,
                     "magic-dock: apps self-test failed (web=%s qt=%s webIcon=%d qtIcon=%d plugins=%s dir=%d "
                     "launch=%d marker=%d true=%s missing=%s)\n",
                     qPrintable(web), qPrintable(qt), webIcon.isNull() ? 0 : 1, qtIcon.isNull() ? 0 : 1,
                     qPrintable(env.value(QStringLiteral("QT_PLUGIN_PATH"))), dirOk ? 1 : 0, fellThrough ? 1 : 0,
                     markerOk ? 1 : 0, qPrintable(foundTrue), qPrintable(missingName));
        return 1;
    }
    const QStringList args = splitExec(QStringLiteral("/usr/bin/true %F %U"), nullptr);
    if (args.size() != 1 || args.at(0) != QLatin1String("/usr/bin/true")) {
        std::fprintf(stderr, "magic-dock: exec field codes parsed wrong\n");
        return 1;
    }
    std::fprintf(stderr, "magic-dock: apps self-test ok\n");
    return 0;
}
