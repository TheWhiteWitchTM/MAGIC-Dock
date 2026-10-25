#include "dock.hpp"

#include <linux/input-event-codes.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

const QString kContributeUrl = QStringLiteral("https://www.paypal.com/ncp/payment/4P85EQRES3RGU");

void startClean(const QString &program, const QStringList &args = {}) {
    startDetachedApp(program, args, {}, "app");
}

void openContribute() {
    startClean(QStringLiteral("xdg-open"), {kContributeUrl});
}

void drawSearchGlyph(QPainter &painter, const QRectF &rect) {
    const double s = std::min(rect.width(), rect.height());
    const QPointF c = rect.center() + QPointF(-s * 0.06, -s * 0.06);
    const double r = s * 0.22;
    painter.setPen(QPen(QColor(255, 255, 255, 235), std::max(1.6, s * 0.07), Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(c, r, r);
    painter.drawLine(c + QPointF(r * 0.62, r * 0.62), c + QPointF(r * 1.45, r * 1.45));
}

void drawMoreGlyph(QPainter &painter, const QRectF &rect) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 230));
    const double s = std::min(rect.width(), rect.height());
    const double r = std::max(1.8, s * 0.055);
    const double gap = s * 0.18;
    for (int i = -1; i <= 1; ++i) painter.drawEllipse(QPointF(rect.center().x() + i * gap, rect.center().y()), r, r);
}

QString keyChar(quint32 code, bool shift) {
    auto pick = [&](QChar lower, QChar upper) { return QString(shift ? upper : lower); };
    if (code >= KEY_1 && code <= KEY_9) {
        const QString digits = QStringLiteral("123456789");
        const QString shifted = QStringLiteral("!@#$%^&*(");
        return QString(shift ? shifted[code - KEY_1] : digits[code - KEY_1]);
    }
    if (code == KEY_0) return shift ? QStringLiteral(")") : QStringLiteral("0");
    if (code >= KEY_Q && code <= KEY_P) {
        const QString row = QStringLiteral("qwertyuiop");
        return pick(row[code - KEY_Q], row[code - KEY_Q].toUpper());
    }
    if (code >= KEY_A && code <= KEY_L) {
        const QString row = QStringLiteral("asdfghjkl");
        return pick(row[code - KEY_A], row[code - KEY_A].toUpper());
    }
    if (code >= KEY_Z && code <= KEY_M) {
        const QString row = QStringLiteral("zxcvbnm");
        return pick(row[code - KEY_Z], row[code - KEY_Z].toUpper());
    }
    switch (code) {
    case KEY_SPACE: return QStringLiteral(" ");
    case KEY_MINUS: return shift ? QStringLiteral("_") : QStringLiteral("-");
    case KEY_EQUAL: return shift ? QStringLiteral("+") : QStringLiteral("=");
    case KEY_LEFTBRACE: return shift ? QStringLiteral("{") : QStringLiteral("[");
    case KEY_RIGHTBRACE: return shift ? QStringLiteral("}") : QStringLiteral("]");
    case KEY_SEMICOLON: return shift ? QStringLiteral(":") : QStringLiteral(";");
    case KEY_APOSTROPHE: return shift ? QStringLiteral("\"") : QStringLiteral("'");
    case KEY_GRAVE: return shift ? QStringLiteral("~") : QStringLiteral("`");
    case KEY_BACKSLASH: return shift ? QStringLiteral("|") : QStringLiteral("\\");
    case KEY_COMMA: return shift ? QStringLiteral("<") : QStringLiteral(",");
    case KEY_DOT: return shift ? QStringLiteral(">") : QStringLiteral(".");
    case KEY_SLASH: return shift ? QStringLiteral("?") : QStringLiteral("/");
    default: return {};
    }
}

QPixmap letterIcon(const QString &text, const QString &seed, int px) {
    static const QColor palette[] = {
        QColor("#4C8DFF"), QColor("#E06C75"), QColor("#3CBF8F"), QColor("#E0A15A"),
        QColor("#A78BFA"), QColor("#3AA8C1"), QColor("#D16BA5"), QColor("#7D8DA1"),
    };
    QPixmap pix(px, px);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette[qHash(seed) % 8]);
    const qreal radius = px * 0.23;
    painter.drawRoundedRect(QRectF(0.5, 0.5, px - 1, px - 1), radius, radius);
    QFont font(QStringLiteral("Fira Sans"));
    font.setPixelSize(std::max(8, px / 2));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(Qt::white);
    const QString letter = text.isEmpty() ? QStringLiteral("?") : text.left(1).toUpper();
    painter.drawText(pix.rect(), Qt::AlignCenter, letter);
    return pix;
}

void drawPixmapFit(QPainter &painter, const QPixmap &pix, const QRectF &rect) {
    if (pix.isNull() || rect.isEmpty()) return;
    painter.drawPixmap(rect, pix, QRectF(0, 0, pix.width(), pix.height()));
}

}  // namespace

struct MenuRow {
    QString action;
    QString label;
    bool enabled = true;
    bool header = false;
    std::vector<MenuRow> sub;
};

struct Dock::Item {
    enum class Kind { Launcher, Workspaces, AppLibrary, Search, App, Stack, Minimize, Minimized, TaskManager, Overflow } kind =
        Kind::App;
    QString id;
    QString name;
    QStringList apps;
};

struct Dock::Live {
    quint64 key = 0;
    QString rawId;
    QString appId;
    QString title;
    bool minimized = false;
    bool activated = false;
    bool fullscreen = false;
    bool maximized = false;
    QList<int> outputs;
    QHash<int, QRect> geom;
};

struct Dock::Slot {
    Item::Kind kind = Item::Kind::App;
    QString uid;
    QString appId;
    QString label;
    int itemIndex = -1;
    quint64 windowKey = 0;
    double gapBefore = 8;
    QRectF rect;
    QRectF hit;
    int running = 0;
    bool active = false;
};

struct Dock::Screen {
    int id = 0;
    int w = 0;
    int h = 0;
    double dpr = 1;
    bool pointer = false;
    double x = -1000;
    double y = -1000;
    double hidden = 0;
    double hiddenTarget = 0;
    qint64 hideAt = 0;
    qint64 showAt = 0;
    bool hideArmed = false;
    bool showArmed = false;
    qint64 born = 0;
    bool pressed = false;
    double pressX = 0;
    double pressY = 0;
    QString pressUid;
    QString pressApp;
    QString pressAction;
    bool pressMenu = false;
    bool pressCell = false;
    bool dragging = false;
    QString dragUid;
    qint64 hoverSince = 0;
    QString hoverUid;
    bool mergeReady = false;
    QString openStack;
    QString shownStack;
    double popup = 0;
    bool menu = false;
    QString menuUid;
    QString menuApp;
    QString renameStack;
    std::vector<MenuRow> rows;
    int menuRow = -1;
    int subRow = -1;
    QRectF menuRect;
    std::vector<QRectF> menuRows;
    QRectF subRect;
    std::vector<QRectF> subRows;
    bool rename = false;
    QString renameText;
    int caret = 0;
    int heldKey = -1;
    bool heldShift = false;
    qint64 repeatAt = 0;
    std::vector<Slot> slots;
    QRectF pill;
    struct Cell {
        QString appId;
        QString label;
        QRectF rect;
        QRectF icon;
    };
    std::vector<Cell> cells;
    QRectF popupRect;
    QString openPreviews;
    QString shownPreviews;
    double previews = 0;
    struct Card {
        quint64 key = 0;
        QString appId;
        QString title;
        bool active = false;
        bool minimized = false;
        QRectF rect;
        QRectF thumb;
        QRectF closeRect;
    };
    std::vector<Card> cards;
    QRectF previewsRect;
    int hoverCard = -1;
    bool pressCard = false;
    bool pressClose = false;
    bool pressSearch = false;
    bool pressAbout = false;
    quint64 pressKey = 0;
    bool search = false;
    QString searchText;
    int searchCaret = 0;
    int searchRow = -1;
    QRectF searchRect;
    QRectF searchField;
    struct SearchHit {
        QString appId;
        QString label;
        QRectF rect;
        QRectF icon;
    };
    std::vector<SearchHit> searchHits;
    bool about = false;
    QRectF aboutRect;
    QRectF aboutCoffee;
    QHash<QString, double> scale;
    QHash<QString, double> target;
    qint64 tipSince = 0;
    QString tipUid;
};

Dock::Dock(AppIndex *apps) : apps_(apps), now_(nowMs()) {}

Dock::~Dock() = default;

double Dock::barThickness() const {
    return pad_ + iconSize_ + 8 + pad_;
}

bool Dock::horizontal() const {
    return edge_ == ShellSpec::Edge::Top || edge_ == ShellSpec::Edge::Bottom;
}

QString Dock::taskId() const {
    const QString resolved = apps_->resolve(QStringLiteral("com.system76.CosmicMonitor"));
    return resolved.isEmpty() ? QStringLiteral("com.system76.CosmicMonitor") : resolved;
}

QString Dock::canonical(const QString &appId) const {
    const QString resolved = apps_->resolve(appId);
    return resolved.isEmpty() ? appId : resolved;
}

QRectF Dock::popupNear(const Screen &screen, const QPointF &anchor, double width, double height) const {
    double x = 8;
    double y = 8;
    if (horizontal()) {
        x = std::clamp(anchor.x() - width / 2.0, 8.0, std::max(8.0, screen.w - width - 8.0));
        if (edge_ == ShellSpec::Edge::Top) {
            y = anchor.y() + 12;
            if (y + height > screen.h - 8) y = std::max(8.0, screen.h - height - 8);
        } else {
            y = anchor.y() - 12 - height;
            if (y < 8) y = 8;
        }
    } else {
        y = std::clamp(anchor.y() - height / 2.0, 8.0, std::max(8.0, screen.h - height - 8.0));
        if (edge_ == ShellSpec::Edge::Left) {
            x = anchor.x() + 12;
            if (x + width > screen.w - 8) x = std::max(8.0, screen.w - width - 8);
        } else {
            x = anchor.x() - 12 - width;
            if (x < 8) x = 8;
        }
    }
    return QRectF(x, y, width, height);
}

QPointF Dock::openingNudge(const QString &appId) const {
    const QString id = canonical(appId);
    if (!opening_.contains(id) || !keysFor(id).isEmpty()) return {};
    const double t = (now_ - opening_.value(id)) / 1000.0;
    if (t < 0 || t > 8.0) return {};
    const double phase = std::fmod(t, 0.46) / 0.46;
    double amp = t < 1.6 ? 11.0 * (1.0 - t / 2.2) : 3.5;
    if (t > 6.0) amp *= std::max(0.0, 1.0 - (t - 6.0) / 2.0);
    const double hop = std::sin(phase * 3.14159265358979323846) * std::max(0.0, amp);
    if (edge_ == ShellSpec::Edge::Bottom) return QPointF(0, -hop);
    if (edge_ == ShellSpec::Edge::Top) return QPointF(0, hop);
    if (edge_ == ShellSpec::Edge::Left) return QPointF(hop, 0);
    return QPointF(-hop, 0);
}

int Dock::activeOutputId() const {
    if (pointerOutput_ >= 0) {
        const auto it = screens_.constFind(pointerOutput_);
        if (it != screens_.constEnd() && it->pointer) return pointerOutput_;
    }
    for (const Live &live : live_) {
        if (live.activated && !live.outputs.isEmpty()) return live.outputs.first();
    }
    return -1;
}

bool Dock::outputWanted(int outputId, const QString &name) const {
    if (outputMode_.isEmpty() || outputMode_ == QLatin1String("All")) return true;
    if (outputMode_ == QLatin1String("Active")) {
        const int active = activeOutputId();
        if (active < 0) return true;
        return outputId == active;
    }
    return name == outputMode_;
}

bool Dock::pulledToDock(const Screen &screen, double x, double y) const {
    if (screen.popupRect.isEmpty()) return false;
    if (edge_ == ShellSpec::Edge::Bottom) return y > screen.popupRect.bottom() + 6;
    if (edge_ == ShellSpec::Edge::Top) return y < screen.popupRect.top() - 6;
    if (edge_ == ShellSpec::Edge::Left) return x < screen.popupRect.left() - 6;
    return x > screen.popupRect.right() + 6;
}

void Dock::noteOpening(const QString &appId) {
    const QString id = canonical(appId);
    if (!id.isEmpty()) opening_.insert(id, now_);
}

void Dock::captureExec(const QString &appId) {
    const QString id = canonical(appId);
    if (id.isEmpty() || execs_.contains(id) || id == taskId()) return;
    bool pinned = false;
    for (const Item &item : items_) {
        if (item.kind == Item::Kind::App && (item.id == id || canonical(item.id) == id)) pinned = true;
        if (item.kind == Item::Kind::Stack) {
            for (const QString &app : item.apps) {
                if (app == id || canonical(app) == id) pinned = true;
            }
        }
    }
    if (!pinned) return;
    const QString program = apps_->programFor(appId);
    if (program.isEmpty()) return;
    execs_.insert(id, program);
    save();
}

void Dock::applyCosmicSettings(const CosmicDockSettings &settings) {
    if (!settings.ok) return;
    ShellSpec::Edge edge = ShellSpec::Edge::Bottom;
    if (settings.anchor == QLatin1String("Top")) edge = ShellSpec::Edge::Top;
    else if (settings.anchor == QLatin1String("Left")) edge = ShellSpec::Edge::Left;
    else if (settings.anchor == QLatin1String("Right")) edge = ShellSpec::Edge::Right;
    Hide mode = Hide::Overlap;
    if (settings.autohide == QLatin1String("Never")) mode = Hide::Never;
    else if (settings.autohide == QLatin1String("Always")) mode = Hide::Always;
    const bool changed = edge != edge_ || exclusive_ != settings.exclusiveZone || margin_ != settings.inset() ||
                         outputMode_ != settings.output || iconSize_ != settings.iconSize;
    edge_ = edge;
    margin_ = settings.inset();
    iconSize_ = std::clamp(settings.iconSize, 16, 96);
    pad_ = std::clamp(settings.padding, 0, 32);
    radius_ = std::clamp(settings.borderRadius, 0, 48);
    opacity_ = settings.opacity;
    exclusive_ = settings.exclusiveZone;
    expand_ = settings.expandToEdges;
    handle_ = std::max(1, settings.handleSize);
    hideWaitMs_ = settings.hideWaitMs;
    unhideDelayMs_ = settings.unhideDelayMs;
    outputMode_ = settings.output;
    hideMode_ = mode;
    settingsSig_ = settings.signature;
    if (changed) shellDirty_ = true;
}

void Dock::pollSettings() {
    if (now_ - settingsPoll_ < 700) return;
    settingsPoll_ = now_;
    if (apps_->rescanIfChanged()) shellDirty_ = true;
    const CosmicDockSettings settings = readCosmicDockSettings();
    if (!settings.ok || settings.signature == settingsSig_) return;
    applyCosmicSettings(settings);
    std::fprintf(stderr, "magic-dock: dock settings %s size %d autohide %s output %s\n", qPrintable(settings.anchor),
                 settings.iconSize, qPrintable(settings.autohide), qPrintable(settings.output));
}

ShellSpec Dock::shellSpec() const {
    ShellSpec spec;
    spec.edge = edge_;
    spec.exclusive = exclusive_;
    spec.reserve = static_cast<int>(std::lround(barThickness() + margin_));
    spec.output = outputMode_;
    spec.activeOutput = activeOutputId();
    return spec;
}

bool Dock::takeShellChange() {
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                            .arg(static_cast<int>(edge_))
                            .arg(exclusive_ ? 1 : 0)
                            .arg(static_cast<int>(std::lround(barThickness() + margin_)))
                            .arg(outputMode_)
                            .arg(activeOutputId());
    static QString applied;
    // shellDirty_ is set when Pop settings change. The key also catches the
    // active monitor moving, which the settings file does not mention.
    if (!shellDirty_ && applied == key) return false;
    shellDirty_ = false;
    applied = key;
    return true;
}

QString Dock::configPath() const {
    const QString path = QDir::homePath() + QStringLiteral("/.config/magic-dock/dock.json");
    const QString old = QDir::homePath() + QStringLiteral("/.config/cosmic-bar/dock.json");
    if (!QFileInfo::exists(path) && QFileInfo::exists(old)) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile::copy(old, path);
    }
    return path;
}

QString Dock::itemName(const QString &appId) const {
    if (const DesktopApp *app = apps_->byId(appId)) return app->name;
    const QString resolved = apps_->resolve(appId);
    if (const DesktopApp *app = apps_->byId(resolved)) return app->name;
    return appId;
}

QPixmap Dock::itemIcon(const QString &appId, int devicePx) const {
    QPixmap pix = apps_->iconFor(appId, devicePx);
    if (pix.isNull()) {
        const QString resolved = apps_->resolve(appId);
        if (!resolved.isEmpty()) pix = apps_->iconFor(resolved, devicePx);
    }
    if (pix.isNull()) pix = letterIcon(itemName(appId), appId, devicePx);
    return pix;
}

void Dock::save() const {
    QJsonArray items;
    for (const Item &item : items_) {
        QJsonObject object;
        switch (item.kind) {
        case Item::Kind::Launcher:
        case Item::Kind::Workspaces:
        case Item::Kind::AppLibrary:
        case Item::Kind::Search:
        case Item::Kind::Minimize:
        case Item::Kind::Minimized:
        case Item::Kind::TaskManager:
        case Item::Kind::Overflow:
            continue;
        case Item::Kind::App:
            object.insert(QStringLiteral("kind"), QStringLiteral("app"));
            object.insert(QStringLiteral("id"), item.id);
            break;
        case Item::Kind::Stack: {
            object.insert(QStringLiteral("kind"), QStringLiteral("stack"));
            object.insert(QStringLiteral("id"), item.id);
            object.insert(QStringLiteral("name"), item.name);
            QJsonArray apps;
            for (const QString &app : item.apps) apps.append(app);
            object.insert(QStringLiteral("apps"), apps);
            break;
        }
        }
        items.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 3);
    root.insert(QStringLiteral("items"), items);
    QJsonObject launch;
    for (auto it = execs_.cbegin(); it != execs_.cend(); ++it) launch.insert(it.key(), it.value());
    if (!launch.isEmpty()) root.insert(QStringLiteral("launch"), launch);
    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    QFile file(configPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void Dock::buildDefault() {
    items_.clear();
    const QStringList favorites = apps_->cosmicFavorites();
    const QStringList socialOrder = {QStringLiteral("hearth-wire"), QStringLiteral("tweety"), QStringLiteral("libresky"),
                                     QStringLiteral("hearth-lantern")};
    QStringList social;
    for (const QString &id : socialOrder) {
        const QString resolved = apps_->resolve(id);
        const QString use = resolved.isEmpty() ? id : resolved;
        if (!apps_->byId(use)) continue;
        const bool pinned = favorites.contains(use) || id == QLatin1String("hearth-lantern");
        if (pinned && !social.contains(use)) social << use;
    }

    bool placed = false;
    for (const QString &fav : favorites) {
        if (social.contains(fav)) {
            if (!placed) {
                Item stack;
                stack.kind = Item::Kind::Stack;
                stack.id = QStringLiteral("social");
                stack.name = QStringLiteral("Social");
                stack.apps = social;
                items_.push_back(stack);
                placed = true;
            }
            continue;
        }
        Item app;
        app.kind = Item::Kind::App;
        app.id = fav;
        items_.push_back(app);
    }
    if (!placed && !social.isEmpty()) {
        Item stack;
        stack.kind = Item::Kind::Stack;
        stack.id = QStringLiteral("social");
        stack.name = QStringLiteral("Social");
        stack.apps = social;
        items_.push_back(stack);
    }
}

void Dock::load() {
    QFile file(configPath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        buildDefault();
        save();
        applyCosmicSettings(readCosmicDockSettings());
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const int version = root.value(QStringLiteral("version")).toInt(1);
    magnify_ = false;
    if (version < 2 && root.contains(QStringLiteral("iconSize"))) {
        iconSize_ = std::clamp(root.value(QStringLiteral("iconSize")).toInt(40), 32, 72);
        if (iconSize_ > 44) iconSize_ = 40;
    }
    execs_.clear();
    const QJsonObject launch = root.value(QStringLiteral("launch")).toObject();
    for (auto it = launch.constBegin(); it != launch.constEnd(); ++it) {
        const QString program = it.value().toString();
        if (!program.isEmpty()) execs_.insert(it.key(), program);
    }
    items_.clear();
    const QJsonArray items = root.value(QStringLiteral("items")).toArray();
    for (const QJsonValue &value : items) {
        const QJsonObject object = value.toObject();
        const QString kind = object.value(QStringLiteral("kind")).toString();
        Item item;
        if (kind == QLatin1String("launcher") || kind == QLatin1String("minimize") ||
            kind == QLatin1String("workspaces") || kind == QLatin1String("applibrary") ||
            kind == QLatin1String("search") || kind == QLatin1String("taskman") ||
            kind == QLatin1String("overflow")) {
            continue;
        } else if (kind == QLatin1String("stack")) {
            item.kind = Item::Kind::Stack;
            item.id = object.value(QStringLiteral("id")).toString();
            item.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Stack"));
            for (const QJsonValue &app : object.value(QStringLiteral("apps")).toArray()) item.apps << app.toString();
            if (item.id.isEmpty() || item.apps.isEmpty()) continue;
        } else if (kind == QLatin1String("app")) {
            item.kind = Item::Kind::App;
            item.id = object.value(QStringLiteral("id")).toString();
            const QString resolved = apps_->resolve(item.id);
            if (!resolved.isEmpty()) item.id = resolved;
            if (item.id.isEmpty()) continue;
            if (item.id == taskId()) continue;
        } else {
            continue;
        }
        items_.push_back(item);
    }
    for (Item &item : items_) {
        if (item.kind == Item::Kind::App) {
            const QString resolved = apps_->adoptProject(item.id);
            if (!resolved.isEmpty() && resolved != taskId()) item.id = resolved;
            if (item.id == taskId()) item.id.clear();
        } else if (item.kind == Item::Kind::Stack) {
            QStringList apps;
            for (const QString &app : item.apps) {
                const QString resolved = apps_->adoptProject(app);
                const QString use = resolved.isEmpty() ? app : resolved;
                if (!use.isEmpty() && use != taskId() && !apps.contains(use)) apps << use;
            }
            item.apps = apps;
        }
    }
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const Item &item) {
                                    return (item.kind == Item::Kind::App && item.id.isEmpty()) ||
                                           (item.kind == Item::Kind::Stack && item.apps.isEmpty());
                                }),
                 items_.end());
    if (items_.empty()) {
        buildDefault();
        save();
    } else if (version < 3) {
        save();
    }
    applyCosmicSettings(readCosmicDockSettings());
}

bool Dock::isApp(const Live &live, const QString &id) const {
    if (id.isEmpty()) return false;
    if (live.appId == id || live.rawId == id) return true;
    const QString resolved = apps_->resolve(live.rawId);
    return !resolved.isEmpty() && resolved == id;
}

Dock::Screen &Dock::screen(int id) {
    auto it = screens_.find(id);
    if (it == screens_.end()) {
        Screen created;
        created.id = id;
        created.born = now_;
        it = screens_.insert(id, created);
    }
    return it.value();
}

int Dock::itemIndex(const QString &uid) const {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item &item = items_[static_cast<size_t>(i)];
        if (item.kind == Item::Kind::App && uid == QLatin1String("app:") + item.id) return i;
        if (item.kind == Item::Kind::Stack && uid == QLatin1String("stack:") + item.id) return i;
    }
    return -1;
}

QVector<quint64> Dock::keysFor(const QString &appId) const {
    QVector<quint64> keys;
    for (const Live &live : live_) {
        if (isApp(live, appId)) keys.push_back(live.key);
    }
    return keys;
}

void Dock::launchId(const QString &appId) {
    apps_->adoptProject(appId);
    const QString id = canonical(appId);
    noteOpening(id);
    const QString saved = execs_.value(id);
    const QString dir = saved.isEmpty() ? QString() : QFileInfo(saved).absolutePath();
    if (!apps_->launch(appId, saved, dir)) {
        opening_.remove(id);
        return;
    }
    const QString program = apps_->programFor(id);
    if (!program.isEmpty() && !execs_.contains(id)) {
        execs_.insert(id, program);
        save();
    }
}

void Dock::newWindow(const QString &appId) {
    apps_->adoptProject(appId);
    const QString resolved = apps_->resolve(appId);
    const QString id = resolved.isEmpty() ? appId : resolved;
    const DesktopApp *app = apps_->byId(id);
    if (!app) {
        launchId(appId);
        return;
    }
    for (const DesktopAction &action : app->actions) {
        const bool named = action.id.contains(QLatin1String("new-window"), Qt::CaseInsensitive) ||
                           action.name.compare(QLatin1String("New Window"), Qt::CaseInsensitive) == 0;
        if (named && !action.exec.isEmpty()) {
            noteOpening(id);
            const QStringList args = splitExec(action.exec, app);
            if (!args.isEmpty() && startDetachedApp(args.first(), args.mid(1), app->workdir, "new window")) return;
        }
    }
    launchId(id);
}

void Dock::minimizeActive() {
    for (const Live &live : live_) {
        if (live.activated && !live.minimized && onMinimize) {
            onMinimize(live.key);
            return;
        }
    }
}

void Dock::moveItem(int from, int to) {
    if (from < 0 || to < 0 || from == to) return;
    if (from >= static_cast<int>(items_.size()) || to >= static_cast<int>(items_.size())) return;
    if (items_[static_cast<size_t>(from)].kind != Item::Kind::App &&
        items_[static_cast<size_t>(from)].kind != Item::Kind::Stack) {
        return;
    }
    if (items_[static_cast<size_t>(to)].kind != Item::Kind::App &&
        items_[static_cast<size_t>(to)].kind != Item::Kind::Stack) {
        return;
    }
    Item item = items_[static_cast<size_t>(from)];
    items_.erase(items_.begin() + from);
    if (to > from) to -= 1;
    items_.insert(items_.begin() + to, item);
    save();
}

void Dock::mergeItems(const QString &dragUid, const QString &targetUid) {
    const int from = itemIndex(dragUid);
    const int to = itemIndex(targetUid);
    if (from < 0 || to < 0 || from == to) return;
    Item drag = items_[static_cast<size_t>(from)];
    Item target = items_[static_cast<size_t>(to)];
    auto appsOf = [](const Item &item) {
        if (item.kind == Item::Kind::App) return QStringList{item.id};
        if (item.kind == Item::Kind::Stack) return item.apps;
        return QStringList{};
    };
    const QStringList incoming = appsOf(drag);
    if (incoming.isEmpty() || (target.kind != Item::Kind::App && target.kind != Item::Kind::Stack)) return;
    if (target.kind == Item::Kind::App) {
        Item stack;
        stack.kind = Item::Kind::Stack;
        stack.id = QUuid::createUuid().toString(QUuid::Id128).left(8);
        stack.name = QStringLiteral("Stack");
        stack.apps << target.id;
        for (const QString &app : incoming) {
            if (!stack.apps.contains(app)) stack.apps << app;
        }
        const int keep = std::min(from, to);
        const int drop = std::max(from, to);
        items_.erase(items_.begin() + drop);
        items_.erase(items_.begin() + keep);
        items_.insert(items_.begin() + keep, stack);
    } else {
        for (const QString &app : incoming) {
            if (!target.apps.contains(app)) target.apps << app;
        }
        items_[static_cast<size_t>(to)] = target;
        items_.erase(items_.begin() + from);
    }
    save();
}

void Dock::pullFromStack(Screen &screen, const QString &appId) {
    const int index = itemIndex(QStringLiteral("stack:") + screen.shownStack);
    if (index < 0) return;
    Item stack = items_[static_cast<size_t>(index)];
    stack.apps.removeAll(appId);
    Item app;
    app.kind = Item::Kind::App;
    app.id = appId;
    if (stack.apps.isEmpty()) {
        items_[static_cast<size_t>(index)] = app;
        screen.openStack.clear();
        screen.shownStack.clear();
    } else {
        items_[static_cast<size_t>(index)] = stack;
        items_.insert(items_.begin() + index + 1, app);
    }
    screen.menu = false;
    save();
}

void Dock::commitRename(Screen &screen, bool apply) {
    if (apply) {
        const QString name = screen.renameText.trimmed().left(32);
        if (!name.isEmpty()) {
            for (Item &item : items_) {
                if (item.kind == Item::Kind::Stack && item.id == screen.renameStack) item.name = name;
            }
            save();
        }
    }
    screen.rename = false;
    screen.heldKey = -1;
}

void Dock::applyKey(Screen &screen, int code, bool shift) {
    if (screen.search) {
        if (code == KEY_ESC) {
            screen.search = false;
            return;
        }
        if (code == KEY_ENTER || code == KEY_KPENTER) {
            const std::vector<AppHit> matches = apps_->search(screen.searchText, 8);
            if (!matches.empty()) {
                const int row = std::clamp(screen.searchRow, 0, static_cast<int>(matches.size()) - 1);
                launchId(matches[static_cast<size_t>(row)].id);
                screen.search = false;
            }
            return;
        }
        if (code == KEY_UP) {
            if (screen.searchRow > 0) screen.searchRow--;
            return;
        }
        if (code == KEY_DOWN) {
            screen.searchRow++;
            return;
        }
        if (code == KEY_BACKSPACE) {
            if (screen.searchCaret > 0 && !screen.searchText.isEmpty()) {
                screen.searchText.remove(screen.searchCaret - 1, 1);
                screen.searchCaret--;
                screen.searchRow = 0;
            }
            return;
        }
        if (code == KEY_DELETE) {
            if (screen.searchCaret < screen.searchText.size()) screen.searchText.remove(screen.searchCaret, 1);
            return;
        }
        if (code == KEY_LEFT) {
            screen.searchCaret = std::max(0, screen.searchCaret - 1);
            return;
        }
        if (code == KEY_RIGHT) {
            screen.searchCaret = std::min(static_cast<int>(screen.searchText.size()), screen.searchCaret + 1);
            return;
        }
        if (code == KEY_HOME) {
            screen.searchCaret = 0;
            return;
        }
        if (code == KEY_END) {
            screen.searchCaret = screen.searchText.size();
            return;
        }
        const QString typed = keyChar(static_cast<quint32>(code), shift);
        if (typed.isEmpty() || screen.searchText.size() >= 48) return;
        screen.searchText.insert(screen.searchCaret, typed);
        screen.searchCaret += typed.size();
        screen.searchRow = 0;
        return;
    }
    if (screen.about) {
        if (code == KEY_ESC) screen.about = false;
        return;
    }
    if (!screen.rename) return;
    if (code == KEY_ESC) {
        commitRename(screen, false);
        return;
    }
    if (code == KEY_ENTER || code == KEY_KPENTER) {
        commitRename(screen, true);
        return;
    }
    if (code == KEY_BACKSPACE) {
        if (screen.caret > 0 && !screen.renameText.isEmpty()) {
            screen.renameText.remove(screen.caret - 1, 1);
            screen.caret--;
        }
        return;
    }
    if (code == KEY_DELETE) {
        if (screen.caret < screen.renameText.size()) screen.renameText.remove(screen.caret, 1);
        return;
    }
    if (code == KEY_LEFT) {
        screen.caret = std::max(0, screen.caret - 1);
        return;
    }
    if (code == KEY_RIGHT) {
        screen.caret = std::min(static_cast<int>(screen.renameText.size()), screen.caret + 1);
        return;
    }
    if (code == KEY_HOME) {
        screen.caret = 0;
        return;
    }
    if (code == KEY_END) {
        screen.caret = screen.renameText.size();
        return;
    }
    const QString text = keyChar(static_cast<quint32>(code), shift);
    if (text.isEmpty() || screen.renameText.size() >= 32) return;
    screen.renameText.insert(screen.caret, text);
    screen.caret += text.size();
}

void Dock::layoutPopup(Screen &screen) {
    screen.cells.clear();
    screen.popupRect = QRectF();
    const QString stackId = !screen.openStack.isEmpty() ? screen.openStack : screen.shownStack;
    if (stackId.isEmpty() || (screen.openStack.isEmpty() && screen.popup < 0.04)) return;
    const Item *stack = nullptr;
    for (const Item &item : items_) {
        if (item.kind == Item::Kind::Stack && item.id == stackId) stack = &item;
    }
    if (!stack || stack->apps.isEmpty()) return;
    const int count = stack->apps.size();
    const int cols = count <= 1 ? 1 : count <= 4 ? 2 : count <= 9 ? 3 : 4;
    const int rows = (count + cols - 1) / cols;
    const double cellW = 98;
    const double cellH = 86;
    const double pad = 14;
    const double header = 30;
    const double width = cols * cellW + pad * 2;
    const double height = header + rows * cellH + 8;
    QPointF anchor(screen.w / 2.0, screen.pill.top());
    for (const Slot &slot : screen.slots) {
        if (slot.uid == QLatin1String("stack:") + stackId) anchor = slot.rect.center();
    }
    screen.popupRect = popupNear(screen, anchor, width, height);
    const double x = screen.popupRect.x();
    const double y = screen.popupRect.y();
    for (int i = 0; i < count; ++i) {
        Screen::Cell cell;
        cell.appId = stack->apps.at(i);
        cell.label = itemName(cell.appId);
        const int column = i % cols;
        const int row = i / cols;
        cell.rect = QRectF(x + pad + column * cellW, y + header + row * cellH, cellW, cellH);
        const double icon = 48;
        cell.icon = QRectF(cell.rect.center().x() - icon / 2.0, cell.rect.top() + 4, icon, icon);
        screen.cells.push_back(cell);
    }
}

void Dock::layoutPreviews(Screen &screen) {
    screen.cards.clear();
    screen.previewsRect = QRectF();
    const QString appId = !screen.openPreviews.isEmpty() ? screen.openPreviews : screen.shownPreviews;
    if (appId.isEmpty() || (screen.openPreviews.isEmpty() && screen.previews < 0.04)) return;
    std::vector<const Live *> windows;
    for (const Live &live : live_) {
        if (isApp(live, appId)) windows.push_back(&live);
    }
    if (windows.empty()) return;
    std::sort(windows.begin(), windows.end(), [](const Live *a, const Live *b) { return a->key < b->key; });
    const int count = static_cast<int>(windows.size());
    const int cols = count <= 6 ? count : (count <= 8 ? 4 : 5);
    const int rows = (count + cols - 1) / cols;
    double cardW = 168;
    double cardH = 126;
    const double gap = 8;
    const double pad = 10;
    double width = cols * cardW + (cols - 1) * gap + pad * 2;
    double height = rows * cardH + (rows - 1) * gap + pad * 2;
    const double maxW = std::max(220.0, screen.w - 16.0);
    if (width > maxW && width > 1) {
        const double scale = maxW / width;
        cardW *= scale;
        cardH = std::max(78.0, cardH * scale);
        width = cols * cardW + (cols - 1) * gap + pad * 2;
        height = rows * cardH + (rows - 1) * gap + pad * 2;
    }
    QPointF anchor(screen.w / 2.0, screen.pill.center().y());
    for (const Slot &slot : screen.slots) {
        if (slot.kind == Item::Kind::App && slot.appId == appId) anchor = slot.rect.center();
    }
    for (const Slot &slot : screen.slots) {
        if (slot.kind != Item::Kind::Stack) continue;
        const int index = itemIndex(slot.uid);
        if (index >= 0 && items_[static_cast<size_t>(index)].apps.contains(appId)) anchor = slot.rect.center();
    }
    screen.previewsRect = popupNear(screen, anchor, width, height);
    const double x = screen.previewsRect.x();
    const double y = screen.previewsRect.y();
    for (int i = 0; i < count; ++i) {
        const Live *live = windows[static_cast<size_t>(i)];
        Screen::Card card;
        card.key = live->key;
        card.appId = live->appId;
        card.title = live->title.isEmpty() ? itemName(live->appId) : live->title;
        card.active = live->activated && !live->minimized;
        card.minimized = live->minimized;
        const int column = i % cols;
        const int row = i / cols;
        card.rect = QRectF(x + pad + column * (cardW + gap), y + pad + row * (cardH + gap), cardW, cardH);
        card.thumb = card.rect.adjusted(7, 7, -7, -24);
        card.closeRect = QRectF(card.thumb.right() - 22, card.thumb.top() + 4, 20, 20);
        screen.cards.push_back(card);
    }
}

void Dock::setSearch(Screen &screen, bool open) {
    screen.search = open;
    if (!open) return;
    screen.searchText.clear();
    screen.searchCaret = 0;
    screen.searchRow = -1;
    screen.menu = false;
    screen.about = false;
    screen.rename = false;
    screen.openStack.clear();
    screen.openPreviews.clear();
}

void Dock::layoutSearch(Screen &screen) {
    screen.searchHits.clear();
    screen.searchRect = QRectF();
    screen.searchField = QRectF();
    if (!screen.search) return;
    const std::vector<AppHit> matches = apps_->search(screen.searchText, 8);
    const double rowH = 36;
    const double fieldH = 36;
    const double pad = 8;
    const double width = 300;
    const int rows = std::max(1, static_cast<int>(matches.size()));
    const double height = pad + fieldH + 8 + rows * rowH + pad;
    QPointF anchor(screen.pill.center());
    for (const Slot &slot : screen.slots) {
        if (slot.uid == QLatin1String("search")) anchor = slot.rect.center();
    }
    screen.searchRect = popupNear(screen, anchor, width, height);
    const double x = screen.searchRect.x();
    const double y = screen.searchRect.y();
    screen.searchField = QRectF(x + pad, y + pad, width - pad * 2, fieldH);
    if (matches.empty()) {
        screen.searchRow = -1;
        return;
    }
    if (screen.searchRow < 0) screen.searchRow = 0;
    if (screen.searchRow >= static_cast<int>(matches.size())) screen.searchRow = static_cast<int>(matches.size()) - 1;
    const double listTop = screen.searchField.bottom() + 8;
    for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
        Screen::SearchHit hit;
        hit.appId = matches[static_cast<size_t>(i)].id;
        hit.label = matches[static_cast<size_t>(i)].name;
        hit.rect = QRectF(x + 4, listTop + i * rowH, width - 8, rowH);
        hit.icon = QRectF(hit.rect.left() + 8, hit.rect.center().y() - 11, 22, 22);
        screen.searchHits.push_back(hit);
    }
}

void Dock::layoutAbout(Screen &screen) {
    screen.aboutRect = QRectF();
    screen.aboutCoffee = QRectF();
    if (!screen.about) return;
    const double width = 300;
    const double height = 168;
    QPointF anchor(screen.pill.center());
    for (const Slot &slot : screen.slots) {
        if (slot.uid == QLatin1String("overflow")) anchor = slot.rect.center();
    }
    screen.aboutRect = popupNear(screen, anchor, width, height);
    screen.aboutCoffee = QRectF(screen.aboutRect.left() + 12, screen.aboutRect.bottom() - 44, width - 24, 32);
}

void Dock::layoutMenu(Screen &screen) {
    screen.menuRect = QRectF();
    screen.menuRows.clear();
    screen.subRect = QRectF();
    screen.subRows.clear();
    if (!screen.menu || screen.rows.empty()) return;
    const double width = 232;
    const double rowH = 32;
    const double pad = 6;
    double height = pad * 2;
    for (const MenuRow &row : screen.rows) height += row.action == QLatin1String("-") ? 9 : rowH;
    QPointF anchor(screen.pill.center());
    if (!screen.menuApp.isEmpty()) {
        for (const Screen::Cell &cell : screen.cells) {
            if (cell.appId == screen.menuApp) anchor = cell.rect.center();
        }
    }
    for (const Slot &slot : screen.slots) {
        if (slot.uid == screen.menuUid) anchor = slot.rect.center();
    }
    screen.menuRect = popupNear(screen, anchor, width, height);
    const double x = screen.menuRect.x();
    const double y = screen.menuRect.y();
    double cursor = y + pad;
    for (const MenuRow &row : screen.rows) {
        const double h = row.action == QLatin1String("-") ? 9 : rowH;
        screen.menuRows.push_back(QRectF(x + 4, cursor, width - 8, h));
        cursor += h;
    }
    if (screen.menuRow >= 0 && screen.menuRow < static_cast<int>(screen.rows.size()) &&
        !screen.rows[static_cast<size_t>(screen.menuRow)].sub.empty()) {
        const MenuRow &parent = screen.rows[static_cast<size_t>(screen.menuRow)];
        const double subH = pad * 2 + parent.sub.size() * rowH;
        const double subW = 210;
        double sx = screen.menuRect.right() + 6;
        if (sx + subW > screen.w - 8) sx = screen.menuRect.left() - 6 - subW;
        double sy = screen.menuRows[static_cast<size_t>(screen.menuRow)].top() - pad;
        sy = std::clamp(sy, 8.0, std::max(8.0, screen.h - subH - 8.0));
        screen.subRect = QRectF(sx, sy, subW, subH);
        for (int i = 0; i < static_cast<int>(parent.sub.size()); ++i) {
            screen.subRows.push_back(QRectF(sx + 4, sy + pad + i * rowH, subW - 8, rowH));
        }
    }
}

void Dock::relayout(Screen &screen) {
    const QString monitor = taskId();
    std::vector<Slot> left;
    auto addWing = [&](Item::Kind kind, const QString &uid, const QString &label) {
        Slot slot;
        slot.kind = kind;
        slot.uid = uid;
        slot.label = label;
        left.push_back(slot);
    };
    addWing(Item::Kind::Launcher, QStringLiteral("launcher"), QStringLiteral("Launcher"));
    addWing(Item::Kind::Workspaces, QStringLiteral("workspaces"), QStringLiteral("Workspaces"));
    addWing(Item::Kind::AppLibrary, QStringLiteral("applibrary"), QStringLiteral("Applications"));
    addWing(Item::Kind::Search, QStringLiteral("search"), QStringLiteral("Search"));

    std::vector<Slot> center;
    QSet<QString> claimed;
    claimed.insert(monitor);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item &item = items_[static_cast<size_t>(i)];
        if (item.kind != Item::Kind::App && item.kind != Item::Kind::Stack) continue;
        if (item.kind == Item::Kind::App && (item.id == monitor || canonical(item.id) == monitor)) continue;
        Slot slot;
        slot.itemIndex = i;
        if (item.kind == Item::Kind::App) {
            slot.kind = Item::Kind::App;
            slot.appId = item.id;
            slot.uid = QStringLiteral("app:") + item.id;
            slot.label = itemName(item.id);
            claimed.insert(item.id);
            claimed.insert(canonical(item.id));
            for (const Live &live : live_) {
                if (!isApp(live, item.id)) continue;
                slot.running++;
                if (live.activated && !live.minimized) slot.active = true;
            }
        } else {
            slot.kind = Item::Kind::Stack;
            slot.appId = item.id;
            slot.uid = QStringLiteral("stack:") + item.id;
            slot.label = item.name.isEmpty() ? QStringLiteral("Stack") : item.name;
            for (const QString &app : item.apps) {
                claimed.insert(app);
                claimed.insert(canonical(app));
                for (const Live &live : live_) {
                    if (!isApp(live, app)) continue;
                    slot.running++;
                    if (live.activated && !live.minimized) slot.active = true;
                }
            }
        }
        center.push_back(slot);
    }

    QSet<QString> seen;
    for (const Live &live : live_) {
        if (live.minimized) continue;
        const QString id = canonical(live.appId);
        if (id.isEmpty() || id == monitor || claimed.contains(live.appId) || claimed.contains(id) || seen.contains(id))
            continue;
        seen.insert(id);
        Slot slot;
        slot.kind = Item::Kind::App;
        slot.appId = id;
        slot.uid = QStringLiteral("run:") + id;
        slot.label = itemName(id);
        slot.itemIndex = -1;
        for (const Live &other : live_) {
            if (other.minimized || !isApp(other, id)) continue;
            slot.running++;
            if (other.activated) slot.active = true;
        }
        center.push_back(slot);
    }

    std::vector<Slot> right;
    for (const Live &live : live_) {
        if (!live.minimized) continue;
        if (!live.outputs.isEmpty() && !live.outputs.contains(screen.id)) continue;
        Slot slot;
        slot.kind = Item::Kind::Minimized;
        slot.appId = canonical(live.appId);
        slot.uid = QStringLiteral("min:") + QString::number(live.key);
        slot.label = live.title.isEmpty() ? itemName(slot.appId) : live.title;
        slot.windowKey = live.key;
        slot.running = 1;
        right.push_back(slot);
    }
    Slot task;
    task.kind = Item::Kind::TaskManager;
    task.uid = QStringLiteral("taskman");
    task.appId = monitor;
    task.label = QStringLiteral("System Monitor");
    task.gapBefore = 18;
    for (const Live &live : live_) {
        if (!isApp(live, monitor)) continue;
        task.running++;
        if (live.activated && !live.minimized) task.active = true;
    }
    right.push_back(task);
    Slot overflow;
    overflow.kind = Item::Kind::Overflow;
    overflow.uid = QStringLiteral("overflow");
    overflow.label = QStringLiteral("Menu");
    right.push_back(overflow);

    const double thick = barThickness();
    const double size = iconSize_;
    const bool hor = horizontal();
    const double length = hor ? screen.w : screen.h;
    QRectF bar;
    if (edge_ == ShellSpec::Edge::Top) bar = QRectF(0, margin_ - screen.hidden, screen.w, thick);
    else if (edge_ == ShellSpec::Edge::Left) bar = QRectF(margin_ - screen.hidden, 0, thick, screen.h);
    else if (edge_ == ShellSpec::Edge::Right)
        bar = QRectF(screen.w - margin_ - thick + screen.hidden, 0, thick, screen.h);
    else
        bar = QRectF(0, screen.h - margin_ - thick + screen.hidden, screen.w, thick);
    double iconCross = bar.top() + pad_;
    if (edge_ == ShellSpec::Edge::Top) iconCross = bar.top() + pad_ + 8;
    else if (edge_ == ShellSpec::Edge::Left) iconCross = bar.left() + pad_ + 8;
    else if (edge_ == ShellSpec::Edge::Right) iconCross = bar.left() + pad_;

    auto spanOf = [&](const std::vector<Slot> &row) {
        if (row.empty()) return 0.0;
        double extra = 0;
        for (const Slot &slot : row) extra += slot.gapBefore;
        return row.size() * (size + kGap) - kGap + extra;
    };
    auto placeRow = [&](std::vector<Slot> &row, double start) {
        double cursor = start;
        for (Slot &slot : row) {
            if (slot.gapBefore > 0) cursor += slot.gapBefore;
            const double scale = magnify_ ? screen.scale.value(slot.uid, 1.0) : 1.0;
            const double side = size * scale;
            if (hor) {
                const double y = edge_ == ShellSpec::Edge::Top ? iconCross : iconCross + (size - side);
                slot.rect = QRectF(cursor, y, side, side);
                slot.hit = QRectF(cursor - kGap / 2.0, bar.top(), side + kGap, bar.height());
            } else {
                const double x = edge_ == ShellSpec::Edge::Right ? iconCross + (size - side) : iconCross;
                slot.rect = QRectF(x, cursor, side, side);
                slot.hit = QRectF(bar.left(), cursor - kGap / 2.0, bar.width(), side + kGap);
            }
            cursor += side + kGap;
        }
        return cursor;
    };

    if (expand_) {
        const double leftStart = pad_ + 4;
        const double leftEnd = placeRow(left, leftStart);
        const double rightSpan = spanOf(right);
        const double rightStart = length - pad_ - 4 - rightSpan;
        placeRow(right, std::max(leftEnd + 12, rightStart));
        const double centerSpan = spanOf(center);
        const double availLeft = leftEnd + 16;
        const double availRight = (right.empty() ? length - pad_ : rightStart) - 16;
        double centerStart = availLeft + std::max(0.0, (availRight - availLeft - centerSpan) / 2.0);
        if (centerStart + centerSpan > availRight) centerStart = std::max(availLeft, availRight - centerSpan);
        placeRow(center, centerStart);
        screen.pill = bar;
    } else {
        const double total = spanOf(left) + spanOf(center) + spanOf(right) + 32;
        double cursor = std::max(8.0, (length - total) / 2.0);
        cursor = placeRow(left, cursor) + 16;
        cursor = placeRow(center, cursor) + 16;
        placeRow(right, cursor);
        QRectF content;
        const std::vector<std::vector<Slot> *> rows = {&left, &center, &right};
        for (const std::vector<Slot> *row : rows) {
            for (const Slot &slot : *row) content = content.united(slot.rect);
        }
        if (hor)
            screen.pill = QRectF(content.left() - 10, bar.top(), content.width() + 20, bar.height());
        else
            screen.pill = QRectF(bar.left(), content.top() - 10, bar.width(), content.height() + 20);
    }

    screen.slots.clear();
    screen.slots.insert(screen.slots.end(), left.begin(), left.end());
    screen.slots.insert(screen.slots.end(), center.begin(), center.end());
    screen.slots.insert(screen.slots.end(), right.begin(), right.end());
    layoutPopup(screen);
    layoutPreviews(screen);
    layoutMenu(screen);
    layoutSearch(screen);
    layoutAbout(screen);
}

Dock::Hit Dock::hitTest(const Screen &screen, double x, double y) const {
    Hit hit;
    const QPointF point(x, y);
    if (screen.menu && screen.subRect.contains(point) && screen.menuRow >= 0 &&
        screen.menuRow < static_cast<int>(screen.rows.size())) {
        const MenuRow &parent = screen.rows[static_cast<size_t>(screen.menuRow)];
        for (int i = 0; i < static_cast<int>(screen.subRows.size()) && i < static_cast<int>(parent.sub.size()); ++i) {
            if (!screen.subRows[static_cast<size_t>(i)].contains(point)) continue;
            hit.type = 4;
            hit.action = parent.sub[static_cast<size_t>(i)].action;
            hit.appId = screen.menuApp;
            hit.index = i;
            return hit;
        }
    }
    if (screen.menu && screen.menuRect.contains(point)) {
        for (int i = 0; i < static_cast<int>(screen.menuRows.size()) && i < static_cast<int>(screen.rows.size()); ++i) {
            if (!screen.menuRows[static_cast<size_t>(i)].contains(point)) continue;
            hit.type = 3;
            hit.action = screen.rows[static_cast<size_t>(i)].action;
            hit.index = i;
            hit.uid = screen.menuUid;
            hit.appId = screen.menuApp;
            return hit;
        }
    }
    if (screen.search && screen.searchRect.contains(point)) {
        for (int i = 0; i < static_cast<int>(screen.searchHits.size()); ++i) {
            const Screen::SearchHit &row = screen.searchHits[static_cast<size_t>(i)];
            if (!row.rect.contains(point)) continue;
            hit.type = 7;
            hit.appId = row.appId;
            hit.index = i;
            return hit;
        }
        hit.type = 7;
        return hit;
    }
    if (screen.about && screen.aboutRect.contains(point)) {
        if (screen.aboutCoffee.contains(point)) {
            hit.type = 9;
            hit.action = QStringLiteral("contribute");
            return hit;
        }
        hit.type = 8;
        return hit;
    }
    if (screen.previews > 0.35 && screen.previewsRect.contains(point)) {
        for (int i = 0; i < static_cast<int>(screen.cards.size()); ++i) {
            const Screen::Card &card = screen.cards[static_cast<size_t>(i)];
            if (card.closeRect.contains(point)) {
                hit.type = 6;
                hit.key = card.key;
                hit.appId = card.appId;
                hit.index = i;
                return hit;
            }
        }
        for (int i = 0; i < static_cast<int>(screen.cards.size()); ++i) {
            const Screen::Card &card = screen.cards[static_cast<size_t>(i)];
            if (!card.rect.contains(point)) continue;
            hit.type = 5;
            hit.key = card.key;
            hit.appId = card.appId;
            hit.index = i;
            return hit;
        }
        hit.type = 5;
        return hit;
    }
    if (screen.popup > 0.35 && screen.popupRect.contains(point)) {
        for (const Screen::Cell &cell : screen.cells) {
            if (!cell.rect.contains(point)) continue;
            hit.type = 2;
            hit.appId = cell.appId;
            hit.uid = QStringLiteral("stack:") + screen.shownStack;
            return hit;
        }
        hit.type = 2;
        return hit;
    }
    if (screen.hidden < barThickness() * 0.55) {
        for (const Slot &slot : screen.slots) {
            if (!slot.hit.contains(point)) continue;
            hit.type = 1;
            hit.uid = slot.uid;
            hit.appId = slot.appId;
            return hit;
        }
    }
    return hit;
}

void Dock::retarget(Screen &screen) {
    const bool over = magnify_ && screen.pointer && screen.hidden < 14 && screen.openStack.isEmpty() && !screen.menu &&
                      !screen.search && !screen.about &&
                      screen.pill.adjusted(-16, -28, 16, 12).contains(screen.x, screen.y);
    const double maxScale = over ? 1.45 : 1.0;
    const double sigma = iconSize_ * 0.92;
    for (const Slot &slot : screen.slots) {
        double target = 1;
        if (over) {
            const double dist = horizontal() ? std::abs(screen.x - slot.rect.center().x())
                                              : std::abs(screen.y - slot.rect.center().y());
            const double influence = std::exp(-(dist * dist) / (2 * sigma * sigma));
            target = 1 + (maxScale - 1) * influence;
        }
        screen.target.insert(slot.uid, target);
    }
}

void Dock::updateHidden(Screen &screen) {
    bool fullscreen = false;
    bool overlap = false;
    const double barH = barThickness();
    QRectF band;
    if (edge_ == ShellSpec::Edge::Top) band = QRectF(0, 0, screen.w, barH + margin_ + 24);
    else if (edge_ == ShellSpec::Edge::Left) band = QRectF(0, 0, barH + margin_ + 24, screen.h);
    else if (edge_ == ShellSpec::Edge::Right)
        band = QRectF(screen.w - (barH + margin_ + 24), 0, barH + margin_ + 24, screen.h);
    else
        band = QRectF(0, screen.h - (barH + margin_ + 24), screen.w, barH + margin_ + 24);
    for (const Live &live : live_) {
        if (!live.outputs.isEmpty() && !live.outputs.contains(screen.id)) continue;
        if (live.minimized) continue;
        if (live.fullscreen) {
            fullscreen = true;
            overlap = true;
        }
        if (live.maximized) overlap = true;
        if (live.geom.contains(screen.id)) {
            const QRect g = live.geom.value(screen.id);
            if (QRectF(g).intersects(band)) overlap = true;
            if (horizontal() && (g.height() > screen.h * 0.72 || (edge_ == ShellSpec::Edge::Bottom && g.bottom() > screen.h - barH - 8) ||
                                 (edge_ == ShellSpec::Edge::Top && g.top() < barH + 8))) {
                overlap = true;
            }
            if (!horizontal() && (g.width() > screen.w * 0.72 || (edge_ == ShellSpec::Edge::Left && g.left() < barH + 8) ||
                                  (edge_ == ShellSpec::Edge::Right && g.right() > screen.w - barH - 8))) {
                overlap = true;
            }
        } else if (live.outputs.contains(screen.id) || live.outputs.isEmpty()) {
            if (live.activated) overlap = true;
        }
    }
    const bool overBar = screen.pointer && screen.pill.adjusted(-8, -8, 8, 8).contains(screen.x, screen.y);
    const bool hold = screen.menu || screen.search || screen.about || !screen.openStack.isEmpty() ||
                      !screen.openPreviews.isEmpty() || screen.dragging || screen.rename ||
                      (screen.pointer && screen.hidden < barH * 0.5 &&
                       (overBar || screen.popupRect.contains(screen.x, screen.y) ||
                        screen.previewsRect.contains(screen.x, screen.y) || screen.searchRect.contains(screen.x, screen.y) ||
                        screen.aboutRect.contains(screen.x, screen.y)));
    const double reach = std::max(handle_ + 4.0, 8.0);
    bool hot = false;
    if (screen.pointer) {
        if (edge_ == ShellSpec::Edge::Top) hot = screen.y <= reach;
        else if (edge_ == ShellSpec::Edge::Left) hot = screen.x <= reach;
        else if (edge_ == ShellSpec::Edge::Right) hot = screen.x >= screen.w - reach;
        else hot = screen.y >= screen.h - reach;
    }
    const double tucked = std::max(0.0, barH - handle_);
    if (fullscreen && !qEnvironmentVariableIsSet("MAGIC_DOCK_SHOW_IN_FULLSCREEN") &&
        !qEnvironmentVariableIsSet("COSMIC_BAR_SHOW_IN_FULLSCREEN") && !hold && !hot) {
        screen.hiddenTarget = tucked;
        screen.hideArmed = false;
        return;
    }
    if (hold) {
        screen.hiddenTarget = 0;
        screen.hideArmed = false;
        screen.showArmed = false;
        return;
    }
    if (hot) {
        if (!screen.showArmed) {
            screen.showArmed = true;
            screen.showAt = now_ + unhideDelayMs_;
        }
        if (now_ >= screen.showAt) screen.hiddenTarget = 0;
        screen.hideArmed = false;
        return;
    }
    screen.showArmed = false;
    const bool wantHide = hideMode_ == Hide::Always || (hideMode_ == Hide::Overlap && overlap);
    if (!wantHide) {
        screen.hiddenTarget = 0;
        screen.hideArmed = false;
        return;
    }
    if (!screen.hideArmed) {
        screen.hideArmed = true;
        screen.hideAt = now_ + hideWaitMs_;
    }
    if (now_ >= screen.hideAt) screen.hiddenTarget = tucked;
}

QRegion Dock::inputRegion(const Screen &screen) const {
    QRegion region;
    auto add = [&](QRectF rect) {
        rect = rect.intersected(QRectF(0, 0, screen.w, screen.h));
        const QRect rounded = rect.toAlignedRect();
        if (rounded.width() > 0 && rounded.height() > 0) region += rounded;
    };
    const bool tucked = screen.hidden > barThickness() * 0.62 && screen.openStack.isEmpty() && screen.openPreviews.isEmpty() &&
                        !screen.menu && !screen.search && !screen.about;
    if (tucked) {
        if (edge_ == ShellSpec::Edge::Top) add(QRectF(0, 0, screen.w, handle_ + margin_));
        else if (edge_ == ShellSpec::Edge::Left) add(QRectF(0, 0, handle_ + margin_, screen.h));
        else if (edge_ == ShellSpec::Edge::Right) add(QRectF(screen.w - handle_ - margin_, 0, handle_ + margin_, screen.h));
        else add(QRectF(0, screen.h - handle_ - margin_, screen.w, handle_ + margin_));
        return region;
    }
    add(screen.pill.adjusted(-2, -2, 2, 2));
    auto bridge = [&](const QRectF &popup) {
        add(popup.adjusted(-4, -4, 4, 4));
        QRectF span(popup.center(), screen.pill.center());
        span = span.normalized().adjusted(-28, -28, 28, 28);
        add(span);
    };
    if (screen.popup > 0.2 && !screen.popupRect.isEmpty()) bridge(screen.popupRect);
    if (screen.previews > 0.2 && !screen.previewsRect.isEmpty()) bridge(screen.previewsRect);
    if (screen.menu) {
        add(screen.menuRect.adjusted(-2, -2, 2, 2));
        add(screen.subRect.adjusted(-2, -2, 2, 2));
    }
    if (screen.search && !screen.searchRect.isEmpty()) bridge(screen.searchRect);
    if (screen.about && !screen.aboutRect.isEmpty()) bridge(screen.aboutRect);
    return region;
}

void Dock::activateApp(const QString &appId) {
    const QVector<quint64> keys = keysFor(appId);
    if (keys.isEmpty()) {
        launchId(appId);
        return;
    }
    const Live *chosen = nullptr;
    int count = 0;
    for (const Live &live : live_) {
        if (!isApp(live, appId)) continue;
        count++;
        if (!live.minimized) chosen = &live;
    }
    if (!chosen) {
        for (const Live &live : live_) {
            if (isApp(live, appId)) chosen = &live;
        }
    }
    if (!chosen) return;
    if (chosen->minimized) {
        if (onUnminimize) onUnminimize(chosen->key);
        return;
    }
    Q_UNUSED(count);
    if (chosen->activated) {
        if (onMinimize) onMinimize(chosen->key);
        return;
    }
    if (onActivate) onActivate(chosen->key);
}

void Dock::clickSlot(Screen &screen, const QString &uid) {
    const Slot *slot = nullptr;
    for (const Slot &candidate : screen.slots) {
        if (candidate.uid == uid) slot = &candidate;
    }
    if (!slot) return;
    if (slot->kind == Item::Kind::Launcher) {
        startClean(QStringLiteral("cosmic-launcher"));
        return;
    }
    if (slot->kind == Item::Kind::Workspaces) {
        startClean(QStringLiteral("cosmic-workspaces"));
        return;
    }
    if (slot->kind == Item::Kind::AppLibrary) {
        startClean(QStringLiteral("cosmic-app-library"));
        return;
    }
    if (slot->kind == Item::Kind::Search) {
        screen.menu = false;
        setSearch(screen, !screen.search);
        return;
    }
    if (slot->kind == Item::Kind::Overflow) {
        screen.search = false;
        if (screen.menu && screen.menuUid == QLatin1String("overflow")) {
            screen.menu = false;
            screen.about = false;
        } else {
            openMenu(screen, QStringLiteral("overflow"), {});
        }
        return;
    }
    if (slot->kind == Item::Kind::TaskManager) {
        launchId(slot->appId);
        return;
    }
    if (slot->kind == Item::Kind::Minimized) {
        if (onUnminimize) onUnminimize(slot->windowKey);
        return;
    }
    if (slot->kind == Item::Kind::Stack) {
        const QString id = slot->appId;
        if (screen.openStack == id) screen.openStack.clear();
        else {
            screen.openStack = id;
            screen.shownStack = id;
            screen.menu = false;
            screen.openPreviews.clear();
        }
        return;
    }
    const int count = keysFor(slot->appId).size();
    if (count > 1) {
        if (screen.openPreviews == slot->appId) {
            screen.openPreviews.clear();
        } else {
            screen.openPreviews = slot->appId;
            screen.shownPreviews = slot->appId;
            screen.menu = false;
            screen.openStack.clear();
        }
        return;
    }
    activateApp(slot->appId);
}

void Dock::openMenu(Screen &screen, const QString &uid, const QString &appId) {
    screen.openPreviews.clear();
    screen.search = false;
    screen.about = false;
    screen.rows.clear();
    screen.menu = true;
    screen.menuUid = uid;
    screen.menuApp = appId;
    screen.menuRow = -1;
    screen.subRow = -1;
    auto add = [&](const QString &action, const QString &label, bool enabled = true) {
        MenuRow row;
        row.action = action;
        row.label = label;
        row.enabled = enabled;
        screen.rows.push_back(row);
    };
    auto sep = [&]() {
        MenuRow row;
        row.action = QStringLiteral("-");
        screen.rows.push_back(row);
    };

    if (uid == QLatin1String("launcher")) {
        add(QStringLiteral("launcher"), QStringLiteral("Open Launcher"));
        return;
    }
    if (uid == QLatin1String("workspaces")) {
        add(QStringLiteral("workspaces"), QStringLiteral("Open Workspaces"));
        return;
    }
    if (uid == QLatin1String("applibrary")) {
        add(QStringLiteral("app-library"), QStringLiteral("Open Applications"));
        return;
    }
    if (uid == QLatin1String("search")) {
        add(QStringLiteral("search"), QStringLiteral("Search Apps"));
        return;
    }
    if (uid == QLatin1String("overflow")) {
        add(QStringLiteral("settings"), QStringLiteral("Settings"));
        add(QStringLiteral("about"), QStringLiteral("About"));
        add(QStringLiteral("contribute"), QStringLiteral("Contribute"));
        return;
    }
    if (uid == QLatin1String("taskman")) {
        add(QStringLiteral("taskman"), QStringLiteral("Open System Monitor"));
        return;
    }
    if (uid.startsWith(QLatin1String("min:"))) {
        add(QStringLiteral("restore"), QStringLiteral("Restore"));
        add(QStringLiteral("close-one"), QStringLiteral("Close"));
        return;
    }
    const int stackAt = itemIndex(uid);
    const bool isStack = stackAt >= 0 && items_[static_cast<size_t>(stackAt)].kind == Item::Kind::Stack && appId == items_[static_cast<size_t>(stackAt)].id;
    if (isStack && appId == items_[static_cast<size_t>(stackAt)].id && (screen.menuApp.isEmpty() || screen.menuApp == items_[static_cast<size_t>(stackAt)].id)) {
        const Item &stack = items_[static_cast<size_t>(stackAt)];
        MenuRow header;
        header.header = true;
        header.label = stack.name;
        header.action = QStringLiteral("header");
        header.enabled = false;
        screen.rows.push_back(header);
        add(QStringLiteral("rename"), QStringLiteral("Rename Stack"));
        add(QStringLiteral("open-all"), QStringLiteral("Open All"));
        add(QStringLiteral("ungroup"), QStringLiteral("Ungroup"));
        return;
    }

    const QString id = appId;
    MenuRow header;
    header.header = true;
    header.enabled = false;
    header.action = QStringLiteral("header");
    header.label = itemName(id);
    screen.rows.push_back(header);
    add(QStringLiteral("new-window"), QStringLiteral("New Window"));
    if (const DesktopApp *app = apps_->byId(apps_->resolve(id).isEmpty() ? id : apps_->resolve(id))) {
        for (const DesktopAction &action : app->actions) {
            if (action.name.isEmpty() || action.id.contains(QLatin1String("new-window"))) continue;
            if (action.name.compare(QLatin1String("New Window"), Qt::CaseInsensitive) == 0) continue;
            add(QStringLiteral("desktop-action:") + action.id, action.name);
        }
    }
    sep();
    const bool pinned = itemIndex(QStringLiteral("app:") + id) >= 0;
    const int stackOf = [&]() {
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (items_[static_cast<size_t>(i)].kind == Item::Kind::Stack && items_[static_cast<size_t>(i)].apps.contains(id)) return i;
        }
        return -1;
    }();
    if (pinned) add(QStringLiteral("unpin"), QStringLiteral("Unpin"));
    else if (stackOf < 0) add(QStringLiteral("pin"), QStringLiteral("Pin to Dock"));
    if (stackOf >= 0) add(QStringLiteral("remove-from-stack"), QStringLiteral("Remove from Stack"));
    MenuRow addTo;
    addTo.action = QStringLiteral("add-submenu");
    addTo.label = QStringLiteral("Add to Stack");
    for (const Item &item : items_) {
        if (item.kind != Item::Kind::Stack) continue;
        if (stackOf >= 0 && items_[static_cast<size_t>(stackOf)].id == item.id) continue;
        MenuRow child;
        child.action = QStringLiteral("add-to:") + item.id;
        child.label = item.name;
        addTo.sub.push_back(child);
    }
    MenuRow created;
    created.action = QStringLiteral("new-stack");
    created.label = QStringLiteral("New Stack");
    addTo.sub.push_back(created);
    screen.rows.push_back(addTo);
    const QVector<quint64> keys = keysFor(id);
    if (!keys.isEmpty()) {
        sep();
        for (quint64 key : keys) {
            QString title;
            for (const Live &live : live_) {
                if (live.key == key) title = live.title;
            }
            if (title.isEmpty()) title = QStringLiteral("Window");
            add(QStringLiteral("focus:") + QString::number(key), title);
        }
        add(QStringLiteral("close-all"), QStringLiteral("Close"));
    }
}

void Dock::runAction(Screen &screen, const QString &action, const QString &appId) {
    const QString id = apps_->resolve(appId).isEmpty() ? appId : apps_->resolve(appId);
    if (action == QLatin1String("-") || action == QLatin1String("header") || action == QLatin1String("add-submenu")) return;
    if (action == QLatin1String("launcher")) {
        startClean(QStringLiteral("cosmic-launcher"));
    } else if (action == QLatin1String("workspaces")) {
        startClean(QStringLiteral("cosmic-workspaces"));
    } else if (action == QLatin1String("app-library")) {
        startClean(QStringLiteral("cosmic-app-library"));
    } else if (action == QLatin1String("search")) {
        setSearch(screen, true);
    } else if (action == QLatin1String("settings")) {
        startClean(QStringLiteral("cosmic-settings"));
    } else if (action == QLatin1String("about")) {
        screen.search = false;
        screen.openStack.clear();
        screen.openPreviews.clear();
        screen.about = true;
    } else if (action == QLatin1String("contribute")) {
        openContribute();
    } else if (action == QLatin1String("taskman")) {
        launchId(QStringLiteral("com.system76.CosmicMonitor"));
    } else if (action == QLatin1String("restore")) {
        bool ok = false;
        const quint64 key = screen.menuUid.mid(4).toULongLong(&ok);
        if (ok && onUnminimize) onUnminimize(key);
    } else if (action == QLatin1String("close-one")) {
        bool ok = false;
        const quint64 key = screen.menuUid.mid(4).toULongLong(&ok);
        if (ok && onClose) onClose(key);
    } else if (action == QLatin1String("new-window")) {
        newWindow(id);
    } else if (action.startsWith(QLatin1String("desktop-action:"))) {
        const QString actionId = action.mid(15);
        if (const DesktopApp *app = apps_->byId(id)) {
            for (const DesktopAction &entry : app->actions) {
                if (entry.id == actionId) {
                    noteOpening(id);
                    const QStringList args = splitExec(entry.exec, app);
                    if (args.isEmpty()) break;
                    startDetachedApp(args.first(), args.mid(1), app->workdir, "desktop action");
                }
            }
        }
    } else if (action == QLatin1String("pin")) {
        const QString adopted = apps_->adoptProject(id);
        const QString use = adopted.isEmpty() ? id : adopted;
        Item item;
        item.kind = Item::Kind::App;
        item.id = use;
        items_.push_back(item);
        const QString program = apps_->programFor(use);
        if (!program.isEmpty()) execs_.insert(canonical(use), program);
        save();
    } else if (action == QLatin1String("unpin")) {
        const int index = itemIndex(QStringLiteral("app:") + id);
        if (index >= 0) items_.erase(items_.begin() + index);
        save();
    } else if (action == QLatin1String("new-stack")) {
        const int index = itemIndex(QStringLiteral("app:") + id);
        Item stack;
        stack.kind = Item::Kind::Stack;
        stack.id = QUuid::createUuid().toString(QUuid::Id128).left(8);
        stack.name = itemName(id);
        stack.apps << id;
        if (index >= 0) items_[static_cast<size_t>(index)] = stack;
        else items_.push_back(stack);
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (items_[static_cast<size_t>(i)].kind == Item::Kind::Stack && items_[static_cast<size_t>(i)].id != stack.id)
                items_[static_cast<size_t>(i)].apps.removeAll(id);
        }
        save();
    } else if (action.startsWith(QLatin1String("add-to:"))) {
        const QString stackId = action.mid(7);
        for (Item &item : items_) {
            if (item.kind == Item::Kind::Stack && item.id == stackId && !item.apps.contains(id)) item.apps << id;
            if (item.kind == Item::Kind::Stack && item.id != stackId) item.apps.removeAll(id);
        }
        const int index = itemIndex(QStringLiteral("app:") + id);
        if (index >= 0) items_.erase(items_.begin() + index);
        save();
    } else if (action == QLatin1String("remove-from-stack")) {
        for (Item &item : items_) {
            if (item.kind == Item::Kind::Stack) item.apps.removeAll(id);
        }
        items_.erase(std::remove_if(items_.begin(), items_.end(),
                                    [](const Item &item) { return item.kind == Item::Kind::Stack && item.apps.isEmpty(); }),
                     items_.end());
        const int index = itemIndex(QStringLiteral("app:") + id);
        if (index < 0) {
            Item item;
            item.kind = Item::Kind::App;
            item.id = id;
            items_.push_back(item);
        }
        save();
    } else if (action == QLatin1String("ungroup")) {
        const int index = itemIndex(screen.menuUid);
        if (index >= 0 && items_[static_cast<size_t>(index)].kind == Item::Kind::Stack) {
            const QStringList apps = items_[static_cast<size_t>(index)].apps;
            items_.erase(items_.begin() + index);
            int at = index;
            for (const QString &app : apps) {
                Item item;
                item.kind = Item::Kind::App;
                item.id = app;
                items_.insert(items_.begin() + at, item);
                at++;
            }
            screen.openStack.clear();
            save();
        }
    } else if (action == QLatin1String("rename")) {
        const int index = itemIndex(screen.menuUid);
        if (index >= 0) {
            screen.rename = true;
            screen.renameStack = items_[static_cast<size_t>(index)].id;
            screen.renameText = items_[static_cast<size_t>(index)].name;
            screen.caret = screen.renameText.size();
        }
    } else if (action == QLatin1String("open-all")) {
        const int index = itemIndex(screen.menuUid);
        if (index >= 0) {
            for (const QString &app : items_[static_cast<size_t>(index)].apps) {
                if (keysFor(app).isEmpty()) launchId(app);
            }
        }
    } else if (action.startsWith(QLatin1String("focus:"))) {
        const quint64 key = action.mid(6).toULongLong();
        bool minimized = false;
        for (const Live &live : live_) {
            if (live.key == key) minimized = live.minimized;
        }
        if (minimized) {
            if (onUnminimize) onUnminimize(key);
        } else if (onActivate) {
            onActivate(key);
        }
    } else if (action == QLatin1String("close-all")) {
        const QVector<quint64> keys = keysFor(id);
        for (quint64 key : keys) {
            if (onClose) onClose(key);
        }
    }
    if (action != QLatin1String("rename") && action != QLatin1String("add-submenu")) screen.menu = false;
}

void Dock::dragMotion(Screen &screen, double x, double y) {
    if (!screen.dragging) return;
    const Hit hit = hitTest(screen, x, y);
    if (screen.pressCell && pulledToDock(screen, x, y)) {
        screen.hoverUid.clear();
        screen.mergeReady = false;
        return;
    }
    const int from = itemIndex(screen.dragUid);
    if (from >= 0 && hit.type == 1 && !hit.uid.isEmpty() && hit.uid != screen.dragUid) {
        const int to = itemIndex(hit.uid);
        if (to >= 0) {
            const Slot *slot = nullptr;
            for (const Slot &candidate : screen.slots)
                if (candidate.uid == hit.uid) slot = &candidate;
            const bool passed = horizontal()
                                    ? ((to < from && x < slot->rect.center().x()) || (to > from && x > slot->rect.center().x()))
                                    : ((to < from && y < slot->rect.center().y()) || (to > from && y > slot->rect.center().y()));
            if (slot && passed) {
                moveItem(from, to);
                relayout(screen);
            }
            if (screen.hoverUid != hit.uid) {
                screen.hoverUid = hit.uid;
                screen.hoverSince = now_;
                screen.mergeReady = false;
            } else if (now_ - screen.hoverSince > 420) {
                const int hoverIndex = itemIndex(hit.uid);
                screen.mergeReady = hoverIndex >= 0;
            }
        }
    } else {
        screen.hoverUid.clear();
        screen.mergeReady = false;
    }
}

void Dock::pressAt(Screen &screen, double x, double y, bool right, bool middle) {
    relayout(screen);
    const Hit hit = hitTest(screen, x, y);
    if (right) {
        if (hit.type == 1) openMenu(screen, hit.uid, hit.appId.isEmpty() ? hit.uid : hit.appId);
        else if (hit.type == 2 && !hit.appId.isEmpty()) openMenu(screen, hit.uid, hit.appId);
        else screen.menu = false;
        return;
    }
    if (middle) {
        if ((hit.type == 5 || hit.type == 6) && hit.key != 0) {
            if (onClose) onClose(hit.key);
            return;
        }
        const QString id = !hit.appId.isEmpty() ? hit.appId : QString();
        if (!id.isEmpty() && hit.type != 3 && hit.type != 4) newWindow(id);
        return;
    }
    screen.pressed = true;
    screen.pressX = x;
    screen.pressY = y;
    screen.pressUid = hit.type == 1 ? hit.uid : QString();
    screen.pressApp = hit.appId;
    screen.pressAction = (hit.type == 3 || hit.type == 4) ? hit.action : QString();
    screen.pressMenu = hit.type == 3 || hit.type == 4;
    screen.pressCell = hit.type == 2 && !hit.appId.isEmpty();
    screen.pressCard = hit.type == 5 || hit.type == 6;
    screen.pressClose = hit.type == 6;
    screen.pressSearch = hit.type == 7;
    screen.pressAbout = hit.type == 8 || hit.type == 9;
    screen.pressKey = hit.key;
    screen.dragging = false;
    if (hit.type == 1 && itemIndex(hit.uid) >= 0) {
        const int index = itemIndex(hit.uid);
        const Item::Kind kind = items_[static_cast<size_t>(index)].kind;
        if (kind == Item::Kind::App || kind == Item::Kind::Stack) screen.dragUid = hit.uid;
        else screen.dragUid.clear();
    } else {
        screen.dragUid.clear();
    }
}

void Dock::releaseAt(Screen &screen, double x, double y) {
    const double distance = std::hypot(x - screen.pressX, y - screen.pressY);
    const bool dragged = screen.dragging && distance > 6;
    const Hit hit = hitTest(screen, x, y);
    if (screen.pressed && screen.pressCell && dragged && pulledToDock(screen, x, y)) {
        pullFromStack(screen, screen.pressApp);
    } else if (screen.dragging && screen.mergeReady && !screen.hoverUid.isEmpty()) {
        mergeItems(screen.dragUid, screen.hoverUid);
        screen.openStack.clear();
    } else if (screen.pressed && !dragged) {
        if (screen.pressMenu && (hit.type == 3 || hit.type == 4) && hit.action == screen.pressAction &&
            hit.action != QLatin1String("add-submenu")) {
            runAction(screen, hit.action, screen.menuApp.isEmpty() ? screen.pressApp : screen.menuApp);
        } else if (!screen.pressMenu && screen.menu && hit.type != 3 && hit.type != 4) {
            if (screen.rename) commitRename(screen, true);
            screen.menu = false;
        } else if (screen.pressSearch && hit.type == 7) {
            if (!hit.appId.isEmpty() && hit.appId == screen.pressApp) {
                launchId(hit.appId);
                screen.search = false;
            }
        } else if (screen.pressAbout && (hit.type == 8 || hit.type == 9)) {
            if (hit.type == 9) {
                openContribute();
                screen.about = false;
            }
        } else if (hit.type == 1 && hit.uid == screen.pressUid) {
            if (screen.menu && hit.uid != screen.menuUid) screen.menu = false;
            if (hit.uid != QLatin1String("search")) screen.search = false;
            if (hit.uid != QLatin1String("overflow")) screen.about = false;
            clickSlot(screen, hit.uid);
        } else if (hit.type == 6 && screen.pressClose && hit.key != 0 && hit.key == screen.pressKey) {
            if (onClose) onClose(hit.key);
        } else if (hit.type == 5 && screen.pressCard && !screen.pressClose && hit.key != 0 && hit.key == screen.pressKey) {
            const Live *live = nullptr;
            for (const Live &candidate : live_) {
                if (candidate.key == hit.key) live = &candidate;
            }
            if (live) {
                if (live->minimized) {
                    if (onUnminimize) onUnminimize(live->key);
                } else if (live->activated) {
                    if (onMinimize) onMinimize(live->key);
                } else if (onActivate) {
                    onActivate(live->key);
                }
            }
            screen.openPreviews.clear();
        } else if (hit.type == 2 && screen.pressCell && hit.appId == screen.pressApp) {
            if (keysFor(hit.appId).size() > 1) {
                screen.openPreviews = hit.appId;
                screen.shownPreviews = hit.appId;
                screen.openStack.clear();
                screen.menu = false;
            } else {
                activateApp(hit.appId);
                screen.openStack.clear();
            }
        } else if (hit.type == 0) {
            if (screen.rename) commitRename(screen, true);
            screen.menu = false;
            screen.search = false;
            screen.about = false;
            screen.openStack.clear();
            screen.openPreviews.clear();
        }
    }
    screen.pressed = false;
    screen.dragging = false;
    screen.dragUid.clear();
    screen.mergeReady = false;
    screen.pressCell = false;
    screen.pressMenu = false;
    screen.pressCard = false;
    screen.pressClose = false;
    screen.pressSearch = false;
    screen.pressAbout = false;
    screen.pressKey = 0;
}

void Dock::pointer(const PointerEv &event) {
    now_ = nowMs();
    int output = event.outputId;
    if (output < 0) output = pointerOutput_;
    if (output < 0 && !screens_.isEmpty()) output = screens_.begin().key();
    if (output < 0) return;
    Screen &current = screen(output);
    if (event.type == PointerEv::Type::Enter) {
        pointerOutput_ = output;
        current.pointer = true;
        current.x = event.x;
        current.y = event.y;
        return;
    }
    if (event.type == PointerEv::Type::Leave) {
        current.pointer = false;
        current.pressed = false;
        current.dragging = false;
        current.x = -1000;
        return;
    }
    if (event.type == PointerEv::Type::Motion) {
        pointerOutput_ = output;
        current.pointer = true;
        current.x = event.x;
        current.y = event.y;
        if (current.pressed && std::hypot(event.x - current.pressX, event.y - current.pressY) > 6 && !current.dragUid.isEmpty())
            current.dragging = true;
        if (current.dragging) dragMotion(current, event.x, event.y);
        const Hit hit = hitTest(current, event.x, event.y);
        if (hit.type == 1) {
            if (current.tipUid != hit.uid) {
                current.tipUid = hit.uid;
                current.tipSince = now_;
            }
        } else {
            current.tipUid.clear();
        }
        if (current.menu) {
            if (hit.type == 4) current.subRow = hit.index;
            else if (hit.type == 3) {
                current.menuRow = hit.index;
                current.subRow = -1;
            }
        }
        if (current.search && hit.type == 7 && !hit.appId.isEmpty()) current.searchRow = hit.index;
        current.hoverCard = (hit.type == 5 || hit.type == 6) ? hit.index : -1;
        return;
    }
    if (event.type == PointerEv::Type::Button) {
        current.x = current.pointer ? current.x : event.x;
        current.y = current.pointer ? current.y : event.y;
        if (event.button == BTN_RIGHT && event.pressed) pressAt(current, current.x, current.y, true, false);
        else if (event.button == BTN_MIDDLE && event.pressed) pressAt(current, current.x, current.y, false, true);
        else if (event.button == BTN_LEFT && event.pressed) pressAt(current, current.x, current.y, false, false);
        else if (event.button == BTN_LEFT && !event.pressed) releaseAt(current, current.x, current.y);
    }
}

void Dock::key(const KeyEv &event) {
    now_ = nowMs();
    if (screens_.isEmpty()) return;
    Screen &current = screen(pointerOutput_ >= 0 ? pointerOutput_ : screens_.begin().key());
    if (event.keycode == KEY_LEFTSHIFT || event.keycode == KEY_RIGHTSHIFT) {
        current.heldShift = event.pressed;
        return;
    }
    if (!event.pressed) {
        if (current.heldKey == static_cast<int>(event.keycode)) current.heldKey = -1;
        return;
    }
    applyKey(current, static_cast<int>(event.keycode), current.heldShift);
    current.heldKey = static_cast<int>(event.keycode);
    current.repeatAt = now_ + 380;
}

bool Dock::wantsKeyboard() const {
    for (auto it = screens_.cbegin(); it != screens_.cend(); ++it) {
        if (it.value().rename || it.value().search || it.value().about) return true;
    }
    return false;
}

bool Dock::tick() {
    now_ = nowMs();
    pollSettings();
    bool busy = false;
    for (auto it = opening_.begin(); it != opening_.end();) {
        if (now_ - it.value() > 8000 || !keysFor(it.key()).isEmpty()) {
            it = opening_.erase(it);
        } else {
            busy = true;
            ++it;
        }
    }
    for (auto it = screens_.begin(); it != screens_.end(); ++it) {
        Screen &current = it.value();
        if (current.w < 16 || current.h < 16) continue;
        relayout(current);
        retarget(current);
        updateHidden(current);
        for (auto scaleIt = current.target.cbegin(); scaleIt != current.target.cend(); ++scaleIt) {
            double value = current.scale.value(scaleIt.key(), 1.0);
            const double target = scaleIt.value();
            value += (target - value) * 0.36;
            if (std::abs(value - target) < 0.004) value = target;
            else busy = true;
            current.scale.insert(scaleIt.key(), value);
        }
        current.hidden += (current.hiddenTarget - current.hidden) * 0.38;
        if (std::abs(current.hidden - current.hiddenTarget) < 0.4) current.hidden = current.hiddenTarget;
        else busy = true;
        const double popupTarget = current.openStack.isEmpty() ? 0.0 : 1.0;
        current.popup += (popupTarget - current.popup) * 0.34;
        if (std::abs(current.popup - popupTarget) < 0.02) current.popup = popupTarget;
        else busy = true;
        if (current.openStack.isEmpty() && current.popup < 0.02) current.shownStack.clear();
        if (!current.openStack.isEmpty()) current.shownStack = current.openStack;
        const double previewTarget = current.openPreviews.isEmpty() ? 0.0 : 1.0;
        current.previews += (previewTarget - current.previews) * 0.34;
        if (std::abs(current.previews - previewTarget) < 0.02) current.previews = previewTarget;
        else busy = true;
        if (current.openPreviews.isEmpty() && current.previews < 0.02) current.shownPreviews.clear();
        if (!current.openPreviews.isEmpty()) current.shownPreviews = current.openPreviews;
        if ((current.rename || current.search) && current.heldKey >= 0 && now_ >= current.repeatAt) {
            applyKey(current, current.heldKey, current.heldShift);
            current.repeatAt = now_ + 45;
            busy = true;
        }
        if (current.rename || current.search) busy = true;
        if (!current.tipUid.isEmpty() && now_ - current.tipSince < 900) busy = true;
    }
    return busy;
}

void Dock::syncRects(int outputId, const Screen &screen) {
    for (const Live &live : live_) {
        QRect rect;
        bool found = false;
        for (const Slot &slot : screen.slots) {
            if (slot.kind == Item::Kind::App && isApp(live, slot.appId)) {
                rect = slot.rect.toAlignedRect();
                found = true;
            } else if (slot.kind == Item::Kind::Stack) {
                const int index = itemIndex(slot.uid);
                if (index >= 0 && items_[static_cast<size_t>(index)].apps.contains(live.appId)) {
                    rect = slot.rect.toAlignedRect();
                    found = true;
                }
            }
        }
        if (!found || rect.width() < 2) continue;
        const QString key = QString::number(live.key) + QLatin1Char(':') + QString::number(outputId);
        if (sentRects_.value(key) == rect) continue;
        sentRects_.insert(key, rect);
        if (onRect) onRect(live.key, outputId, rect);
    }
}

void Dock::paintScreen(QPainter &painter, Screen &screen) {
    if (previewBackdrop_) {
        QLinearGradient backdrop(0, 0, 0, screen.h);
        backdrop.setColorAt(0, QColor(QStringLiteral("#243044")));
        backdrop.setColorAt(1, QColor(QStringLiteral("#121820")));
        painter.fillRect(QRectF(0, 0, screen.w, screen.h), backdrop);
    }
    const int device = std::max(32, qRound(iconSize_ * std::max(1.0, screen.dpr) * 1.5));
    auto iconFor = [&](const QString &id, int px) {
        QPixmap pix = itemIcon(id, px);
        return pix;
    };

    auto glass = [&](const QRectF &rect, qreal radius, int topAlpha) {
        if (rect.isEmpty()) return;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 78));
        painter.drawRoundedRect(rect.translated(0, 5).adjusted(-1, 0, 1, 2), radius, radius);
        QLinearGradient fill(rect.topLeft(), rect.bottomLeft());
        fill.setColorAt(0, QColor(78, 80, 88, topAlpha));
        fill.setColorAt(0.42, QColor(34, 35, 40, topAlpha - 8));
        fill.setColorAt(1, QColor(16, 16, 18, std::min(230, topAlpha + 10)));
        painter.setBrush(fill);
        painter.setPen(QPen(QColor(255, 255, 255, 48), 1));
        painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 16));
        painter.drawRoundedRect(rect.adjusted(12, 1.5, -12, -rect.height() * 0.58), radius, radius);
    };

    if (screen.hidden < std::max(screen.w, screen.h) * 0.85 && !screen.pill.isEmpty()) {
        painter.save();
        painter.setOpacity(opacity_);
        const QRectF bar = screen.pill;
        const qreal radius = radius_ > 0 ? radius_ : (expand_ ? 0 : 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 70));
        painter.drawRoundedRect(bar.translated(0, 1), radius, radius);
        QLinearGradient fill(bar.topLeft(), bar.bottomLeft());
        fill.setColorAt(0, QColor(46, 46, 52, 236));
        fill.setColorAt(1, QColor(22, 22, 26, 242));
        painter.setBrush(fill);
        if (radius > 0) painter.drawRoundedRect(bar, radius, radius);
        else painter.drawRect(bar);
        painter.setPen(QPen(QColor(255, 255, 255, 38), 1));
        if (edge_ == ShellSpec::Edge::Top)
            painter.drawLine(bar.bottomLeft() + QPointF(0, -0.5), bar.bottomRight() + QPointF(0, -0.5));
        else if (edge_ == ShellSpec::Edge::Left)
            painter.drawLine(bar.topRight() + QPointF(-0.5, 0), bar.bottomRight() + QPointF(-0.5, 0));
        else if (edge_ == ShellSpec::Edge::Right)
            painter.drawLine(bar.topLeft() + QPointF(0.5, 0), bar.bottomLeft() + QPointF(0.5, 0));
        else
            painter.drawLine(bar.topLeft() + QPointF(0, 0.5), bar.topRight() + QPointF(0, 0.5));
        painter.restore();
    }

    auto drawRunning = [&](const QRectF &icon, int running, bool active) {
        if (running <= 0) return;
        const double width = running > 1 ? 13.0 : 4.5;
        QRectF dot;
        if (edge_ == ShellSpec::Edge::Top)
            dot = QRectF(icon.center().x() - width / 2.0, icon.top() - 8, width, 4.2);
        else if (edge_ == ShellSpec::Edge::Left)
            dot = QRectF(icon.left() - 8, icon.center().y() - width / 2.0, 4.2, width);
        else if (edge_ == ShellSpec::Edge::Right)
            dot = QRectF(icon.right() + 4, icon.center().y() - width / 2.0, 4.2, width);
        else
            dot = QRectF(icon.center().x() - width / 2.0, icon.bottom() + 4, width, 4.2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(active ? QColor(255, 255, 255, 235) : QColor(255, 255, 255, 145));
        painter.drawRoundedRect(dot, 2.1, 2.1);
    };

    for (int i = 0; i < static_cast<int>(screen.slots.size()); ++i) {
        const Slot &slot = screen.slots[static_cast<size_t>(i)];
        const bool hovered = screen.pointer && slot.hit.contains(screen.x, screen.y) && !screen.dragging &&
                             screen.openStack.isEmpty() && screen.openPreviews.isEmpty() && !screen.menu && !screen.search &&
                             !screen.about;
        if (hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 24));
            painter.drawRoundedRect(slot.rect.adjusted(-4, -3, 4, 3), 10, 10);
        }
        const bool merging = screen.mergeReady && slot.uid == screen.hoverUid;
        if (merging) {
            painter.setPen(QPen(QColor(120, 186, 255, 220), 2));
            painter.setBrush(QColor(120, 186, 255, 36));
            painter.drawRoundedRect(slot.rect.adjusted(-4, -4, 4, 4), 16, 16);
        }
        const QPointF nudge = openingNudge(slot.appId);
        painter.save();
        painter.translate(nudge);
        if (slot.kind == Item::Kind::Search) {
            drawSearchGlyph(painter, slot.rect);
        } else if (slot.kind == Item::Kind::Overflow) {
            drawMoreGlyph(painter, slot.rect);
        } else if (slot.kind == Item::Kind::Launcher || slot.kind == Item::Kind::Workspaces ||
            slot.kind == Item::Kind::AppLibrary || slot.kind == Item::Kind::TaskManager) {
            QString iconName = QStringLiteral("com.system76.CosmicPanelLauncherButton");
            if (slot.kind == Item::Kind::Workspaces)
                iconName = QStringLiteral("com.system76.CosmicPanelWorkspacesButton");
            else if (slot.kind == Item::Kind::AppLibrary)
                iconName = QStringLiteral("com.system76.CosmicPanelAppButton");
            else if (slot.kind == Item::Kind::TaskManager)
                iconName = QStringLiteral("com.system76.CosmicMonitor");
            QPixmap pix = apps_->iconNamed(iconName, device);
            if (pix.isNull() && slot.kind == Item::Kind::Launcher)
                pix = apps_->iconNamed(QStringLiteral("com.system76.CosmicLauncher"), device);
            if (pix.isNull() && slot.kind == Item::Kind::TaskManager)
                pix = apps_->iconNamed(QStringLiteral("utilities-system-monitor"), device);
            if (!pix.isNull()) drawPixmapFit(painter, pix, slot.rect);
            else {
                painter.setPen(Qt::NoPen);
                painter.setBrush(slot.kind == Item::Kind::TaskManager ? QColor(90, 140, 220) : QColor(34, 159, 173));
                painter.drawRoundedRect(slot.rect, 8, 8);
            }
            if (slot.kind == Item::Kind::TaskManager) drawRunning(slot.rect, slot.running, slot.active);
        } else if (slot.kind == Item::Kind::Minimized) {
            const QRectF inner = slot.rect.adjusted(3, 3, -3, -3);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 22));
            painter.drawRoundedRect(slot.rect, 8, 8);
            drawPixmapFit(painter, iconFor(slot.appId, device), inner);
        } else if (slot.kind == Item::Kind::Stack) {
            const int index = itemIndex(slot.uid);
            QStringList apps;
            if (index >= 0) apps = items_[static_cast<size_t>(index)].apps;
            const QRectF front = slot.rect.adjusted(0, 0, -3, -3);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 32));
            painter.drawRoundedRect(slot.rect.adjusted(4, 4, 0, 0), front.width() * 0.24, front.width() * 0.24);
            painter.setBrush(QColor(255, 255, 255, 16));
            painter.setPen(QPen(QColor(255, 255, 255, 50), 1));
            painter.drawRoundedRect(front, front.width() * 0.24, front.width() * 0.24);
            const int shown = std::min(4, static_cast<int>(apps.size()));
            if (shown <= 1) {
                const QString id = apps.isEmpty() ? QString() : apps.first();
                drawPixmapFit(painter, iconFor(id, device / 2), front.adjusted(6, 6, -6, -6));
            } else {
                const QRectF inner = front.adjusted(5, 5, -5, -5);
                const double gap = 3;
                const int cols = 2;
                const int rows = shown > 2 ? 2 : 1;
                const double cw = (inner.width() - gap) / cols;
                const double ch = rows == 1 ? inner.height() * 0.72 : (inner.height() - gap) / rows;
                const double y0 = rows == 1 ? inner.center().y() - ch / 2 : inner.top();
                for (int n = 0; n < shown; ++n) {
                    const int col = n % cols;
                    const int row = n / cols;
                    const QRectF cell(inner.left() + col * (cw + gap), y0 + row * (ch + gap), cw, ch);
                    drawPixmapFit(painter, iconFor(apps.at(n), std::max(16, device / 3)), cell);
                }
            }
            drawRunning(slot.rect, slot.running, slot.active);
        } else {
            drawPixmapFit(painter, iconFor(slot.appId, device), slot.rect);
            drawRunning(slot.rect, slot.running, slot.active);
        }
        painter.restore();
    }

    if (screen.popup > 0.03 && !screen.popupRect.isEmpty()) {
        painter.save();
        painter.setOpacity(screen.popup);
        const QRectF pop = screen.popupRect.translated(0, (1.0 - screen.popup) * 10);
        glass(pop, 18, 214);
        QString title = QStringLiteral("Stack");
        for (const Item &item : items_) {
            if (item.kind == Item::Kind::Stack && item.id == screen.shownStack) title = item.name;
        }
        QFont font(QStringLiteral("Fira Sans"));
        font.setPixelSize(13);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(QColor(255, 255, 255, 210));
        painter.drawText(QRectF(pop.left() + 16, pop.top() + 6, pop.width() - 32, 22), Qt::AlignLeft | Qt::AlignVCenter, title);
        QFont labelFont(QStringLiteral("Fira Sans"));
        labelFont.setPixelSize(11);
        painter.setFont(labelFont);
        const QFontMetrics metrics(labelFont);
        for (const Screen::Cell &cell : screen.cells) {
            int running = 0;
            bool active = false;
            for (const Live &live : live_) {
                if (!isApp(live, cell.appId)) continue;
                running++;
                if (live.activated && !live.minimized) active = true;
            }
            drawPixmapFit(painter, iconFor(cell.appId, qRound(48 * screen.dpr)), cell.icon.translated(0, (1.0 - screen.popup) * 10));
            drawRunning(cell.icon.translated(0, (1.0 - screen.popup) * 10), running, active);
            painter.setPen(QColor(255, 255, 255, 215));
            const QString label = metrics.elidedText(cell.label, Qt::ElideRight, static_cast<int>(cell.rect.width() - 8));
            painter.drawText(QRectF(cell.rect.left(), cell.icon.bottom() + 4, cell.rect.width(), 16), Qt::AlignHCenter | Qt::AlignTop,
                             label);
        }
        painter.restore();
    }

    if (screen.previews > 0.03 && !screen.previewsRect.isEmpty()) {
        painter.save();
        painter.setOpacity(screen.previews);
        const QRectF pop = screen.previewsRect.translated(0, (1.0 - screen.previews) * 10);
        const double dy = (1.0 - screen.previews) * 10;
        glass(pop, 16, 220);
        QFont titleFont(QStringLiteral("Fira Sans"));
        titleFont.setPixelSize(12);
        painter.setFont(titleFont);
        const QFontMetrics metrics(titleFont);
        for (int i = 0; i < static_cast<int>(screen.cards.size()); ++i) {
            const Screen::Card &card = screen.cards[static_cast<size_t>(i)];
            const QRectF rect = card.rect.translated(0, dy);
            const QRectF thumb = card.thumb.translated(0, dy);
            const bool hover = i == screen.hoverCard;
            painter.setPen(Qt::NoPen);
            painter.setBrush(hover ? QColor(255, 255, 255, 28) : QColor(255, 255, 255, 10));
            painter.drawRoundedRect(rect, 12, 12);
            painter.setBrush(QColor(12, 12, 14, 230));
            painter.setPen(QPen(card.active ? QColor(255, 255, 255, 210) : QColor(255, 255, 255, 40), card.active ? 2 : 1));
            painter.drawRoundedRect(thumb, 8, 8);
            const QImage image = thumbs_.value(card.key);
            painter.setClipRect(thumb.adjusted(2, 2, -2, -2));
            painter.setPen(Qt::NoPen);
            if (!image.isNull()) {
                const double scale = std::min(thumb.width() / image.width(), thumb.height() / image.height());
                const double w = image.width() * scale;
                const double h = image.height() * scale;
                const QRectF fitted(thumb.center().x() - w / 2.0, thumb.center().y() - h / 2.0, w, h);
                painter.drawImage(fitted, image);
            } else {
                const QPixmap icon = iconFor(card.appId, qRound(48 * screen.dpr));
                const double side = std::min(thumb.width(), thumb.height()) * 0.46;
                const QRectF iconRect(thumb.center().x() - side / 2.0, thumb.center().y() - side / 2.0, side, side);
                drawPixmapFit(painter, icon, iconRect);
            }
            painter.setClipping(false);
            if (card.minimized) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 90));
                painter.drawRoundedRect(thumb, 8, 8);
            }
            if (hover) {
                const QRectF close = card.closeRect.translated(0, dy);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(190, 48, 48, 235));
                painter.drawEllipse(close);
                painter.setPen(QPen(Qt::white, 1.6));
                painter.drawLine(close.center() + QPointF(-4, -4), close.center() + QPointF(4, 4));
                painter.drawLine(close.center() + QPointF(4, -4), close.center() + QPointF(-4, 4));
            }
            painter.setPen(QColor(255, 255, 255, 220));
            painter.setFont(titleFont);
            const QString label = metrics.elidedText(card.title, Qt::ElideRight, static_cast<int>(rect.width() - 12));
            painter.drawText(QRectF(rect.left() + 4, thumb.bottom() + 2, rect.width() - 8, 18), Qt::AlignHCenter | Qt::AlignVCenter,
                             label);
        }
        painter.restore();
    }

    if (screen.menu && !screen.menuRect.isEmpty()) {
        auto panel = [&](const QRectF &rect) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 90));
            painter.drawRoundedRect(rect.translated(0, 3), 12, 12);
            painter.setBrush(QColor(28, 29, 34, 236));
            painter.setPen(QPen(QColor(255, 255, 255, 36), 1));
            painter.drawRoundedRect(rect, 12, 12);
        };
        panel(screen.menuRect);
        QFont font(QStringLiteral("Fira Sans"));
        font.setPixelSize(13);
        painter.setFont(font);
        for (int i = 0; i < static_cast<int>(screen.rows.size()) && i < static_cast<int>(screen.menuRows.size()); ++i) {
            const MenuRow &row = screen.rows[static_cast<size_t>(i)];
            const QRectF rect = screen.menuRows[static_cast<size_t>(i)];
            if (row.action == QLatin1String("-")) {
                painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
                painter.drawLine(QPointF(rect.left() + 8, rect.center().y()), QPointF(rect.right() - 8, rect.center().y()));
                continue;
            }
            const bool hover = i == screen.menuRow && row.enabled;
            if (hover) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 28));
                painter.drawRoundedRect(rect.adjusted(2, 1, -2, -1), 8, 8);
            }
            if (screen.rename && row.header) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 18));
                painter.drawRoundedRect(rect.adjusted(6, 4, -6, -4), 6, 6);
                QString shown = screen.renameText;
                shown.insert(screen.caret, (now_ / 500) % 2 ? QStringLiteral("|") : QStringLiteral(" "));
                painter.setPen(Qt::white);
                painter.drawText(rect.adjusted(12, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, shown);
                continue;
            }
            painter.setPen(row.enabled ? QColor(255, 255, 255, row.header ? 150 : 230) : QColor(255, 255, 255, 150));
            QFont use = font;
            use.setWeight(row.header ? QFont::DemiBold : QFont::Normal);
            painter.setFont(use);
            painter.drawText(rect.adjusted(12, 0, -22, 0), Qt::AlignVCenter | Qt::AlignLeft, row.label);
            if (!row.sub.empty()) {
                painter.drawText(rect.adjusted(0, 0, -8, 0), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("›"));
            }
        }
        if (!screen.subRect.isEmpty() && screen.menuRow >= 0 && screen.menuRow < static_cast<int>(screen.rows.size())) {
            panel(screen.subRect);
            const MenuRow &parent = screen.rows[static_cast<size_t>(screen.menuRow)];
            painter.setFont(font);
            for (int i = 0; i < static_cast<int>(parent.sub.size()) && i < static_cast<int>(screen.subRows.size()); ++i) {
                const QRectF rect = screen.subRows[static_cast<size_t>(i)];
                if (i == screen.subRow) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(255, 255, 255, 28));
                    painter.drawRoundedRect(rect.adjusted(2, 1, -2, -1), 8, 8);
                }
                painter.setPen(QColor(255, 255, 255, 230));
                painter.drawText(rect.adjusted(12, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, parent.sub[static_cast<size_t>(i)].label);
            }
        }
    }

    if (screen.search && !screen.searchRect.isEmpty()) {
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 90));
        painter.drawRoundedRect(screen.searchRect.translated(0, 3), 12, 12);
        painter.setBrush(QColor(28, 29, 34, 242));
        painter.setPen(QPen(QColor(255, 255, 255, 36), 1));
        painter.drawRoundedRect(screen.searchRect, 12, 12);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 16));
        painter.drawRoundedRect(screen.searchField, 8, 8);
        QFont font(QStringLiteral("Fira Sans"));
        font.setPixelSize(13);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        const QString shown = screen.searchText.isEmpty() ? QStringLiteral("Search apps") : screen.searchText;
        painter.setPen(screen.searchText.isEmpty() ? QColor(255, 255, 255, 110) : QColor(255, 255, 255, 235));
        painter.drawText(screen.searchField.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, shown);
        if ((now_ / 500) % 2 == 0) {
            const int caret = std::clamp(screen.searchCaret, 0, static_cast<int>(screen.searchText.size()));
            const double advance = metrics.horizontalAdvance(screen.searchText.left(caret));
            const double cx = screen.searchField.left() + 10 + advance;
            painter.setPen(QPen(Qt::white, 1));
            painter.drawLine(QPointF(cx, screen.searchField.top() + 8), QPointF(cx, screen.searchField.bottom() - 8));
        }
        if (screen.searchHits.empty()) {
            painter.setPen(QColor(255, 255, 255, 140));
            const QString hint = screen.searchText.isEmpty() ? QStringLiteral("Type to search") : QStringLiteral("No matching apps");
            painter.drawText(QRectF(screen.searchField.left(), screen.searchField.bottom() + 8, screen.searchField.width(), 36),
                             Qt::AlignVCenter | Qt::AlignLeft, hint);
        }
        for (int i = 0; i < static_cast<int>(screen.searchHits.size()); ++i) {
            const Screen::SearchHit &row = screen.searchHits[static_cast<size_t>(i)];
            if (i == screen.searchRow) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 28));
                painter.drawRoundedRect(row.rect.adjusted(2, 2, -2, -2), 8, 8);
            }
            drawPixmapFit(painter, iconFor(row.appId, qRound(22 * screen.dpr)), row.icon);
            painter.setPen(QColor(255, 255, 255, 230));
            painter.setFont(font);
            const QString label = metrics.elidedText(row.label, Qt::ElideRight, static_cast<int>(row.rect.width() - 44));
            painter.drawText(QRectF(row.icon.right() + 8, row.rect.top(), row.rect.width() - 44, row.rect.height()),
                             Qt::AlignVCenter | Qt::AlignLeft, label);
        }
        painter.restore();
    }

    if (screen.about && !screen.aboutRect.isEmpty()) {
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 90));
        painter.drawRoundedRect(screen.aboutRect.translated(0, 3), 12, 12);
        painter.setBrush(QColor(28, 29, 34, 242));
        painter.setPen(QPen(QColor(255, 255, 255, 36), 1));
        painter.drawRoundedRect(screen.aboutRect, 12, 12);
        QFont title(QStringLiteral("Fira Sans"));
        title.setPixelSize(16);
        title.setWeight(QFont::DemiBold);
        painter.setFont(title);
        painter.setPen(QColor(255, 255, 255, 235));
        painter.drawText(QRectF(screen.aboutRect.left() + 16, screen.aboutRect.top() + 14, screen.aboutRect.width() - 32, 24),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("MAGIC Dock"));
        QFont body(QStringLiteral("Fira Sans"));
        body.setPixelSize(13);
        painter.setFont(body);
        painter.setPen(QColor(255, 255, 255, 190));
        painter.drawText(QRectF(screen.aboutRect.left() + 16, screen.aboutRect.top() + 40, screen.aboutRect.width() - 32, 20),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("A dock for the COSMIC desktop."));
        painter.drawText(QRectF(screen.aboutRect.left() + 16, screen.aboutRect.top() + 62, screen.aboutRect.width() - 32, 20),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Mira, The White Witch"));
        const bool coffeeHover = screen.aboutCoffee.contains(screen.x, screen.y);
        painter.setPen(Qt::NoPen);
        painter.setBrush(coffeeHover ? QColor(255, 255, 255, 28) : QColor(255, 255, 255, 12));
        painter.drawRoundedRect(screen.aboutCoffee, 8, 8);
        painter.setPen(QColor(255, 255, 255, 230));
        painter.setFont(body);
        painter.drawText(screen.aboutCoffee, Qt::AlignCenter, QStringLiteral("Buy me a coffee"));
        painter.restore();
    }

    if (!screen.menu && !screen.search && !screen.about && screen.openStack.isEmpty() && screen.openPreviews.isEmpty() &&
        !screen.tipUid.isEmpty() && now_ - screen.tipSince > 280) {
        const Slot *slot = nullptr;
        for (const Slot &candidate : screen.slots)
            if (candidate.uid == screen.tipUid) slot = &candidate;
        if (slot) {
            QString tip = slot->label;
            if (slot->running > 0) tip += slot->running == 1 ? QStringLiteral(" — open") : QStringLiteral(" — %1 windows").arg(slot->running);
            QFont font(QStringLiteral("Fira Sans"));
            font.setPixelSize(12);
            font.setWeight(QFont::Medium);
            painter.setFont(font);
            const QFontMetrics metrics(font);
            const double width = metrics.horizontalAdvance(tip) + 18;
            const double height = 24;
            QRectF tipRect;
            if (horizontal()) {
                double x = std::clamp(slot->rect.center().x() - width / 2, 6.0, std::max(6.0, screen.w - width - 6));
                const double y = edge_ == ShellSpec::Edge::Top ? slot->rect.bottom() + 8 : slot->rect.top() - 8 - height;
                tipRect = QRectF(x, y, width, height);
            } else {
                double y = std::clamp(slot->rect.center().y() - height / 2, 6.0, std::max(6.0, screen.h - height - 6));
                const double x = edge_ == ShellSpec::Edge::Left ? slot->rect.right() + 8 : slot->rect.left() - 8 - width;
                tipRect = QRectF(x, y, width, height);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(20, 20, 24, 225));
            painter.drawRoundedRect(tipRect, 8, 8);
            painter.setPen(Qt::white);
            painter.drawText(tipRect, Qt::AlignCenter, tip);
        }
    }
}

QRegion Dock::paint(int outputId, QPainter &painter, const OutputGeom &geom) {
    now_ = nowMs();
    Screen &current = screen(outputId);
    current.w = geom.logicalW;
    current.h = geom.logicalH;
    current.dpr = geom.scale > 0.1 ? geom.scale : 1;
    if (current.born == 0) current.born = now_;
    if (!outputWanted(outputId, geom.name)) {
        current.slots.clear();
        current.pill = QRectF();
        return {};
    }
    relayout(current);
    paintScreen(painter, current);
    syncRects(outputId, current);
    return inputRegion(current);
}

void Dock::setWindows(const std::vector<WindowState> &windows) {
    QHash<quint64, Live> mapped;
    for (const WindowState &state : windows) {
        Live &live = mapped[state.key];
        live.key = state.key;
        live.rawId = state.appId;
        apps_->adoptProject(state.appId);
        const QString resolved = apps_->resolve(state.appId);
        live.appId = resolved.isEmpty() ? state.appId : resolved;
        live.title = state.title;
        live.minimized = state.minimized;
        live.activated = state.activated;
        live.fullscreen = state.fullscreen;
        live.maximized = state.maximized;
        if (state.onOutput) {
            if (!live.outputs.contains(state.outputId)) live.outputs.append(state.outputId);
            if (state.hasGeom) live.geom.insert(state.outputId, QRect(state.x, state.y, state.w, state.h));
        }
    }
    live_.clear();
    live_.reserve(static_cast<size_t>(mapped.size()));
    QSet<quint64> alive;
    QSet<QString> seenApps;
    for (auto it = mapped.cbegin(); it != mapped.cend(); ++it) {
        live_.push_back(it.value());
        alive.insert(it.key());
        const QString id = canonical(it->appId);
        if (!id.isEmpty() && !seenApps.contains(id)) {
            seenApps.insert(id);
            opening_.remove(id);
            captureExec(id);
        }
    }
    for (auto it = thumbs_.begin(); it != thumbs_.end();) {
        if (alive.contains(it.key())) {
            ++it;
        } else {
            thumbSerial_.remove(it.key());
            it = thumbs_.erase(it);
        }
    }
    for (auto screenIt = screens_.begin(); screenIt != screens_.end(); ++screenIt) {
        Screen &current = screenIt.value();
        if (!current.openPreviews.isEmpty() && keysFor(current.openPreviews).isEmpty()) current.openPreviews.clear();
    }
}

std::vector<quint64> Dock::previewKeys() const {
    std::vector<quint64> keys;
    QSet<QString> seen;
    for (auto it = screens_.cbegin(); it != screens_.cend(); ++it) {
        const Screen &current = it.value();
        const QString appId = !current.openPreviews.isEmpty() ? current.openPreviews : current.shownPreviews;
        if (appId.isEmpty() || (current.openPreviews.isEmpty() && current.previews < 0.05)) continue;
        if (seen.contains(appId)) continue;
        seen.insert(appId);
        for (quint64 key : keysFor(appId)) keys.push_back(key);
    }
    return keys;
}

bool Dock::noteThumbnail(quint64 key, const QImage &image, quint64 serial) {
    if (image.isNull() || serial == 0) return false;
    if (thumbSerial_.value(key) == serial) return false;
    thumbSerial_.insert(key, serial);
    thumbs_.insert(key, image);
    return true;
}

int Dock::preview(const QString &directory) {
    QDir().mkpath(directory);
    load();
    edge_ = ShellSpec::Edge::Bottom;
    expand_ = true;
    margin_ = 0;
    hideMode_ = Hide::Never;
    bool social = false;
    QStringList socialApps;
    for (const Item &item : items_) {
        if (item.kind == Item::Kind::Stack && item.id == QLatin1String("social")) {
            social = true;
            socialApps = item.apps;
        }
    }
    if (!social || socialApps.size() < 2) {
        std::fprintf(stderr, "magic-dock: preview expected a Social stack\n");
        return 1;
    }
    Live live;
    live.key = 7;
    live.appId = socialApps.first();
    live.rawId = live.appId;
    live.title = itemName(live.appId);
    live.activated = true;
    live.outputs = {1};
    live.geom.insert(1, QRect(80, 30, 760, 420));
    live_.push_back(live);
    Live other;
    other.key = 9;
    other.appId = live.appId;
    other.rawId = live.appId;
    other.title = QStringLiteral("Second window");
    other.outputs = {1};
    other.geom.insert(1, QRect(120, 80, 640, 400));
    live_.push_back(other);
    QImage thumbA(320, 200, QImage::Format_ARGB32_Premultiplied);
    thumbA.fill(QColor(QStringLiteral("#2f5f8f")));
    {
        QPainter painter(&thumbA);
        painter.setPen(Qt::white);
        painter.drawText(thumbA.rect(), Qt::AlignCenter, QStringLiteral("Window A"));
    }
    QImage thumbB(320, 200, QImage::Format_ARGB32_Premultiplied);
    thumbB.fill(QColor(QStringLiteral("#8f4a2f")));
    {
        QPainter painter(&thumbB);
        painter.setPen(Qt::white);
        painter.drawText(thumbB.rect(), Qt::AlignCenter, QStringLiteral("Window B"));
    }
    thumbs_.insert(live.key, thumbA);
    thumbs_.insert(other.key, thumbB);
    thumbSerial_.insert(live.key, 1);
    thumbSerial_.insert(other.key, 1);
    Live minimized;
    minimized.key = 8;
    minimized.appId = QStringLiteral("com.system76.CosmicFiles");
    minimized.rawId = minimized.appId;
    minimized.title = QStringLiteral("Home");
    minimized.minimized = true;
    minimized.outputs = {1};
    live_.push_back(minimized);

    previewBackdrop_ = true;
    auto render = [&](const QString &name) {
        Screen &current = screen(1);
        current.w = 960;
        current.h = 540;
        current.dpr = 2;
        current.born = now_ - 5000;
        relayout(current);
        retarget(current);
        for (auto it = current.target.cbegin(); it != current.target.cend(); ++it) current.scale.insert(it.key(), it.value());
        relayout(current);
        QImage image(1920, 1080, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(2);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 960;
        geom.logicalH = 540;
        geom.outputW = 960;
        geom.outputH = 540;
        geom.scale = 2;
        paint(1, painter, geom);
        painter.end();
        image.save(directory + QLatin1Char('/') + name);
        return image;
    };

    Screen &current = screen(1);
    current.pointer = false;
    current.hidden = 0;
    QImage rest = render(QStringLiteral("rest.png"));
    const Slot *stackSlot = nullptr;
    for (const Slot &slot : screen(1).slots) {
        if (slot.kind == Item::Kind::Stack) stackSlot = &slot;
    }
    if (!stackSlot) return 1;
    current.pointer = true;
    current.x = stackSlot->rect.center().x();
    current.y = stackSlot->rect.center().y();
    render(QStringLiteral("hover.png"));
    current.openStack = QStringLiteral("social");
    current.shownStack = QStringLiteral("social");
    current.popup = 1;
    render(QStringLiteral("stack.png"));
    current.openStack.clear();
    current.popup = 0;
    current.openPreviews = live.appId;
    current.shownPreviews = live.appId;
    current.previews = 1;
    current.hoverCard = 1;
    QImage windows = render(QStringLiteral("windows.png"));
    if (screen(1).cards.size() < 2) {
        std::fprintf(stderr, "magic-dock: preview expected two window cards, got %zu\n", screen(1).cards.size());
        return 3;
    }
    int windowInk = 0;
    const QRectF cards = screen(1).previewsRect;
    for (int y = static_cast<int>(cards.top() * 2); y < static_cast<int>(cards.bottom() * 2); y += 3) {
        for (int x = static_cast<int>(cards.left() * 2); x < static_cast<int>(cards.right() * 2); x += 3) {
            if (x < 0 || y < 0 || x >= windows.width() || y >= windows.height()) continue;
            if (qAlpha(windows.pixel(x, y)) > 20) windowInk++;
        }
    }
    if (windowInk < 40) {
        std::fprintf(stderr, "magic-dock: window preview strip did not draw (%d)\n", windowInk);
        return 3;
    }
    current.openPreviews.clear();
    current.previews = 0;
    current.hoverCard = -1;
    openMenu(current, QStringLiteral("stack:social"), QStringLiteral("social"));
    current.menuRow = 1;
    render(QStringLiteral("menu.png"));
    previewBackdrop_ = false;

    auto countInk = [&](int x0, int x1, int y0, int y1) {
        int ink = 0;
        for (int y = y0; y < y1; y += 4) {
            for (int x = x0; x < x1; x += 4) {
                if (qAlpha(rest.pixel(x, y)) > 20) ink++;
            }
        }
        return ink;
    };
    const int leftInk = countInk(0, 220, 940, 1080);
    const int centerInk = countInk(700, 1220, 940, 1080);
    const int rightInk = countInk(1680, 1920, 940, 1080);
    if (leftInk < 20 || centerInk < 20 || rightInk < 20) {
        std::fprintf(stderr, "magic-dock: preview dock missing a wing (left=%d center=%d right=%d)\n", leftInk,
                     centerInk, rightInk);
        return 2;
    }
    std::fprintf(stderr, "magic-dock: preview ok, social=%s left=%d center=%d right=%d\n",
                 qPrintable(socialApps.join(", ")), leftInk, centerInk, rightInk);
    return 0;
}

int Dock::selfTest() {
    // Do not call load(). That rewrites the user's dock.json.
    items_.clear();
    live_.clear();
    screens_.clear();
    opening_.clear();
    Item app;
    app.kind = Item::Kind::App;
    app.id = QStringLiteral("demo-app");
    items_.push_back(app);
    iconSize_ = 40;
    pad_ = 4;
    margin_ = 0;
    radius_ = 0;
    expand_ = true;
    exclusive_ = false;
    hideMode_ = Hide::Never;
    outputMode_ = QStringLiteral("All");
    opacity_ = 1;
    now_ = nowMs();

    auto ink = [](const QImage &image, int x0, int x1, int y0, int y1) {
        int count = 0;
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(image.width(), x1);
        y1 = std::min(image.height(), y1);
        for (int y = y0; y < y1; y += 3) {
            for (int x = x0; x < x1; x += 3) {
                if (qAlpha(image.pixel(x, y)) > 20) count++;
            }
        }
        return count;
    };
    auto render = [&](ShellSpec::Edge edge, const QString &outputName) {
        edge_ = edge;
        screens_.clear();
        QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 400;
        geom.logicalH = 300;
        geom.scale = 1;
        geom.name = outputName;
        paint(1, painter, geom);
        painter.end();
        return image;
    };

    struct Expect {
        ShellSpec::Edge edge;
        const char *name;
        int x0, x1, y0, y1;
        int fx0, fx1, fy0, fy1;
        bool vertical;
    };
    const Expect edges[] = {
        {ShellSpec::Edge::Bottom, "bottom", 0, 400, 230, 300, 0, 400, 0, 40, false},
        {ShellSpec::Edge::Top, "top", 0, 400, 0, 70, 0, 400, 250, 300, false},
        {ShellSpec::Edge::Left, "left", 0, 80, 0, 300, 320, 400, 0, 300, true},
        {ShellSpec::Edge::Right, "right", 320, 400, 0, 300, 0, 80, 0, 300, true},
    };
    for (const Expect &expect : edges) {
        const QImage image = render(expect.edge, QStringLiteral("TEST"));
        const int near = ink(image, expect.x0, expect.x1, expect.y0, expect.y1);
        const int far = ink(image, expect.fx0, expect.fx1, expect.fy0, expect.fy1);
        const Slot *launcher = nullptr;
        const Slot *library = nullptr;
        const Slot *search = nullptr;
        const Slot *task = nullptr;
        const Slot *more = nullptr;
        for (const Slot &slot : screen(1).slots) {
            if (slot.uid == QLatin1String("launcher")) launcher = &slot;
            if (slot.uid == QLatin1String("applibrary")) library = &slot;
            if (slot.uid == QLatin1String("search")) search = &slot;
            if (slot.uid == QLatin1String("taskman")) task = &slot;
            if (slot.uid == QLatin1String("overflow")) more = &slot;
        }
        const auto after = [&](const Slot *a, const Slot *b) {
            if (!a || !b) return false;
            return expect.vertical ? a->rect.center().y() > b->rect.center().y() + 8
                                   : a->rect.center().x() > b->rect.center().x() + 8;
        };
        const bool order = launcher && task && (expect.vertical ? task->rect.center().y() > launcher->rect.center().y() + 40
                                                                : task->rect.center().x() > launcher->rect.center().x() + 40);
        const bool wings = after(search, library) && after(more, task);
        if (near < 30 || far > 6 || !order || !wings) {
            std::fprintf(stderr, "magic-dock: self-test %s failed (near=%d far=%d order=%d wings=%d)\n", expect.name, near,
                         far, order ? 1 : 0, wings ? 1 : 0);
            if (launcher && task) {
                std::fprintf(stderr, "magic-dock: launcher %.0f,%.0f task %.0f,%.0f\n", launcher->rect.center().x(),
                             launcher->rect.center().y(), task->rect.center().x(), task->rect.center().y());
            }
            return 1;
        }
    }

    edge_ = ShellSpec::Edge::Bottom;
    now_ = nowMs();
    opening_.insert(canonical(QStringLiteral("demo-app")), now_ - 200);
    const QPointF nudge = openingNudge(QStringLiteral("demo-app"));
    if (nudge.y() > -1.0) {
        std::fprintf(stderr, "magic-dock: opening nudge failed (%.2f, %.2f)\n", nudge.x(), nudge.y());
        return 1;
    }

    edge_ = ShellSpec::Edge::Right;
    const ShellSpec spec = shellSpec();
    const int reserve = static_cast<int>(std::lround(barThickness()));
    if (spec.edge != ShellSpec::Edge::Right || spec.exclusive || spec.reserve != reserve || reserve < 40) {
        std::fprintf(stderr, "magic-dock: shell spec failed (edge=%d reserve=%d)\n", static_cast<int>(spec.edge),
                     spec.reserve);
        return 1;
    }

    outputMode_ = QStringLiteral("DP-9");
    const QImage filtered = render(ShellSpec::Edge::Bottom, QStringLiteral("DP-1"));
    if (ink(filtered, 0, 400, 0, 300) > 0 || !screen(1).slots.empty()) {
        std::fprintf(stderr, "magic-dock: output filter still drew a dock\n");
        return 1;
    }

    CosmicDockSettings settings;
    settings.ok = true;
    settings.anchor = QStringLiteral("Top");
    settings.autohide = QStringLiteral("Never");
    settings.iconSize = 32;
    settings.padding = 4;
    settings.output = QStringLiteral("All");
    settings.expandToEdges = true;
    applyCosmicSettings(settings);
    if (edge_ != ShellSpec::Edge::Top || iconSize_ != 32 || hideMode_ != Hide::Never || !shellDirty_) {
        std::fprintf(stderr, "magic-dock: applyCosmicSettings did not take the anchor\n");
        return 1;
    }

    Screen &menuScreen = screen(9);
    menuScreen.w = 800;
    menuScreen.h = 600;
    openMenu(menuScreen, QStringLiteral("overflow"), {});
    bool sawSettings = false;
    bool sawAbout = false;
    bool sawContribute = false;
    for (const MenuRow &row : menuScreen.rows) {
        if (row.action == QLatin1String("settings")) sawSettings = true;
        if (row.action == QLatin1String("about")) sawAbout = true;
        if (row.action == QLatin1String("contribute")) sawContribute = true;
    }
    setSearch(menuScreen, true);
    applyKey(menuScreen, KEY_T, false);
    applyKey(menuScreen, KEY_E, false);
    if (!sawSettings || !sawAbout || !sawContribute || menuScreen.searchText != QLatin1String("te") || !wantsKeyboard()) {
        std::fprintf(stderr, "magic-dock: menu or search self-test failed\n");
        return 1;
    }
    applyKey(menuScreen, KEY_ESC, false);
    if (menuScreen.search) {
        std::fprintf(stderr, "magic-dock: search did not close\n");
        return 1;
    }
    std::fprintf(stderr, "magic-dock: dock self-test ok\n");
    return 0;
}

