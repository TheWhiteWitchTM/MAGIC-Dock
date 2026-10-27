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
#include <QPainterPath>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

QColor mixColor(const QColor &a, const QColor &b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor(qRound(a.red() + (b.red() - a.red()) * t), qRound(a.green() + (b.green() - a.green()) * t),
                  qRound(a.blue() + (b.blue() - a.blue()) * t));
}

// Idle is green, busy is orange, and full is red.
QColor usageColor(double fraction) {
    const double t = std::clamp(fraction, 0.0, 1.0);
    const QColor green(52, 199, 89);
    const QColor orange(255, 149, 0);
    const QColor red(255, 69, 58);
    if (t < 0.55) return mixColor(green, orange, t / 0.55);
    return mixColor(orange, red, (t - 0.55) / 0.45);
}

QString formatBytes(quint64 bytes) {
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gib >= 10.0) return QString::number(gib, 'f', 0) + QStringLiteral(" GB");
    if (gib >= 1.0) return QString::number(gib, 'f', 1) + QStringLiteral(" GB");
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QString::number(std::max(0.0, mib), 'f', 0) + QStringLiteral(" MB");
}

void drawUsageBars(QPainter &painter, const QRectF &rect, double cpu, double mem, bool cpuReady, bool memReady) {
    const double gap = std::max(3.0, rect.height() * 0.16);
    const double insetX = std::max(1.0, rect.width() * 0.04);
    const QRectF area = rect.adjusted(insetX, rect.height() * 0.1, -insetX, -rect.height() * 0.1);
    const double h = std::max(3.0, (area.height() - gap) / 2.0);
    const double radius = h / 2.0;
    auto bar = [&](double y, double fraction, bool ready) {
        const QRectF track(area.left(), y, area.width(), h);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 52));
        painter.drawRoundedRect(track, radius, radius);
        if (!ready) return;
        const double f = std::clamp(fraction, 0.0, 1.0);
        if (f < 0.008) return;
        const double w = std::min(track.width(), std::max(2.0, track.width() * f));
        painter.setBrush(usageColor(f));
        painter.drawRoundedRect(QRectF(track.left(), track.top(), w, track.height()), radius, radius);
    };
    bar(area.top(), cpu, cpuReady);
    bar(area.top() + h + gap, mem, memReady);
}

void drawTrashGlyph(QPainter &painter, const QRectF &rect, bool full) {
    const double s = std::min(rect.width(), rect.height());
    const QPointF c = rect.center();
    const QRectF body(c.x() - s * 0.26, c.y() - s * 0.08, s * 0.52, s * 0.42);
    painter.setPen(QPen(QColor(255, 255, 255, 230), std::max(1.4, s * 0.055), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(full ? QColor(255, 255, 255, 40) : Qt::NoBrush);
    painter.drawRoundedRect(body, s * 0.05, s * 0.05);
    const double lid = body.top() - s * 0.05;
    painter.drawLine(QPointF(body.left() - s * 0.05, lid), QPointF(body.right() + s * 0.05, lid));
    painter.drawRoundedRect(QRectF(c.x() - s * 0.09, lid - s * 0.09, s * 0.18, s * 0.09), s * 0.03, s * 0.03);
    const double inset = s * 0.1;
    painter.drawLine(QPointF(body.left() + inset, body.top() + s * 0.08), QPointF(body.left() + inset, body.bottom() - s * 0.07));
    painter.drawLine(QPointF(c.x(), body.top() + s * 0.08), QPointF(c.x(), body.bottom() - s * 0.07));
    painter.drawLine(QPointF(body.right() - inset, body.top() + s * 0.08), QPointF(body.right() - inset, body.bottom() - s * 0.07));
}

QRectF badgeBox(const QRectF &icon, double w, double h, ShellSpec::Edge edge) {
    double x = icon.right() - w + 3;
    double y = icon.top() - 2;
    if (edge == ShellSpec::Edge::Top) y = icon.bottom() - h + 2;
    if (edge == ShellSpec::Edge::Right) x = icon.left() - 3;
    return QRectF(x, y, w, h);
}

void drawCountBadge(QPainter &painter, const QRectF &icon, const QColor &color, int count, bool alwaysNumber,
                    ShellSpec::Edge edge) {
    if (count <= 0) return;
    const bool number = alwaysNumber || count > 1;
    const double h = std::clamp(icon.height() * (number ? 0.4 : 0.26), number ? 13.0 : 8.0, number ? 18.0 : 11.0);
    QString text;
    QFont font(QStringLiteral("Fira Sans"));
    font.setPixelSize(std::max(9, static_cast<int>(std::lround(h * 0.62))));
    font.setWeight(QFont::DemiBold);
    double w = h;
    if (number) {
        text = count > 99 ? QStringLiteral("99+") : QString::number(count);
        w = std::max(h, QFontMetrics(font).horizontalAdvance(text) + h * 0.5);
    }
    const QRectF box = badgeBox(icon, w, h, edge);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 100));
    painter.drawRoundedRect(box.translated(0, 1), h / 2.0, h / 2.0);
    painter.setPen(QPen(QColor(16, 16, 20, 235), 1.2));
    painter.setBrush(color);
    painter.drawRoundedRect(box, h / 2.0, h / 2.0);
    if (!number) return;
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, text);
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

// Popups stay this far from the screen edge, so a stroke and its shadow
// are never clipped by the output. The dock keeps a couple of pixels even
// when COSMIC's margin is zero, or the screen clips the bar's border.
constexpr double kScreenMargin = 14.0;
constexpr int kEdgeGuard = 2;
constexpr double kPopupGap = 10.0;
constexpr qreal kPopupRadius = 10.0;
constexpr double kCloseSize = 18.0;
constexpr double kCloseInset = 8.0;

qreal chromeWidth(int hintPx) {
    if (hintPx <= 0) return 1.5;
    return std::min(1.5, std::max(1.0, static_cast<double>(hintPx)));
}

// The pen is centered on the path. Inset past half the pen plus the
// antialiased fringe, or the screen edge cuts the border in half.
void strokeChrome(QPainter &painter, const QRectF &rect, qreal radius, const QColor &color, qreal penW) {
    if (rect.width() < 8 || rect.height() < 8) return;
    const qreal inset = penW * 0.5 + 1.5;
    const QRectF box = rect.adjusted(inset, inset, -inset, -inset);
    if (box.width() < 4 || box.height() < 4) return;
    const qreal rad = std::max(0.0, std::min(radius, std::min(box.width(), box.height()) * 0.5 - 0.5));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, penW, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.drawRoundedRect(box, rad, rad);
}

QRectF closeBox(const QRectF &popup) {
    return QRectF(popup.right() - kCloseInset - kCloseSize, popup.top() + kCloseInset, kCloseSize, kCloseSize);
}

void drawCloseCircle(QPainter &painter, const QRectF &circle, bool hot) {
    if (circle.isEmpty()) return;
    painter.setPen(Qt::NoPen);
    painter.setBrush(hot ? QColor(255, 69, 58, 235) : QColor(255, 255, 255, 36));
    painter.drawEllipse(circle);
    painter.setPen(QPen(QColor(255, 255, 255, hot ? 250 : 230), 1.5, Qt::SolidLine, Qt::RoundCap));
    const QPointF center = circle.center();
    const double arm = circle.width() * 0.22;
    painter.drawLine(center + QPointF(-arm, -arm), center + QPointF(arm, arm));
    painter.drawLine(center + QPointF(arm, -arm), center + QPointF(-arm, arm));
}

QColor creditColor(int percent) {
    if (percent > 70) return QColor(52, 199, 89);
    if (percent <= 10) return QColor(255, 69, 58);
    return QColor(255, 149, 0);
}

// The ring is how much of the Grok allowance is still left. The number sits inside it.

// Older credit lines still say "week" or "reset". The card calls that a summary.
static QString allowanceLine(bool ok, int left, const QString &line) {
    const bool dated = line.contains(QLatin1String("week"), Qt::CaseInsensitive) ||
                       line.contains(QLatin1String("reset"), Qt::CaseInsensitive);
    if (!ok) return line.isEmpty() || dated ? QStringLiteral("Checking credits") : line;
    if (line.isEmpty() || dated) return QStringLiteral("%1% left").arg(left);
    return line;
}
void drawCreditRing(QPainter &painter, const QRectF &rect, int percent, bool known) {
    const double inset = std::max(1.0, rect.width() * 0.1);
    const QRectF box = rect.adjusted(inset, inset, -inset, -inset);
    const double penW = std::max(2.6, rect.width() * 0.08);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 46), penW, Qt::SolidLine, Qt::RoundCap));
    painter.drawEllipse(box);
    const QColor color = known ? creditColor(percent) : QColor(186, 190, 198);
    if (known) {
        const int span = -qRound(360.0 * 16.0 * std::clamp(percent, 0, 100) / 100.0);
        if (span != 0) {
            painter.setPen(QPen(color, penW, Qt::SolidLine, Qt::RoundCap));
            painter.drawArc(box, 90 * 16, span);
        }
    }
    QFont font(QStringLiteral("Fira Sans"));
    font.setPixelSize(std::max(8, qRound(rect.height() * (known && percent >= 100 ? 0.26 : 0.32))));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(known ? color : QColor(255, 255, 255, 180));
    painter.drawText(rect, Qt::AlignCenter, known ? QString::number(std::clamp(percent, 0, 100)) : QStringLiteral("–"));
}

QString wallpaperFile() {
    const QString name = QStringLiteral("resources/preview-wallpaper.ppm");
    const QFileInfo exe(QString::fromLocal8Bit("/proc/self/exe"));
    const QString linked = exe.exists() ? exe.canonicalFilePath() : QString();
    if (!linked.isEmpty()) {
        const QString beside = QFileInfo(linked).absolutePath() + QLatin1Char('/') + name;
        if (QFileInfo::exists(beside)) return beside;
    }
    if (QFileInfo::exists(name)) return QFileInfo(name).absoluteFilePath();
    return {};
}

// A stack reads as a folder even when it holds one app. The tab, lip, and
// highlight are the same cues as a macOS folder sitting on the dock.
// A stack that holds another stack gets a second tab so the nesting is visible.
void drawStackFolder(QPainter &painter, const QRectF &rect, const QList<QPixmap> &icons, bool nested) {
    if (rect.isEmpty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double w = rect.width();
    const double h = rect.height();
    const double radius = std::max(3.0, w * 0.18);
    const QRectF body = rect.adjusted(w * 0.05, h * 0.18, -w * 0.07, -h * 0.05);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 75));
    painter.drawRoundedRect(body.translated(0, h * 0.065), radius, radius);

    const QRectF back(body.left() + w * 0.03, rect.top() + h * 0.07, body.width() * 0.90, h * 0.62);
    painter.setBrush(QColor(176, 112, 14));
    painter.drawRoundedRect(back.translated(w * 0.04, h * 0.02), radius * 0.75, radius * 0.75);
    const QRectF tab(back.left(), rect.top() + h * 0.012, back.width() * 0.46, h * 0.20);
    if (nested) {
        const QRectF rear(tab.left() + w * 0.16, tab.top() - h * 0.045, tab.width() * 0.72, tab.height() * 0.7);
        painter.setBrush(QColor(150, 92, 10));
        painter.drawRoundedRect(rear, radius * 0.4, radius * 0.4);
    }
    painter.setBrush(QColor(212, 146, 28));
    painter.drawRoundedRect(tab, radius * 0.5, radius * 0.5);
    painter.setBrush(QColor(196, 128, 18));
    painter.drawRoundedRect(back, radius * 0.8, radius * 0.8);

    const QRectF front(body.left(), body.top() + h * 0.06, body.width(), body.bottom() - (body.top() + h * 0.06));
    painter.setBrush(QColor(154, 92, 8));
    painter.drawRoundedRect(front.translated(w * 0.014, h * 0.028), radius, radius);
    QLinearGradient face(front.topLeft(), front.bottomLeft());
    face.setColorAt(0.0, QColor(255, 246, 204));
    face.setColorAt(0.22, QColor(255, 224, 112));
    face.setColorAt(0.72, QColor(245, 186, 42));
    face.setColorAt(1.0, QColor(214, 140, 18));
    painter.setBrush(face);
    painter.setPen(QPen(QColor(255, 252, 236, 200), 1));
    painter.drawRoundedRect(front, radius, radius);

    painter.setPen(Qt::NoPen);
    QLinearGradient shine(front.topLeft(), front.bottomLeft());
    shine.setColorAt(0.0, QColor(255, 255, 255, 150));
    shine.setColorAt(0.55, QColor(255, 255, 255, 20));
    shine.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.setBrush(shine);
    painter.drawRoundedRect(front.adjusted(w * 0.05, h * 0.02, -w * 0.05, -front.height() * 0.58), radius * 0.65, radius * 0.65);
    painter.setPen(QPen(QColor(150, 86, 6, 110), std::max(1.0, h * 0.018)));
    const double lip = front.top() + std::max(2.0, h * 0.045);
    painter.drawLine(QPointF(front.left() + radius * 0.8, lip), QPointF(front.right() - radius * 0.8, lip));

    painter.setPen(Qt::NoPen);
    const QRectF well = front.adjusted(w * 0.09, h * 0.07, -w * 0.09, -h * 0.07);
    const int shown = std::min(4, static_cast<int>(icons.size()));
    auto place = [&](const QPixmap &pix, const QRectF &cell) {
        painter.setBrush(QColor(90, 50, 0, 50));
        painter.drawEllipse(QRectF(cell.left() + cell.width() * 0.14, cell.bottom() - cell.height() * 0.02,
                                   cell.width() * 0.72, std::max(2.0, cell.height() * 0.14)));
        drawPixmapFit(painter, pix, cell);
    };
    if (shown == 1) {
        const double side = std::min(well.width(), well.height()) * 0.78;
        place(icons.at(0), QRectF(well.center().x() - side / 2.0, well.center().y() - side / 2.0, side, side));
    } else if (shown > 1) {
        const int rows = shown > 2 ? 2 : 1;
        const double gap = std::max(1.5, w * 0.035);
        const double cw = (well.width() - gap) / 2.0;
        const double ch = rows == 1 ? well.height() * 0.7 : (well.height() - gap) / rows;
        const double y0 = rows == 1 ? well.center().y() - ch / 2.0 : well.top();
        for (int n = 0; n < shown; ++n) {
            const QRectF cell(well.left() + (n % 2) * (cw + gap), y0 + (n / 2) * (ch + gap), cw, ch);
            const double side = std::min(cell.width(), cell.height());
            place(icons.at(n), QRectF(cell.center().x() - side / 2.0, cell.center().y() - side / 2.0, side, side));
        }
    }
    painter.restore();
}

// Shadow plus a short mirrored fade, the same depth cue as an icon on the macOS dock.
void drawLiftedIcon(QPainter &painter, const QPixmap &pix, const QRectF &icon) {
    if (pix.isNull() || icon.width() < 8) return;
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 70));
    painter.drawEllipse(QRectF(icon.left() + icon.width() * 0.16, icon.bottom() - icon.height() * 0.05,
                               icon.width() * 0.68, std::max(3.0, icon.height() * 0.16)));
    drawPixmapFit(painter, pix, icon);

    const double refH = icon.height() * 0.30;
    qreal dpr = 1;
    if (painter.device()) dpr = std::max<qreal>(1, painter.device()->devicePixelRatioF());
    QImage face(std::max(1, qRound(icon.width() * dpr)), std::max(1, qRound(icon.height() * dpr)),
                QImage::Format_ARGB32_Premultiplied);
    face.setDevicePixelRatio(dpr);
    face.fill(Qt::transparent);
    {
        QPainter gp(&face);
        gp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        gp.drawPixmap(QRectF(0, 0, icon.width(), icon.height()), pix, QRectF(0, 0, pix.width(), pix.height()));
    }
    const QImage flipped = face.flipped(Qt::Vertical);
    QImage plate(flipped.width(), std::max(1, qRound(refH * dpr)), QImage::Format_ARGB32_Premultiplied);
    plate.setDevicePixelRatio(dpr);
    plate.fill(Qt::transparent);
    {
        QPainter gp(&plate);
        gp.drawImage(QPointF(0, 0), flipped);
        gp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QLinearGradient fade(0, 0, 0, refH);
        fade.setColorAt(0.0, QColor(0, 0, 0, 115));
        fade.setColorAt(1.0, QColor(0, 0, 0, 0));
        gp.fillRect(QRectF(0, 0, icon.width(), refH), fade);
    }
    painter.drawImage(QRectF(icon.left(), icon.bottom() + 1, icon.width(), refH), plate);
    painter.restore();
}

QString desktopValue(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('\n'), QStringLiteral(" "));
    value.replace(QLatin1Char('\r'), QString());
    return value;
}

QString menuIconKind(const QString &action) {
    if (action == QLatin1String("pin")) return QStringLiteral("pin");
    if (action == QLatin1String("unpin") || action == QLatin1String("unpin-stack-now") ||
        action == QLatin1String("ask-unpin-stack"))
        return QStringLiteral("unpin");
    if (action == QLatin1String("remove-from-stack")) return QStringLiteral("remove");
    if (action == QLatin1String("new-window") || action == QLatin1String("new-stack")) return QStringLiteral("plus");
    if (action == QLatin1String("close-one") || action == QLatin1String("close-all") || action == QLatin1String("cancel"))
        return QStringLiteral("close");
    if (action == QLatin1String("empty-trash") || action == QLatin1String("empty-now")) return QStringLiteral("trash");
    if (action == QLatin1String("open-trash") || action == QLatin1String("open-all")) return QStringLiteral("open");
    if (action == QLatin1String("launcher") || action == QLatin1String("app-library")) return QStringLiteral("grid");
    if (action == QLatin1String("workspaces")) return QStringLiteral("windows");
    if (action == QLatin1String("search")) return QStringLiteral("search");
    if (action == QLatin1String("settings") || action == QLatin1String("manage")) return QStringLiteral("gear");
    if (action == QLatin1String("about") || action == QLatin1String("note") || action == QLatin1String("drop-hint"))
        return QStringLiteral("info");
    if (action == QLatin1String("contribute")) return QStringLiteral("heart");
    if (action == QLatin1String("taskman")) return QStringLiteral("bars");
    if (action == QLatin1String("grok")) return QStringLiteral("ring");
    if (action == QLatin1String("restore")) return QStringLiteral("up");
    if (action == QLatin1String("rename")) return QStringLiteral("pencil");
    if (action == QLatin1String("send-desktop")) return QStringLiteral("desktop");
    if (action == QLatin1String("ask-split") || action == QLatin1String("split-now")) return QStringLiteral("split");
    if (action == QLatin1String("add-submenu") || action.startsWith(QLatin1String("add-to:"))) return QStringLiteral("stack");
    if (action == QLatin1String("stack-back")) return QStringLiteral("back");
    if (action.startsWith(QLatin1String("focus:"))) return QStringLiteral("window");
    if (action.startsWith(QLatin1String("desktop-action:"))) return QStringLiteral("play");
    if (action == QLatin1String("header") || action == QLatin1String("-")) return {};
    return QStringLiteral("dot");
}

void drawMenuIcon(QPainter &painter, const QRectF &box, const QString &kind, const QColor &color) {
    if (kind.isEmpty() || box.isEmpty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double s = std::min(box.width(), box.height()) / 16.0;
    const QPointF o = box.center();
    auto at = [&](double x, double y) { return o + QPointF((x - 8.0) * s, (y - 8.0) * s); };
    auto line = [&](double x1, double y1, double x2, double y2) { painter.drawLine(at(x1, y1), at(x2, y2)); };
    auto boxOf = [&](double x1, double y1, double x2, double y2) { return QRectF(at(x1, y1), at(x2, y2)); };
    painter.setPen(QPen(color, std::max(1.25, 1.45 * s), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (kind == QLatin1String("pin") || kind == QLatin1String("unpin")) {
        painter.drawEllipse(at(8, 4.7), 2.15 * s, 2.15 * s);
        line(8, 6.8, 8, 12.1);
        line(6.2, 12.1, 8, 14.3);
        line(9.8, 12.1, 8, 14.3);
        if (kind == QLatin1String("unpin")) line(3.5, 3.5, 12.5, 12.5);
    } else if (kind == QLatin1String("remove")) {
        painter.drawEllipse(at(8, 8), 5.1 * s, 5.1 * s);
        line(5.2, 8, 10.8, 8);
    } else if (kind == QLatin1String("plus")) {
        line(8, 3.6, 8, 12.4);
        line(3.6, 8, 12.4, 8);
    } else if (kind == QLatin1String("close")) {
        line(4.2, 4.2, 11.8, 11.8);
        line(11.8, 4.2, 4.2, 11.8);
    } else if (kind == QLatin1String("trash")) {
        line(3.6, 5.4, 12.4, 5.4);
        line(6.5, 3.5, 9.5, 3.5);
        line(4.7, 5.4, 5.5, 12.6);
        line(11.3, 5.4, 10.5, 12.6);
        line(5.5, 12.6, 10.5, 12.6);
        line(8, 7.2, 8, 10.8);
    } else if (kind == QLatin1String("open") || kind == QLatin1String("up")) {
        line(8, 12.6, 8, 3.8);
        line(4.6, 7.2, 8, 3.6);
        line(11.4, 7.2, 8, 3.6);
    } else if (kind == QLatin1String("grid")) {
        painter.drawRoundedRect(boxOf(3.2, 3.2, 6.9, 6.9), 0.8 * s, 0.8 * s);
        painter.drawRoundedRect(boxOf(9.1, 3.2, 12.8, 6.9), 0.8 * s, 0.8 * s);
        painter.drawRoundedRect(boxOf(3.2, 9.1, 6.9, 12.8), 0.8 * s, 0.8 * s);
        painter.drawRoundedRect(boxOf(9.1, 9.1, 12.8, 12.8), 0.8 * s, 0.8 * s);
    } else if (kind == QLatin1String("windows")) {
        painter.drawRoundedRect(boxOf(2.6, 4.8, 9.2, 12.4), 1.0 * s, 1.0 * s);
        painter.drawRoundedRect(boxOf(6.6, 3.2, 13.4, 9.4), 1.0 * s, 1.0 * s);
    } else if (kind == QLatin1String("search")) {
        painter.drawEllipse(at(7.1, 7.1), 3.3 * s, 3.3 * s);
        line(9.6, 9.6, 12.8, 12.8);
    } else if (kind == QLatin1String("gear")) {
        painter.drawEllipse(at(8, 8), 2.3 * s, 2.3 * s);
        for (int i = 0; i < 8; ++i) {
            const double a = i * 3.14159265358979323846 / 4.0;
            const QPointF dir(std::cos(a), std::sin(a));
            painter.drawLine(o + dir * 3.7 * s, o + dir * 5.7 * s);
        }
    } else if (kind == QLatin1String("info")) {
        painter.drawEllipse(at(8, 8), 5.2 * s, 5.2 * s);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(at(8, 5.4), 0.75 * s, 0.75 * s);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, std::max(1.25, 1.45 * s), Qt::SolidLine, Qt::RoundCap));
        line(8, 7.5, 8, 11.3);
    } else if (kind == QLatin1String("heart")) {
        QPainterPath heart;
        heart.moveTo(at(8, 13.2));
        heart.cubicTo(at(2.2, 9.0), at(2.6, 4.2), at(6.2, 4.2));
        heart.cubicTo(at(8, 4.2), at(8, 6.5), at(8, 6.5));
        heart.cubicTo(at(8, 6.5), at(8, 4.2), at(9.8, 4.2));
        heart.cubicTo(at(13.4, 4.2), at(13.8, 9.0), at(8, 13.2));
        painter.drawPath(heart);
    } else if (kind == QLatin1String("bars")) {
        line(4.4, 12.4, 4.4, 8.0);
        line(8, 12.4, 8, 4.0);
        line(11.6, 12.4, 11.6, 6.4);
    } else if (kind == QLatin1String("ring")) {
        painter.setPen(QPen(QColor(color.red(), color.green(), color.blue(), 70), std::max(1.25, 1.45 * s)));
        painter.drawEllipse(at(8, 8), 4.6 * s, 4.6 * s);
        painter.setPen(QPen(color, std::max(1.35, 1.6 * s), Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(at(3.4, 3.4), at(12.6, 12.6)), 90 * 16, -250 * 16);
    } else if (kind == QLatin1String("pencil")) {
        line(3.8, 12.2, 11.4, 4.6);
        line(10.2, 3.4, 12.6, 5.8);
        line(3.2, 13.4, 5.4, 11.2);
    } else if (kind == QLatin1String("desktop")) {
        painter.drawRoundedRect(boxOf(3.2, 3.4, 12.8, 10.0), 1.1 * s, 1.1 * s);
        line(6.2, 10.0, 6.2, 12.3);
        line(9.8, 10.0, 9.8, 12.3);
        line(5.2, 12.6, 10.8, 12.6);
    } else if (kind == QLatin1String("split")) {
        painter.drawRoundedRect(boxOf(2.4, 4.2, 7.0, 11.8), 1.0 * s, 1.0 * s);
        painter.drawRoundedRect(boxOf(9.0, 4.2, 13.6, 11.8), 1.0 * s, 1.0 * s);
    } else if (kind == QLatin1String("stack")) {
        painter.drawRoundedRect(boxOf(3.0, 6.0, 13.0, 12.6), 1.1 * s, 1.1 * s);
        painter.drawRoundedRect(boxOf(3.0, 3.6, 8.2, 6.6), 0.8 * s, 0.8 * s);
    } else if (kind == QLatin1String("back")) {
        line(10.8, 3.6, 5.2, 8);
        line(5.2, 8, 10.8, 12.4);
    } else if (kind == QLatin1String("forward")) {
        line(5.2, 3.6, 10.8, 8);
        line(10.8, 8, 5.2, 12.4);
    } else if (kind == QLatin1String("window")) {
        painter.drawRoundedRect(boxOf(3.0, 3.4, 13.0, 12.6), 1.2 * s, 1.2 * s);
        line(3.2, 6.2, 12.8, 6.2);
    } else if (kind == QLatin1String("play")) {
        QPainterPath play;
        play.moveTo(at(5.6, 3.6));
        play.lineTo(at(12.4, 8));
        play.lineTo(at(5.6, 12.4));
        play.closeSubpath();
        painter.drawPath(play);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(at(8, 8), 1.7 * s, 1.7 * s);
    }
    painter.restore();
}

}  // namespace

struct MenuRow {
    QString action;
    QString label;
    bool enabled = true;
    bool header = false;
    bool toggle = false;
    bool checked = false;
    std::vector<MenuRow> sub;
};

// A row that opens another menu, either a flyout or a following page.
bool menuContinues(const MenuRow &row) {
    if (row.header || row.toggle || row.action == QLatin1String("-")) return false;
    if (!row.sub.empty()) return true;
    return row.action == QLatin1String("manage") || row.action == QLatin1String("ask-split") ||
           row.action == QLatin1String("ask-unpin-stack");
}

struct Dock::Item {
    enum class Kind {
        Launcher, Workspaces, AppLibrary, Search, App, Stack, Minimize, Minimized, Trash, TaskManager, Grok, Overflow
    } kind = Kind::App;
    QString id;
    QString name;
    std::vector<Item> children;
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
    QRectF menuClose;
    std::vector<QRectF> menuRows;
    QRectF subRect;
    QRectF subClose;
    std::vector<QRectF> subRows;
    bool rename = false;
    QString renameText;
    bool renameSelectAll = false;
    int caret = 0;
    QRectF titleRect;
    bool pressTitle = false;
    qint64 lastTitleClick = 0;
    QString lastTitleStack;
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
        bool nested = false;
        QString stackId;
    };
    std::vector<Cell> cells;
    QRectF popupRect;
    QRectF popupClose;
    QRectF stackBack;
    QRectF stackManage;
    // A stack inside the open stack. It sits to the left or right so icons can move between the two.
    QString sideStack;
    QString shownSide;
    double side = 0;
    std::vector<Cell> sideCells;
    QRectF sideRect;
    QRectF sideClose;
    QRectF sideTitle;
    QRectF sideManage;
    bool pressSide = false;
    int dropIndex = -1;
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
    std::vector<Card> stackWindows;
    std::vector<Card> sideWindows;
    QRectF previewsRect;
    QRectF previewsClose;
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
    QRectF searchClose;
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
    QRectF aboutClose;
    QRectF aboutCoffee;
    QHash<QString, double> scale;
    QHash<QString, double> target;
    qint64 tipSince = 0;
    QString tipUid;
    bool fileDrag = false;
    QRectF meterCard;
    QRectF meterClose;
    QRectF grokCard;
    QRectF grokClose;
    QString tipDismissed;
    bool pressDismiss = false;
    QString pressDismissId;
    QRectF grokTabGrok;
    QRectF grokTabBots;
    QRectF grokLaunch;
    int grokPage = 0;
    bool pressGrokTab = false;
    int pressGrokPage = -1;
    bool pressGrokLaunch = false;
    bool pressGrokBody = false;
    bool pressBack = false;
    bool pressManage = false;
};

Dock::Dock(AppIndex *apps) : apps_(apps), now_(nowMs()) {
    probeGrokTools();
    grokProbe_ = now_;
}

Dock::~Dock() {
    auto stop = [](QProcess *&proc) {
        if (!proc) return;
        proc->disconnect();
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(200);
        }
        delete proc;
        proc = nullptr;
    };
    stop(creditProc_);
    stop(botProc_);
}

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

int Dock::screenGap() const { return std::max(margin_, kEdgeGuard); }

QRectF Dock::popupRoom(const Screen &screen) const {
    const double edge = kScreenMargin;
    double left = edge;
    double top = edge;
    double right = std::max(left + 64.0, screen.w - edge);
    double bottom = std::max(top + 64.0, screen.h - edge);
    const QRectF dock = screen.pill;
    if (dock.width() > 8 && dock.height() > 8) {
        if (edge_ == ShellSpec::Edge::Bottom) bottom = std::min(bottom, dock.top() - kPopupGap);
        else if (edge_ == ShellSpec::Edge::Top) top = std::max(top, dock.bottom() + kPopupGap);
        else if (edge_ == ShellSpec::Edge::Left) left = std::max(left, dock.right() + kPopupGap);
        else right = std::min(right, dock.left() - kPopupGap);
    }
    if (bottom < top + 64) bottom = top + 64;
    if (right < left + 64) right = left + 64;
    return QRectF(left, top, std::max(64.0, right - left), std::max(64.0, bottom - top));
}

QRectF Dock::popupNear(const Screen &screen, const QPointF &anchor, double width, double height) const {
    const QRectF room = popupRoom(screen);
    const double w = width;
    const double h = height;
    double x = room.left();
    double y = room.top();
    if (horizontal()) {
        x = std::clamp(anchor.x() - w / 2.0, room.left(), std::max(room.left(), room.right() - w));
        y = edge_ == ShellSpec::Edge::Top ? room.top() : room.bottom() - h;
    } else {
        y = std::clamp(anchor.y() - h / 2.0, room.top(), std::max(room.top(), room.bottom() - h));
        x = edge_ == ShellSpec::Edge::Left ? room.left() : room.right() - w;
    }
    if (y < room.top()) y = room.top();
    if (x < room.left()) x = room.left();
    y = std::clamp(y, kScreenMargin, std::max(kScreenMargin, screen.h - h - kScreenMargin));
    x = std::clamp(x, kScreenMargin, std::max(kScreenMargin, screen.w - w - kScreenMargin));
    return QRectF(x, y, w, h);
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
    const bool inMain = !screen.popupRect.isEmpty() && screen.popupRect.contains(x, y);
    const bool inSide = !screen.sideRect.isEmpty() && screen.sideRect.contains(x, y);
    if (inMain || inSide) return false;
    double near = 0;
    bool any = false;
    auto consider = [&](const QRectF &rect) {
        if (rect.isEmpty()) return;
        double edge = rect.bottom();
        if (edge_ == ShellSpec::Edge::Top) edge = rect.top();
        else if (edge_ == ShellSpec::Edge::Left) edge = rect.left();
        else if (edge_ == ShellSpec::Edge::Right) edge = rect.right();
        const bool towardMax = edge_ == ShellSpec::Edge::Bottom || edge_ == ShellSpec::Edge::Right;
        if (!any) near = edge;
        else near = towardMax ? std::max(near, edge) : std::min(near, edge);
        any = true;
    };
    consider(screen.popupRect);
    consider(screen.sideRect);
    if (!any) return false;
    if (edge_ == ShellSpec::Edge::Bottom) return y > near + 6;
    if (edge_ == ShellSpec::Edge::Top) return y < near - 6;
    if (edge_ == ShellSpec::Edge::Left) return x < near - 6;
    return x > near + 6;
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
        if (item.kind == Item::Kind::Stack && holdsApp(item, id)) pinned = true;
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

void Dock::refreshChrome() {
    const CosmicChrome chrome = readCosmicChrome();
    if (!chrome.ok || chrome.signature == chromeSig_) return;
    chromeR_ = chrome.red;
    chromeG_ = chrome.green;
    chromeB_ = chrome.blue;
    hintPx_ = chrome.hintPx;
    chromeSig_ = chrome.signature;
    chromeDirty_ = true;
}

void Dock::pollSettings() {
    if (now_ - settingsPoll_ < 700) return;
    settingsPoll_ = now_;
    if (apps_->rescanIfChanged()) shellDirty_ = true;
    refreshChrome();
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
    spec.reserve = static_cast<int>(std::lround(barThickness() + screenGap()));
    spec.output = outputMode_;
    spec.activeOutput = activeOutputId();
    return spec;
}

bool Dock::takeShellChange() {
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                            .arg(static_cast<int>(edge_))
                            .arg(exclusive_ ? 1 : 0)
                            .arg(static_cast<int>(std::lround(barThickness() + screenGap())))
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
    if (!persist_) return;
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
        case Item::Kind::Trash:
        case Item::Kind::TaskManager:
        case Item::Kind::Grok:
        case Item::Kind::Overflow:
            continue;
        case Item::Kind::App:
            object.insert(QStringLiteral("kind"), QStringLiteral("app"));
            object.insert(QStringLiteral("id"), item.id);
            break;
        case Item::Kind::Stack:
            object = writeChild(item).toObject();
            break;
        }
        items.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 4);
    root.insert(QStringLiteral("items"), items);
    QJsonObject applets;
    applets.insert(QStringLiteral("trash"), showTrash_);
    applets.insert(QStringLiteral("taskman"), showTask_);
    applets.insert(QStringLiteral("grok"), showGrok_);
    root.insert(QStringLiteral("applets"), applets);
    QJsonObject launch;
    for (auto it = execs_.cbegin(); it != execs_.cend(); ++it) launch.insert(it.key(), it.value());
    if (!launch.isEmpty()) root.insert(QStringLiteral("launch"), launch);
    if (!grogyPath_.isEmpty()) {
        QJsonObject tools;
        tools.insert(QStringLiteral("grogy"), grogyPath_);
        root.insert(QStringLiteral("tools"), tools);
    }
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
                for (const QString &id : social) {
                    Item app;
                    app.kind = Item::Kind::App;
                    app.id = id;
                    stack.children.push_back(app);
                }
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
        for (const QString &id : social) {
            Item app;
            app.kind = Item::Kind::App;
            app.id = id;
            stack.children.push_back(app);
        }
        items_.push_back(stack);
    }
}

void Dock::load() {
    QFile file(configPath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        buildDefault();
        loaded_ = true;
        save();
        applyCosmicSettings(readCosmicDockSettings());
        refreshChrome();
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
    const QJsonObject applets = root.value(QStringLiteral("applets")).toObject();
    if (applets.contains(QStringLiteral("trash"))) showTrash_ = applets.value(QStringLiteral("trash")).toBool(true);
    if (applets.contains(QStringLiteral("taskman"))) showTask_ = applets.value(QStringLiteral("taskman")).toBool(true);
    if (applets.contains(QStringLiteral("grok"))) showGrok_ = applets.value(QStringLiteral("grok")).toBool(true);
    items_.clear();
    const QJsonArray items = root.value(QStringLiteral("items")).toArray();
    for (const QJsonValue &value : items) {
        const QString kind = value.toObject().value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("launcher") || kind == QLatin1String("minimize") ||
            kind == QLatin1String("workspaces") || kind == QLatin1String("applibrary") ||
            kind == QLatin1String("search") || kind == QLatin1String("taskman") ||
            kind == QLatin1String("overflow") || kind == QLatin1String("grok")) {
            continue;
        }
        Item item;
        if (!readNode(item, value)) continue;
        if (item.kind == Item::Kind::App && (item.id.isEmpty() || item.id == taskId())) continue;
        items_.push_back(item);
    }
    pruneEmpty(items_);
    const QString savedGrogy = root.value(QStringLiteral("tools")).toObject().value(QStringLiteral("grogy")).toString();
    if (!savedGrogy.isEmpty() && QFileInfo(savedGrogy).isExecutable()) {
        if (grogyPath_.isEmpty()) {
            grogyPath_ = QFileInfo(savedGrogy).absoluteFilePath();
            grogyOnPath_ = true;
        }
    } else if (!grogyPath_.isEmpty()) {
        grogySave_ = true;
    }
    if (!savedGrogy.isEmpty() && savedGrogy == grogyPath_) grogySave_ = false;
    loaded_ = true;
    if (items_.empty()) {
        buildDefault();
        save();
    } else if (version < 4 || grogySave_) {
        grogySave_ = false;
        save();
    }
    applyCosmicSettings(readCosmicDockSettings());
    refreshChrome();
}

void Dock::dismissPopup(Screen &screen, const QString &which) {
    if (which == QLatin1String("stack")) {
        if (screen.rename) commitRename(screen, true);
        screen.openStack.clear();
        screen.sideStack.clear();
        screen.menu = false;
    } else if (which == QLatin1String("side")) {
        screen.sideStack.clear();
    } else if (which == QLatin1String("previews")) {
        screen.openPreviews.clear();
    } else if (which == QLatin1String("menu")) {
        if (screen.rename) commitRename(screen, true);
        screen.menu = false;
    } else if (which == QLatin1String("sub")) {
        screen.menuRow = -1;
        screen.subRow = -1;
    } else if (which == QLatin1String("search")) {
        screen.search = false;
    } else if (which == QLatin1String("about")) {
        screen.about = false;
    } else if (which == QLatin1String("meter")) {
        screen.tipDismissed = QStringLiteral("taskman");
        screen.tipUid.clear();
    } else if (which == QLatin1String("grok")) {
        screen.tipDismissed = QStringLiteral("grok");
        screen.tipUid.clear();
    }
}

bool Dock::isApp(const Live &live, const QString &id) const {
    if (id.isEmpty()) return false;
    const QString want = canonical(id);
    if (live.appId == id || live.rawId == id || live.appId == want || live.rawId == want) return true;
    const QString fromLive = canonical(live.appId.isEmpty() ? live.rawId : live.appId);
    if (!fromLive.isEmpty() && (fromLive == id || fromLive == want)) return true;
    const QString resolved = apps_->resolve(live.rawId);
    return !resolved.isEmpty() && (resolved == id || resolved == want);
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

QString Dock::cleanAppId(const QString &raw) {
    QString id = raw.trimmed();
    if (id.isEmpty()) return {};
    const QString resolved = apps_->resolve(id);
    if (!resolved.isEmpty()) id = resolved;
    const QString adopted = apps_->adoptProject(id);
    if (!adopted.isEmpty()) id = adopted;
    if (id == taskId() || canonical(id) == taskId()) return {};
    return id;
}

bool Dock::readNode(Item &item, const QJsonValue &value) {
    if (value.isString()) {
        item.kind = Item::Kind::App;
        item.id = cleanAppId(value.toString());
        item.children.clear();
        return !item.id.isEmpty();
    }
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QString kind = object.value(QStringLiteral("kind")).toString();
    const bool stack = kind == QLatin1String("stack") || object.contains(QStringLiteral("children")) ||
                       object.contains(QStringLiteral("apps"));
    if (stack && kind != QLatin1String("app")) {
        item.kind = Item::Kind::Stack;
        item.id = object.value(QStringLiteral("id")).toString();
        if (item.id.isEmpty()) item.id = QUuid::createUuid().toString(QUuid::Id128).left(8);
        item.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Stack"));
        item.children.clear();
        const QJsonArray rows = object.contains(QStringLiteral("children")) ? object.value(QStringLiteral("children")).toArray()
                                                                             : object.value(QStringLiteral("apps")).toArray();
        for (const QJsonValue &row : rows) {
            Item child;
            if (!readNode(child, row)) continue;
            if (child.kind == Item::Kind::App && holdsApp(item, child.id)) continue;
            if (child.kind == Item::Kind::Stack && child.id == item.id) continue;
            item.children.push_back(child);
        }
        return !item.children.empty();
    }
    item.kind = Item::Kind::App;
    item.id = cleanAppId(object.value(QStringLiteral("id")).toString());
    item.children.clear();
    return !item.id.isEmpty();
}

QJsonValue Dock::writeChild(const Item &item) const {
    if (item.kind != Item::Kind::Stack) return QJsonValue(item.id);
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("stack"));
    object.insert(QStringLiteral("id"), item.id);
    object.insert(QStringLiteral("name"), item.name.isEmpty() ? QStringLiteral("Stack") : item.name);
    QJsonArray children;
    for (const Item &child : item.children) children.append(writeChild(child));
    object.insert(QStringLiteral("children"), children);
    return object;
}

QStringList Dock::appsIn(const Item &item) const {
    QStringList ids;
    const std::function<void(const Item &)> walk = [&](const Item &node) {
        if (node.kind == Item::Kind::App) {
            if (!node.id.isEmpty() && !ids.contains(node.id)) ids << node.id;
            return;
        }
        for (const Item &child : node.children) walk(child);
    };
    walk(item);
    return ids;
}

bool Dock::holdsApp(const Item &item, const QString &id) const {
    if (id.isEmpty()) return false;
    const QString want = canonical(id);
    for (const QString &app : appsIn(item)) {
        if (app == id || app == want || canonical(app) == want) return true;
    }
    return false;
}

bool Dock::containsStack(const Item &item, const QString &stackId) const {
    if (stackId.isEmpty() || item.kind != Item::Kind::Stack) return false;
    for (const Item &child : item.children) {
        if (child.kind != Item::Kind::Stack) continue;
        if (child.id == stackId || containsStack(child, stackId)) return true;
    }
    return false;
}

std::vector<Dock::Item> *Dock::childList(const QString &stackId, int *index) {
    const std::function<std::vector<Item> *(std::vector<Item> &)> walk = [&](std::vector<Item> &nodes) -> std::vector<Item> * {
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            Item &node = nodes[static_cast<size_t>(i)];
            if (node.kind == Item::Kind::Stack && node.id == stackId) {
                if (index) *index = i;
                return &nodes;
            }
            if (node.kind == Item::Kind::Stack) {
                if (std::vector<Item> *found = walk(node.children)) return found;
            }
        }
        return nullptr;
    };
    return walk(items_);
}

Dock::Item *Dock::findStack(const QString &id) {
    int index = -1;
    std::vector<Item> *home = childList(id, &index);
    if (!home || index < 0 || index >= static_cast<int>(home->size())) return nullptr;
    return &(*home)[static_cast<size_t>(index)];
}

QString Dock::parentStackId(const QString &id) const {
    QString parent;
    const std::function<bool(const std::vector<Item> &, const QString &)> walk =
        [&](const std::vector<Item> &nodes, const QString &owner) {
            for (const Item &node : nodes) {
                if (node.kind != Item::Kind::Stack) continue;
                if (node.id == id) {
                    parent = owner;
                    return true;
                }
                if (walk(node.children, node.id)) return true;
            }
            return false;
        };
    walk(items_, {});
    return parent;
}

QString Dock::topStackId(const QString &id) const {
    QString current = id;
    for (int guard = 0; guard < 16; ++guard) {
        const QString parent = parentStackId(current);
        if (parent.isEmpty()) return current;
        current = parent;
    }
    return id;
}

void Dock::pruneEmpty(std::vector<Item> &nodes, const QString &keep) {
    for (Item &node : nodes) {
        if (node.kind == Item::Kind::Stack) pruneEmpty(node.children, keep);
    }
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [&](const Item &node) {
                                   return node.kind == Item::Kind::Stack && node.children.empty() && node.id != keep;
                               }),
                nodes.end());
}

void Dock::stripApp(std::vector<Item> &nodes, const QString &id, const QString &keepStack) {
    const QString want = canonical(id);
    for (Item &node : nodes) {
        if (node.kind != Item::Kind::Stack) continue;
        // A direct child of the target stays. Copies inside a folder of that target still leave,
        // so "add to this stack" can lift an app out of a nested folder.
        const bool keepDirect = !keepStack.isEmpty() && node.id == keepStack;
        if (!keepDirect) {
            node.children.erase(std::remove_if(node.children.begin(), node.children.end(),
                                               [&](const Item &child) {
                                                   return child.kind == Item::Kind::App &&
                                                          (child.id == id || child.id == want || canonical(child.id) == want);
                                               }),
                                node.children.end());
        }
        stripApp(node.children, id, keepStack);
    }
    pruneEmpty(nodes, keepStack);
}

void Dock::placeInStack(const QString &appId, const QString &stackId) {
    if (appId.isEmpty() || stackId.isEmpty() || !findStack(stackId)) return;
    const QString want = canonical(appId);
    Item moving;
    moving.kind = Item::Kind::App;
    moving.id = want;
    bool found = false;
    const std::function<void(const std::vector<Item> &)> remember = [&](const std::vector<Item> &nodes) {
        for (const Item &node : nodes) {
            if (node.kind == Item::Kind::App &&
                (node.id == appId || node.id == want || canonical(node.id) == want)) {
                moving = node;
                found = true;
                return;
            }
            if (node.kind == Item::Kind::Stack) {
                remember(node.children);
                if (found) return;
            }
        }
    };
    remember(items_);
    stripApp(items_, moving.id, stackId);
    auto takePin = [&](const QString &id) {
        const int index = itemIndex(QStringLiteral("app:") + id);
        if (index < 0) return;
        if (!found) {
            moving = items_[static_cast<size_t>(index)];
            found = true;
        }
        items_.erase(items_.begin() + index);
    };
    takePin(moving.id);
    if (moving.id != appId) takePin(appId);
    Item *stack = findStack(stackId);
    if (!stack) {
        if (found) items_.push_back(moving);
        save();
        return;
    }
    bool direct = false;
    for (const Item &child : stack->children) {
        if (child.kind == Item::Kind::App &&
            (child.id == moving.id || child.id == want || canonical(child.id) == want)) {
            direct = true;
            break;
        }
    }
    if (!direct) stack->children.push_back(std::move(moving));
    pruneEmpty(items_);
    save();
}

void Dock::closeStackIfGone(Screen &screen) {
    if (!screen.openStack.isEmpty() && !findStack(screen.openStack)) {
        screen.openStack.clear();
        screen.shownStack.clear();
        screen.sideStack.clear();
        screen.shownSide.clear();
        return;
    }
    if (screen.sideStack.isEmpty()) return;
    const Item *open = screen.openStack.isEmpty() ? nullptr : findStack(screen.openStack);
    if (!findStack(screen.sideStack) || !open || !containsStack(*open, screen.sideStack)) {
        screen.sideStack.clear();
        screen.shownSide.clear();
    }
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
    if (from < 0) return;
    Item drag = items_[static_cast<size_t>(from)];
    if (drag.kind != Item::Kind::App && drag.kind != Item::Kind::Stack) return;
    if (targetUid.startsWith(QLatin1String("stack:"))) {
        const QString targetId = targetUid.mid(6);
        const Item *target = findStack(targetId);
        if (!target) return;
        if (drag.kind == Item::Kind::Stack && (drag.id == targetId || containsStack(drag, targetId))) return;
        const bool already = drag.kind == Item::Kind::App && holdsApp(*target, drag.id);
        items_.erase(items_.begin() + from);
        Item *stack = findStack(targetId);
        if (!stack) {
            items_.insert(items_.begin() + std::min(from, static_cast<int>(items_.size())), drag);
            return;
        }
        if (drag.kind == Item::Kind::App) {
            if (!already) {
                stripApp(items_, drag.id, targetId);
                stack = findStack(targetId);
                if (stack && !holdsApp(*stack, drag.id)) stack->children.push_back(drag);
            }
        } else if (!containsStack(*stack, drag.id)) {
            stack->children.push_back(drag);
        }
        for (auto it = screens_.begin(); it != screens_.end(); ++it) closeStackIfGone(it.value());
        save();
        return;
    }
    const int to = itemIndex(targetUid);
    if (to < 0 || to == from) return;
    Item target = items_[static_cast<size_t>(to)];
    if ((drag.kind != Item::Kind::App && drag.kind != Item::Kind::Stack) || target.kind != Item::Kind::App) return;
    Item stack;
    stack.kind = Item::Kind::Stack;
    stack.id = QUuid::createUuid().toString(QUuid::Id128).left(8);
    stack.name = QStringLiteral("Stack");
    if (target.kind == Item::Kind::App) {
        Item app;
        app.kind = Item::Kind::App;
        app.id = target.id;
        stack.children.push_back(app);
    }
    if (drag.kind == Item::Kind::App) {
        if (!holdsApp(stack, drag.id)) stack.children.push_back(drag);
    } else {
        stack.children.push_back(drag);
    }
    const int keep = std::min(from, to);
    const int drop = std::max(from, to);
    items_.erase(items_.begin() + drop);
    items_.erase(items_.begin() + keep);
    items_.insert(items_.begin() + keep, stack);
    save();
}

void Dock::pullFromStack(Screen &screen, const QString &appId) {
    const QString stackId = screen.pressSide && !screen.shownSide.isEmpty() ? screen.shownSide : screen.shownStack;
    const QString up = parentStackId(stackId);
    Item *stack = findStack(stackId);
    if (!stack) return;
    Item app;
    bool found = false;
    for (auto it = stack->children.begin(); it != stack->children.end(); ++it) {
        if (it->kind == Item::Kind::App && (it->id == appId || canonical(it->id) == canonical(appId))) {
            app = *it;
            stack->children.erase(it);
            found = true;
            break;
        }
    }
    if (!found) return;
    if (stack->children.empty()) {
        int index = -1;
        std::vector<Item> *home = childList(stackId, &index);
        if (!home || index < 0) return;
        const bool top = home == &items_;
        home->erase(home->begin() + index);
        if (top) {
            items_.insert(items_.begin() + std::min(index, static_cast<int>(items_.size())), app);
            screen.openStack.clear();
            screen.shownStack.clear();
        } else {
            items_.push_back(app);
            screen.openStack = up;
        }
    } else {
        const int at = itemIndex(QStringLiteral("stack:") + topStackId(stackId));
        const int insertAt = at < 0 ? static_cast<int>(items_.size()) : at + 1;
        items_.insert(items_.begin() + insertAt, app);
    }
    screen.menu = false;
    closeStackIfGone(screen);
    save();
}

int Dock::childIndex(const Item &stack, const QString &appId, const QString &stackId) const {
    for (int i = 0; i < static_cast<int>(stack.children.size()); ++i) {
        const Item &child = stack.children[static_cast<size_t>(i)];
        if (!stackId.isEmpty()) {
            if (child.kind == Item::Kind::Stack && child.id == stackId) return i;
            continue;
        }
        if (!appId.isEmpty() && child.kind == Item::Kind::App &&
            (child.id == appId || canonical(child.id) == canonical(appId))) {
            return i;
        }
    }
    return -1;
}

void Dock::moveChild(const QString &stackId, int from, int to) {
    Item *stack = findStack(stackId);
    if (!stack) return;
    const int count = static_cast<int>(stack->children.size());
    if (from < 0 || to < 0 || from >= count || to >= count || from == to) return;
    Item item = stack->children[static_cast<size_t>(from)];
    stack->children.erase(stack->children.begin() + from);
    if (to > from) to -= 1;
    stack->children.insert(stack->children.begin() + to, std::move(item));
    save();
}

void Dock::moveChildTo(Screen &screen, const QString &sourceId, const QString &targetId) {
    if (sourceId.isEmpty() || targetId.isEmpty() || sourceId == targetId) return;
    Item *source = findStack(sourceId);
    if (!source) return;
    const QString dragStack = screen.pressUid.startsWith(QLatin1String("nest:")) ? screen.pressUid.mid(5) : QString();
    const int from = childIndex(*source, dragStack.isEmpty() ? screen.pressApp : QString(), dragStack);
    if (from < 0) return;
    Item moving = source->children[static_cast<size_t>(from)];
    if (moving.kind == Item::Kind::Stack && (moving.id == targetId || containsStack(moving, targetId))) return;
    if (!findStack(targetId)) return;
    const int insertAt = screen.dropIndex;
    source->children.erase(source->children.begin() + from);
    Item *target = findStack(targetId);
    if (!target) return;
    if (moving.kind == Item::Kind::App) {
        stripApp(target->children, moving.id);
        target = findStack(targetId);
        if (!target) return;
        for (const Item &child : target->children) {
            if (child.kind == Item::Kind::App &&
                (child.id == moving.id || canonical(child.id) == canonical(moving.id))) {
                pruneEmpty(items_);
                closeStackIfGone(screen);
                save();
                return;
            }
        }
    }
    int at = insertAt;
    const int count = static_cast<int>(target->children.size());
    if (at < 0 || at > count) at = count;
    target->children.insert(target->children.begin() + at, std::move(moving));
    pruneEmpty(items_);
    closeStackIfGone(screen);
    save();
}

void Dock::dropChildInto(Screen &screen, const QString &sourceId, const QString &targetId) {
    const QString homeId = sourceId.isEmpty() ? screen.shownStack : sourceId;
    if (homeId.isEmpty() || targetId.isEmpty() || homeId == targetId) return;
    Item *home = findStack(homeId);
    if (!home) return;
    const QString dragStack = screen.pressUid.startsWith(QLatin1String("nest:")) ? screen.pressUid.mid(5) : QString();
    const int from = childIndex(*home, dragStack.isEmpty() ? screen.pressApp : QString(), dragStack);
    if (from < 0) return;
    Item moving = home->children[static_cast<size_t>(from)];
    if (moving.kind == Item::Kind::Stack && (moving.id == targetId || containsStack(moving, targetId))) return;
    Item *target = findStack(targetId);
    if (!target) return;
    const bool already = moving.kind == Item::Kind::App && holdsApp(*target, moving.id);
    home->children.erase(home->children.begin() + from);
    if (!already) {
        target = findStack(targetId);
        if (!target) return;
        target->children.push_back(std::move(moving));
    }
    pruneEmpty(items_);
    closeStackIfGone(screen);
    save();
}

void Dock::dragCell(Screen &screen, double x, double y) {
    screen.dropIndex = -1;
    auto clear = [&]() {
        screen.hoverUid.clear();
        screen.mergeReady = false;
        screen.dropIndex = -1;
    };
    if (pulledToDock(screen, x, y)) {
        clear();
        return;
    }
    const QString sourceId = screen.pressSide && !screen.shownSide.isEmpty() ? screen.shownSide : screen.shownStack;
    const Item *sourceItem = findStack(sourceId);
    const bool overSide = !screen.sideRect.isEmpty() && screen.sideRect.contains(x, y);
    const bool overMain = !screen.popupRect.isEmpty() && screen.popupRect.contains(x, y);
    if (!sourceItem || (!overSide && !overMain)) {
        clear();
        return;
    }
    const bool useSide = overSide;
    const QString targetId = useSide ? screen.shownSide : screen.shownStack;
    const std::vector<Screen::Cell> &cells = useSide ? screen.sideCells : screen.cells;
    const Item *target = findStack(targetId);
    if (!target) {
        clear();
        return;
    }
    const Screen::Cell *over = nullptr;
    for (const Screen::Cell &cell : cells) {
        if (!cell.rect.contains(x, y)) continue;
        over = &cell;
        break;
    }
    const QString dragStack = screen.pressUid.startsWith(QLatin1String("nest:")) ? screen.pressUid.mid(5) : QString();
    auto illegalNest = [&](const QString &nestId) {
        if (nestId.isEmpty() || nestId == dragStack) return true;
        if (dragStack.isEmpty()) return false;
        const Item *drag = findStack(dragStack);
        return drag && containsStack(*drag, nestId);
    };
    if (over && over->nested && over->icon.contains(x, y) && !illegalNest(over->stackId)) {
        const QString nest = QStringLiteral("stack:") + over->stackId;
        if (screen.hoverUid != nest) {
            screen.hoverUid = nest;
            screen.hoverSince = now_;
        }
        screen.mergeReady = true;
        return;
    }
    if (targetId == sourceId) {
        if (!over) {
            clear();
            return;
        }
        const bool self = !dragStack.isEmpty() ? over->nested && over->stackId == dragStack
                                               : !over->nested && !screen.pressApp.isEmpty() && over->appId == screen.pressApp;
        if (self) {
            clear();
            return;
        }
        const bool overNested = over->nested;
        const QString overApp = over->appId;
        const QString overStack = over->stackId;
        const QPointF overCenter = over->rect.center();
        const int from = childIndex(*sourceItem, dragStack.isEmpty() ? screen.pressApp : QString(), dragStack);
        const int to = childIndex(*sourceItem, overNested ? QString() : overApp, overNested ? overStack : QString());
        if (from >= 0 && to >= 0 && from != to) {
            const Screen::Cell *source = nullptr;
            for (const Screen::Cell &cell : cells) {
                const bool match = !dragStack.isEmpty() ? cell.nested && cell.stackId == dragStack
                                                        : !cell.nested && cell.appId == screen.pressApp;
                if (match) source = &cell;
            }
            if (source) {
                const QPointF delta = overCenter - source->rect.center();
                const QPointF travel(x - source->rect.center().x(), y - source->rect.center().y());
                const double span = delta.x() * delta.x() + delta.y() * delta.y();
                const double along = travel.x() * delta.x() + travel.y() * delta.y();
                if (span > 1.0 && along > span * 0.55) {
                    moveChild(sourceId, from, to);
                    relayout(screen);
                    screen.hoverUid = overNested ? QStringLiteral("nest:") + overStack : QStringLiteral("cell:") + overApp;
                    screen.mergeReady = false;
                    return;
                }
            }
        }
        const QString mark = overNested ? QStringLiteral("nest:") + overStack : QStringLiteral("cell:") + overApp;
        if (screen.hoverUid != mark) {
            screen.hoverUid = mark;
            screen.hoverSince = now_;
        }
        screen.mergeReady = false;
        return;
    }
    if (!dragStack.isEmpty()) {
        if (dragStack == targetId) {
            clear();
            return;
        }
        if (const Item *drag = findStack(dragStack)) {
            if (containsStack(*drag, targetId)) {
                clear();
                return;
            }
        }
    }
    if (over) {
        const bool overNested = over->nested;
        int index = childIndex(*target, overNested ? QString() : over->appId, overNested ? over->stackId : QString());
        if (index >= 0 && x > over->rect.center().x()) index += 1;
        screen.dropIndex = index;
    }
    const QString mark = QStringLiteral("stack:") + targetId;
    if (screen.hoverUid != mark) {
        screen.hoverUid = mark;
        screen.hoverSince = now_;
    }
    screen.mergeReady = true;
}

void Dock::pullNested(Screen &screen, const QString &stackId) {
    const QString parentId = screen.pressSide && !screen.shownSide.isEmpty() ? screen.shownSide : screen.shownStack;
    const QString up = parentStackId(parentId);
    Item *parent = findStack(parentId);
    if (!parent) return;
    Item moved;
    bool found = false;
    for (auto it = parent->children.begin(); it != parent->children.end(); ++it) {
        if (it->kind == Item::Kind::Stack && it->id == stackId) {
            moved = *it;
            parent->children.erase(it);
            found = true;
            break;
        }
    }
    if (!found) return;
    if (parent->children.empty()) {
        int index = -1;
        std::vector<Item> *home = childList(parentId, &index);
        if (!home || index < 0) return;
        const bool top = home == &items_;
        home->erase(home->begin() + index);
        if (top) {
            items_.insert(items_.begin() + std::min(index, static_cast<int>(items_.size())), moved);
            screen.openStack.clear();
            screen.shownStack.clear();
        } else {
            items_.push_back(moved);
            screen.openStack = up;
        }
    } else {
        const int at = itemIndex(QStringLiteral("stack:") + topStackId(parentId));
        const int insertAt = at < 0 ? static_cast<int>(items_.size()) : at + 1;
        items_.insert(items_.begin() + insertAt, moved);
    }
    screen.menu = false;
    closeStackIfGone(screen);
    save();
}

void Dock::unpinApp(Screen &screen, const QString &id) {
    const int pin = itemIndex(QStringLiteral("app:") + id);
    if (pin >= 0) items_.erase(items_.begin() + pin);
    stripApp(items_, id);
    closeStackIfGone(screen);
    save();
}

void Dock::beginRename(Screen &screen, const QString &stackId) {
    const QString id = !stackId.isEmpty() ? stackId : (!screen.shownStack.isEmpty() ? screen.shownStack : screen.openStack);
    const Item *stack = findStack(id);
    if (!stack) return;
    screen.rename = true;
    screen.renameStack = stack->id;
    screen.renameText = stack->name;
    screen.caret = stack->name.size();
    screen.renameSelectAll = true;
    screen.menu = false;
    screen.heldKey = -1;
}

QString Dock::desktopDirectory() const {
    if (!desktopDirForTest_.isEmpty()) return desktopDirForTest_;
    const QString home = QDir::homePath();
    QString dir = home + QStringLiteral("/Desktop");
    QFile file(home + QStringLiteral("/.config/user-dirs.dirs"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (!line.startsWith(QLatin1String("XDG_DESKTOP_DIR"))) continue;
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq < 0) break;
            QString value = line.mid(eq + 1).trimmed();
            if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                value = value.mid(1, value.size() - 2);
            value.replace(QStringLiteral("$HOME"), home);
            value.replace(QStringLiteral("${HOME}"), home);
            if (!value.isEmpty()) dir = value;
            break;
        }
    }
    QDir().mkpath(dir);
    return dir;
}

void Dock::sendToDesktop(const QString &appId) {
    const QString id = canonical(appId);
    if (id.isEmpty()) return;
    const DesktopApp *app = apps_->byId(id);
    QString base = id;
    base.replace(QLatin1Char('/'), QLatin1Char('-'));
    const QString dest = desktopDirectory() + QLatin1Char('/') + base + QStringLiteral(".desktop");

    QByteArray body;
    if (app && !app->path.isEmpty()) {
        QFile in(app->path);
        if (in.open(QIODevice::ReadOnly)) body = in.readAll();
    }
    if (!body.isEmpty()) {
        QStringList kept;
        const QStringList lines = QString::fromUtf8(body).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (!trimmed.startsWith(QLatin1Char('#')) &&
                (trimmed.startsWith(QLatin1String("NoDisplay="), Qt::CaseInsensitive) ||
                 trimmed.startsWith(QLatin1String("Hidden="), Qt::CaseInsensitive))) {
                continue;
            }
            kept << line;
        }
        body = kept.join(QLatin1Char('\n')).toUtf8();
    } else {
        QString name = itemName(id);
        name.replace(QLatin1Char('\n'), QLatin1Char(' '));
        QString exec;
        QString icon;
        QString wmClass;
        bool rawPath = false;
        if (app) {
            exec = app->exec;
            icon = app->icon;
            wmClass = app->wmClass;
        }
        if (exec.isEmpty() && execs_.contains(id)) {
            exec = execs_.value(id);
            rawPath = true;
        }
        if (exec.isEmpty()) {
            exec = apps_->programFor(id);
            rawPath = !exec.isEmpty();
        }
        if (name.isEmpty() || exec.isEmpty()) {
            std::fprintf(stderr, "magic-dock: no launcher to put %s on the desktop\n", qPrintable(id));
            return;
        }
        // A remembered program is a bare path. A desktop Exec line is already formatted.
        if (rawPath && exec.contains(QLatin1Char(' ')) && !exec.startsWith(QLatin1Char('"')))
            exec = QLatin1Char('"') + exec + QLatin1Char('"');
        QString text = QStringLiteral("[Desktop Entry]\nType=Application\nVersion=1.0\nName=%1\nExec=%2\nTerminal=false\nStartupNotify=true\n")
                           .arg(desktopValue(name), desktopValue(exec));
        if (!icon.isEmpty()) text += QStringLiteral("Icon=%1\n").arg(desktopValue(icon));
        if (!wmClass.isEmpty()) text += QStringLiteral("StartupWMClass=%1\n").arg(desktopValue(wmClass));
        body = text.toUtf8();
    }
    if (!body.endsWith('\n')) body += '\n';
    QFile out(dest);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(body) != body.size()) {
        std::fprintf(stderr, "magic-dock: could not write %s\n", qPrintable(dest));
        return;
    }
    out.close();
    QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                    QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                    QFileDevice::ExeOther);
    // COSMIC shows an executable .desktop file. GNOME also wants this trust bit.
    if (desktopDirForTest_.isEmpty()) {
        const QString gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
        if (!gio.isEmpty())
            startDetachedApp(gio, {QStringLiteral("set"), dest, QStringLiteral("metadata::trusted"), QStringLiteral("true")},
                             QDir::homePath(), "desktop trust");
    }
}

void Dock::commitRename(Screen &screen, bool apply) {
    if (apply) {
        const QString name = screen.renameText.trimmed().left(32);
        if (!name.isEmpty()) {
            if (Item *stack = findStack(screen.renameStack)) stack->name = name;
            save();
        }
    }
    screen.rename = false;
    screen.renameSelectAll = false;
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
    if (code == KEY_BACKSPACE || code == KEY_DELETE) {
        if (screen.renameSelectAll) {
            screen.renameText.clear();
            screen.caret = 0;
            screen.renameSelectAll = false;
            return;
        }
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
        screen.renameSelectAll = false;
        screen.caret = std::max(0, screen.caret - 1);
        return;
    }
    if (code == KEY_RIGHT) {
        screen.renameSelectAll = false;
        screen.caret = std::min(static_cast<int>(screen.renameText.size()), screen.caret + 1);
        return;
    }
    if (code == KEY_HOME) {
        screen.renameSelectAll = false;
        screen.caret = 0;
        return;
    }
    if (code == KEY_END) {
        screen.renameSelectAll = false;
        screen.caret = screen.renameText.size();
        return;
    }
    const QString text = keyChar(static_cast<quint32>(code), shift);
    if (text.isEmpty() || (!screen.renameSelectAll && screen.renameText.size() >= 32)) return;
    if (screen.renameSelectAll) {
        screen.renameText = text.left(32);
        screen.caret = screen.renameText.size();
        screen.renameSelectAll = false;
        return;
    }
    if (screen.renameText.size() >= 32) return;
    screen.renameText.insert(screen.caret, text);
    screen.caret += text.size();
}

void Dock::layoutPopup(Screen &screen) {
    screen.cells.clear();
    screen.sideCells.clear();
    screen.stackWindows.clear();
    screen.sideWindows.clear();
    screen.popupRect = QRectF();
    screen.popupClose = QRectF();
    screen.titleRect = QRectF();
    screen.stackBack = QRectF();
    screen.stackManage = QRectF();
    screen.sideRect = QRectF();
    screen.sideClose = QRectF();
    screen.sideTitle = QRectF();
    screen.sideManage = QRectF();
    const QString stackId = !screen.openStack.isEmpty() ? screen.openStack : screen.shownStack;
    if (stackId.isEmpty() || (screen.openStack.isEmpty() && screen.popup < 0.04)) return;
    const Item *stack = findStack(stackId);
    if (!stack || stack->children.empty()) return;
    QString sideId = !screen.sideStack.isEmpty() ? screen.sideStack : screen.shownSide;
    if (!sideId.isEmpty() && (sideId == stackId || !containsStack(*stack, sideId) || !findStack(sideId))) {
        sideId.clear();
        screen.sideStack.clear();
        screen.shownSide.clear();
    }
    const Item *side = sideId.isEmpty() ? nullptr : findStack(sideId);
    if (side && side->children.empty()) side = nullptr;

    constexpr double cellW = 98;
    constexpr double cellH = 96;
    constexpr double pad = 14;
    constexpr double header = 36;
    constexpr double bottomPad = 18;
    constexpr double winGap = 8;
    const QRectF room = popupRoom(screen);
    struct Listed {
        std::vector<const Live *> windows;
        bool mixed = false;
    };
    auto windowsOf = [&](const Item &item) {
        Listed listed;
        QSet<QString> apps;
        for (const QString &app : appsIn(item)) {
            for (const Live &live : live_) {
                if (!isApp(live, app)) continue;
                listed.windows.push_back(&live);
                apps.insert(canonical(app));
            }
        }
        std::sort(listed.windows.begin(), listed.windows.end(), [](const Live *a, const Live *b) {
            if (a->appId != b->appId) return a->appId < b->appId;
            return a->key < b->key;
        });
        listed.mixed = apps.size() > 1;
        return listed;
    };
    struct Plan {
        double width = 0;
        double height = 0;
        double cardW = 156;
        double cardH = 104;
        int cols = 1;
        int winCols = 0;
        int winRows = 0;
        double winBlock = 0;
    };
    auto planFor = [&](const Item &item, double maxW) {
        Plan plan;
        const int count = static_cast<int>(item.children.size());
        plan.cols = count <= 1 ? 1 : count <= 4 ? 2 : count <= 9 ? 3 : 4;
        const int iconRows = std::max(1, (count + plan.cols - 1) / plan.cols);
        const double iconsW = plan.cols * cellW + pad * 2;
        const double iconsH = iconRows * cellH;
        const Listed listed = windowsOf(item);
        const int winCount = static_cast<int>(listed.windows.size());
        plan.width = iconsW;
        plan.height = header + iconsH + bottomPad;
        if (winCount <= 0) return plan;
        plan.winCols = std::min(winCount, 3);
        const double twoRows = header + 20 + 2 * plan.cardH + winGap + 8 + iconsH + bottomPad;
        if (winCount > 3 && twoRows > room.height()) plan.winCols = std::min(winCount, 4);
        double strip = plan.winCols * plan.cardW + (plan.winCols - 1) * winGap + pad * 2;
        if (strip > maxW) {
            plan.cardW = std::max(108.0, (maxW - pad * 2 - (plan.winCols - 1) * winGap) / std::max(1, plan.winCols));
            strip = plan.winCols * plan.cardW + (plan.winCols - 1) * winGap + pad * 2;
        }
        plan.winRows = (winCount + plan.winCols - 1) / plan.winCols;
        plan.winBlock = 20 + plan.winRows * plan.cardH + (plan.winRows - 1) * winGap + 8;
        plan.width = std::max(iconsW, strip);
        plan.height = header + plan.winBlock + iconsH + bottomPad;
        if (plan.height > room.height() && plan.winRows > 0 && plan.cardH > 76) {
            const double shrink = std::min(plan.cardH - 76.0, (plan.height - room.height()) / plan.winRows);
            plan.cardH -= shrink;
            plan.winBlock -= shrink * plan.winRows;
            plan.height -= shrink * plan.winRows;
        }
        return plan;
    };
    auto fill = [&](const Item &item, const Plan &plan, const QRectF &rect, bool nested, QRectF &title, QRectF &back,
                    QRectF &manage, QRectF &close, std::vector<Screen::Cell> &cells, std::vector<Screen::Card> &cards) {
        const int count = static_cast<int>(item.children.size());
        const double x = rect.x();
        const double y = rect.y();
        if (nested) back = QRectF(x + 8, y + 7, 24, 22);
        else back = QRectF();
        close = closeBox(rect);
        manage = QRectF(close.left() - 6 - 24, y + 7, 24, 22);
        const double titleLeft = nested ? 36.0 : 8.0;
        const double titleRight = manage.left() - 4;
        title = QRectF(x + titleLeft, y + 4, std::max(24.0, titleRight - (x + titleLeft)), 28);
        cards.clear();
        if (plan.winCols > 0) {
            const Listed listed = windowsOf(item);
            const double top = y + header + 20;
            for (int i = 0; i < static_cast<int>(listed.windows.size()); ++i) {
                const Live *live = listed.windows[static_cast<size_t>(i)];
                Screen::Card card;
                card.key = live->key;
                card.appId = live->appId;
                const QString name = itemName(live->appId);
                const QString windowTitle = live->title.isEmpty() ? name : live->title;
                card.title = listed.mixed && windowTitle != name ? name + QStringLiteral(" · ") + windowTitle : windowTitle;
                card.active = live->activated && !live->minimized;
                card.minimized = live->minimized;
                const int column = i % plan.winCols;
                const int row = i / plan.winCols;
                card.rect = QRectF(x + pad + column * (plan.cardW + winGap), top + row * (plan.cardH + winGap), plan.cardW,
                                   plan.cardH);
                card.thumb = card.rect.adjusted(7, 7, -7, -24);
                card.closeRect = QRectF(card.thumb.right() - 22, card.thumb.top() + 4, 20, 20);
                cards.push_back(card);
            }
        }
        const double cellsTop = y + header + plan.winBlock;
        for (int i = 0; i < count; ++i) {
            const Item &child = item.children[static_cast<size_t>(i)];
            Screen::Cell cell;
            cell.nested = child.kind == Item::Kind::Stack;
            if (cell.nested) {
                cell.stackId = child.id;
                cell.label = child.name.isEmpty() ? QStringLiteral("Stack") : child.name;
            } else {
                cell.appId = child.id;
                cell.label = itemName(child.id);
            }
            const int column = i % plan.cols;
            const int row = i / plan.cols;
            cell.rect = QRectF(x + pad + column * cellW, cellsTop + row * cellH, cellW, cellH);
            const double icon = 44;
            cell.icon = QRectF(cell.rect.center().x() - icon / 2.0, cell.rect.top() + 2, icon, icon);
            cells.push_back(cell);
        }
    };

    const bool hasSide = side != nullptr;
    const double sideMax = hasSide ? std::max(180.0, (room.width() - 12.0) * 0.42) : room.width();
    const double mainMax = hasSide ? std::max(220.0, room.width() - 12.0 - std::min(sideMax, 360.0)) : room.width();
    const Plan mainPlan = planFor(*stack, mainMax);
    const QString rootId = topStackId(stackId);
    QPointF anchor(screen.w / 2.0, screen.pill.top());
    for (const Slot &slot : screen.slots) {
        if (slot.uid == QLatin1String("stack:") + rootId) anchor = slot.rect.center();
    }
    QRectF mainRect = popupNear(screen, anchor, mainPlan.width, mainPlan.height);
    QRectF sideRect;
    Plan sidePlan;
    if (side) {
        sidePlan = planFor(*side, sideMax);
        const double gap = 12;
        const double roomRight = screen.w - kScreenMargin - (mainRect.right() + gap);
        const double roomLeft = mainRect.left() - kScreenMargin - gap;
        const bool onRight = roomRight >= sidePlan.width || roomRight >= roomLeft;
        if (onRight) {
            double x = mainRect.right() + gap;
            if (x + sidePlan.width > screen.w - kScreenMargin) {
                mainRect.translate(-(x + sidePlan.width - (screen.w - kScreenMargin)), 0);
                x = mainRect.right() + gap;
            }
            if (mainRect.left() < kScreenMargin) {
                const double fix = kScreenMargin - mainRect.left();
                mainRect.translate(fix, 0);
                x = mainRect.right() + gap;
            }
            sideRect = QRectF(x, mainRect.top(), sidePlan.width, sidePlan.height);
        } else {
            double x = mainRect.left() - gap - sidePlan.width;
            if (x < kScreenMargin) {
                mainRect.translate(kScreenMargin - x, 0);
                x = mainRect.left() - gap - sidePlan.width;
            }
            sideRect = QRectF(x, mainRect.top(), sidePlan.width, sidePlan.height);
        }
        if (edge_ == ShellSpec::Edge::Bottom) sideRect.moveBottom(std::min(mainRect.bottom(), room.bottom()));
        if (edge_ == ShellSpec::Edge::Top) sideRect.moveTop(std::max(mainRect.top(), room.top()));
        if (sideRect.left() < room.left()) sideRect.moveLeft(room.left());
        if (sideRect.right() > room.right()) sideRect.moveRight(room.right());
        if (sideRect.top() < room.top()) sideRect.moveTop(room.top());
        if (sideRect.bottom() > room.bottom()) sideRect.moveBottom(room.bottom());
    }
    if (mainRect.bottom() > room.bottom()) mainRect.moveBottom(room.bottom());
    if (mainRect.top() < room.top()) mainRect.moveTop(room.top());
    QRectF unusedBack;
    fill(*stack, mainPlan, mainRect, !parentStackId(stackId).isEmpty(), screen.titleRect, screen.stackBack,
         screen.stackManage, screen.popupClose, screen.cells, screen.stackWindows);
    screen.popupRect = mainRect;
    if (side) {
        fill(*side, sidePlan, sideRect, false, screen.sideTitle, unusedBack, screen.sideManage, screen.sideClose,
             screen.sideCells, screen.sideWindows);
        screen.sideRect = sideRect;
    }
}

void Dock::layoutPreviews(Screen &screen) {
    screen.cards.clear();
    screen.previewsRect = QRectF();
    screen.previewsClose = QRectF();
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
    const double head = 28;
    const QRectF room = popupRoom(screen);
    double width = cols * cardW + (cols - 1) * gap + pad * 2;
    double height = head + rows * cardH + (rows - 1) * gap + pad * 2 + 8;
    const double maxW = std::max(220.0, room.width());
    if (width > maxW && width > 1) {
        const double scale = maxW / width;
        cardW *= scale;
        cardH = std::max(78.0, cardH * scale);
        width = cols * cardW + (cols - 1) * gap + pad * 2;
        height = head + rows * cardH + (rows - 1) * gap + pad * 2 + 8;
    }
    if (height > room.height() && cardH > 78) {
        const double shrink = std::min(cardH - 78.0, (height - room.height()) / std::max(1, rows));
        cardH -= shrink;
        height -= shrink * rows;
    }
    QPointF anchor(screen.w / 2.0, screen.pill.center().y());
    for (const Slot &slot : screen.slots) {
        if (slot.kind == Item::Kind::App && slot.appId == appId) anchor = slot.rect.center();
    }
    for (const Slot &slot : screen.slots) {
        if (slot.kind != Item::Kind::Stack) continue;
        const int index = itemIndex(slot.uid);
        if (index >= 0 && holdsApp(items_[static_cast<size_t>(index)], appId)) anchor = slot.rect.center();
    }
    screen.previewsRect = popupNear(screen, anchor, width, height);
    screen.previewsClose = closeBox(screen.previewsRect);
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
        card.rect = QRectF(x + pad + column * (cardW + gap), y + pad + head + row * (cardH + gap), cardW, cardH);
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
    screen.sideStack.clear();
    screen.openPreviews.clear();
}

void Dock::layoutSearch(Screen &screen) {
    screen.searchHits.clear();
    screen.searchRect = QRectF();
    screen.searchClose = QRectF();
    screen.searchField = QRectF();
    if (!screen.search) return;
    const std::vector<AppHit> matches = apps_->search(screen.searchText, 8);
    const double rowH = 40;
    const double fieldH = 40;
    const double pad = 8;
    const double width = 320;
    const int rows = std::max(1, static_cast<int>(matches.size()));
    const double height = pad + fieldH + 8 + rows * rowH + pad;
    QPointF anchor(screen.pill.center());
    for (const Slot &slot : screen.slots) {
        if (slot.uid == QLatin1String("search")) anchor = slot.rect.center();
    }
    screen.searchRect = popupNear(screen, anchor, width, height);
    screen.searchClose = closeBox(screen.searchRect);
    const double x = screen.searchRect.x();
    const double y = screen.searchRect.y();
    screen.searchField = QRectF(x + pad, y + pad, width - pad * 2 - (kCloseInset + kCloseSize), fieldH);
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
        hit.icon = QRectF(hit.rect.left() + 8, hit.rect.center().y() - 12, 24, 24);
        screen.searchHits.push_back(hit);
    }
}

void Dock::layoutAbout(Screen &screen) {
    screen.aboutRect = QRectF();
    screen.aboutClose = QRectF();
    screen.aboutCoffee = QRectF();
    if (!screen.about) return;
    const double width = 320;
    const double height = 188;
    QPointF anchor(screen.pill.center());
    for (const Slot &slot : screen.slots) {
        if (slot.uid == QLatin1String("overflow")) anchor = slot.rect.center();
    }
    screen.aboutRect = popupNear(screen, anchor, width, height);
    screen.aboutClose = closeBox(screen.aboutRect);
    screen.aboutCoffee = QRectF(screen.aboutRect.left() + 14, screen.aboutRect.bottom() - 50, width - 28, 36);
}

void Dock::layoutMenu(Screen &screen) {
    screen.menuRect = QRectF();
    screen.menuClose = QRectF();
    screen.menuRows.clear();
    screen.subRect = QRectF();
    screen.subClose = QRectF();
    screen.subRows.clear();
    if (!screen.menu || screen.rows.empty()) return;
    QFont menuFont(QStringLiteral("Fira Sans"));
    menuFont.setPixelSize(15);
    const QFontMetrics menuMetrics(menuFont);
    double width = 210;
    for (const MenuRow &row : screen.rows) {
        const bool plain = row.header || row.action == QLatin1String("-");
        double need = menuMetrics.horizontalAdvance(row.label) + (plain ? 36 : 68);
        if (row.toggle) need += 4;
        if (menuContinues(row)) need += 18;
        width = std::max(width, need);
    }
    if (screen.w > 80) width = std::min(width, screen.w - kScreenMargin * 2);
    double rowH = 36;
    const double pad = 14;
    const double band = 28;
    auto menuHeight = [&](double rows) {
        double measured = pad + band;
        for (const MenuRow &row : screen.rows) measured += row.action == QLatin1String("-") ? 9.0 : rows;
        return measured;
    };
    double height = menuHeight(rowH);
    const QRectF room = popupRoom(screen);
    if (height > room.height() && room.height() > band + pad + 28.0) {
        int stretch = 0;
        for (const MenuRow &row : screen.rows)
            if (row.action != QLatin1String("-")) stretch++;
        if (stretch > 0) {
            const double fixed = height - stretch * rowH;
            rowH = std::clamp((room.height() - fixed) / stretch, 28.0, 36.0);
            height = menuHeight(rowH);
        }
    }
    QPointF anchor(screen.pill.center());
    if (!screen.menuApp.isEmpty()) {
        for (const Screen::Cell &cell : screen.cells) {
            if (cell.appId == screen.menuApp) anchor = cell.rect.center();
        }
        for (const Screen::Cell &cell : screen.sideCells) {
            if (cell.appId == screen.menuApp) anchor = cell.rect.center();
        }
    }
    for (const Slot &slot : screen.slots) {
        if (slot.uid == screen.menuUid) anchor = slot.rect.center();
    }
    screen.menuRect = popupNear(screen, anchor, width, height);
    screen.menuClose = closeBox(screen.menuRect);
    const double x = screen.menuRect.x();
    const double y = screen.menuRect.y();
    double cursor = y + band;
    for (const MenuRow &row : screen.rows) {
        const double h = row.action == QLatin1String("-") ? 9 : rowH;
        screen.menuRows.push_back(QRectF(x + 4, cursor, width - 8, h));
        cursor += h;
    }
    if (screen.menuRow >= 0 && screen.menuRow < static_cast<int>(screen.rows.size()) &&
        !screen.rows[static_cast<size_t>(screen.menuRow)].sub.empty()) {
        const MenuRow &parent = screen.rows[static_cast<size_t>(screen.menuRow)];
        const double subH = pad + band + parent.sub.size() * rowH;
        double subW = 220;
        for (const MenuRow &child : parent.sub) {
            double need = menuMetrics.horizontalAdvance(child.label) + 68.0;
            if (menuContinues(child)) need += 18;
            subW = std::max(subW, need);
        }
        if (screen.w > 80) subW = std::min(subW, screen.w - kScreenMargin * 2);
        double sx = screen.menuRect.right() + 6;
        if (sx + subW > screen.w - kScreenMargin) sx = screen.menuRect.left() - 6 - subW;
        if (sx < kScreenMargin) sx = kScreenMargin;
        double sy = screen.menuRows[static_cast<size_t>(screen.menuRow)].top() - band;
        sy = std::clamp(sy, kScreenMargin, std::max(kScreenMargin, screen.h - subH - kScreenMargin));
        screen.subRect = QRectF(sx, sy, subW, subH);
        screen.subClose = closeBox(screen.subRect);
        for (int i = 0; i < static_cast<int>(parent.sub.size()); ++i) {
            screen.subRows.push_back(QRectF(sx + 4, sy + band + i * rowH, subW - 8, rowH));
        }
    }
}

QString Dock::trashFilesDir() const {
    const QByteArray xdg = qgetenv("XDG_DATA_HOME");
    const QString root = xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : QString::fromLocal8Bit(xdg);
    return root + QStringLiteral("/Trash/files");
}

int countTrashFiles(const QString &dirPath) {
    DIR *dir = ::opendir(dirPath.toLocal8Bit().constData());
    if (!dir) return 0;
    int count = 0;
    while (const dirent *ent = ::readdir(dir)) {
        const char *name = ent->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        count++;
    }
    ::closedir(dir);
    return count;
}

// /proc/mounts encodes a space as \040. The other escaped bytes are tab, newline, and backslash.
QByteArray unescapeMount(const QByteArray &raw) {
    QByteArray out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        if (raw.at(i) == '\\' && i + 3 < raw.size()) {
            const char a = raw.at(i + 1);
            const char b = raw.at(i + 2);
            const char c = raw.at(i + 3);
            if (a >= '0' && a <= '7' && b >= '0' && b <= '7' && c >= '0' && c <= '7') {
                out.append(static_cast<char>(((a - '0') << 6) | ((b - '0') << 3) | (c - '0')));
                i += 3;
                continue;
            }
        }
        out.append(raw.at(i));
    }
    return out;
}

bool virtualTrashFs(const QByteArray &type) {
    static const char *skip[] = {"proc",        "sysfs",        "tmpfs",      "devtmpfs",
                                 "devpts",      "cgroup",       "cgroup2",    "autofs",
                                 "debugfs",     "bpf",          "securityfs", "pstore",
                                 "tracefs",     "fusectl",      "configfs",   "rpc_pipefs",
                                 "nfsd",        "overlay",      "nsfs",       "efivarfs",
                                 "ramfs",       "hugetlbfs",    "mqueue",     "binfmt_misc",
                                 "squashfs",    "functionfs",   "binder",     "none"};
    for (const char *name : skip) {
        if (type == name) return true;
    }
    return type.startsWith("fuse.gvfs") || type.startsWith("fuse.portal");
}

bool networkTrashFs(const QByteArray &type) {
    return type == "nfs" || type == "nfs4" || type == "cifs" || type == "smb" || type == "smb3" || type == "smbfs" ||
           type.startsWith("fuse.ssh");
}

// The spec ignores $topdir/.Trash unless it is a real directory with the sticky bit.
bool stickyTrashDir(const QByteArray &path) {
    struct stat st {};
    if (::lstat(path.constData(), &st) != 0) return false;
    return S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX);
}

quint64 readField(const QByteArray &data, const char *key) {
    const int at = data.indexOf(key);
    if (at < 0) return 0;
    int i = at + static_cast<int>(std::strlen(key));
    while (i < data.size() && (data.at(i) == ' ' || data.at(i) == '\t' || data.at(i) == ':')) i++;
    quint64 value = 0;
    while (i < data.size() && data.at(i) >= '0' && data.at(i) <= '9') {
        value = value * 10 + static_cast<quint64>(data.at(i) - '0');
        i++;
    }
    return value;
}

bool Dock::pollMeters() {
    bool changed = false;
    const qint64 trashEvery = now_ < trashFastUntil_ ? 400 : 1500;
    if (trashPoll_ == 0 || now_ - trashPoll_ >= trashEvery) {
        trashPoll_ = now_;
        refreshTrashDirs();
        int count = 0;
        for (const QString &dir : trashDirs_) count += countTrashFiles(dir);
        if (count != trashCount_) {
            trashCount_ = count;
            changed = true;
        }
    }
    if (usagePoll_ != 0 && now_ - usagePoll_ < 1000) return changed;
    usagePoll_ = now_;

    quint64 idle = 0;
    quint64 total = 0;
    QFile statFile(QStringLiteral("/proc/stat"));
    if (statFile.open(QIODevice::ReadOnly)) {
        const QByteArray line = statFile.readLine();
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() >= 5 && parts.first() == "cpu") {
            for (int i = 1; i < parts.size(); ++i) total += parts.at(i).toULongLong();
            idle = parts.at(4).toULongLong();
            if (parts.size() > 5) idle += parts.at(5).toULongLong();
        }
    }
    if (total > cpuTotalPrev_ && cpuTotalPrev_ > 0) {
        const quint64 totalDelta = total - cpuTotalPrev_;
        const quint64 idleDelta = idle >= cpuIdlePrev_ ? idle - cpuIdlePrev_ : 0;
        const double next = totalDelta == 0 ? cpu_ : 1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta);
        const double clamped = std::clamp(next, 0.0, 1.0);
        if (qRound(clamped * 100.0) != qRound(cpu_ * 100.0) || !cpuReady_) changed = true;
        cpu_ = clamped;
        cpuReady_ = true;
    }
    if (total > 0) {
        cpuIdlePrev_ = idle;
        cpuTotalPrev_ = total;
    }

    QFile memFile(QStringLiteral("/proc/meminfo"));
    if (memFile.open(QIODevice::ReadOnly)) {
        const QByteArray data = memFile.read(4096);
        const quint64 totalKb = readField(data, "MemTotal:");
        const quint64 availKb = readField(data, "MemAvailable:");
        if (totalKb > 0 && data.contains("MemAvailable:")) {
            const quint64 usedKb = totalKb > availKb ? totalKb - availKb : 0;
            const quint64 used = usedKb * 1024;
            const quint64 all = totalKb * 1024;
            const double fraction = std::clamp(static_cast<double>(usedKb) / static_cast<double>(totalKb), 0.0, 1.0);
            if (qRound(fraction * 100.0) != qRound(mem_ * 100.0) || memTotal_ == 0) changed = true;
            mem_ = fraction;
            memUsed_ = used;
            memTotal_ = all;
        }
    }
    return changed;
}

void Dock::applyCreditBytes(const QByteArray &bytes) {
    const int start = bytes.indexOf('{');
    const int end = bytes.lastIndexOf('}');
    if (start < 0 || end <= start) return;
    const QJsonObject obj = QJsonDocument::fromJson(bytes.mid(start, end - start + 1)).object();
    if (obj.isEmpty()) return;
    const QJsonObject report = obj.contains(QStringLiteral("report")) ? obj.value(QStringLiteral("report")).toObject() : obj;
    const QJsonValue remaining = report.value(QStringLiteral("remaining"));
    const bool ok = report.value(QStringLiteral("ok")).toBool(false) && remaining.isDouble();
    const double left = remaining.toDouble(-1);
    if (!ok || left < 0 || left > 100) {
        creditOk_ = false;
        creditLeft_ = -1;
        const QString line = report.value(QStringLiteral("line")).toString();
        creditLine_ = line.isEmpty() ? QStringLiteral("Credits did not answer.") : line;
    } else {
        creditOk_ = true;
        creditLeft_ = qRound(left);
        creditLine_ = report.value(QStringLiteral("line")).toString();
        if (creditLine_.isEmpty()) creditLine_ = QStringLiteral("%1% left").arg(creditLeft_);
    }
    creditReset_ = report.value(QStringLiteral("reset")).toString();
    creditDetail_ = report.value(QStringLiteral("detail")).toString();
    creditPrepaid_ = report.value(QStringLiteral("prepaid")).toString();
    creditProducts_.clear();
    for (const QJsonValue &row : report.value(QStringLiteral("products")).toArray()) {
        const QJsonObject product = row.toObject();
        const QString name = product.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || !product.value(QStringLiteral("used")).isDouble()) continue;
        creditProducts_ << QStringLiteral("%1 %2%").arg(name).arg(qRound(product.value(QStringLiteral("used")).toDouble()));
    }
    if (creditDetail_.isEmpty() && !creditProducts_.isEmpty()) creditDetail_ = creditProducts_.join(QStringLiteral(" · "));
    grokDirty_ = true;
}

void Dock::applyBotBytes(const QByteArray &bytes) {
    const int start = bytes.indexOf('{');
    const int end = bytes.lastIndexOf('}');
    if (start < 0 || end <= start) return;
    const QJsonObject obj = QJsonDocument::fromJson(bytes.mid(start, end - start + 1)).object();
    if (obj.isEmpty()) return;
    QString writer = obj.value(QStringLiteral("writer")).toString().trimmed();
    if (writer != QLatin1String("up") && writer != QLatin1String("down")) writer.clear();
    QList<GrokBot> next;
    QStringList running;
    for (const QJsonValue &row : obj.value(QStringLiteral("bots")).toArray()) {
        const QJsonObject bot = row.toObject();
        GrokBot item;
        item.name = bot.value(QStringLiteral("name")).toString().trimmed().left(40);
        item.role = bot.value(QStringLiteral("role")).toString().trimmed().left(40);
        item.note = bot.value(QStringLiteral("note")).toString().simplified().left(160);
        item.running = bot.value(QStringLiteral("running")).toBool();
        if (item.name.isEmpty()) continue;
        next.push_back(item);
        if (item.running && !running.contains(item.name)) running << item.name;
    }
    const QString notice = obj.value(QStringLiteral("notice")).toString();
    if (running.isEmpty() && notice.startsWith(QLatin1String("Still running:"))) {
        QString list = notice.mid(QStringLiteral("Still running:").size()).trimmed();
        if (list.endsWith(QLatin1Char('.'))) list.chop(1);
        for (const QString &name : list.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString clean = name.trimmed();
            if (clean.isEmpty() || running.contains(clean)) continue;
            running << clean;
            bool known = false;
            for (GrokBot &item : next) {
                if (item.name != clean) continue;
                item.running = true;
                known = true;
            }
            if (!known) {
                GrokBot item;
                item.name = clean;
                item.running = true;
                next.push_back(item);
            }
        }
    }
    auto sameBot = [](const GrokBot &a, const GrokBot &b) {
        return a.name == b.name && a.role == b.role && a.note == b.note && a.running == b.running;
    };
    bool same = writer == grokWriter_ && next.size() == grokBots_.size() && running == runningBots_;
    if (same) {
        for (int i = 0; i < next.size(); ++i) {
            if (!sameBot(next.at(i), grokBots_.at(i))) same = false;
        }
    }
    botsKnown_ = true;
    if (!same) {
        grokWriter_ = writer;
        grokBots_ = next;
        runningBots_ = running;
        grokDirty_ = true;
    }
}

void Dock::readCreditCache() {
    const QString override = qEnvironmentVariable("HEARTH_CREDITS_FILE");
    const QString path = override.isEmpty()
                             ? QDir::homePath() + QStringLiteral("/.local/state/thewhitewitch/hearth/credits.json")
                             : override;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    applyCreditBytes(file.readAll());
}

QString Dock::resolveGrogy() const {
    const QString override = qEnvironmentVariable("MAGIC_DOCK_GROGY");
    if (!override.isEmpty()) {
        const QFileInfo info(override);
        return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
    }
    QStringList candidates;
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("grogy"));
    if (!onPath.isEmpty()) candidates << onPath;
    if (!grogyPath_.isEmpty()) candidates << grogyPath_;
    const QString home = QDir::homePath();
    candidates << home + QStringLiteral("/.local/bin/grogy") << home + QStringLiteral("/Projects/grogy/grogy");
    QSet<QString> seen;
    for (const QString &candidate : candidates) {
        if (candidate.isEmpty() || seen.contains(candidate)) continue;
        seen.insert(candidate);
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) return info.absoluteFilePath();
    }
    return {};
}

bool Dock::probeGrokTools() {
    const QString homeBin = QDir::homePath() + QStringLiteral("/.grok/bin/grok");
    const bool installed = QFileInfo(homeBin).isExecutable() ||
                           !QStandardPaths::findExecutable(QStringLiteral("grok")).isEmpty();
    const QString grogy = resolveGrogy();
    const bool onPath = !grogy.isEmpty();
    const bool becameInstalled = installed && !grokInstalled_;
    const bool pathChanged = grogy != grogyPath_;
    const bool changed = installed != grokInstalled_ || onPath != grogyOnPath_ || pathChanged;
    grokInstalled_ = installed;
    grogyOnPath_ = onPath;
    grogyPath_ = grogy;
    if (pathChanged) {
        grogySave_ = true;
        if (onPath) std::fprintf(stderr, "magic-dock: grogy is %s\n", qPrintable(grogy));
    }
    if (becameInstalled) {
        creditPoll_ = 0;
        botPoll_ = 0;
    }
    return changed;
}

void Dock::openGrogy() {
    if (grogyPath_.isEmpty() || !QFileInfo(grogyPath_).isExecutable()) {
        grogyPath_ = resolveGrogy();
        grogyOnPath_ = !grogyPath_.isEmpty();
        if (grogyOnPath_) grogySave_ = true;
    }
    if (grogySave_ && persist_ && loaded_) {
        grogySave_ = false;
        save();
    }
    if (grogyPath_.isEmpty()) {
        grogyOnPath_ = false;
        std::fprintf(stderr, "magic-dock: grogy is not on the path\n");
        return;
    }
    if (startDetachedApp(grogyPath_, {}, QDir::homePath(), "grogy")) noteOpening(QStringLiteral("grogy"));
}

bool Dock::pollGrok() {
    bool changed = false;
    if (grokDirty_) {
        grokDirty_ = false;
        changed = true;
    }
    if (grokProbe_ == 0 || now_ - grokProbe_ >= 5000) {
        grokProbe_ = now_ > 0 ? now_ : 1;
        if (probeGrokTools()) changed = true;
        if (grogySave_ && persist_ && loaded_) {
            grogySave_ = false;
            save();
            changed = true;
        }
    }
    if (!persist_ || !showGrok_ || !grokInstalled_) return changed;
    auto stopIfStuck = [&](QProcess *proc, qint64 started) {
        if (!proc || proc->state() == QProcess::NotRunning || started == 0) return;
        if (now_ - started > 20000) proc->kill();
    };
    stopIfStuck(creditProc_, creditStarted_);
    stopIfStuck(botProc_, botStarted_);

    if (creditPoll_ == 0 || now_ - creditPoll_ >= 20000) {
        creditPoll_ = now_;
        readCreditCache();
        const QString override = qEnvironmentVariable("HEARTH_CREDITS_FILE");
        const QString path = override.isEmpty()
                                 ? QDir::homePath() + QStringLiteral("/.local/state/thewhitewitch/hearth/credits.json")
                                 : override;
        const QFileInfo info(path);
        const bool stale = !info.exists() || info.lastModified().secsTo(QDateTime::currentDateTime()) > 12 * 60;
        const QString program = qEnvironmentVariable("GROGY_CREDITS").isEmpty()
                                    ? QDir::homePath() + QStringLiteral("/Projects/bots/credits.py")
                                    : qEnvironmentVariable("GROGY_CREDITS");
        if (stale && QFileInfo::exists(program) && (!creditProc_ || creditProc_->state() == QProcess::NotRunning)) {
            if (!creditProc_) {
                creditProc_ = new QProcess();
                QObject::connect(creditProc_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), creditProc_,
                                 [this](int, QProcess::ExitStatus) {
                                     applyCreditBytes(creditProc_->readAllStandardOutput());
                                     readCreditCache();
                                 });
            }
            creditStarted_ = now_;
            creditProc_->start(QStringLiteral("python3"), {program, QStringLiteral("--json")});
        }
    }
    if (botPoll_ == 0 || now_ - botPoll_ >= 8000) {
        botPoll_ = now_;
        const QString program = qEnvironmentVariable("GROGY_CREW").isEmpty()
                                    ? QDir::homePath() + QStringLiteral("/Projects/bots/crew.py")
                                    : qEnvironmentVariable("GROGY_CREW");
        if (QFileInfo::exists(program) && (!botProc_ || botProc_->state() == QProcess::NotRunning)) {
            if (!botProc_) {
                botProc_ = new QProcess();
                QObject::connect(botProc_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), botProc_,
                                 [this](int, QProcess::ExitStatus) { applyBotBytes(botProc_->readAllStandardOutput()); });
            }
            botStarted_ = now_;
            botProc_->start(QStringLiteral("python3"), {program, QStringLiteral("--live")});
        }
    }
    if (grokDirty_) {
        grokDirty_ = false;
        changed = true;
    }
    return changed;
}

void Dock::noteTrashChanged() {
    trashPoll_ = 0;
    trashScan_ = 0;
    trashFastUntil_ = now_ + 3200;
}

void Dock::refreshTrashDirs() {
    // Local bins are cheap to look up. Network mounts are checked about every 30 seconds, and again after a drop or
    // an empty, so a stalled share does not stall the dock on every sample.
    const bool discoverNet = trashScan_ == 0 || now_ - trashScan_ >= 30000;
    if (discoverNet) {
        trashScan_ = now_;
        trashNetDirs_.clear();
    }
    QStringList dirs;
    QSet<QString> seen;
    auto add = [&](const QByteArray &files, bool net) {
        char resolved[PATH_MAX];
        if (::realpath(files.constData(), resolved) == nullptr) return;
        const QString canon = QString::fromLocal8Bit(resolved);
        if (seen.contains(canon)) return;
        seen.insert(canon);
        dirs.push_back(canon);
        if (net) trashNetDirs_.push_back(canon);
    };
    add(trashFilesDir().toLocal8Bit(), false);
    if (!discoverNet) {
        for (const QString &dir : trashNetDirs_) add(dir.toLocal8Bit(), false);
    }

    QFile mounts(QStringLiteral("/proc/mounts"));
    if (mounts.open(QIODevice::ReadOnly)) {
        const QByteArray table = mounts.readAll();
        const QByteArray uid = QByteArray::number(static_cast<uint>(::geteuid()));
        int lineStart = 0;
        while (lineStart < table.size()) {
            int lineEnd = table.indexOf('\n', lineStart);
            if (lineEnd < 0) lineEnd = table.size();
            const QByteArray line = table.mid(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;
            if (line.isEmpty()) continue;
            const int sp1 = line.indexOf(' ');
            const int sp2 = sp1 < 0 ? -1 : line.indexOf(' ', sp1 + 1);
            if (sp2 < 0) continue;
            const int sp3 = line.indexOf(' ', sp2 + 1);
            const QByteArray mount = unescapeMount(line.mid(sp1 + 1, sp2 - sp1 - 1));
            const QByteArray type = sp3 < 0 ? line.mid(sp2 + 1) : line.mid(sp2 + 1, sp3 - sp2 - 1);
            if (mount.isEmpty() || mount.at(0) != '/' || virtualTrashFs(type)) continue;
            const bool net = networkTrashFs(type);
            if (net && !discoverNet) continue;
            const QByteArray dot = mount + "/.Trash";
            const QByteArray files = stickyTrashDir(dot) ? dot + "/" + uid + "/files" : mount + "/.Trash-" + uid + "/files";
            add(files, net);
        }
    }
    trashDirs_ = dirs;
}

QStringList Dock::trashBins() const {
    if (!trashDirs_.isEmpty()) return trashDirs_;
    return {trashFilesDir()};
}

void Dock::openTrash() const {
    const QString files = QStandardPaths::findExecutable(QStringLiteral("cosmic-files"));
    if (!files.isEmpty()) {
        startClean(files, {QStringLiteral("--trash")});
        return;
    }
    startClean(QStringLiteral("xdg-open"), {QStringLiteral("trash:///")});
}

void Dock::emptyTrash() {
    const QString gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
    if (!gio.isEmpty()) {
        startDetachedApp(gio, {QStringLiteral("trash"), QStringLiteral("--empty")}, QDir::homePath(), "empty trash");
    } else {
        auto clear = [](const QString &path) {
            QDir dir(path);
            if (!dir.exists()) return;
            const QFileInfoList list = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            for (const QFileInfo &item : list) {
                if (item.isDir() && !item.isSymLink()) QDir(item.absoluteFilePath()).removeRecursively();
                else QFile::remove(item.absoluteFilePath());
            }
        };
        for (const QString &files : trashBins()) {
            clear(files);
            clear(QFileInfo(files).absolutePath() + QStringLiteral("/info"));
        }
    }
    trashCount_ = 0;
    noteTrashChanged();
}

void Dock::dropOntoTrash(Screen &screen) {
    const QString uid = screen.dragUid;
    if (uid.startsWith(QLatin1String("min:"))) {
        bool ok = false;
        const quint64 key = uid.mid(4).toULongLong(&ok);
        if (ok && onClose) onClose(key);
        return;
    }
    const int index = itemIndex(uid);
    if (index < 0) return;
    const Item &item = items_[static_cast<size_t>(index)];
    if (item.kind == Item::Kind::Stack) {
        openStackPage(screen, item.id, QStringLiteral("unpin"));
        return;
    }
    items_.erase(items_.begin() + index);
    save();
}

void Dock::dropFiles(const QStringList &uris) {
    for (auto it = screens_.begin(); it != screens_.end(); ++it) it->fileDrag = false;
    const QStringList bins = trashBins();
    QStringList paths;
    for (const QString &uri : uris) {
        if (paths.size() >= 200) break;
        const QUrl url = QUrl::fromEncoded(uri.toUtf8());
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (path.isEmpty() || !QFileInfo::exists(path)) continue;
        bool alreadyTrash = false;
        for (const QString &files : bins) {
            const QString root = QFileInfo(files).absolutePath();
            if (path == root || path.startsWith(root + QLatin1Char('/'))) {
                alreadyTrash = true;
                break;
            }
        }
        if (alreadyTrash) continue;
        paths << path;
    }
    if (paths.isEmpty()) return;
    const QString gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
    if (gio.isEmpty()) {
        std::fprintf(stderr, "magic-dock: gio is missing, so the drop was not trashed\n");
        return;
    }
    for (int i = 0; i < paths.size(); i += 40) {
        QStringList args;
        args << QStringLiteral("trash");
        for (int j = i; j < paths.size() && j < i + 40; ++j) args << paths.at(j);
        startDetachedApp(gio, args, QDir::homePath(), "trash");
    }
    noteTrashChanged();
}

bool Dock::acceptsFileDrop(int outputId, double x, double y) const {
    const auto it = screens_.constFind(outputId);
    if (it == screens_.constEnd()) return false;
    const Hit hit = hitTest(it.value(), x, y);
    return hit.uid == QLatin1String("trash");
}

void Dock::fileDrag(int outputId, double x, double y, bool active) {
    if (outputId < 0) {
        for (auto it = screens_.begin(); it != screens_.end(); ++it) {
            it->fileDrag = false;
            it->pointer = false;
        }
        return;
    }
    Screen &current = screen(outputId);
    current.fileDrag = active;
    if (!active) {
        current.pointer = false;
        current.x = -1000;
        current.y = -1000;
        return;
    }
    pointerOutput_ = outputId;
    current.pointer = true;
    current.x = x;
    current.y = y;
    const Hit hit = hitTest(current, x, y);
    if (hit.uid == QLatin1String("trash")) {
        if (current.tipUid != QLatin1String("trash")) {
            current.tipUid = QStringLiteral("trash");
            current.tipSince = now_;
        }
    } else if (current.tipUid == QLatin1String("trash")) {
        current.tipUid.clear();
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
            for (const QString &app : appsIn(item)) {
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
    bool firstApplet = true;
    auto pushApplet = [&](Slot slot) {
        slot.gapBefore = firstApplet ? 18 : 8;
        firstApplet = false;
        right.push_back(slot);
    };
    if (showTrash_) {
        Slot trash;
        trash.kind = Item::Kind::Trash;
        trash.uid = QStringLiteral("trash");
        trash.label = QStringLiteral("Trash");
        pushApplet(trash);
    }
    if (showGrok_ && grokInstalled_) {
        Slot grok;
        grok.kind = Item::Kind::Grok;
        grok.uid = QStringLiteral("grok");
        grok.label = QStringLiteral("Grok");
        pushApplet(grok);
    }
    if (showTask_) {
        Slot task;
        task.kind = Item::Kind::TaskManager;
        task.uid = QStringLiteral("taskman");
        task.appId = monitor;
        task.label = QStringLiteral("System Monitor");
        for (const Live &live : live_) {
            if (!isApp(live, monitor)) continue;
            task.running++;
            if (live.activated && !live.minimized) task.active = true;
        }
        pushApplet(task);
    }
    Slot overflow;
    if (firstApplet) overflow.gapBefore = 18;
    overflow.kind = Item::Kind::Overflow;
    overflow.uid = QStringLiteral("overflow");
    overflow.label = QStringLiteral("Menu");
    right.push_back(overflow);

    const double thick = barThickness();
    const double size = iconSize_;
    const bool hor = horizontal();
    const double length = hor ? screen.w : screen.h;
    const double gap = screenGap();
    QRectF bar;
    if (edge_ == ShellSpec::Edge::Top) bar = QRectF(0, gap - screen.hidden, screen.w, thick);
    else if (edge_ == ShellSpec::Edge::Left) bar = QRectF(gap - screen.hidden, 0, thick, screen.h);
    else if (edge_ == ShellSpec::Edge::Right)
        bar = QRectF(screen.w - gap - thick + screen.hidden, 0, thick, screen.h);
    else
        bar = QRectF(0, screen.h - gap - thick + screen.hidden, screen.w, thick);
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
    auto dismissHit = [&](const QRectF &rect, const char *id) {
        if (rect.isEmpty() || !rect.contains(point)) return false;
        hit.type = 16;
        hit.action = QString::fromLatin1(id);
        return true;
    };
    if (screen.menu && dismissHit(screen.subClose, "sub")) return hit;
    if (screen.menu && dismissHit(screen.menuClose, "menu")) return hit;
    if (screen.search && dismissHit(screen.searchClose, "search")) return hit;
    if (screen.about && dismissHit(screen.aboutClose, "about")) return hit;
    if (screen.previews > 0.35 && dismissHit(screen.previewsClose, "previews")) return hit;
    if (!screen.meterCard.isEmpty() && dismissHit(screen.meterClose, "meter")) return hit;
    if (!screen.grokCard.isEmpty() && dismissHit(screen.grokClose, "grok")) return hit;
    if (!screen.sideStack.isEmpty() && screen.side > 0.35 && dismissHit(screen.sideClose, "side")) return hit;
    if (!screen.openStack.isEmpty() && screen.popup > 0.35 && dismissHit(screen.popupClose, "stack")) return hit;
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
    if (!screen.meterCard.isEmpty() && screen.meterCard.contains(point)) {
        hit.type = 1;
        hit.uid = QStringLiteral("taskman");
        hit.appId = taskId();
        return hit;
    }
    if (!screen.grokCard.isEmpty() && screen.grokCard.contains(point)) {
        hit.uid = QStringLiteral("grok");
        if (screen.grokTabGrok.contains(point)) {
            hit.type = 13;
            hit.index = 0;
            return hit;
        }
        if (screen.grokTabBots.contains(point)) {
            hit.type = 13;
            hit.index = 1;
            return hit;
        }
        if (!screen.grokLaunch.isEmpty() && screen.grokLaunch.contains(point)) {
            hit.type = 14;
            return hit;
        }
        hit.type = 15;
        return hit;
    }
    auto hitPopup = [&](bool sidePanel, bool live, double open, const QRectF &rect, const QRectF &manage, const QRectF &back,
                        const QRectF &title, const std::vector<Screen::Cell> &cells, const QString &stackId) {
        if ((!live && open < 0.35) || rect.isEmpty() || !rect.contains(point) || stackId.isEmpty()) return false;
        hit.side = sidePanel;
        if (manage.contains(point)) {
            hit.type = 12;
            hit.uid = QStringLiteral("stack:") + stackId;
            hit.appId = stackId;
            return true;
        }
        if (back.contains(point)) {
            hit.type = 11;
            hit.uid = QStringLiteral("stack:") + stackId;
            return true;
        }
        if (title.contains(point)) {
            hit.type = 10;
            hit.uid = QStringLiteral("stack:") + stackId;
            hit.appId = stackId;
            return true;
        }
        for (const Screen::Cell &cell : cells) {
            if (!cell.rect.contains(point)) continue;
            hit.type = 2;
            hit.uid = cell.nested ? QStringLiteral("nest:") + cell.stackId : QStringLiteral("stack:") + stackId;
            hit.appId = cell.nested ? QString() : cell.appId;
            return true;
        }
        hit.type = 2;
        hit.uid = QStringLiteral("stack:") + stackId;
        return true;
    };
    auto hitCards = [&](double open, const std::vector<Screen::Card> &cards) {
        if (open < 0.35) return false;
        for (int i = 0; i < static_cast<int>(cards.size()); ++i) {
            const Screen::Card &card = cards[static_cast<size_t>(i)];
            if (!card.closeRect.contains(point)) continue;
            hit.type = 6;
            hit.key = card.key;
            hit.appId = card.appId;
            hit.index = i;
            return true;
        }
        for (int i = 0; i < static_cast<int>(cards.size()); ++i) {
            const Screen::Card &card = cards[static_cast<size_t>(i)];
            if (!card.rect.contains(point)) continue;
            hit.type = 5;
            hit.key = card.key;
            hit.appId = card.appId;
            hit.index = i;
            return true;
        }
        return false;
    };
    if (hitCards(std::max(screen.side, screen.sideStack.isEmpty() ? 0.0 : 0.35), screen.sideWindows)) return hit;
    if (hitCards(screen.popup, screen.stackWindows)) return hit;
    if (hitPopup(true, !screen.sideStack.isEmpty(), screen.side, screen.sideRect, screen.sideManage, QRectF(),
                 screen.sideTitle, screen.sideCells, screen.shownSide)) {
        return hit;
    }
    if (hitPopup(false, !screen.openStack.isEmpty(), screen.popup, screen.popupRect, screen.stackManage, screen.stackBack,
                 screen.titleRect, screen.cells, screen.shownStack)) {
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
    // The gap between a hover card and its icon is still that icon. A click there
    // launches it, and the card stays up while the pointer crosses the gap.
    if (screen.tipUid == QLatin1String("grok") && tipZone(screen, screen.tipUid).contains(point)) {
        hit.type = 1;
        hit.uid = QStringLiteral("grok");
        return hit;
    }
    if (screen.tipUid == QLatin1String("taskman") && tipZone(screen, screen.tipUid).contains(point)) {
        hit.type = 1;
        hit.uid = QStringLiteral("taskman");
        hit.appId = taskId();
        return hit;
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
    const double edgeGap = screenGap();
    if (edge_ == ShellSpec::Edge::Top) band = QRectF(0, 0, screen.w, barH + edgeGap + 24);
    else if (edge_ == ShellSpec::Edge::Left) band = QRectF(0, 0, barH + edgeGap + 24, screen.h);
    else if (edge_ == ShellSpec::Edge::Right)
        band = QRectF(screen.w - (barH + edgeGap + 24), 0, barH + edgeGap + 24, screen.h);
    else
        band = QRectF(0, screen.h - (barH + edgeGap + 24), screen.w, barH + edgeGap + 24);
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
    const bool overCard = (!screen.meterCard.isEmpty() && screen.meterCard.contains(screen.x, screen.y)) ||
                          (!screen.grokCard.isEmpty() && screen.grokCard.contains(screen.x, screen.y));
    const bool hold = screen.menu || screen.search || screen.about || !screen.openStack.isEmpty() ||
                      !screen.openPreviews.isEmpty() || screen.dragging || screen.fileDrag || screen.rename ||
                      (screen.pointer && screen.hidden < barH * 0.5 &&
                       (overBar || overCard || screen.popupRect.contains(screen.x, screen.y) ||
                        screen.sideRect.contains(screen.x, screen.y) || screen.previewsRect.contains(screen.x, screen.y) ||
                        screen.searchRect.contains(screen.x, screen.y) || screen.aboutRect.contains(screen.x, screen.y)));
    const double reach = std::max(handle_ + 4.0, 8.0);
    bool hot = false;
    if (screen.pointer) {
        if (edge_ == ShellSpec::Edge::Top) hot = screen.y <= reach;
        else if (edge_ == ShellSpec::Edge::Left) hot = screen.x <= reach;
        else if (edge_ == ShellSpec::Edge::Right) hot = screen.x >= screen.w - reach;
        else hot = screen.y >= screen.h - reach;
    }
    const double tucked = std::max(0.0, barH - handle_);
    if (screen.fileDrag) {
        screen.hiddenTarget = 0;
        screen.hideArmed = false;
        screen.showArmed = false;
        return;
    }
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
        const double edgeGap = screenGap();
        if (edge_ == ShellSpec::Edge::Top) add(QRectF(0, 0, screen.w, handle_ + edgeGap));
        else if (edge_ == ShellSpec::Edge::Left) add(QRectF(0, 0, handle_ + edgeGap, screen.h));
        else if (edge_ == ShellSpec::Edge::Right)
            add(QRectF(screen.w - handle_ - edgeGap, 0, handle_ + edgeGap, screen.h));
        else add(QRectF(0, screen.h - handle_ - edgeGap, screen.w, handle_ + edgeGap));
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
    if (screen.side > 0.2 && !screen.sideRect.isEmpty()) bridge(screen.sideRect);
    if (!screen.popupRect.isEmpty() && !screen.sideRect.isEmpty()) {
        QRectF span(screen.popupRect.center(), screen.sideRect.center());
        add(span.normalized().adjusted(-16, -16, 16, 16));
    }
    if (screen.previews > 0.2 && !screen.previewsRect.isEmpty()) bridge(screen.previewsRect);
    if (screen.menu) {
        add(screen.menuRect.adjusted(-2, -2, 2, 2));
        add(screen.subRect.adjusted(-2, -2, 2, 2));
    }
    if (screen.search && !screen.searchRect.isEmpty()) bridge(screen.searchRect);
    if (screen.about && !screen.aboutRect.isEmpty()) bridge(screen.aboutRect);
    if (!screen.meterCard.isEmpty()) add(tipZone(screen, QStringLiteral("taskman")));
    if (!screen.grokCard.isEmpty()) add(tipZone(screen, QStringLiteral("grok")));
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
    if (slot->kind == Item::Kind::Trash) {
        openTrash();
        return;
    }
    if (slot->kind == Item::Kind::TaskManager) {
        launchId(slot->appId);
        return;
    }
    if (slot->kind == Item::Kind::Grok) {
        openGrogy();
        return;
    }
    if (slot->kind == Item::Kind::Minimized) {
        if (onUnminimize) onUnminimize(slot->windowKey);
        return;
    }
    if (slot->kind == Item::Kind::Stack) {
        const QString id = slot->appId;
        if (screen.openStack == id) {
            screen.openStack.clear();
            screen.sideStack.clear();
        } else {
            screen.openStack = id;
            screen.shownStack = id;
            screen.sideStack.clear();
            screen.shownSide.clear();
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
            screen.sideStack.clear();
        }
        return;
    }
    activateApp(slot->appId);
}

void Dock::openStackPage(Screen &screen, const QString &stackId, const QString &page) {
    const Item *stack = findStack(stackId);
    if (!stack) return;
    screen.openPreviews.clear();
    screen.search = false;
    screen.about = false;
    screen.rows.clear();
    screen.menu = true;
    screen.menuUid = QStringLiteral("stack:") + stackId;
    screen.menuApp = stackId;
    screen.menuRow = -1;
    screen.subRow = -1;
    const QString name = stack->name.isEmpty() ? QStringLiteral("Stack") : stack->name;
    auto add = [&](const QString &action, const QString &label, bool enabled = true) {
        MenuRow row;
        row.action = action;
        row.label = label;
        row.enabled = enabled;
        screen.rows.push_back(row);
    };
    auto header = [&](const QString &label) {
        MenuRow row;
        row.header = true;
        row.enabled = false;
        row.action = QStringLiteral("header");
        row.label = label;
        screen.rows.push_back(row);
    };
    if (page == QLatin1String("split")) {
        header(QStringLiteral("Split %1?").arg(name));
        add(QStringLiteral("note"), QStringLiteral("What is inside stays on the dock."), false);
        add(QStringLiteral("split-now"), QStringLiteral("Split"));
        add(QStringLiteral("stack-back"), QStringLiteral("Back"));
        return;
    }
    if (page == QLatin1String("unpin")) {
        header(QStringLiteral("Unpin %1?").arg(name));
        add(QStringLiteral("note"), QStringLiteral("The whole stack leaves the dock."), false);
        add(QStringLiteral("unpin-stack-now"), QStringLiteral("Unpin stack"));
        add(QStringLiteral("stack-back"), QStringLiteral("Back"));
        return;
    }
    const int count = static_cast<int>(stack->children.size());
    header(name);
    add(QStringLiteral("note"), count == 1 ? QStringLiteral("1 inside") : QStringLiteral("%1 inside").arg(count), false);
    add(QStringLiteral("ask-split"), QStringLiteral("Split onto the dock"));
    add(QStringLiteral("ask-unpin-stack"), QStringLiteral("Unpin the stack"));
}

void Dock::openMenu(Screen &screen, const QString &uid, const QString &appId) {
    if (persist_ && (uid == QLatin1String("grok") || uid == QLatin1String("overflow"))) probeGrokTools();
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
        sep();
        MenuRow header;
        header.header = true;
        header.enabled = false;
        header.action = QStringLiteral("header");
        header.label = QStringLiteral("Applets");
        screen.rows.push_back(header);
        auto toggle = [&](const QString &action, const QString &label, bool on) {
            MenuRow row;
            row.action = action;
            row.label = label;
            row.toggle = true;
            row.checked = on;
            screen.rows.push_back(row);
        };
        toggle(QStringLiteral("trash-toggle"), QStringLiteral("Trash"), showTrash_);
        if (grokInstalled_) toggle(QStringLiteral("grok-toggle"), QStringLiteral("Grok"), showGrok_);
        toggle(QStringLiteral("task-toggle"), QStringLiteral("System Monitor"), showTask_);
        return;
    }
    if (uid == QLatin1String("trash")) {
        MenuRow header;
        header.header = true;
        header.enabled = false;
        header.action = QStringLiteral("header");
        if (trashCount_ <= 0) header.label = QStringLiteral("Trash is empty");
        else if (trashCount_ == 1) header.label = QStringLiteral("1 item");
        else header.label = QStringLiteral("%1 items").arg(trashCount_);
        screen.rows.push_back(header);
        add(QStringLiteral("open-trash"), QStringLiteral("Open"));
        add(QStringLiteral("empty-trash"), QStringLiteral("Empty Trash"), trashCount_ > 0);
        sep();
        add(QStringLiteral("drop-hint"), QStringLiteral("Drop files here"), false);
        return;
    }
    if (uid == QLatin1String("taskman")) {
        add(QStringLiteral("taskman"), QStringLiteral("Open System Monitor"));
        return;
    }
    if (uid == QLatin1String("grok")) {
        MenuRow header;
        header.header = true;
        header.enabled = false;
        header.action = QStringLiteral("header");
        if (creditOk_) header.label = QStringLiteral("Summary · %1% left").arg(creditLeft_);
        else header.label = QStringLiteral("Summary");
        screen.rows.push_back(header);
        if (grogyOnPath_) add(QStringLiteral("grok"), QStringLiteral("Open Grogy"));
        else add(QStringLiteral("note"), QStringLiteral("Grogy is not on the path"), false);
        return;
    }
    if (uid.startsWith(QLatin1String("min:"))) {
        add(QStringLiteral("restore"), QStringLiteral("Restore"));
        add(QStringLiteral("close-one"), QStringLiteral("Close"));
        return;
    }
    const QString stackKey = uid.startsWith(QLatin1String("stack:")) ? uid.mid(6) : QString();
    const Item *stackItem = stackKey.isEmpty() ? nullptr : findStack(stackKey);
    if (stackItem && (appId.isEmpty() || appId == stackItem->id)) {
        MenuRow header;
        header.header = true;
        header.label = stackItem->name.isEmpty() ? QStringLiteral("Stack") : stackItem->name;
        header.action = QStringLiteral("header");
        header.enabled = false;
        screen.rows.push_back(header);
        add(QStringLiteral("open-all"), QStringLiteral("Open All"));
        add(QStringLiteral("rename"), QStringLiteral("Rename"));
        add(QStringLiteral("manage"), QStringLiteral("Stack"));
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
    QString stackOf;
    {
        const std::function<bool(const std::vector<Item> &)> walk = [&](const std::vector<Item> &nodes) {
            for (const Item &item : nodes) {
                if (item.kind != Item::Kind::Stack) continue;
                for (const Item &child : item.children) {
                    if (child.kind == Item::Kind::App && (child.id == id || canonical(child.id) == canonical(id))) {
                        stackOf = item.id;
                        return true;
                    }
                }
                if (walk(item.children)) return true;
            }
            return false;
        };
        walk(items_);
    }
    if (pinned || !stackOf.isEmpty()) add(QStringLiteral("unpin"), QStringLiteral("Unpin"));
    else add(QStringLiteral("pin"), QStringLiteral("Pin to Dock"));
    if (!stackOf.isEmpty()) add(QStringLiteral("remove-from-stack"), QStringLiteral("Remove from Stack"));
    add(QStringLiteral("send-desktop"), QStringLiteral("Send to Desktop"));
    MenuRow addTo;
    addTo.action = QStringLiteral("add-submenu");
    addTo.label = QStringLiteral("Add to Stack");
    const std::function<void(const Item &, const QString &)> listStacks = [&](const Item &item, const QString &prefix) {
        if (item.kind != Item::Kind::Stack) return;
        if (item.id != stackOf) {
            MenuRow child;
            child.action = QStringLiteral("add-to:") + item.id;
            child.label = prefix.isEmpty() ? item.name : prefix + QStringLiteral(" / ") + item.name;
            addTo.sub.push_back(child);
        }
        for (const Item &child : item.children) {
            if (child.kind == Item::Kind::Stack) listStacks(child, prefix.isEmpty() ? item.name : prefix + QStringLiteral(" / ") + item.name);
        }
    };
    for (const Item &item : items_) listStacks(item, {});
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
    if (action == QLatin1String("-") || action == QLatin1String("header") || action == QLatin1String("add-submenu") ||
        action == QLatin1String("drop-hint") || action == QLatin1String("note")) {
        return;
    }
    if (action == QLatin1String("empty-trash") && trashCount_ <= 0) return;
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
        screen.sideStack.clear();
        screen.openPreviews.clear();
        screen.about = true;
    } else if (action == QLatin1String("contribute")) {
        openContribute();
    } else if (action == QLatin1String("taskman")) {
        launchId(QStringLiteral("com.system76.CosmicMonitor"));
    } else if (action == QLatin1String("grok")) {
        openGrogy();
    } else if (action == QLatin1String("trash-toggle") || action == QLatin1String("task-toggle") ||
               action == QLatin1String("grok-toggle")) {
        if (action == QLatin1String("trash-toggle")) showTrash_ = !showTrash_;
        if (action == QLatin1String("task-toggle")) showTask_ = !showTask_;
        if (action == QLatin1String("grok-toggle")) showGrok_ = !showGrok_;
        save();
        openMenu(screen, QStringLiteral("overflow"), {});
        return;
    } else if (action == QLatin1String("open-trash")) {
        openTrash();
    } else if (action == QLatin1String("empty-trash")) {
        screen.rows.clear();
        screen.menuRow = -1;
        screen.subRow = -1;
        MenuRow header;
        header.header = true;
        header.enabled = false;
        header.action = QStringLiteral("header");
        header.label = trashCount_ == 1 ? QStringLiteral("Delete 1 item?") : QStringLiteral("Delete %1 items?").arg(trashCount_);
        screen.rows.push_back(header);
        MenuRow confirm;
        confirm.action = QStringLiteral("empty-now");
        confirm.label = QStringLiteral("Empty Trash");
        screen.rows.push_back(confirm);
        MenuRow cancel;
        cancel.action = QStringLiteral("cancel");
        cancel.label = QStringLiteral("Cancel");
        screen.rows.push_back(cancel);
        return;
    } else if (action == QLatin1String("empty-now")) {
        emptyTrash();
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
        unpinApp(screen, id);
    } else if (action == QLatin1String("send-desktop")) {
        sendToDesktop(id);
    } else if (action == QLatin1String("new-stack")) {
        const int index = itemIndex(QStringLiteral("app:") + id);
        Item stack;
        stack.kind = Item::Kind::Stack;
        stack.id = QUuid::createUuid().toString(QUuid::Id128).left(8);
        stack.name = itemName(id);
        Item child;
        child.kind = Item::Kind::App;
        child.id = id;
        stack.children.push_back(child);
        if (index >= 0) items_[static_cast<size_t>(index)] = stack;
        else items_.push_back(stack);
        stripApp(items_, id, stack.id);
        save();
    } else if (action.startsWith(QLatin1String("add-to:"))) {
        const QString stackId = action.mid(7);
        placeInStack(screen.menuApp.isEmpty() ? id : screen.menuApp, stackId);
        closeStackIfGone(screen);
    } else if (action == QLatin1String("remove-from-stack")) {
        stripApp(items_, id);
        if (itemIndex(QStringLiteral("app:") + id) < 0) {
            Item item;
            item.kind = Item::Kind::App;
            item.id = id;
            items_.push_back(item);
        }
        closeStackIfGone(screen);
        save();
    } else if (action == QLatin1String("manage")) {
        const QString stackId = screen.menuUid.startsWith(QLatin1String("stack:")) ? screen.menuUid.mid(6) : screen.menuApp;
        openStackPage(screen, stackId, QStringLiteral("home"));
        return;
    } else if (action == QLatin1String("ask-split")) {
        const QString stackId = screen.menuUid.startsWith(QLatin1String("stack:")) ? screen.menuUid.mid(6) : screen.menuApp;
        openStackPage(screen, stackId, QStringLiteral("split"));
        return;
    } else if (action == QLatin1String("ask-unpin-stack")) {
        const QString stackId = screen.menuUid.startsWith(QLatin1String("stack:")) ? screen.menuUid.mid(6) : screen.menuApp;
        openStackPage(screen, stackId, QStringLiteral("unpin"));
        return;
    } else if (action == QLatin1String("stack-back")) {
        const QString stackId = screen.menuUid.startsWith(QLatin1String("stack:")) ? screen.menuUid.mid(6) : screen.menuApp;
        openStackPage(screen, stackId, QStringLiteral("home"));
        return;
    } else if (action == QLatin1String("split-now") || action == QLatin1String("ungroup")) {
        const QString stackId = screen.menuUid.startsWith(QLatin1String("stack:")) ? screen.menuUid.mid(6) : QString();
        const QString up = parentStackId(stackId);
        int index = -1;
        std::vector<Item> *home = childList(stackId, &index);
        if (home && index >= 0) {
            const Item stack = (*home)[static_cast<size_t>(index)];
            home->erase(home->begin() + index);
            int at = index;
            for (const Item &child : stack.children) {
                home->insert(home->begin() + at, child);
                ++at;
            }
            if (screen.openStack == stackId) screen.openStack = up;
            closeStackIfGone(screen);
            save();
        }
    } else if (action == QLatin1String("unpin-stack-now")) {
        const QString stackId = screen.menuUid.startsWith(QLatin1String("stack:")) ? screen.menuUid.mid(6) : QString();
        const QString up = parentStackId(stackId);
        bool close = screen.openStack == stackId;
        if (const Item *stack = findStack(stackId)) close = close || containsStack(*stack, screen.openStack);
        int index = -1;
        std::vector<Item> *home = childList(stackId, &index);
        if (home && index >= 0) home->erase(home->begin() + index);
        if (close) screen.openStack = up;
        closeStackIfGone(screen);
        save();
    } else if (action == QLatin1String("rename")) {
        const QString stackId = screen.menuUid.startsWith(QLatin1String("stack:")) ? screen.menuUid.mid(6) : QString();
        if (const Item *stack = findStack(stackId)) {
            screen.rename = true;
            screen.renameStack = stack->id;
            screen.renameText = stack->name;
            screen.caret = screen.renameText.size();
            screen.renameSelectAll = true;
            if (screen.openStack != stack->id) {
                screen.openStack = stack->id;
                screen.shownStack = stack->id;
            }
        }
    } else if (action == QLatin1String("open-all")) {
        const QString stackId = screen.menuUid.startsWith(QLatin1String("stack:")) ? screen.menuUid.mid(6) : QString();
        if (const Item *stack = findStack(stackId)) {
            for (const QString &app : appsIn(*stack)) {
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
    const bool keepMenu = action == QLatin1String("rename") || action == QLatin1String("add-submenu") ||
                          action == QLatin1String("manage") || action == QLatin1String("ask-split") ||
                          action == QLatin1String("ask-unpin-stack") || action == QLatin1String("stack-back") ||
                          action.endsWith(QLatin1String("-toggle"));
    if (!keepMenu) screen.menu = false;
}

bool Dock::nestDropTarget(const Screen &screen, double x, double y, QString *uid) const {
    if (screen.dragUid.isEmpty() || itemIndex(screen.dragUid) < 0) return false;
    const Hit hit = hitTest(screen, x, y);
    if (hit.uid == QLatin1String("trash")) return false;
    auto accept = [&](const QString &target) {
        if (target.isEmpty() || target == screen.dragUid) return false;
        const int from = itemIndex(screen.dragUid);
        if (from >= 0 && target.startsWith(QLatin1String("stack:"))) {
            const Item &drag = items_[static_cast<size_t>(from)];
            const QString id = target.mid(6);
            if (drag.kind == Item::Kind::Stack && (drag.id == id || containsStack(drag, id))) return false;
        }
        if (uid) *uid = target;
        return true;
    };
    if (!screen.pressCell) {
        auto intoPopup = [&](bool live, double open, const QRectF &rect, const QString &stackId,
                             const std::vector<Screen::Cell> &cells) {
            if ((!live && open < 0.35) || rect.isEmpty() || !rect.contains(x, y) || stackId.isEmpty()) return false;
            QString target = QStringLiteral("stack:") + stackId;
            for (const Screen::Cell &cell : cells) {
                if (!cell.nested || !cell.rect.contains(x, y)) continue;
                target = QStringLiteral("stack:") + cell.stackId;
                break;
            }
            return accept(target);
        };
        if (intoPopup(!screen.sideStack.isEmpty(), screen.side, screen.sideRect, screen.shownSide, screen.sideCells))
            return true;
        if (intoPopup(!screen.openStack.isEmpty(), screen.popup, screen.popupRect, screen.shownStack, screen.cells))
            return true;
    }
    if (hit.type != 1 || hit.uid.isEmpty() || hit.uid == screen.dragUid || itemIndex(hit.uid) < 0) return false;
    const Slot *slot = nullptr;
    for (const Slot &candidate : screen.slots)
        if (candidate.uid == hit.uid) slot = &candidate;
    if (!slot || (slot->kind != Item::Kind::App && slot->kind != Item::Kind::Stack)) return false;
    const double axis = horizontal() ? x : y;
    const double lead = horizontal() ? slot->rect.left() : slot->rect.top();
    const double trail = horizontal() ? slot->rect.right() : slot->rect.bottom();
    const double span = std::max(1.0, trail - lead);
    const double along = (axis - lead) / span;
    // The middle of the icon nests. The outer edges still reorder.
    if (along < 0.18 || along > 0.82) return false;
    return accept(hit.uid);
}

QRectF Dock::tipZone(const Screen &screen, const QString &uid) const {
    const QRectF card = uid == QLatin1String("grok") ? screen.grokCard : screen.meterCard;
    if (card.isEmpty()) return {};
    QRectF slotRect;
    for (const Slot &slot : screen.slots) {
        if (slot.uid != uid) continue;
        slotRect = slot.hit.united(slot.rect);
        break;
    }
    QRectF zone = card.adjusted(-12, -12, 12, 12);
    if (!slotRect.isEmpty()) zone = zone.united(slotRect);
    return zone;
}

bool Dock::keepsTip(const Screen &screen, double x, double y) const {
    if (screen.tipUid != QLatin1String("grok") && screen.tipUid != QLatin1String("taskman")) return false;
    const QRectF zone = tipZone(screen, screen.tipUid);
    return !zone.isEmpty() && zone.contains(x, y);
}

void Dock::dragMotion(Screen &screen, double x, double y) {
    if (!screen.dragging) return;
    if (screen.pressCell) {
        dragCell(screen, x, y);
        return;
    }
    const Hit hit = hitTest(screen, x, y);
    if (hit.uid == QLatin1String("trash")) {
        screen.hoverUid = QStringLiteral("trash");
        screen.mergeReady = false;
        return;
    }
    if (screen.pressCell && pulledToDock(screen, x, y)) {
        screen.hoverUid.clear();
        screen.mergeReady = false;
        return;
    }
    QString nest;
    if (nestDropTarget(screen, x, y, &nest)) {
        if (screen.hoverUid != nest) {
            screen.hoverUid = nest;
            screen.hoverSince = now_;
        }
        screen.mergeReady = true;
        return;
    }
    const int from = itemIndex(screen.dragUid);
    if (from >= 0 && hit.type == 1 && !hit.uid.isEmpty() && hit.uid != screen.dragUid) {
        const int to = itemIndex(hit.uid);
        const Slot *slot = nullptr;
        for (const Slot &candidate : screen.slots)
            if (candidate.uid == hit.uid) slot = &candidate;
        if (to >= 0 && slot) {
            const bool passed = horizontal()
                                    ? ((to < from && x < slot->rect.center().x()) || (to > from && x > slot->rect.center().x()))
                                    : ((to < from && y < slot->rect.center().y()) || (to > from && y > slot->rect.center().y()));
            if (passed) {
                moveItem(from, to);
                relayout(screen);
            }
        }
    }
    screen.hoverUid.clear();
    screen.mergeReady = false;
}

void Dock::pressAt(Screen &screen, double x, double y, bool right, bool middle) {
    relayout(screen);
    const Hit hit = hitTest(screen, x, y);
    if (right) {
        if (hit.type == 13 || hit.type == 14 || hit.type == 15) {
            openMenu(screen, QStringLiteral("grok"), {});
            return;
        }
        if (hit.type == 1) openMenu(screen, hit.uid, hit.appId.isEmpty() ? hit.uid : hit.appId);
        else if (hit.type == 2 && hit.uid.startsWith(QLatin1String("nest:")))
            openMenu(screen, QStringLiteral("stack:") + hit.uid.mid(5), hit.uid.mid(5));
        else if (hit.type == 2 && !hit.appId.isEmpty()) openMenu(screen, hit.uid, hit.appId);
        else if (hit.type == 12) openStackPage(screen, screen.shownStack, QStringLiteral("home"));
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
    screen.pressUid = (hit.type == 1 || hit.type == 10 || hit.type == 11 || hit.type == 12 ||
                       hit.uid.startsWith(QLatin1String("nest:")))
                          ? hit.uid
                          : QString();
    screen.pressApp = hit.appId;
    screen.pressAction = (hit.type == 3 || hit.type == 4) ? hit.action : QString();
    screen.pressMenu = hit.type == 3 || hit.type == 4;
    screen.pressSide = hit.side;
    screen.pressCell = hit.type == 2 && (!hit.appId.isEmpty() || hit.uid.startsWith(QLatin1String("nest:")));
    screen.pressTitle = hit.type == 10;
    screen.pressBack = hit.type == 11;
    screen.pressManage = hit.type == 12;
    screen.pressCard = hit.type == 5 || hit.type == 6;
    screen.pressClose = hit.type == 6;
    screen.pressSearch = hit.type == 7;
    screen.pressAbout = hit.type == 8 || hit.type == 9;
    screen.pressGrokTab = hit.type == 13;
    screen.pressGrokPage = hit.type == 13 ? hit.index : -1;
    screen.pressGrokLaunch = hit.type == 14;
    screen.pressGrokBody = hit.type == 15;
    screen.pressDismiss = hit.type == 16;
    screen.pressDismissId = hit.type == 16 ? hit.action : QString();
    screen.pressKey = hit.key;
    screen.dragging = false;
    screen.dragUid.clear();
    if (hit.type == 1 && hit.uid.startsWith(QLatin1String("min:"))) {
        screen.dragUid = hit.uid;
    } else if (hit.type == 1 && itemIndex(hit.uid) >= 0) {
        const int index = itemIndex(hit.uid);
        const Item::Kind kind = items_[static_cast<size_t>(index)].kind;
        if (kind == Item::Kind::App || kind == Item::Kind::Stack) screen.dragUid = hit.uid;
    } else if (screen.pressCell) {
        screen.dragUid = hit.uid.startsWith(QLatin1String("nest:")) ? hit.uid : QStringLiteral("cell:") + hit.appId;
    }
}

void Dock::releaseAt(Screen &screen, double x, double y) {
    const double distance = std::hypot(x - screen.pressX, y - screen.pressY);
    const bool dragged = screen.dragging && distance > 6;
    const Hit hit = hitTest(screen, x, y);
    const bool onTrash = hit.uid == QLatin1String("trash") || screen.hoverUid == QLatin1String("trash");
    if (dragged && onTrash && !screen.pressCell && !screen.dragUid.isEmpty()) {
        dropOntoTrash(screen);
    } else if (dragged && screen.dragUid.startsWith(QLatin1String("min:")) && hit.uid == screen.pressUid) {
        clickSlot(screen, screen.pressUid);
    } else if (screen.pressed && screen.pressCell && dragged && pulledToDock(screen, x, y)) {
        if (screen.pressUid.startsWith(QLatin1String("nest:"))) pullNested(screen, screen.pressUid.mid(5));
        else pullFromStack(screen, screen.pressApp);
    } else if (screen.pressed && screen.pressCell && dragged && screen.mergeReady &&
               screen.hoverUid.startsWith(QLatin1String("stack:"))) {
        const QString targetId = screen.hoverUid.mid(6);
        const QString sourceId = screen.pressSide && !screen.shownSide.isEmpty() ? screen.shownSide : screen.shownStack;
        if (targetId == screen.shownStack || targetId == screen.shownSide) {
            if (targetId != sourceId) moveChildTo(screen, sourceId, targetId);
        } else {
            dropChildInto(screen, sourceId, targetId);
        }
    } else if (dragged && !screen.pressCell && !screen.dragUid.isEmpty()) {
        QString nest;
        if (!nestDropTarget(screen, x, y, &nest) && screen.mergeReady && !screen.hoverUid.isEmpty()) {
            const Hit still = hitTest(screen, x, y);
            if (still.uid == screen.hoverUid) nest = screen.hoverUid;
        }
        if (!nest.isEmpty()) {
            const bool intoPopup = screen.popup > 0.35 && screen.popupRect.contains(x, y);
            mergeItems(screen.dragUid, nest);
            if (intoPopup && nest.startsWith(QLatin1String("stack:")) && findStack(nest.mid(6))) {
                screen.openStack = nest.mid(6);
                screen.shownStack = screen.openStack;
            } else {
                screen.openStack.clear();
            }
        }
    } else if (screen.pressed && !dragged && screen.pressDismiss && hit.type == 16 && hit.action == screen.pressDismissId) {
        dismissPopup(screen, hit.action);
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
            if (screen.rename) commitRename(screen, true);
            if (screen.menu && hit.uid != screen.menuUid) screen.menu = false;
            if (hit.uid != QLatin1String("search")) screen.search = false;
            if (hit.uid != QLatin1String("overflow")) screen.about = false;
            clickSlot(screen, hit.uid);
        } else if (hit.type == 12 && screen.pressManage && hit.uid == screen.pressUid) {
            const QString stackId = hit.uid.startsWith(QLatin1String("stack:")) ? hit.uid.mid(6) : screen.shownStack;
            openStackPage(screen, stackId, QStringLiteral("home"));
        } else if (hit.type == 11 && screen.pressBack) {
            const QString parent = parentStackId(screen.shownStack);
            if (!parent.isEmpty()) {
                screen.openStack = parent;
                screen.shownStack = parent;
                screen.sideStack.clear();
            }
        } else if (hit.type == 2 && screen.pressCell && hit.uid.startsWith(QLatin1String("nest:")) &&
                   hit.uid == screen.pressUid) {
            if (screen.rename) commitRename(screen, true);
            const QString nestId = hit.uid.mid(5);
            if (screen.pressSide) {
                if (screen.sideStack == nestId) {
                    screen.sideStack.clear();
                } else if (!screen.shownSide.isEmpty()) {
                    screen.openStack = screen.shownSide;
                    screen.shownStack = screen.shownSide;
                    screen.sideStack = nestId;
                    screen.shownSide = nestId;
                }
            } else if (screen.sideStack == nestId) {
                screen.sideStack.clear();
            } else {
                screen.sideStack = nestId;
                screen.shownSide = nestId;
            }
            screen.menu = false;
        } else if (hit.type == 10 && screen.pressTitle && hit.uid == screen.pressUid) {
            const QString stackId = hit.uid.startsWith(QLatin1String("stack:")) ? hit.uid.mid(6) : screen.shownStack;
            const bool same = screen.lastTitleStack == stackId;
            const qint64 elapsed = now_ - screen.lastTitleClick;
            screen.lastTitleClick = now_;
            screen.lastTitleStack = stackId;
            if (same && elapsed < 450 && !(screen.rename && screen.renameStack == stackId)) beginRename(screen, stackId);
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
            screen.openStack.clear();
            screen.sideStack.clear();
        } else if (hit.type == 13 && screen.pressGrokTab && hit.index >= 0 && hit.index == screen.pressGrokPage) {
            screen.grokPage = hit.index;
        } else if (hit.type == 14 && screen.pressGrokLaunch) {
            openGrogy();
        } else if (hit.type == 15 && screen.pressGrokBody) {
            openGrogy();
        } else if (hit.type == 2 && screen.pressCell && hit.appId == screen.pressApp) {
            if (screen.rename) commitRename(screen, true);
            if (keysFor(hit.appId).size() > 1) {
                screen.openPreviews = hit.appId;
                screen.shownPreviews = hit.appId;
                screen.openStack.clear();
                screen.sideStack.clear();
                screen.menu = false;
            } else {
                activateApp(hit.appId);
                screen.openStack.clear();
                screen.sideStack.clear();
            }
        } else if (hit.type == 0) {
            if (screen.rename) commitRename(screen, true);
            screen.menu = false;
            screen.search = false;
            screen.about = false;
            screen.openStack.clear();
            screen.sideStack.clear();
            screen.openPreviews.clear();
        }
    }
    screen.pressed = false;
    screen.dragging = false;
    screen.dragUid.clear();
    screen.mergeReady = false;
    screen.pressCell = false;
    screen.pressSide = false;
    screen.dropIndex = -1;
    screen.pressTitle = false;
    screen.pressBack = false;
    screen.pressManage = false;
    screen.pressMenu = false;
    screen.pressCard = false;
    screen.pressClose = false;
    screen.pressSearch = false;
    screen.pressAbout = false;
    screen.pressGrokTab = false;
    screen.pressGrokLaunch = false;
    screen.pressGrokBody = false;
    screen.pressGrokPage = -1;
    screen.pressDismiss = false;
    screen.pressDismissId.clear();
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
            if (current.tipDismissed == hit.uid) {
                current.tipUid.clear();
            } else {
                current.tipDismissed.clear();
                if (current.tipUid != hit.uid) {
                    current.tipUid = hit.uid;
                    current.tipSince = now_;
                }
            }
        } else if (!keepsTip(current, event.x, event.y)) {
            current.tipUid.clear();
            if (hit.type != 13 && hit.type != 14 && hit.type != 15 && hit.type != 16) current.tipDismissed.clear();
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
    const bool meters = pollMeters();
    const bool grok = pollGrok();
    bool busy = meters || grok || chromeDirty_;
    chromeDirty_ = false;
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
        const double sideTarget = current.sideStack.isEmpty() ? 0.0 : 1.0;
        current.side += (sideTarget - current.side) * 0.34;
        if (std::abs(current.side - sideTarget) < 0.02) current.side = sideTarget;
        else busy = true;
        if (current.sideStack.isEmpty() && current.side < 0.02) current.shownSide.clear();
        if (!current.sideStack.isEmpty()) current.shownSide = current.sideStack;
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
                if (index >= 0 && holdsApp(items_[static_cast<size_t>(index)], live.appId)) {
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
        if (previewWallpaper_.isNull()) {
            const QString path = wallpaperFile();
            if (!path.isEmpty()) previewWallpaper_.load(path);
        }
        if (!previewWallpaper_.isNull()) {
            painter.drawImage(QRectF(0, 0, screen.w, screen.h), previewWallpaper_);
        } else {
            QLinearGradient backdrop(0, 0, 0, screen.h);
            backdrop.setColorAt(0, QColor(QStringLiteral("#243044")));
            backdrop.setColorAt(1, QColor(QStringLiteral("#121820")));
            painter.fillRect(QRectF(0, 0, screen.w, screen.h), backdrop);
        }
    }
    const int device = std::max(32, qRound(iconSize_ * std::max(1.0, screen.dpr) * 1.5));
    auto iconFor = [&](const QString &id, int px) {
        QPixmap pix = itemIcon(id, px);
        return pix;
    };

    const QColor accentEdge(chromeR_, chromeG_, chromeB_, 242);
    const qreal accentPen = chromeWidth(hintPx_);
    auto glass = [&](const QRectF &rect, qreal radius, int topAlpha) {
        if (rect.isEmpty()) return;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 78));
        painter.drawRoundedRect(rect.translated(0, 4).adjusted(2, 0, -2, 2), radius, radius);
        QLinearGradient fill(rect.topLeft(), rect.bottomLeft());
        fill.setColorAt(0, QColor(78, 80, 88, topAlpha));
        fill.setColorAt(0.42, QColor(34, 35, 40, topAlpha - 8));
        fill.setColorAt(1, QColor(16, 16, 18, std::min(230, topAlpha + 10)));
        painter.setBrush(fill);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(rect, radius, radius);
        painter.setBrush(QColor(255, 255, 255, 16));
        painter.drawRoundedRect(rect.adjusted(14, 5, -14, -rect.height() * 0.62), radius, radius);
    };
    auto frame = [&](const QRectF &rect, qreal radius) { strokeChrome(painter, rect, radius, accentEdge, accentPen); };
    auto dismiss = [&](const QRectF &rect) {
        if (rect.isEmpty()) return;
        drawCloseCircle(painter, rect, screen.pointer && rect.contains(screen.x, screen.y));
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
        frame(bar, radius);
        painter.restore();
    }

    auto drawBadge = [&](const QRectF &icon, int running, bool active) {
        if (running <= 0) return;
        const QColor color = active ? QColor(52, 199, 89) : QColor(46, 168, 86);
        drawCountBadge(painter, icon, color, running, false, edge_);
    };

    for (int i = 0; i < static_cast<int>(screen.slots.size()); ++i) {
        const Slot &slot = screen.slots[static_cast<size_t>(i)];
        const bool hovered = screen.pointer && !screen.fileDrag && slot.hit.contains(screen.x, screen.y) && !screen.dragging &&
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
        const bool dropHot = slot.kind == Item::Kind::Trash &&
                             ((screen.dragging && screen.hoverUid == QLatin1String("trash")) ||
                              (screen.fileDrag && slot.hit.contains(screen.x, screen.y)));
        if (dropHot) {
            painter.setPen(QPen(QColor(255, 90, 78, 230), 2));
            painter.setBrush(QColor(255, 69, 58, 42));
            painter.drawRoundedRect(slot.rect.adjusted(-4, -4, 4, 4), 14, 14);
        }
        const QPointF nudge = openingNudge(slot.appId);
        painter.save();
        painter.translate(nudge);
        if (slot.kind == Item::Kind::Search) {
            drawSearchGlyph(painter, slot.rect);
        } else if (slot.kind == Item::Kind::Overflow) {
            drawMoreGlyph(painter, slot.rect);
        } else if (slot.kind == Item::Kind::TaskManager) {
            drawUsageBars(painter, slot.rect, cpu_, mem_, cpuReady_, memTotal_ > 0);
        } else if (slot.kind == Item::Kind::Grok) {
            drawCreditRing(painter, slot.rect, creditLeft_, creditOk_);
            drawCountBadge(painter, slot.rect, QColor(64, 156, 255), static_cast<int>(runningBots_.size()), true, edge_);
        } else if (slot.kind == Item::Kind::Trash) {
            const QString iconName = trashCount_ > 0 ? QStringLiteral("user-trash-full") : QStringLiteral("user-trash");
            QPixmap pix = apps_->iconNamed(iconName, device);
            if (pix.isNull()) pix = apps_->iconNamed(QStringLiteral("user-trash"), device);
            if (!pix.isNull()) drawPixmapFit(painter, pix, slot.rect);
            else drawTrashGlyph(painter, slot.rect, trashCount_ > 0);
            drawCountBadge(painter, slot.rect, QColor(255, 69, 58), trashCount_, true, edge_);
        } else if (slot.kind == Item::Kind::Launcher || slot.kind == Item::Kind::Workspaces ||
                   slot.kind == Item::Kind::AppLibrary) {
            QString iconName = QStringLiteral("com.system76.CosmicPanelLauncherButton");
            if (slot.kind == Item::Kind::Workspaces)
                iconName = QStringLiteral("com.system76.CosmicPanelWorkspacesButton");
            else if (slot.kind == Item::Kind::AppLibrary)
                iconName = QStringLiteral("com.system76.CosmicPanelAppButton");
            QPixmap pix = apps_->iconNamed(iconName, device);
            if (pix.isNull() && slot.kind == Item::Kind::Launcher)
                pix = apps_->iconNamed(QStringLiteral("com.system76.CosmicLauncher"), device);
            if (!pix.isNull()) drawPixmapFit(painter, pix, slot.rect);
            else {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(34, 159, 173));
                painter.drawRoundedRect(slot.rect, 8, 8);
            }
        } else if (slot.kind == Item::Kind::Minimized) {
            const QRectF inner = slot.rect.adjusted(3, 3, -3, -3);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 22));
            painter.drawRoundedRect(slot.rect, 8, 8);
            drawPixmapFit(painter, iconFor(slot.appId, device), inner);
        } else if (slot.kind == Item::Kind::Stack) {
            const int index = itemIndex(slot.uid);
            QStringList apps;
            bool nested = false;
            if (index >= 0) {
                const Item &item = items_[static_cast<size_t>(index)];
                apps = appsIn(item);
                for (const Item &child : item.children)
                    if (child.kind == Item::Kind::Stack) nested = true;
            }
            QList<QPixmap> icons;
            const int shown = std::min(4, static_cast<int>(apps.size()));
            const int px = std::max(16, shown <= 1 ? device / 2 : device / 3);
            for (int n = 0; n < shown; ++n) icons.append(iconFor(apps.at(n), px));
            const QString opened = !screen.openStack.isEmpty() ? screen.openStack
                                                                : (screen.popup > 0.05 ? screen.shownStack : QString());
            bool marked = !opened.isEmpty() && topStackId(opened) == slot.appId;
            if (!marked && screen.previews > 0.15 && !screen.openPreviews.isEmpty() && index >= 0)
                marked = holdsApp(items_[static_cast<size_t>(index)], screen.openPreviews);
            if (marked) {
                painter.setPen(QPen(accentEdge, std::max<qreal>(2.0, accentPen)));
                painter.setBrush(QColor(accentEdge.red(), accentEdge.green(), accentEdge.blue(), 42));
                painter.drawRoundedRect(slot.rect.adjusted(-5, -5, 5, 5), 16, 16);
            }
            drawStackFolder(painter, slot.rect, icons, nested);
            if (slot.running > 0) {
                const QColor color = slot.active ? QColor(52, 199, 89) : QColor(46, 168, 86);
                drawCountBadge(painter, slot.rect, color, slot.running, true, edge_);
            }
        } else {
            if (screen.previews > 0.15 && !screen.openPreviews.isEmpty() && slot.appId == screen.openPreviews) {
                painter.setPen(QPen(accentEdge, std::max<qreal>(2.0, accentPen)));
                painter.setBrush(QColor(accentEdge.red(), accentEdge.green(), accentEdge.blue(), 42));
                painter.drawRoundedRect(slot.rect.adjusted(-5, -5, 5, 5), 16, 16);
            }
            drawPixmapFit(painter, iconFor(slot.appId, device), slot.rect);
            drawBadge(slot.rect, slot.running, slot.active);
        }
        painter.restore();
    }

    auto paintWindowCard = [&](const Screen::Card &card, double dy, bool hover) {
        const QRectF rect = card.rect.translated(0, dy);
        const QRectF thumb = card.thumb.translated(0, dy);
        painter.setPen(Qt::NoPen);
        painter.setBrush(hover ? QColor(255, 255, 255, 28) : QColor(255, 255, 255, 10));
        painter.drawRoundedRect(rect, 12, 12);
        painter.setBrush(QColor(12, 12, 14, 230));
        painter.setPen(QPen(card.active ? QColor(255, 255, 255, 210) : QColor(255, 255, 255, 40), card.active ? 2 : 1));
        painter.drawRoundedRect(thumb, 8, 8);
        painter.save();
        painter.setClipRect(thumb.adjusted(2, 2, -2, -2));
        painter.setPen(Qt::NoPen);
        const QImage image = thumbs_.value(card.key);
        if (!image.isNull()) {
            const double scale = std::min(thumb.width() / std::max(1, image.width()), thumb.height() / std::max(1, image.height()));
            const double w = image.width() * scale;
            const double h = image.height() * scale;
            painter.drawImage(QRectF(thumb.center().x() - w / 2.0, thumb.center().y() - h / 2.0, w, h), image);
        } else {
            const QPixmap icon = iconFor(card.appId, qRound(48 * screen.dpr));
            const double side = std::min(thumb.width(), thumb.height()) * 0.46;
            drawPixmapFit(painter, icon, QRectF(thumb.center().x() - side / 2.0, thumb.center().y() - side / 2.0, side, side));
        }
        painter.restore();
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
        QFont titleFont(QStringLiteral("Fira Sans"));
        titleFont.setPixelSize(14);
        painter.setFont(titleFont);
        painter.setPen(QColor(255, 255, 255, 220));
        const QString label =
            QFontMetrics(titleFont).elidedText(card.title, Qt::ElideRight, std::max(8, static_cast<int>(rect.width() - 12)));
        painter.drawText(QRectF(rect.left() + 4, thumb.bottom() + 2, rect.width() - 8, 20), Qt::AlignHCenter | Qt::AlignVCenter,
                         label);
    };
    auto drawStackPopup = [&](double open, const QRectF &popupRect, const QRectF &titleSource, const QRectF &backSource,
                               const QRectF &manageSource, const QRectF &closeSource, const QString &stackId,
                               const std::vector<Screen::Cell> &cells, const std::vector<Screen::Card> &windows) {
        if (open <= 0.03 || popupRect.isEmpty() || stackId.isEmpty()) return;
        painter.save();
        painter.setOpacity(open);
        const QRectF pop = popupRect.translated(0, (1.0 - open) * 8);
        glass(pop, kPopupRadius, 214);
        if (screen.mergeReady && screen.hoverUid == QLatin1String("stack:") + stackId) {
            painter.setPen(QPen(QColor(120, 186, 255, 220), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(pop.adjusted(3, 3, -3, -3), kPopupRadius, kPopupRadius);
        }
        QString title = QStringLiteral("Stack");
        if (const Item *shown = findStack(stackId)) {
            if (!shown->name.isEmpty()) title = shown->name;
        }
        const double rise = (1.0 - open) * 10;
        const QRectF titleRect = titleSource.translated(0, rise);
        const bool naming = screen.rename && screen.renameStack == stackId;
        const bool titleHot = !naming && screen.pointer && titleSource.contains(screen.x, screen.y);
        if (naming || titleHot) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(naming ? QColor(0, 0, 0, 55) : QColor(255, 255, 255, 22));
            painter.drawRoundedRect(titleRect.adjusted(2, 1, -2, -1), 6, 6);
        }
        QFont font(QStringLiteral("Fira Sans"));
        font.setPixelSize(15);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        if (naming) {
            QString shown = screen.renameText;
            if (screen.renameSelectAll && !shown.isEmpty()) {
                const double advance = QFontMetrics(font).horizontalAdvance(shown);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(70, 130, 220, 170));
                painter.drawRoundedRect(QRectF(titleRect.left() + 8, titleRect.top() + 4, advance + 6, titleRect.height() - 8), 3, 3);
            } else {
                shown.insert(screen.caret, (now_ / 500) % 2 ? QStringLiteral("|") : QStringLiteral(" "));
            }
            painter.setPen(Qt::white);
            painter.drawText(titleRect.adjusted(10, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, shown);
        } else {
            painter.setPen(QColor(255, 255, 255, titleHot ? 235 : 210));
            painter.drawText(titleRect.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, title);
        }
        auto chip = [&](const QRectF &rect, bool hot) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(hot ? QColor(255, 255, 255, 36) : QColor(255, 255, 255, 16));
            painter.drawRoundedRect(rect, 7, 7);
        };
        if (!backSource.isEmpty()) {
            const QRectF back = backSource.translated(0, rise);
            const bool hot = screen.pointer && backSource.contains(screen.x, screen.y);
            chip(back, hot);
            painter.setPen(QPen(QColor(255, 255, 255, 230), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF c = back.center();
            painter.drawLine(c + QPointF(3, -4), c + QPointF(-3, 0));
            painter.drawLine(c + QPointF(-3, 0), c + QPointF(3, 4));
        }
        if (!manageSource.isEmpty()) {
            const QRectF gear = manageSource.translated(0, rise);
            const bool hot = screen.pointer && manageSource.contains(screen.x, screen.y);
            chip(gear, hot);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 220));
            const double r = std::max(1.3, gear.height() * 0.055);
            const double gap = gear.width() * 0.18;
            for (int i = -1; i <= 1; ++i) painter.drawEllipse(QPointF(gear.center().x() + i * gap, gear.center().y()), r, r);
        }
        if (!windows.empty()) {
            QFont caption(QStringLiteral("Fira Sans"));
            caption.setPixelSize(12);
            caption.setWeight(QFont::DemiBold);
            painter.setFont(caption);
            painter.setPen(QColor(255, 255, 255, 150));
            const QRectF captionRect(windows.front().rect.left(), windows.front().rect.top() - 20, 120, 18);
            painter.drawText(captionRect.translated(0, rise), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Open"));
            for (const Screen::Card &card : windows) {
                const bool hover = screen.pointer && card.rect.contains(screen.x, screen.y);
                paintWindowCard(card, rise, hover);
            }
        }
        QFont labelFont(QStringLiteral("Fira Sans"));
        labelFont.setPixelSize(13);
        painter.setFont(labelFont);
        const QFontMetrics metrics(labelFont);
        for (const Screen::Cell &cell : cells) {
            int running = 0;
            bool active = false;
            QStringList watched;
            if (cell.nested) {
                if (const Item *nested = findStack(cell.stackId)) watched = appsIn(*nested);
            } else if (!cell.appId.isEmpty()) {
                watched << cell.appId;
            }
            for (const QString &app : watched) {
                for (const Live &live : live_) {
                    if (!isApp(live, app)) continue;
                    running++;
                    if (live.activated && !live.minimized) active = true;
                }
            }
            const QRectF icon = cell.icon.translated(0, rise);
            const bool lifted = screen.dragging && screen.pressCell &&
                                ((cell.nested && screen.pressUid == QLatin1String("nest:") + cell.stackId) ||
                                 (!cell.nested && !screen.pressApp.isEmpty() && cell.appId == screen.pressApp));
            const bool reorderHot = screen.dragging && screen.pressCell && !screen.mergeReady &&
                                    ((cell.nested && screen.hoverUid == QLatin1String("nest:") + cell.stackId) ||
                                     (!cell.nested && screen.hoverUid == QLatin1String("cell:") + cell.appId));
            const bool openedBeside = cell.nested && !screen.sideStack.isEmpty() && cell.stackId == screen.sideStack &&
                                      stackId == screen.shownStack;
            if (openedBeside) {
                painter.setPen(QPen(QColor(255, 196, 64, 230), 2));
                painter.setBrush(QColor(255, 196, 64, 28));
                painter.drawRoundedRect(cell.rect.translated(0, rise).adjusted(8, 6, -8, -8), 12, 12);
            }
            if (screen.mergeReady && cell.nested && screen.hoverUid == QLatin1String("stack:") + cell.stackId) {
                painter.setPen(QPen(QColor(120, 186, 255, 220), 2));
                painter.setBrush(QColor(120, 186, 255, 36));
                painter.drawRoundedRect(cell.rect.translated(0, rise).adjusted(8, 6, -8, -8), 12, 12);
            } else if (reorderHot) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 28));
                painter.drawRoundedRect(cell.rect.translated(0, rise).adjusted(8, 6, -8, -8), 12, 12);
            }
            painter.save();
            if (lifted) painter.setOpacity(painter.opacity() * 0.45);
            if (cell.nested) {
                const Item *nested = findStack(cell.stackId);
                QList<QPixmap> icons;
                if (nested) {
                    const QStringList apps = appsIn(*nested);
                    const int shown = std::min(4, static_cast<int>(apps.size()));
                    for (int n = 0; n < shown; ++n) icons.append(iconFor(apps.at(n), 24));
                }
                bool deeper = false;
                if (nested) {
                    for (const Item &child : nested->children)
                        if (child.kind == Item::Kind::Stack) deeper = true;
                }
                drawStackFolder(painter, icon, icons, deeper);
            } else {
                drawLiftedIcon(painter, iconFor(cell.appId, qRound(48 * screen.dpr)), icon);
            }
            if (running > 0) {
                const QColor color = active ? QColor(52, 199, 89) : QColor(46, 168, 86);
                drawCountBadge(painter, icon, color, running, cell.nested || running > 1, edge_);
            }
            painter.setPen(QColor(255, 255, 255, 215));
            painter.setFont(labelFont);
            const QString label = metrics.elidedText(cell.label, Qt::ElideRight, static_cast<int>(cell.rect.width() - 8));
            painter.drawText(QRectF(cell.rect.left(), cell.icon.bottom() + 16, cell.rect.width(), 18).translated(0, rise),
                             Qt::AlignHCenter | Qt::AlignTop, label);
            painter.restore();
        }
        frame(pop, kPopupRadius);
        if (!closeSource.isEmpty()) dismiss(closeSource.translated(0, (1.0 - open) * 8));
        painter.restore();
    };
    drawStackPopup(screen.popup, screen.popupRect, screen.titleRect, screen.stackBack, screen.stackManage, screen.popupClose,
                   screen.shownStack, screen.cells, screen.stackWindows);
    drawStackPopup(std::max(screen.side, screen.sideStack.isEmpty() ? 0.0 : 0.35), screen.sideRect, screen.sideTitle, QRectF(),
                   screen.sideManage, screen.sideClose, screen.shownSide, screen.sideCells, screen.sideWindows);

    if (screen.previews > 0.03 && !screen.previewsRect.isEmpty()) {
        painter.save();
        painter.setOpacity(screen.previews);
        const QRectF pop = screen.previewsRect.translated(0, (1.0 - screen.previews) * 8);
        const double dy = (1.0 - screen.previews) * 8;
        glass(pop, kPopupRadius, 220);
        for (int i = 0; i < static_cast<int>(screen.cards.size()); ++i)
            paintWindowCard(screen.cards[static_cast<size_t>(i)], dy, i == screen.hoverCard);
        frame(pop, kPopupRadius);
        dismiss(screen.previewsClose.translated(0, dy));
        painter.restore();
    }

    if (screen.menu && !screen.menuRect.isEmpty()) {
        auto panel = [&](const QRectF &rect) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 70));
            painter.drawRoundedRect(rect.translated(0, 4).adjusted(2, 0, -2, 2), kPopupRadius, kPopupRadius);
            painter.setBrush(QColor(28, 29, 34, 236));
            painter.drawRoundedRect(rect, kPopupRadius, kPopupRadius);
        };
        panel(screen.menuRect);
        const bool menuDrop = screen.menuUid == QLatin1String("trash") &&
                              (screen.fileDrag || (screen.dragging && screen.hoverUid == QLatin1String("trash"))) &&
                              (screen.menuRect.contains(screen.x, screen.y) || screen.hoverUid == QLatin1String("trash"));
        if (menuDrop) {
            painter.setPen(QPen(QColor(255, 90, 78, 230), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(screen.menuRect.adjusted(3, 3, -3, -3), kPopupRadius, kPopupRadius);
        }
        QFont font(QStringLiteral("Fira Sans"));
        font.setPixelSize(15);
        painter.setFont(font);
        auto paintItem = [&](const MenuRow &row, const QRectF &rect, bool hover) {
            if (row.action == QLatin1String("-")) {
                painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
                painter.drawLine(QPointF(rect.left() + 10, rect.center().y()), QPointF(rect.right() - 10, rect.center().y()));
                return;
            }
            if (hover && row.enabled) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 34));
                painter.drawRoundedRect(rect.adjusted(2, 1, -2, -1), 8, 8);
            }
            if (screen.rename && row.header) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 18));
                painter.drawRoundedRect(rect.adjusted(6, 4, -6, -4), 6, 6);
                QString shown = screen.renameText;
                if (screen.renameSelectAll && !shown.isEmpty()) {
                    const double advance = QFontMetrics(font).horizontalAdvance(shown);
                    painter.setBrush(QColor(70, 130, 220, 170));
                    painter.drawRoundedRect(QRectF(rect.left() + 12, rect.top() + 6, advance + 6, rect.height() - 12), 3, 3);
                } else {
                    shown.insert(screen.caret, (now_ / 500) % 2 ? QStringLiteral("|") : QStringLiteral(" "));
                }
                painter.setPen(Qt::white);
                painter.setFont(font);
                painter.drawText(rect.adjusted(14, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, shown);
                return;
            }
            const QColor ink = row.enabled ? QColor(255, 255, 255, row.header ? 168 : 235) : QColor(255, 255, 255, 120);
            QFont use = font;
            use.setWeight(row.header ? QFont::DemiBold : QFont::Normal);
            painter.setFont(use);
            double textLeft = row.header ? 14 : 36;
            if (row.toggle) {
                const QRectF mark(rect.left() + 11, rect.center().y() - 7, 14, 14);
                painter.setPen(QPen(QColor(255, 255, 255, row.checked ? 230 : 80), 1.4));
                painter.setBrush(row.checked ? QColor(52, 199, 89) : Qt::NoBrush);
                painter.drawEllipse(mark);
            } else if (!row.header) {
                const QRectF icon(rect.left() + 10, rect.center().y() - 8, 16, 16);
                drawMenuIcon(painter, icon, menuIconKind(row.action), ink);
            }
            painter.setPen(ink);
            painter.setFont(use);
            const bool more = menuContinues(row);
            const double rightPad = more ? 28 : 12;
            const QString shown = QFontMetrics(use).elidedText(row.label, Qt::ElideRight,
                                                               std::max(8, static_cast<int>(rect.width() - textLeft - rightPad)));
            painter.drawText(rect.adjusted(textLeft, 0, -rightPad, 0), Qt::AlignVCenter | Qt::AlignLeft, shown);
            if (more) {
                const QRectF arrow(rect.right() - 24, rect.center().y() - 8, 16, 16);
                drawMenuIcon(painter, arrow, QStringLiteral("forward"), ink);
            }
        };
        for (int i = 0; i < static_cast<int>(screen.rows.size()) && i < static_cast<int>(screen.menuRows.size()); ++i) {
            const bool hover = i == screen.menuRow;
            paintItem(screen.rows[static_cast<size_t>(i)], screen.menuRows[static_cast<size_t>(i)], hover);
        }
        if (!screen.subRect.isEmpty() && screen.menuRow >= 0 && screen.menuRow < static_cast<int>(screen.rows.size())) {
            panel(screen.subRect);
            const MenuRow &parent = screen.rows[static_cast<size_t>(screen.menuRow)];
            for (int i = 0; i < static_cast<int>(parent.sub.size()) && i < static_cast<int>(screen.subRows.size()); ++i) {
                paintItem(parent.sub[static_cast<size_t>(i)], screen.subRows[static_cast<size_t>(i)], i == screen.subRow);
            }
            frame(screen.subRect, kPopupRadius);
            dismiss(screen.subClose);
        }
        frame(screen.menuRect, kPopupRadius);
        dismiss(screen.menuClose);
    }

    if (screen.search && !screen.searchRect.isEmpty()) {
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 70));
        painter.drawRoundedRect(screen.searchRect.translated(0, 4).adjusted(2, 0, -2, 2), kPopupRadius, kPopupRadius);
        painter.setBrush(QColor(28, 29, 34, 242));
        painter.drawRoundedRect(screen.searchRect, kPopupRadius, kPopupRadius);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 16));
        painter.drawRoundedRect(screen.searchField, 8, 8);
        QFont font(QStringLiteral("Fira Sans"));
        font.setPixelSize(15);
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
            drawPixmapFit(painter, iconFor(row.appId, qRound(24 * screen.dpr)), row.icon);
            painter.setPen(QColor(255, 255, 255, 230));
            painter.setFont(font);
            const QString label = metrics.elidedText(row.label, Qt::ElideRight, static_cast<int>(row.rect.width() - 48));
            painter.drawText(QRectF(row.icon.right() + 10, row.rect.top(), row.rect.width() - 48, row.rect.height()),
                             Qt::AlignVCenter | Qt::AlignLeft, label);
        }
        frame(screen.searchRect, kPopupRadius);
        dismiss(screen.searchClose);
        painter.restore();
    }

    if (screen.about && !screen.aboutRect.isEmpty()) {
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 70));
        painter.drawRoundedRect(screen.aboutRect.translated(0, 4).adjusted(2, 0, -2, 2), kPopupRadius, kPopupRadius);
        painter.setBrush(QColor(28, 29, 34, 242));
        painter.drawRoundedRect(screen.aboutRect, kPopupRadius, kPopupRadius);
        QFont title(QStringLiteral("Fira Sans"));
        title.setPixelSize(18);
        title.setWeight(QFont::DemiBold);
        painter.setFont(title);
        painter.setPen(QColor(255, 255, 255, 235));
        painter.drawText(QRectF(screen.aboutRect.left() + 18, screen.aboutRect.top() + 16, screen.aboutRect.width() - 56, 28),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("MAGIC Dock"));
        QFont body(QStringLiteral("Fira Sans"));
        body.setPixelSize(15);
        painter.setFont(body);
        painter.setPen(QColor(255, 255, 255, 190));
        painter.drawText(QRectF(screen.aboutRect.left() + 18, screen.aboutRect.top() + 48, screen.aboutRect.width() - 36, 22),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("A dock for the COSMIC desktop."));
        painter.drawText(QRectF(screen.aboutRect.left() + 18, screen.aboutRect.top() + 72, screen.aboutRect.width() - 36, 22),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Mira, The White Witch"));
        const bool coffeeHover = screen.aboutCoffee.contains(screen.x, screen.y);
        painter.setPen(Qt::NoPen);
        painter.setBrush(coffeeHover ? QColor(255, 255, 255, 28) : QColor(255, 255, 255, 12));
        painter.drawRoundedRect(screen.aboutCoffee, 8, 8);
        painter.setPen(QColor(255, 255, 255, 230));
        painter.setFont(body);
        painter.drawText(screen.aboutCoffee, Qt::AlignCenter, QStringLiteral("Buy me a coffee"));
        frame(screen.aboutRect, kPopupRadius);
        dismiss(screen.aboutClose);
        painter.restore();
    }

    screen.meterCard = QRectF();
    screen.meterClose = QRectF();
    screen.grokCard = QRectF();
    screen.grokClose = QRectF();
    screen.grokTabGrok = QRectF();
    screen.grokTabBots = QRectF();
    screen.grokLaunch = QRectF();
    if (!screen.menu && !screen.search && !screen.about && screen.openStack.isEmpty() && screen.openPreviews.isEmpty() &&
        !screen.tipUid.isEmpty() && now_ - screen.tipSince > 280) {
        const Slot *slot = nullptr;
        for (const Slot &candidate : screen.slots)
            if (candidate.uid == screen.tipUid) slot = &candidate;
        if (slot && slot->kind == Item::Kind::TaskManager) {
            const double width = 260;
            const double height = 132;
            QPointF anchor = slot->rect.center();
            if (edge_ == ShellSpec::Edge::Bottom) anchor.setY(slot->rect.top());
            else if (edge_ == ShellSpec::Edge::Top) anchor.setY(slot->rect.bottom());
            else if (edge_ == ShellSpec::Edge::Left) anchor.setX(slot->rect.right());
            else anchor.setX(slot->rect.left());
            const QRectF card = popupNear(screen, anchor, width, height);
            screen.meterCard = card;
            screen.meterClose = closeBox(card);
            glass(card, kPopupRadius, 226);
            QFont title(QStringLiteral("Fira Sans"));
            title.setPixelSize(16);
            title.setWeight(QFont::DemiBold);
            painter.setFont(title);
            painter.setPen(QColor(255, 255, 255, 235));
            painter.drawText(QRectF(card.left() + 16, card.top() + 12, card.width() - 56, 22), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("System Monitor"));
            QFont rowFont(QStringLiteral("Fira Sans"));
            rowFont.setPixelSize(14);
            painter.setFont(rowFont);
            auto row = [&](double y, const QString &label, double fraction, bool ready) {
                painter.setPen(QColor(255, 255, 255, 210));
                painter.drawText(QRectF(card.left() + 16, y, 70, 20), Qt::AlignLeft | Qt::AlignVCenter, label);
                const QRectF bar(card.left() + 90, y + 5, 100, 10);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 28));
                painter.drawRoundedRect(bar, 5, 5);
                if (ready) {
                    const double f = std::clamp(fraction, 0.0, 1.0);
                    if (f >= 0.008) {
                        painter.setBrush(usageColor(f));
                        painter.drawRoundedRect(QRectF(bar.left(), bar.top(), std::max(2.0, bar.width() * f), bar.height()), 5, 5);
                    }
                }
                painter.setPen(QColor(255, 255, 255, 230));
                const QString pct = ready ? QString::number(qRound(std::clamp(fraction, 0.0, 1.0) * 100.0)) + QStringLiteral("%")
                                          : QStringLiteral("—");
                painter.drawText(QRectF(bar.right() + 8, y, 50, 20), Qt::AlignRight | Qt::AlignVCenter, pct);
            };
            row(card.top() + 40, QStringLiteral("CPU"), cpu_, cpuReady_);
            row(card.top() + 64, QStringLiteral("Memory"), mem_, memTotal_ > 0);
            painter.setPen(QColor(255, 255, 255, 160));
            const QString detail = memTotal_ > 0 ? QStringLiteral("%1 of %2").arg(formatBytes(memUsed_), formatBytes(memTotal_))
                                                 : QStringLiteral("Reading memory");
            painter.drawText(QRectF(card.left() + 16, card.top() + 92, card.width() - 32, 22), Qt::AlignLeft | Qt::AlignVCenter, detail);
            frame(card, kPopupRadius);
            dismiss(screen.meterClose);
        } else if (slot && slot->kind == Item::Kind::Grok) {
            struct ProductLine {
                QString name;
                int used = -1;
            };
            QList<ProductLine> products;
            for (const QString &row : creditProducts_) {
                ProductLine line;
                line.name = row;
                const int space = row.lastIndexOf(QLatin1Char(' '));
                if (space > 0 && row.endsWith(QLatin1Char('%'))) {
                    bool ok = false;
                    const int used = row.mid(space + 1, row.size() - space - 2).toInt(&ok);
                    if (ok) {
                        line.name = row.left(space);
                        line.used = std::clamp(used, 0, 100);
                    }
                }
                products.push_back(line);
                if (products.size() == 3) break;
            }
            const int productCount = products.size();
            const int page = screen.grokPage == 1 ? 1 : 0;
            QList<GrokBot> desk;
            for (const GrokBot &bot : grokBots_) {
                if (bot.running) desk.push_back(bot);
            }
            for (const GrokBot &bot : grokBots_) {
                if (!bot.running) desk.push_back(bot);
            }
            if (desk.isEmpty()) {
                for (const QString &name : runningBots_) {
                    GrokBot bot;
                    bot.name = name;
                    bot.running = true;
                    desk.push_back(bot);
                }
            }
            const int botCap = 6;
            const int extraBots = std::max(0, static_cast<int>(desk.size()) - botCap);
            if (desk.size() > botCap) desk = desk.mid(0, botCap);
            const bool showPrepaid = !creditPrepaid_.isEmpty();
            const bool showDetail = creditProducts_.isEmpty() && !creditDetail_.isEmpty();
            const bool showMeta = showPrepaid || showDetail || productCount > 0;
            const bool quietBots = botsKnown_ && desk.isEmpty();
            const bool showWriter = botsKnown_ && (grokWriter_ == QLatin1String("up") || grokWriter_ == QLatin1String("down"));
            const bool launch = grogyOnPath_;
            const double width = 340;
            double height = 12 + 34 + 12;
            if (page == 0) {
                height += 54;
                if (showMeta) {
                    height += 8;
                    if (showPrepaid) height += 22;
                    if (showDetail) height += 22;
                    if (productCount > 0) height += 20 + productCount * 24 + 4;
                }
            } else {
                height += 22;
                if (!botsKnown_) {
                    height += 22;
                } else if (quietBots) {
                    height += 46;
                } else {
                    if (showWriter) height += 22;
                    for (const GrokBot &bot : desk) height += bot.note.isEmpty() ? 28.0 : 46.0;
                    if (extraBots > 0) height += 18;
                }
            }
            if (launch) height += 12 + 36;
            height += 14;
            QPointF anchor = slot->rect.center();
            if (edge_ == ShellSpec::Edge::Bottom) anchor.setY(slot->rect.top());
            else if (edge_ == ShellSpec::Edge::Top) anchor.setY(slot->rect.bottom());
            else if (edge_ == ShellSpec::Edge::Left) anchor.setX(slot->rect.right());
            else anchor.setX(slot->rect.left());
            const QRectF card = popupNear(screen, anchor, width, height);
            screen.grokCard = card;
            screen.grokClose = closeBox(card);
            glass(card, kPopupRadius, 230);

            const QRectF tabs(card.left() + 12, card.top() + 12, card.width() - 12 - kCloseInset - kCloseSize - 8, 34);
            const double half = tabs.width() / 2.0;
            screen.grokTabGrok = QRectF(tabs.left(), tabs.top(), half, tabs.height());
            screen.grokTabBots = QRectF(tabs.left() + half, tabs.top(), tabs.right() - (tabs.left() + half), tabs.height());
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 14));
            painter.drawRoundedRect(tabs, 10, 10);
            const bool hoverGrok = page != 0 && screen.grokTabGrok.contains(screen.x, screen.y);
            const bool hoverBots = page != 1 && screen.grokTabBots.contains(screen.x, screen.y);
            auto tabFill = [&](const QRectF &rect, int alpha) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, alpha));
                painter.drawRoundedRect(rect.adjusted(3, 3, -3, -3), 8, 8);
            };
            tabFill(page == 0 ? screen.grokTabGrok : screen.grokTabBots, 34);
            if (hoverGrok) tabFill(screen.grokTabGrok, 18);
            if (hoverBots) tabFill(screen.grokTabBots, 18);
            QFont tabFont(QStringLiteral("Fira Sans"));
            tabFont.setPixelSize(14);
            tabFont.setWeight(QFont::DemiBold);
            painter.setFont(tabFont);
            auto tabLabel = [&](const QRectF &rect, const QString &text, bool on) {
                painter.setPen(QColor(255, 255, 255, on ? 240 : 150));
                painter.drawText(rect, Qt::AlignCenter, text);
            };
            tabLabel(screen.grokTabGrok, QStringLiteral("Summary"), page == 0);
            tabLabel(screen.grokTabBots, QStringLiteral("Bots"), page == 1);

            double y = tabs.bottom() + 12;
            QFont body(QStringLiteral("Fira Sans"));
            body.setPixelSize(14);
            QFont caption(QStringLiteral("Fira Sans"));
            caption.setPixelSize(12);
            caption.setWeight(QFont::DemiBold);
            caption.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
            auto bodyLine = [&](const QString &text, int alpha = 210) {
                painter.setPen(QColor(255, 255, 255, alpha));
                painter.setFont(body);
                const QString shown = QFontMetrics(body).elidedText(text, Qt::ElideRight, static_cast<int>(card.width() - 36));
                painter.drawText(QRectF(card.left() + 18, y, card.width() - 36, 22), Qt::AlignLeft | Qt::AlignVCenter, shown);
                y += 22;
            };
            auto section = [&](const QString &label, const QString &aside) {
                painter.setFont(caption);
                painter.setPen(QColor(255, 255, 255, 140));
                painter.drawText(QRectF(card.left() + 18, y, 140, 18), Qt::AlignLeft | Qt::AlignVCenter, label);
                if (!aside.isEmpty()) {
                    painter.drawText(QRectF(card.right() - 150, y, 132, 18), Qt::AlignRight | Qt::AlignVCenter, aside);
                }
                y += 22;
            };
            if (page == 0) {
                const QColor accent = creditOk_ ? creditColor(creditLeft_) : QColor(176, 180, 188);
                const QRectF ring(card.left() + 16, y, 44, 44);
                drawCreditRing(painter, ring, creditLeft_, creditOk_);
                QFont title(QStringLiteral("Fira Sans"));
                title.setPixelSize(17);
                title.setWeight(QFont::DemiBold);
                painter.setFont(title);
                painter.setPen(QColor(255, 255, 255, 240));
                painter.drawText(QRectF(ring.right() + 12, y, card.width() - 160, 24), Qt::AlignLeft | Qt::AlignVCenter,
                                 QStringLiteral("Summary"));
                QFont sub = body;
                painter.setFont(sub);
                painter.setPen(QColor(255, 255, 255, 185));
                const QString summary = allowanceLine(creditOk_, creditLeft_, creditLine_);
                const QString summaryShown =
                    QFontMetrics(sub).elidedText(summary, Qt::ElideRight, static_cast<int>(card.width() - 168));
                painter.drawText(QRectF(ring.right() + 12, y + 24, card.width() - 168, 20), Qt::AlignLeft | Qt::AlignVCenter,
                                 summaryShown);
                if (creditOk_) {
                    QFont pct = title;
                    pct.setPixelSize(20);
                    painter.setFont(pct);
                    painter.setPen(accent);
                    painter.drawText(QRectF(card.right() - 86, y + 4, 70, 28), Qt::AlignRight | Qt::AlignVCenter,
                                     QString::number(creditLeft_) + QStringLiteral("%"));
                }
                y += 54;
                if (showMeta) {
                    painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
                    painter.drawLine(QPointF(card.left() + 16, y), QPointF(card.right() - 16, y));
                    y += 8;
                    if (showPrepaid) bodyLine(QStringLiteral("Prepaid %1").arg(creditPrepaid_));
                    if (showDetail) bodyLine(creditDetail_);
                    if (productCount > 0) {
                        section(QStringLiteral("USAGE"), QString());
                        for (const ProductLine &line : products) {
                            painter.setFont(body);
                            painter.setPen(QColor(255, 255, 255, 220));
                            const QString name = QFontMetrics(body).elidedText(line.name, Qt::ElideRight, 118);
                            painter.drawText(QRectF(card.left() + 18, y, 120, 22), Qt::AlignLeft | Qt::AlignVCenter, name);
                            const QRectF bar(card.left() + 144, y + 7, 86, 8);
                            painter.setPen(Qt::NoPen);
                            painter.setBrush(QColor(255, 255, 255, 28));
                            painter.drawRoundedRect(bar, 4, 4);
                            if (line.used >= 0) {
                                const double f = std::clamp(line.used / 100.0, 0.0, 1.0);
                                if (f >= 0.008) {
                                    painter.setBrush(usageColor(f));
                                    painter.drawRoundedRect(
                                        QRectF(bar.left(), bar.top(), std::max(2.0, bar.width() * f), bar.height()), 4, 4);
                                }
                            }
                            painter.setPen(QColor(255, 255, 255, 210));
                            painter.setFont(body);
                            const QString pct = line.used >= 0 ? QString::number(line.used) + QStringLiteral("%") : QString();
                            painter.drawText(QRectF(bar.right() + 8, y, 48, 22), Qt::AlignRight | Qt::AlignVCenter, pct);
                            y += 24;
                        }
                    }
                }
            } else {
                const int runningCount = runningBots_.size();
                const QString botAside = !botsKnown_ ? QString()
                                         : runningCount == 0 ? QStringLiteral("quiet")
                                         : runningCount == 1 ? QStringLiteral("1 running")
                                                             : QStringLiteral("%1 running").arg(runningCount);
                section(QStringLiteral("GROK BOTS"), botAside);
                if (!botsKnown_) {
                    bodyLine(QStringLiteral("Checking which bots are running"), 170);
                } else if (desk.isEmpty()) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(255, 255, 255, 90));
                    painter.drawEllipse(QRectF(card.left() + 20, y + 8, 8, 8));
                    painter.setFont(body);
                    painter.setPen(QColor(255, 255, 255, 200));
                    painter.drawText(QRectF(card.left() + 36, y, card.width() - 54, 24), Qt::AlignLeft | Qt::AlignVCenter,
                                     QStringLiteral("Quiet"));
                    y += 24;
                    bodyLine(QStringLiteral("No Grok bots are on the desk."), 160);
                } else {
                    if (showWriter) {
                        bodyLine(grokWriter_ == QLatin1String("up") ? QStringLiteral("Writer is up")
                                                                    : QStringLiteral("Writer is down"),
                                 grokWriter_ == QLatin1String("up") ? 210 : 170);
                    }
                    QFont noteFont = body;
                    noteFont.setPixelSize(12);
                    for (const GrokBot &bot : desk) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(bot.running ? QColor(52, 199, 89) : QColor(255, 255, 255, 70));
                        painter.drawEllipse(QRectF(card.left() + 20, y + 8, 8, 8));
                        painter.setFont(body);
                        painter.setPen(QColor(255, 255, 255, bot.running ? 230 : 180));
                        const int roleW = bot.role.isEmpty() ? 0 : 96;
                        const QString name = QFontMetrics(body).elidedText(bot.name, Qt::ElideRight,
                                                                           static_cast<int>(card.width() - 58 - roleW));
                        painter.drawText(QRectF(card.left() + 36, y, card.width() - 54 - roleW, 24),
                                         Qt::AlignLeft | Qt::AlignVCenter, name);
                        if (!bot.role.isEmpty()) {
                            painter.setPen(QColor(255, 255, 255, 140));
                            const QString role = QFontMetrics(body).elidedText(bot.role, Qt::ElideRight, roleW - 4);
                            painter.drawText(QRectF(card.right() - 18 - roleW, y, roleW, 24), Qt::AlignRight | Qt::AlignVCenter,
                                             role);
                        }
                        y += 28;
                        if (!bot.note.isEmpty()) {
                            painter.setFont(noteFont);
                            painter.setPen(QColor(255, 255, 255, 160));
                            const QString note = QFontMetrics(noteFont).elidedText(bot.note, Qt::ElideRight,
                                                                                   static_cast<int>(card.width() - 54));
                            painter.drawText(QRectF(card.left() + 36, y, card.width() - 54, 18), Qt::AlignLeft | Qt::AlignVCenter,
                                             note);
                            y += 18;
                        }
                    }
                    if (extraBots > 0) {
                        painter.setFont(body);
                        painter.setPen(QColor(255, 255, 255, 150));
                        painter.drawText(QRectF(card.left() + 36, y, card.width() - 54, 18), Qt::AlignLeft | Qt::AlignVCenter,
                                         QStringLiteral("+ %1 more").arg(extraBots));
                        y += 18;
                    }
                }
            }

            screen.grokLaunch = QRectF();
            if (launch) {
                const QRectF button(card.left() + 14, card.bottom() - 50, card.width() - 28, 36);
                screen.grokLaunch = button;
                const bool hot = button.contains(screen.x, screen.y);
                painter.setPen(Qt::NoPen);
                painter.setBrush(hot ? QColor(255, 255, 255, 40) : QColor(255, 255, 255, 18));
                painter.drawRoundedRect(button, 9, 9);
                const QColor markInk(255, 255, 255, 230);
                drawMenuIcon(painter, QRectF(button.left() + 12, button.center().y() - 8, 16, 16), QStringLiteral("ring"),
                             markInk);
                QFont buttonFont = body;
                buttonFont.setWeight(QFont::DemiBold);
                painter.setFont(buttonFont);
                painter.setPen(markInk);
                painter.drawText(button.adjusted(36, 0, -28, 0), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Open Grogy"));
                drawMenuIcon(painter, QRectF(button.right() - 28, button.center().y() - 8, 16, 16), QStringLiteral("forward"),
                             markInk);
            }
            frame(card, kPopupRadius);
            dismiss(screen.grokClose);
        } else if (slot) {
            QString tip = slot->label;
            if (slot->kind == Item::Kind::Trash) {
                if (trashCount_ == 1) tip = QStringLiteral("Trash — 1 item");
                else if (trashCount_ > 1) tip = QStringLiteral("Trash — %1 items").arg(trashCount_);
            } else if (slot->running > 0) {
                tip += slot->running == 1 ? QStringLiteral(" — open") : QStringLiteral(" — %1 windows").arg(slot->running);
            }
            QFont font(QStringLiteral("Fira Sans"));
            font.setPixelSize(14);
            font.setWeight(QFont::Medium);
            painter.setFont(font);
            const QFontMetrics metrics(font);
            const double width = metrics.horizontalAdvance(tip) + 22;
            const double height = 28;
            QRectF tipRect;
            if (horizontal()) {
                double x = std::clamp(slot->rect.center().x() - width / 2, kScreenMargin,
                                      std::max(kScreenMargin, screen.w - width - kScreenMargin));
                const double y = edge_ == ShellSpec::Edge::Top ? slot->rect.bottom() + 8 : slot->rect.top() - 8 - height;
                tipRect = QRectF(x, y, width, height);
            } else {
                double y = std::clamp(slot->rect.center().y() - height / 2, kScreenMargin,
                                      std::max(kScreenMargin, screen.h - height - kScreenMargin));
                const double x = edge_ == ShellSpec::Edge::Left ? slot->rect.right() + 8 : slot->rect.left() - 8 - width;
                tipRect = QRectF(x, y, width, height);
            }
            if (tipRect.top() < kScreenMargin) tipRect.moveTop(kScreenMargin);
            if (tipRect.bottom() > screen.h - kScreenMargin) tipRect.moveBottom(screen.h - kScreenMargin);
            if (tipRect.left() < kScreenMargin) tipRect.moveLeft(kScreenMargin);
            if (tipRect.right() > screen.w - kScreenMargin) tipRect.moveRight(screen.w - kScreenMargin);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(20, 20, 24, 225));
            painter.drawRoundedRect(tipRect, 8, 8);
            painter.setPen(Qt::white);
            painter.drawText(tipRect, Qt::AlignCenter, tip);
            frame(tipRect, 8);
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
        current.meterCard = QRectF();
        current.meterClose = QRectF();
        current.grokCard = QRectF();
        current.grokClose = QRectF();
        current.grokTabGrok = QRectF();
        current.grokTabBots = QRectF();
        current.grokLaunch = QRectF();
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
    // Sample frames must not rewrite the pin file. A dock without the demo
    // Social stack still gets one in memory so the pictures can be drawn.
    persist_ = false;
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
            socialApps = appsIn(item);
        }
    }
    if (!social || socialApps.size() < 2) {
        Item demo;
        demo.kind = Item::Kind::Stack;
        demo.id = QStringLiteral("social");
        demo.name = QStringLiteral("Social");
        for (const QString &id : {QStringLiteral("tweety"), QStringLiteral("libresky")}) {
            Item app;
            app.kind = Item::Kind::App;
            const QString resolved = apps_->resolve(id);
            app.id = resolved.isEmpty() ? id : resolved;
            demo.children.push_back(app);
        }
        if (demo.children.size() >= 2) {
            items_.insert(items_.begin(), demo);
            socialApps = appsIn(demo);
            social = socialApps.size() >= 2;
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
    persist_ = false;
    grokInstalled_ = true;
    grogyOnPath_ = true;
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
        const Slot *trash = nullptr;
        const Slot *grok = nullptr;
        const Slot *task = nullptr;
        const Slot *more = nullptr;
        for (const Slot &slot : screen(1).slots) {
            if (slot.uid == QLatin1String("launcher")) launcher = &slot;
            if (slot.uid == QLatin1String("applibrary")) library = &slot;
            if (slot.uid == QLatin1String("search")) search = &slot;
            if (slot.uid == QLatin1String("trash")) trash = &slot;
            if (slot.uid == QLatin1String("grok")) grok = &slot;
            if (slot.uid == QLatin1String("taskman")) task = &slot;
            if (slot.uid == QLatin1String("overflow")) more = &slot;
        }
        const auto after = [&](const Slot *a, const Slot *b) {
            if (!a || !b) return false;
            return expect.vertical ? a->rect.center().y() > b->rect.center().y() + 8
                                   : a->rect.center().x() > b->rect.center().x() + 8;
        };
        const bool order = launcher && task && trash && grok &&
                           (expect.vertical ? task->rect.center().y() > launcher->rect.center().y() + 40
                                            : task->rect.center().x() > launcher->rect.center().x() + 40);
        const bool wings = after(search, library) && after(grok, trash) && after(task, grok) && after(more, task);
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
    const int reserve = static_cast<int>(std::lround(barThickness() + kEdgeGuard));
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
    openMenu(menuScreen, QStringLiteral("trash"), {});
    bool sawOpen = false;
    bool sawEmpty = false;
    bool sawHint = false;
    for (const MenuRow &row : menuScreen.rows) {
        if (row.action == QLatin1String("open-trash")) sawOpen = true;
        if (row.action == QLatin1String("empty-trash") && !row.enabled) sawEmpty = true;
        if (row.action == QLatin1String("drop-hint")) sawHint = true;
    }
    if (!sawOpen || !sawEmpty || !sawHint) {
        std::fprintf(stderr, "magic-dock: trash menu self-test failed\n");
        return 1;
    }
    openMenu(menuScreen, QStringLiteral("app:demo-app"), QStringLiteral("demo-app"));
    bool sawSubArrow = false;
    for (const MenuRow &row : menuScreen.rows) {
        if (row.action == QLatin1String("add-submenu") && menuContinues(row)) sawSubArrow = true;
        if (row.action == QLatin1String("manage") && menuContinues(row)) sawSubArrow = true;
    }
    if (!sawSubArrow) {
        std::fprintf(stderr, "magic-dock: add-to-stack row has no submenu arrow\n");
        return 1;
    }

    usagePoll_ = 0;
    trashPoll_ = now_;
    if (!pollMeters() && memTotal_ == 0) {
        std::fprintf(stderr, "magic-dock: memory sample failed\n");
        return 1;
    }
    if (memTotal_ == 0 || mem_ < 0 || mem_ > 1) {
        std::fprintf(stderr, "magic-dock: memory sample out of range\n");
        return 1;
    }

    trashScan_ = 0;
    refreshTrashDirs();
    auto binListed = [&](const QByteArray &files) {
        char resolved[PATH_MAX];
        if (::realpath(files.constData(), resolved) == nullptr) return true;
        return trashDirs_.contains(QString::fromLocal8Bit(resolved));
    };
    if (!binListed(trashFilesDir().toLocal8Bit())) {
        std::fprintf(stderr, "magic-dock: home trash bin missing from scan\n");
        return 1;
    }
    QFile mountTable(QStringLiteral("/proc/mounts"));
    if (mountTable.open(QIODevice::ReadOnly)) {
        const QByteArray table = mountTable.readAll();
        const QByteArray uid = QByteArray::number(static_cast<uint>(::geteuid()));
        int lineStart = 0;
        while (lineStart < table.size()) {
            int lineEnd = table.indexOf('\n', lineStart);
            if (lineEnd < 0) lineEnd = table.size();
            const QByteArray line = table.mid(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;
            const int sp1 = line.indexOf(' ');
            const int sp2 = sp1 < 0 ? -1 : line.indexOf(' ', sp1 + 1);
            if (sp2 < 0) continue;
            const int sp3 = line.indexOf(' ', sp2 + 1);
            const QByteArray mount = unescapeMount(line.mid(sp1 + 1, sp2 - sp1 - 1));
            const QByteArray type = sp3 < 0 ? line.mid(sp2 + 1) : line.mid(sp2 + 1, sp3 - sp2 - 1);
            if (mount.isEmpty() || mount.at(0) != '/' || virtualTrashFs(type)) continue;
            const QByteArray dot = mount + "/.Trash";
            const QByteArray files = stickyTrashDir(dot) ? dot + "/" + uid + "/files" : mount + "/.Trash-" + uid + "/files";
            if (!binListed(files)) {
                std::fprintf(stderr, "magic-dock: trash bin missing from scan: %s\n", files.constData());
                return 1;
            }
        }
    }

    edge_ = ShellSpec::Edge::Bottom;
    outputMode_ = QStringLiteral("All");
    expand_ = true;
    hideMode_ = Hide::Never;
    iconSize_ = 40;
    screens_.clear();
    cpu_ = 0.95;
    mem_ = 0.12;
    cpuReady_ = true;
    memTotal_ = 16ull * 1024 * 1024 * 1024;
    memUsed_ = static_cast<quint64>(0.12 * static_cast<double>(memTotal_));
    trashCount_ = 5;
    Live running;
    running.key = 4;
    running.appId = QStringLiteral("demo-app");
    running.rawId = running.appId;
    running.activated = true;
    running.outputs = {1};
    live_.push_back(running);
    Live second = running;
    second.key = 5;
    second.activated = false;
    live_.push_back(second);
    QImage meters(960, 300, QImage::Format_ARGB32_Premultiplied);
    meters.fill(Qt::transparent);
    {
        QPainter painter(&meters);
        painter.setRenderHint(QPainter::Antialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 960;
        geom.logicalH = 300;
        geom.scale = 1;
        geom.name = QStringLiteral("TEST");
        paint(1, painter, geom);
    }
    const Slot *trashSlot = nullptr;
    const Slot *taskSlot = nullptr;
    const Slot *appSlot = nullptr;
    for (const Slot &slot : screen(1).slots) {
        if (slot.uid == QLatin1String("trash")) trashSlot = &slot;
        if (slot.uid == QLatin1String("taskman")) taskSlot = &slot;
        if (slot.uid == QLatin1String("app:demo-app")) appSlot = &slot;
    }
    if (!trashSlot || !taskSlot || !appSlot) {
        std::fprintf(stderr, "magic-dock: meter layout missing a slot\n");
        return 1;
    }
    auto colored = [&](const QRectF &rect, auto &&pred) {
        int hits = 0;
        const int x0 = std::max(0, static_cast<int>(std::floor(rect.left())) - 4);
        const int y0 = std::max(0, static_cast<int>(std::floor(rect.top())) - 6);
        const int x1 = std::min(meters.width(), static_cast<int>(std::ceil(rect.right())) + 8);
        const int y1 = std::min(meters.height(), static_cast<int>(std::ceil(rect.bottom())) + 6);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const QColor color = meters.pixelColor(x, y);
                if (color.alpha() < 40) continue;
                if (pred(color)) hits++;
            }
        }
        return hits;
    };
    const int trashRed = colored(trashSlot->rect, [](const QColor &color) {
        return color.red() > 200 && color.green() < 120 && color.blue() < 110;
    });
    const int cpuRed = colored(taskSlot->rect, [](const QColor &color) {
        return color.red() > 200 && color.green() < 140 && color.blue() < 120;
    });
    const int memGreen = colored(taskSlot->rect, [](const QColor &color) {
        return color.green() > 140 && color.red() < 180 && color.green() > color.red();
    });
    const int badgeGreen = colored(appSlot->rect.adjusted(-4, -8, 8, 4), [](const QColor &color) {
        return color.green() > 150 && color.red() < 120 && color.blue() < 160;
    });
    if (trashRed < 4 || cpuRed < 4 || memGreen < 2 || badgeGreen < 4) {
        std::fprintf(stderr, "magic-dock: meter paint failed (trash=%d cpu=%d mem=%d badge=%d)\n", trashRed, cpuRed, memGreen,
                     badgeGreen);
        return 1;
    }
    if (!acceptsFileDrop(1, trashSlot->rect.center().x(), trashSlot->rect.center().y()) ||
        acceptsFileDrop(1, 4, 4)) {
        std::fprintf(stderr, "magic-dock: trash drop target failed\n");
        return 1;
    }

    const QPointF from = appSlot->rect.center();
    const QPointF to = trashSlot->rect.center();
    auto motion = [&](const QPointF &point) {
        PointerEv ev;
        ev.type = PointerEv::Type::Motion;
        ev.outputId = 1;
        ev.x = point.x();
        ev.y = point.y();
        pointer(ev);
    };
    auto button = [&](bool pressed) {
        PointerEv ev;
        ev.type = PointerEv::Type::Button;
        ev.outputId = 1;
        ev.button = BTN_LEFT;
        ev.pressed = pressed;
        pointer(ev);
    };
    motion(from);
    button(true);
    motion(to);
    button(false);
    if (itemIndex(QStringLiteral("app:demo-app")) >= 0) {
        std::fprintf(stderr, "magic-dock: drop on trash did not remove the pin\n");
        return 1;
    }

    Item solo;
    solo.kind = Item::Kind::Stack;
    solo.id = QStringLiteral("solo");
    solo.name = QStringLiteral("Solo");
    Item soloApp;
    soloApp.kind = Item::Kind::App;
    soloApp.id = QStringLiteral("demo-app");
    solo.children.push_back(soloApp);
    items_.push_back(solo);
    execs_.insert(canonical(QStringLiteral("demo-app")), QStringLiteral("/bin/true"));
    desktopDirForTest_ = QDir::tempPath() + QStringLiteral("/magic-dock-desktop-test");
    QDir(desktopDirForTest_).removeRecursively();
    QDir().mkpath(desktopDirForTest_);
    auto finishDesktop = [&]() {
        QDir(desktopDirForTest_).removeRecursively();
        desktopDirForTest_.clear();
    };
    QImage folder(960, 400, QImage::Format_ARGB32_Premultiplied);
    folder.fill(Qt::transparent);
    {
        QPainter painter(&folder);
        painter.setRenderHint(QPainter::Antialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 960;
        geom.logicalH = 400;
        geom.scale = 1;
        geom.name = QStringLiteral("TEST");
        paint(1, painter, geom);
    }
    const Slot *soloSlot = nullptr;
    for (const Slot &slot : screen(1).slots) {
        if (slot.uid == QLatin1String("stack:solo")) soloSlot = &slot;
    }
    int yellow = 0;
    if (soloSlot) {
        const int x0 = std::max(0, static_cast<int>(std::floor(soloSlot->rect.left())));
        const int y0 = std::max(0, static_cast<int>(std::floor(soloSlot->rect.top())));
        const int x1 = std::min(folder.width(), static_cast<int>(std::ceil(soloSlot->rect.right())));
        const int y1 = std::min(folder.height(), static_cast<int>(std::ceil(soloSlot->rect.bottom())));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const QColor color = folder.pixelColor(x, y);
                if (color.alpha() < 80) continue;
                if (color.red() > 220 && color.green() > 170 && color.blue() < 180 && color.red() > color.blue() + 50)
                    yellow++;
            }
        }
    }
    if (!soloSlot || yellow < 40) {
        std::fprintf(stderr, "magic-dock: stack folder did not draw (%d)\n", yellow);
        finishDesktop();
        return 1;
    }

    Screen &stackScreen = screen(1);
    openMenu(stackScreen, QStringLiteral("stack:solo"), QStringLiteral("demo-app"));
    bool sawUnpin = false;
    bool sawRemove = false;
    bool sawDesktop = false;
    for (const MenuRow &row : stackScreen.rows) {
        if (row.action == QLatin1String("unpin")) sawUnpin = true;
        if (row.action == QLatin1String("remove-from-stack")) sawRemove = true;
        if (row.action == QLatin1String("send-desktop")) sawDesktop = true;
    }
    if (!sawUnpin || !sawRemove || !sawDesktop) {
        std::fprintf(stderr, "magic-dock: stack item menu missing unpin, remove, or send\n");
        finishDesktop();
        return 1;
    }
    runAction(stackScreen, QStringLiteral("send-desktop"), QStringLiteral("demo-app"));
    const QString launcher = desktopDirForTest_ + QStringLiteral("/demo-app.desktop");
    QFile written(launcher);
    const QString launcherText = written.open(QIODevice::ReadOnly) ? QString::fromUtf8(written.readAll()) : QString();
    if (!QFileInfo(launcher).isExecutable() || !launcherText.contains(QStringLiteral("Name=demo-app")) ||
        !launcherText.contains(QStringLiteral("Exec=/bin/true"))) {
        std::fprintf(stderr, "magic-dock: send to desktop failed (%s)\n", qPrintable(launcherText.left(180)));
        finishDesktop();
        return 1;
    }
    finishDesktop();

    stackScreen.openStack = QStringLiteral("solo");
    stackScreen.shownStack = QStringLiteral("solo");
    stackScreen.popup = 1;
    stackScreen.menu = false;
    stackScreen.w = 960;
    stackScreen.h = 400;
    stackScreen.hidden = 0;
    relayout(stackScreen);
    if (stackScreen.titleRect.isEmpty()) {
        std::fprintf(stderr, "magic-dock: stack title was not laid out\n");
        return 1;
    }
    motion(stackScreen.titleRect.center());
    button(true);
    button(false);
    button(true);
    button(false);
    if (!stackScreen.rename || stackScreen.renameStack != QLatin1String("solo") || !stackScreen.renameSelectAll) {
        std::fprintf(stderr, "magic-dock: double-click did not rename the stack\n");
        return 1;
    }
    applyKey(stackScreen, KEY_Z, true);
    applyKey(stackScreen, KEY_ENTER, false);
    bool renamed = false;
    for (const Item &item : items_) {
        if (item.kind == Item::Kind::Stack && item.id == QLatin1String("solo") && item.name == QLatin1String("Z"))
            renamed = true;
    }
    if (stackScreen.rename || !renamed) {
        std::fprintf(stderr, "magic-dock: stack rename did not commit\n");
        return 1;
    }

    openMenu(stackScreen, QStringLiteral("stack:solo"), QStringLiteral("demo-app"));
    runAction(stackScreen, QStringLiteral("unpin"), QStringLiteral("demo-app"));
    if (itemIndex(QStringLiteral("stack:solo")) >= 0 || itemIndex(QStringLiteral("app:demo-app")) >= 0) {
        std::fprintf(stderr, "magic-dock: unpin inside a stack left the app on the dock\n");
        return 1;
    }

    Item again;
    again.kind = Item::Kind::Stack;
    again.id = QStringLiteral("solo");
    again.name = QStringLiteral("Solo");
    Item againApp;
    againApp.kind = Item::Kind::App;
    againApp.id = QStringLiteral("demo-app");
    again.children.push_back(againApp);
    items_.push_back(again);
    openMenu(stackScreen, QStringLiteral("stack:solo"), QStringLiteral("demo-app"));
    runAction(stackScreen, QStringLiteral("remove-from-stack"), QStringLiteral("demo-app"));
    if (itemIndex(QStringLiteral("stack:solo")) >= 0 || itemIndex(QStringLiteral("app:demo-app")) < 0) {
        std::fprintf(stderr, "magic-dock: remove from stack did not leave a pin\n");
        return 1;
    }

    items_.clear();
    Item pile;
    pile.kind = Item::Kind::Stack;
    pile.id = QStringLiteral("pile");
    pile.name = QStringLiteral("Pile");
    Item pileA;
    pileA.kind = Item::Kind::App;
    pileA.id = QStringLiteral("demo-app");
    Item pileB;
    pileB.kind = Item::Kind::App;
    pileB.id = QStringLiteral("other-app");
    pile.children.push_back(pileA);
    pile.children.push_back(pileB);
    items_.push_back(pile);
    openMenu(stackScreen, QStringLiteral("stack:pile"), QStringLiteral("pile"));
    bool sawManage = false;
    bool sawUngroup = false;
    for (const MenuRow &row : stackScreen.rows) {
        if (row.action == QLatin1String("manage")) sawManage = true;
        if (row.action == QLatin1String("ungroup") || row.action == QLatin1String("split-now")) sawUngroup = true;
    }
    if (!sawManage || sawUngroup) {
        std::fprintf(stderr, "magic-dock: stack menu should offer Stack and not split immediately\n");
        return 1;
    }
    runAction(stackScreen, QStringLiteral("manage"), QStringLiteral("pile"));
    bool sawAskSplit = false;
    bool sawAskUnpin = false;
    for (const MenuRow &row : stackScreen.rows) {
        if (row.action == QLatin1String("ask-split")) sawAskSplit = true;
        if (row.action == QLatin1String("ask-unpin-stack")) sawAskUnpin = true;
    }
    if (!stackScreen.menu || !sawAskSplit || !sawAskUnpin) {
        std::fprintf(stderr, "magic-dock: stack page missing split or unpin\n");
        return 1;
    }
    runAction(stackScreen, QStringLiteral("ask-split"), QStringLiteral("pile"));
    runAction(stackScreen, QStringLiteral("split-now"), QStringLiteral("pile"));
    if (itemIndex(QStringLiteral("stack:pile")) >= 0 || itemIndex(QStringLiteral("app:demo-app")) < 0 ||
        itemIndex(QStringLiteral("app:other-app")) < 0) {
        std::fprintf(stderr, "magic-dock: split did not put both apps on the dock\n");
        return 1;
    }

    items_.clear();
    items_.push_back(pile);
    QImage pileFrame(960, 300, QImage::Format_ARGB32_Premultiplied);
    pileFrame.fill(Qt::transparent);
    {
        QPainter painter(&pileFrame);
        painter.setRenderHint(QPainter::Antialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 960;
        geom.logicalH = 300;
        geom.scale = 1;
        geom.name = QStringLiteral("TEST");
        paint(1, painter, geom);
    }
    const Slot *pileSlot = nullptr;
    const Slot *trashForPile = nullptr;
    for (const Slot &slot : screen(1).slots) {
        if (slot.uid == QLatin1String("stack:pile")) pileSlot = &slot;
        if (slot.uid == QLatin1String("trash")) trashForPile = &slot;
    }
    if (!pileSlot || !trashForPile) {
        std::fprintf(stderr, "magic-dock: pile or trash missing before the confirm drop\n");
        return 1;
    }
    motion(pileSlot->rect.center());
    button(true);
    motion(trashForPile->rect.center());
    button(false);
    if (itemIndex(QStringLiteral("stack:pile")) < 0) {
        std::fprintf(stderr, "magic-dock: dropping a stack on trash removed it without asking\n");
        return 1;
    }
    bool sawUnpinNow = false;
    for (const MenuRow &row : screen(1).rows) {
        if (row.action == QLatin1String("unpin-stack-now")) sawUnpinNow = true;
    }
    if (!sawUnpinNow) {
        std::fprintf(stderr, "magic-dock: stack drop did not ask to unpin\n");
        return 1;
    }
    runAction(screen(1), QStringLiteral("unpin-stack-now"), QStringLiteral("pile"));
    if (itemIndex(QStringLiteral("stack:pile")) >= 0 || itemIndex(QStringLiteral("app:demo-app")) >= 0) {
        std::fprintf(stderr, "magic-dock: unpin stack left something on the dock\n");
        return 1;
    }

    Item outer;
    outer.kind = Item::Kind::Stack;
    outer.id = QStringLiteral("outer");
    outer.name = QStringLiteral("Outer");
    Item inner;
    inner.kind = Item::Kind::Stack;
    inner.id = QStringLiteral("inner");
    inner.name = QStringLiteral("Inner");
    Item deep;
    deep.kind = Item::Kind::App;
    deep.id = QStringLiteral("demo-app");
    inner.children.push_back(deep);
    outer.children.push_back(inner);
    Item loose;
    loose.kind = Item::Kind::Stack;
    loose.id = QStringLiteral("loose");
    loose.name = QStringLiteral("Loose");
    Item looseApp;
    looseApp.kind = Item::Kind::App;
    looseApp.id = QStringLiteral("other-app");
    loose.children.push_back(looseApp);
    items_.clear();
    items_.push_back(outer);
    items_.push_back(loose);
    mergeItems(QStringLiteral("stack:loose"), QStringLiteral("stack:outer"));
    const Item *outerNow = findStack(QStringLiteral("outer"));
    bool nestedLoose = false;
    if (outerNow) {
        for (const Item &child : outerNow->children) {
            if (child.kind == Item::Kind::Stack && child.id == QLatin1String("loose")) nestedLoose = true;
        }
    }
    if (!nestedLoose || itemIndex(QStringLiteral("stack:loose")) >= 0 || !holdsApp(*outerNow, QStringLiteral("other-app"))) {
        std::fprintf(stderr, "magic-dock: dropping a stack on a stack flattened it\n");
        return 1;
    }
    stackScreen.openStack = QStringLiteral("outer");
    stackScreen.shownStack = QStringLiteral("outer");
    stackScreen.popup = 1;
    stackScreen.w = 960;
    stackScreen.h = 400;
    relayout(stackScreen);
    bool sawInner = false;
    bool sawLoose = false;
    for (const Screen::Cell &cell : stackScreen.cells) {
        if (cell.nested && cell.stackId == QLatin1String("inner")) sawInner = true;
        if (cell.nested && cell.stackId == QLatin1String("loose")) sawLoose = true;
    }
    if (!sawInner || !sawLoose || stackScreen.stackManage.isEmpty() || !stackScreen.stackBack.isEmpty()) {
        std::fprintf(stderr, "magic-dock: outer stack did not show nested stacks\n");
        return 1;
    }
    const Screen::Cell *innerCell = nullptr;
    for (const Screen::Cell &cell : stackScreen.cells) {
        if (cell.nested && cell.stackId == QLatin1String("inner")) innerCell = &cell;
    }
    if (!innerCell) {
        std::fprintf(stderr, "magic-dock: nested stack cell missing\n");
        return 1;
    }
    motion(innerCell->rect.center());
    button(true);
    button(false);
    relayout(stackScreen);
    bool sideHasDemo = false;
    for (const Screen::Cell &cell : stackScreen.sideCells) {
        if (cell.appId == QLatin1String("demo-app")) sideHasDemo = true;
    }
    const bool beside = !stackScreen.sideRect.isEmpty() && !stackScreen.popupRect.intersects(stackScreen.sideRect) &&
                        std::abs(stackScreen.sideRect.center().x() - stackScreen.popupRect.center().x()) > 40.0;
    if (stackScreen.openStack != QLatin1String("outer") || stackScreen.sideStack != QLatin1String("inner") || !sideHasDemo ||
        !beside) {
        std::fprintf(stderr, "magic-dock: clicking a nested stack did not open it beside the parent\n");
        return 1;
    }
    stackScreen.sideStack.clear();
    stackScreen.shownSide.clear();
    stackScreen.openStack = QStringLiteral("inner");
    stackScreen.shownStack = QStringLiteral("inner");
    relayout(stackScreen);
    if (stackScreen.cells.size() != 1 || stackScreen.cells[0].nested || stackScreen.cells[0].appId != QLatin1String("demo-app") ||
        stackScreen.stackBack.isEmpty() || parentStackId(QStringLiteral("inner")) != QLatin1String("outer")) {
        std::fprintf(stderr, "magic-dock: nested stack did not open\n");
        return 1;
    }
    stackScreen.openStack = QStringLiteral("outer");
    stackScreen.shownStack = QStringLiteral("outer");
    pullNested(stackScreen, QStringLiteral("loose"));
    if (itemIndex(QStringLiteral("stack:loose")) < 0 || !findStack(QStringLiteral("inner"))) {
        std::fprintf(stderr, "magic-dock: pulling a nested stack out failed\n");
        return 1;
    }

    items_.clear();
    items_.push_back(outer);
    items_.push_back(loose);
    screens_.clear();
    {
        QImage frame(960, 400, QImage::Format_ARGB32_Premultiplied);
        frame.fill(Qt::transparent);
        QPainter painter(&frame);
        painter.setRenderHint(QPainter::Antialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 960;
        geom.logicalH = 400;
        geom.scale = 1;
        geom.name = QStringLiteral("TEST");
        paint(1, painter, geom);
    }
    const Slot *outerSlot = nullptr;
    const Slot *looseSlot = nullptr;
    for (const Slot &slot : screen(1).slots) {
        if (slot.uid == QLatin1String("stack:outer")) outerSlot = &slot;
        if (slot.uid == QLatin1String("stack:loose")) looseSlot = &slot;
    }
    if (!outerSlot || !looseSlot) {
        std::fprintf(stderr, "magic-dock: stack drag targets were not laid out\n");
        return 1;
    }
    motion(looseSlot->rect.center());
    button(true);
    motion(outerSlot->rect.center());
    button(false);
    const Item *dropped = findStack(QStringLiteral("outer"));
    bool droppedLoose = false;
    if (dropped) {
        for (const Item &child : dropped->children) {
            if (child.kind == Item::Kind::Stack && child.id == QLatin1String("loose")) droppedLoose = true;
        }
    }
    if (!droppedLoose || itemIndex(QStringLiteral("stack:loose")) >= 0) {
        std::fprintf(stderr, "magic-dock: dragging a stack onto a stack did not nest it\n");
        return 1;
    }
    Item extra;
    extra.kind = Item::Kind::Stack;
    extra.id = QStringLiteral("extra");
    extra.name = QStringLiteral("Extra");
    Item extraApp;
    extraApp.kind = Item::Kind::App;
    extraApp.id = QStringLiteral("third-app");
    extra.children.push_back(extraApp);
    items_.push_back(extra);
    Screen &nestScreen = screen(1);
    nestScreen.openStack = QStringLiteral("outer");
    nestScreen.shownStack = QStringLiteral("outer");
    nestScreen.popup = 1;
    nestScreen.menu = false;
    nestScreen.w = 960;
    nestScreen.h = 400;
    nestScreen.hidden = 0;
    {
        QImage frame(960, 400, QImage::Format_ARGB32_Premultiplied);
        frame.fill(Qt::transparent);
        QPainter painter(&frame);
        painter.setRenderHint(QPainter::Antialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 960;
        geom.logicalH = 400;
        geom.scale = 1;
        geom.name = QStringLiteral("TEST");
        paint(1, painter, geom);
    }
    const Slot *extraSlot = nullptr;
    for (const Slot &slot : screen(1).slots)
        if (slot.uid == QLatin1String("stack:extra")) extraSlot = &slot;
    if (!extraSlot || screen(1).popupRect.isEmpty() || screen(1).titleRect.isEmpty()) {
        std::fprintf(stderr, "magic-dock: open stack was not ready for a drop\n");
        return 1;
    }
    motion(extraSlot->rect.center());
    button(true);
    motion(screen(1).titleRect.center());
    button(false);
    const Item *afterDrop = findStack(QStringLiteral("outer"));
    bool droppedExtra = false;
    if (afterDrop) {
        for (const Item &child : afterDrop->children) {
            if (child.kind == Item::Kind::Stack && child.id == QLatin1String("extra")) droppedExtra = true;
        }
    }
    if (!droppedExtra || itemIndex(QStringLiteral("stack:extra")) >= 0 || nestScreen.openStack != QLatin1String("outer")) {
        std::fprintf(stderr, "magic-dock: dropping a stack into an open stack did not nest it\n");
        return 1;
    }

    edge_ = ShellSpec::Edge::Bottom;
    items_.clear();
    Item home;
    home.kind = Item::Kind::Stack;
    home.id = QStringLiteral("home");
    home.name = QStringLiteral("Home");
    Item kept;
    kept.kind = Item::Kind::App;
    kept.id = QStringLiteral("demo-app");
    Item nestFolder;
    nestFolder.kind = Item::Kind::Stack;
    nestFolder.id = QStringLiteral("folder");
    nestFolder.name = QStringLiteral("Folder");
    Item inside;
    inside.kind = Item::Kind::App;
    inside.id = QStringLiteral("other-app");
    nestFolder.children.push_back(inside);
    home.children.push_back(kept);
    home.children.push_back(nestFolder);
    items_.push_back(home);
    Screen &page = screen(1);
    page.openStack = QStringLiteral("home");
    page.shownStack = QStringLiteral("home");
    page.popup = 1;
    page.menu = false;
    page.w = 960;
    page.h = 400;
    page.hidden = 0;
    page.pointer = true;
    relayout(page);
    const Screen::Cell *keptCell = nullptr;
    const Screen::Cell *folderCell = nullptr;
    for (const Screen::Cell &cell : page.cells) {
        if (!cell.nested && cell.appId == QLatin1String("demo-app")) keptCell = &cell;
        if (cell.nested && cell.stackId == QLatin1String("folder")) folderCell = &cell;
    }
    if (!keptCell || !folderCell) {
        std::fprintf(stderr, "magic-dock: open stack cells were not laid out\n");
        return 1;
    }
    const QRectF keptRect = keptCell->rect;
    const QRectF folderRect = folderCell->rect;
    motion(folderRect.center());
    button(true);
    motion(QPointF(keptRect.center().x(), keptRect.bottom() - 6));
    button(false);
    const Item *reordered = findStack(QStringLiteral("home"));
    if (!reordered || reordered->children.size() != 2 || reordered->children[0].id != QLatin1String("folder") ||
        reordered->children[1].id != QLatin1String("demo-app")) {
        std::fprintf(stderr, "magic-dock: dragging inside an open stack did not reorder it\n");
        return 1;
    }
    relayout(page);
    keptCell = nullptr;
    folderCell = nullptr;
    for (const Screen::Cell &cell : page.cells) {
        if (!cell.nested && cell.appId == QLatin1String("demo-app")) keptCell = &cell;
        if (cell.nested && cell.stackId == QLatin1String("folder")) folderCell = &cell;
    }
    if (!keptCell || !folderCell) {
        std::fprintf(stderr, "magic-dock: reordered stack cells were not laid out\n");
        return 1;
    }
    const QRectF appRect = keptCell->rect;
    const QRectF nestIcon = folderCell->icon;
    motion(appRect.center());
    button(true);
    motion(nestIcon.center());
    button(false);
    const Item *tucked = findStack(QStringLiteral("folder"));
    const Item *homeNow = findStack(QStringLiteral("home"));
    const bool holdsDemo = tucked && holdsApp(*tucked, QStringLiteral("demo-app"));
    const bool homeOnlyFolder = homeNow && homeNow->children.size() == 1 && homeNow->children[0].id == QLatin1String("folder");
    if (!holdsDemo || !homeOnlyFolder) {
        std::fprintf(stderr, "magic-dock: dropping onto a nested stack did not move the app inside\n");
        return 1;
    }
    page.openStack = QStringLiteral("folder");
    page.shownStack = QStringLiteral("folder");
    relayout(page);
    if (page.cells.size() != 2 || page.stackBack.isEmpty() || page.titleRect.isEmpty()) {
        std::fprintf(stderr, "magic-dock: opened nested stack did not show both apps\n");
        return 1;
    }
    QRectF demoRect;
    for (const Screen::Cell &cell : page.cells) {
        if (cell.appId == QLatin1String("demo-app")) demoRect = cell.rect;
    }
    if (demoRect.isEmpty()) {
        std::fprintf(stderr, "magic-dock: nested stack hid the dropped app\n");
        return 1;
    }
    motion(demoRect.center());
    button(true);
    motion(QPointF(page.popupRect.center().x(), page.popupRect.bottom() + 40));
    button(false);
    const Item *folderNow = findStack(QStringLiteral("folder"));
    if (itemIndex(QStringLiteral("app:demo-app")) < 0 || !folderNow || holdsApp(*folderNow, QStringLiteral("demo-app"))) {
        std::fprintf(stderr, "magic-dock: dragging out of a nested stack did not lift the app\n");
        return 1;
    }

    items_.clear();
    Item shelf;
    shelf.kind = Item::Kind::Stack;
    shelf.id = QStringLiteral("shelf");
    shelf.name = QStringLiteral("Shelf");
    Item shelfApp;
    shelfApp.kind = Item::Kind::App;
    shelfApp.id = QStringLiteral("demo-app");
    Item drawer;
    drawer.kind = Item::Kind::Stack;
    drawer.id = QStringLiteral("drawer");
    drawer.name = QStringLiteral("Drawer");
    Item drawerApp;
    drawerApp.kind = Item::Kind::App;
    drawerApp.id = QStringLiteral("other-app");
    drawer.children.push_back(drawerApp);
    shelf.children.push_back(shelfApp);
    shelf.children.push_back(drawer);
    items_.push_back(shelf);
    Screen &pair = screen(1);
    pair.openStack = QStringLiteral("shelf");
    pair.shownStack = QStringLiteral("shelf");
    pair.sideStack.clear();
    pair.shownSide.clear();
    pair.popup = 1;
    pair.side = 0;
    pair.menu = false;
    pair.w = 960;
    pair.h = 500;
    pair.hidden = 0;
    pair.pointer = true;
    pair.dragging = false;
    relayout(pair);
    const Screen::Cell *drawerCell = nullptr;
    for (const Screen::Cell &cell : pair.cells) {
        if (cell.nested && cell.stackId == QLatin1String("drawer")) drawerCell = &cell;
    }
    if (!drawerCell) {
        std::fprintf(stderr, "magic-dock: drawer cell missing before the side open\n");
        return 1;
    }
    motion(drawerCell->icon.center());
    button(true);
    button(false);
    relayout(pair);
    QRectF otherRect;
    for (const Screen::Cell &cell : pair.sideCells) {
        if (cell.appId == QLatin1String("other-app")) otherRect = cell.rect;
    }
    const bool sideBySide = pair.openStack == QLatin1String("shelf") && pair.sideStack == QLatin1String("drawer") &&
                            !otherRect.isEmpty() && !pair.sideRect.intersects(pair.popupRect) &&
                            std::abs(pair.sideRect.center().x() - pair.popupRect.center().x()) > 40.0;
    if (!sideBySide) {
        std::fprintf(stderr, "magic-dock: nested stack did not open to the side\n");
        return 1;
    }
    motion(otherRect.center());
    button(true);
    motion(pair.titleRect.center());
    button(false);
    const Item *shelfNow = findStack(QStringLiteral("shelf"));
    const Item *drawerNow = findStack(QStringLiteral("drawer"));
    bool otherOnShelf = false;
    if (shelfNow) {
        for (const Item &child : shelfNow->children) {
            if (child.kind == Item::Kind::App && child.id == QLatin1String("other-app")) otherOnShelf = true;
        }
    }
    if (!otherOnShelf || (drawerNow && holdsApp(*drawerNow, QStringLiteral("other-app")))) {
        std::fprintf(stderr, "magic-dock: dragging from the side stack into the parent did not move the app\n");
        return 1;
    }

    items_.clear();
    drawer.children.clear();
    drawer.children.push_back(drawerApp);
    shelf.children.clear();
    shelf.children.push_back(shelfApp);
    shelf.children.push_back(drawer);
    items_.push_back(shelf);
    Item bin;
    bin.kind = Item::Kind::Stack;
    bin.id = QStringLiteral("bin");
    bin.name = QStringLiteral("Bin");
    Item binApp;
    binApp.kind = Item::Kind::App;
    binApp.id = QStringLiteral("third-app");
    bin.children.push_back(binApp);
    items_.push_back(bin);
    pair.sideStack.clear();
    pair.shownSide.clear();
    pair.openStack = QStringLiteral("drawer");
    pair.shownStack = QStringLiteral("drawer");
    pair.menu = false;
    openMenu(pair, QStringLiteral("stack:drawer"), QStringLiteral("other-app"));
    int addRow = -1;
    for (int i = 0; i < static_cast<int>(pair.rows.size()); ++i) {
        if (pair.rows[static_cast<size_t>(i)].action == QLatin1String("add-submenu")) addRow = i;
    }
    if (addRow < 0) {
        std::fprintf(stderr, "magic-dock: add to stack row missing inside a nested stack\n");
        return 1;
    }
    pair.menuRow = addRow;
    relayout(pair);
    int parentRow = -1;
    int binRow = -1;
    if (addRow < static_cast<int>(pair.rows.size())) {
        const std::vector<MenuRow> &sub = pair.rows[static_cast<size_t>(addRow)].sub;
        for (int i = 0; i < static_cast<int>(sub.size()); ++i) {
            if (sub[static_cast<size_t>(i)].action == QLatin1String("add-to:shelf")) parentRow = i;
            if (sub[static_cast<size_t>(i)].action == QLatin1String("add-to:bin")) binRow = i;
        }
    }
    if (parentRow < 0 || parentRow >= static_cast<int>(pair.subRows.size())) {
        std::fprintf(stderr, "magic-dock: parent stack missing from the add-to menu\n");
        return 1;
    }
    motion(pair.subRows[static_cast<size_t>(parentRow)].center());
    button(true);
    button(false);
    const Item *shelfMoved = findStack(QStringLiteral("shelf"));
    const Item *drawerMoved = findStack(QStringLiteral("drawer"));
    bool lifted = false;
    if (shelfMoved) {
        for (const Item &child : shelfMoved->children) {
            if (child.kind == Item::Kind::App && child.id == QLatin1String("other-app")) lifted = true;
        }
    }
    if (!lifted || (drawerMoved && holdsApp(*drawerMoved, QStringLiteral("other-app")))) {
        std::fprintf(stderr, "magic-dock: add to stack did not move an app up to the parent stack\n");
        return 1;
    }
    openMenu(pair, QStringLiteral("stack:shelf"), QStringLiteral("other-app"));
    addRow = -1;
    for (int i = 0; i < static_cast<int>(pair.rows.size()); ++i) {
        if (pair.rows[static_cast<size_t>(i)].action == QLatin1String("add-submenu")) addRow = i;
    }
    pair.menuRow = addRow;
    relayout(pair);
    binRow = -1;
    if (addRow >= 0 && addRow < static_cast<int>(pair.rows.size())) {
        const std::vector<MenuRow> &sub = pair.rows[static_cast<size_t>(addRow)].sub;
        for (int i = 0; i < static_cast<int>(sub.size()); ++i) {
            if (sub[static_cast<size_t>(i)].action == QLatin1String("add-to:bin")) binRow = i;
        }
    }
    if (binRow < 0 || binRow >= static_cast<int>(pair.subRows.size())) {
        std::fprintf(stderr, "magic-dock: sibling stack missing from the add-to menu\n");
        return 1;
    }
    motion(pair.subRows[static_cast<size_t>(binRow)].center());
    button(true);
    button(false);
    const Item *binNow = findStack(QStringLiteral("bin"));
    const Item *shelfLeft = findStack(QStringLiteral("shelf"));
    bool inBin = false;
    if (binNow) {
        for (const Item &child : binNow->children) {
            if (child.kind == Item::Kind::App && child.id == QLatin1String("other-app")) inBin = true;
        }
    }
    if (!inBin || (shelfLeft && holdsApp(*shelfLeft, QStringLiteral("other-app")))) {
        std::fprintf(stderr, "magic-dock: add to stack did not move an app into the other stack\n");
        return 1;
    }

    showTrash_ = false;
    showGrok_ = false;
    showTask_ = false;
    screens_.clear();
    const QImage hiddenApplets = render(ShellSpec::Edge::Bottom, QStringLiteral("TEST"));
    bool sawHidden = false;
    for (const Slot &slot : screen(1).slots) {
        if (slot.uid == QLatin1String("trash") || slot.uid == QLatin1String("grok") || slot.uid == QLatin1String("taskman"))
            sawHidden = true;
    }
    Q_UNUSED(hiddenApplets);
    if (sawHidden) {
        std::fprintf(stderr, "magic-dock: hidden applets still drew\n");
        return 1;
    }
    showTrash_ = true;
    showGrok_ = true;
    showTask_ = true;

    creditOk_ = true;
    creditLeft_ = 82;
    runningBots_.clear();
    botsKnown_ = true;
    auto ringHits = [&](int percent, auto &&pred) {
        creditLeft_ = percent;
        creditOk_ = true;
        screens_.clear();
        QImage image(960, 300, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 960;
        geom.logicalH = 300;
        geom.scale = 1;
        geom.name = QStringLiteral("TEST");
        paint(1, painter, geom);
        painter.end();
        const Slot *grokSlot = nullptr;
        for (const Slot &slot : screen(1).slots)
            if (slot.uid == QLatin1String("grok")) grokSlot = &slot;
        if (!grokSlot) return 0;
        int hits = 0;
        const int x0 = std::max(0, static_cast<int>(std::floor(grokSlot->rect.left())));
        const int y0 = std::max(0, static_cast<int>(std::floor(grokSlot->rect.top())));
        const int x1 = std::min(image.width(), static_cast<int>(std::ceil(grokSlot->rect.right())) + 8);
        const int y1 = std::min(image.height(), static_cast<int>(std::ceil(grokSlot->rect.bottom())) + 4);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alpha() < 40) continue;
                if (pred(color)) hits++;
            }
        }
        return hits;
    };
    const int greenRing = ringHits(82, [](const QColor &color) {
        return color.green() > 150 && color.red() < 140 && color.blue() < 180 && color.green() > color.red();
    });
    const int orangeRing = ringHits(40, [](const QColor &color) {
        return color.red() > 200 && color.green() > 80 && color.green() < 210 && color.blue() < 80;
    });
    const int redRing = ringHits(8, [](const QColor &color) {
        return color.red() > 200 && color.green() < 120 && color.blue() < 120;
    });
    runningBots_ = {QStringLiteral("Imagine")};
    const int botBadge = ringHits(82, [](const QColor &color) {
        return color.blue() > 180 && color.red() < 140 && color.green() > 80 && color.green() < 220;
    });
    if (greenRing < 8 || orangeRing < 8 || redRing < 8 || botBadge < 4) {
        std::fprintf(stderr, "magic-dock: grok ring failed (green=%d orange=%d red=%d badge=%d)\n", greenRing, orangeRing,
                     redRing, botBadge);
        return 1;
    }
    creditLine_ = QStringLiteral("82% of the week left");
    creditReset_ = QStringLiteral("Sat 31 Oct");
    runningBots_ = {QStringLiteral("Imagine")};
    screens_.clear();
    QImage card(960, 300, QImage::Format_ARGB32_Premultiplied);
    card.fill(Qt::transparent);
    {
        QPainter painter(&card);
        painter.setRenderHint(QPainter::Antialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 960;
        geom.logicalH = 300;
        geom.scale = 1;
        geom.name = QStringLiteral("TEST");
        paint(1, painter, geom);
        Screen &grokScreen = screen(1);
        grokScreen.tipUid = QStringLiteral("grok");
        grokScreen.tipSince = now_ - 1000;
        grokScreen.pointer = true;
        grokScreen.menu = false;
        grokScreen.search = false;
        grokScreen.about = false;
        grokScreen.openStack.clear();
        grokScreen.openPreviews.clear();
        paintScreen(painter, grokScreen);
    }
    if (screen(1).grokCard.isEmpty() || !screen(1).grokCard.contains(screen(1).grokCard.center())) {
        std::fprintf(stderr, "magic-dock: grok overview card did not open\n");
        return 1;
    }
    if (screen(1).grokCard.height() < 160) {
        std::fprintf(stderr, "magic-dock: grok overview card is too small\n");
        return 1;
    }
    const double summaryHeight = screen(1).grokCard.height();
    applyBotBytes(QByteArray("{\"writer\":\"up\",\"bots\":["
                             "{\"name\":\"Imagine\",\"role\":\"Video\",\"running\":true,\"note\":\"Clip is made.\"},"
                             "{\"name\":\"Wire\",\"role\":\"X\",\"running\":false,\"note\":\"Watching the timeline.\"}]}"));
    if (!botsKnown_ || grokWriter_ != QLatin1String("up") || grokBots_.size() != 2 || grokBots_.at(0).role != QLatin1String("Video") ||
        grokBots_.at(0).note.isEmpty() || runningBots_.size() != 1 || runningBots_.at(0) != QLatin1String("Imagine")) {
        std::fprintf(stderr, "magic-dock: bot detail was dropped\n");
        return 1;
    }
    {
        Screen &grokScreen = screen(1);
        grokScreen.grokPage = 1;
        QImage botsCard(960, 300, QImage::Format_ARGB32_Premultiplied);
        botsCard.fill(Qt::transparent);
        QPainter painter(&botsCard);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintScreen(painter, grokScreen);
    }
    if (screen(1).grokCard.height() <= summaryHeight) {
        std::fprintf(stderr, "magic-dock: bots card did not grow to fit the notes\n");
        return 1;
    }
    screen(1).grokPage = 0;
    if (screen(1).grokTabGrok.isEmpty() || screen(1).grokTabBots.isEmpty() ||
        !screen(1).grokCard.contains(screen(1).grokTabGrok.center()) ||
        !screen(1).grokCard.contains(screen(1).grokTabBots.center())) {
        std::fprintf(stderr, "magic-dock: grok card tabs missing\n");
        return 1;
    }
    if (screen(1).grokLaunch.isEmpty() || !screen(1).grokCard.contains(screen(1).grokLaunch.center())) {
        std::fprintf(stderr, "magic-dock: grok card hid Open Grogy\n");
        return 1;
    }
    {
        Screen &grokScreen = screen(1);
        const QPointF gap(grokScreen.grokCard.left() + 18, grokScreen.grokCard.bottom() + 5);
        PointerEv across;
        across.type = PointerEv::Type::Motion;
        across.outputId = 1;
        across.x = gap.x();
        across.y = gap.y();
        pointer(across);
        if (grokScreen.tipUid != QLatin1String("grok")) {
            std::fprintf(stderr, "magic-dock: grok card closed in the gap before the pointer reached it\n");
            return 1;
        }
        const QPointF botsAt = grokScreen.grokTabBots.center();
        across.x = botsAt.x();
        across.y = botsAt.y();
        pointer(across);
        if (grokScreen.tipUid != QLatin1String("grok")) {
            std::fprintf(stderr, "magic-dock: grok card closed while moving onto a tab\n");
            return 1;
        }
        pressAt(grokScreen, botsAt.x(), botsAt.y(), false, false);
        releaseAt(grokScreen, botsAt.x(), botsAt.y());
        if (grokScreen.grokPage != 1) {
            std::fprintf(stderr, "magic-dock: grok bots tab did not open\n");
            return 1;
        }
        const Slot *grokSlot = nullptr;
        for (const Slot &slot : grokScreen.slots)
            if (slot.uid == QLatin1String("grok")) grokSlot = &slot;
        if (!grokSlot) {
            std::fprintf(stderr, "magic-dock: grok slot missing for the click\n");
            return 1;
        }
        qputenv("MAGIC_DOCK_GROGY", QByteArray("/bin/true"));
        probeGrokTools();
        const QString grogyBin = QFileInfo(QStringLiteral("/bin/true")).absoluteFilePath();
        if (!grogyOnPath_ || grogyPath_ != grogyBin) {
            std::fprintf(stderr, "magic-dock: grogy was not remembered (%s)\n", qPrintable(grogyPath_));
            qunsetenv("MAGIC_DOCK_GROGY");
            return 1;
        }
        opening_.remove(QStringLiteral("grogy"));
        across.x = grokSlot->rect.center().x();
        across.y = grokSlot->rect.center().y();
        pointer(across);
        PointerEv click;
        click.type = PointerEv::Type::Button;
        click.outputId = 1;
        click.button = BTN_LEFT;
        click.pressed = true;
        pointer(click);
        click.pressed = false;
        pointer(click);
        if (!opening_.contains(QStringLiteral("grogy"))) {
            std::fprintf(stderr, "magic-dock: one click on Grok did not open grogy\n");
            qunsetenv("MAGIC_DOCK_GROGY");
            return 1;
        }
        qunsetenv("MAGIC_DOCK_GROGY");
        grogyOnPath_ = true;
    }

    Screen &grokMenu = screen(9);
    grogyOnPath_ = false;
    {
        QImage quiet(960, 300, QImage::Format_ARGB32_Premultiplied);
        quiet.fill(Qt::transparent);
        QPainter painter(&quiet);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintScreen(painter, screen(1));
    }
    if (screen(1).grokCard.isEmpty() || !screen(1).grokLaunch.isEmpty() || screen(1).grokTabBots.isEmpty()) {
        std::fprintf(stderr, "magic-dock: grok card needs Grogy to be useful\n");
        return 1;
    }
    openMenu(grokMenu, QStringLiteral("grok"), {});
    bool sawLaunch = false;
    bool sawMissing = false;
    for (const MenuRow &row : grokMenu.rows) {
        if (row.action == QLatin1String("grok") && row.enabled) sawLaunch = true;
        if (row.action == QLatin1String("note") && row.label.contains(QLatin1String("path"))) sawMissing = true;
    }
    if (sawLaunch || !sawMissing) {
        std::fprintf(stderr, "magic-dock: grogy missing still offered a launch\n");
        return 1;
    }
    grogyOnPath_ = true;
    openMenu(grokMenu, QStringLiteral("grok"), {});
    sawLaunch = false;
    for (const MenuRow &row : grokMenu.rows)
        if (row.action == QLatin1String("grok") && row.enabled) sawLaunch = true;
    if (!sawLaunch) {
        std::fprintf(stderr, "magic-dock: grogy on PATH did not offer Open Grogy\n");
        return 1;
    }

    grokInstalled_ = false;
    showGrok_ = true;
    const QImage noGrok = render(ShellSpec::Edge::Bottom, QStringLiteral("TEST"));
    Q_UNUSED(noGrok);
    bool sawGrokSlot = false;
    for (const Slot &slot : screen(1).slots)
        if (slot.uid == QLatin1String("grok")) sawGrokSlot = true;
    Screen &hiddenMenu = screen(9);
    openMenu(hiddenMenu, QStringLiteral("overflow"), {});
    bool sawToggle = false;
    for (const MenuRow &row : hiddenMenu.rows)
        if (row.action == QLatin1String("grok-toggle")) sawToggle = true;
    if (sawGrokSlot || sawToggle) {
        std::fprintf(stderr, "magic-dock: grok applet showed without the Grok CLI\n");
        return 1;
    }

    chromeR_ = 255;
    chromeG_ = 90;
    chromeB_ = 0;
    hintPx_ = 3;
    edge_ = ShellSpec::Edge::Bottom;
    expand_ = true;
    radius_ = 0;
    hideMode_ = Hide::Never;
    Screen &chromeScreen = screen(1);
    chromeScreen.w = 800;
    chromeScreen.h = 500;
    chromeScreen.hidden = 0;
    chromeScreen.menu = false;
    chromeScreen.search = false;
    chromeScreen.about = true;
    chromeScreen.openStack.clear();
    chromeScreen.sideStack.clear();
    chromeScreen.openPreviews.clear();
    chromeScreen.pointer = true;
    QImage chromeFrame(800, 500, QImage::Format_ARGB32_Premultiplied);
    chromeFrame.fill(Qt::transparent);
    {
        QPainter painter(&chromeFrame);
        painter.setRenderHint(QPainter::Antialiasing, true);
        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 800;
        geom.logicalH = 500;
        geom.scale = 1;
        geom.name = QStringLiteral("TEST");
        paint(1, painter, geom);
    }
    const QRectF aboutBox = screen(1).aboutRect;
    const QRectF aboutX = screen(1).aboutClose;
    if (aboutBox.isEmpty() || aboutX.isEmpty() || !aboutBox.contains(aboutX) || aboutBox.left() < kScreenMargin - 0.5 ||
        aboutBox.top() < kScreenMargin - 0.5 || aboutBox.right() > 800 - kScreenMargin + 0.5 ||
        aboutBox.bottom() > 500 - kScreenMargin + 0.5) {
        std::fprintf(stderr, "magic-dock: about popup border is not inset (%.1f,%.1f %.0fx%.0f)\n", aboutBox.left(),
                     aboutBox.top(), aboutBox.width(), aboutBox.height());
        return 1;
    }
    int borderHits = 0;
    auto chromePixel = [&](int x, int y) {
        if (x < 1 || y < 1 || x >= chromeFrame.width() - 1 || y >= chromeFrame.height() - 1) return;
        const QColor color = chromeFrame.pixelColor(x, y);
        if (color.alpha() > 80 && color.red() > 170 && color.red() > color.green() + 40 && color.blue() < 80) borderHits++;
    };
    const int left = qRound(aboutBox.left());
    const int right = qRound(aboutBox.right()) - 1;
    const int top = qRound(aboutBox.top());
    const int bottom = qRound(aboutBox.bottom()) - 1;
    for (int x = left; x <= right; x += 2) {
        for (int band = 1; band <= 4; ++band) {
            chromePixel(x, top + band);
            chromePixel(x, bottom - band);
        }
    }
    for (int y = top; y <= bottom; y += 2) {
        for (int band = 1; band <= 4; ++band) {
            chromePixel(left + band, y);
            chromePixel(right - band, y);
        }
    }
    const QColor outside = chromeFrame.pixelColor(qRound(aboutBox.left()) - 2, qRound(aboutBox.center().y()));
    if (borderHits < 16 || (outside.alpha() > 40 && outside.red() > 170)) {
        std::fprintf(stderr, "magic-dock: popup border missing or clipped (%d)\n", borderHits);
        return 1;
    }
    const QPointF aboutAt = aboutX.center();
    const Hit aboutHit = hitTest(screen(1), aboutAt.x(), aboutAt.y());
    if (aboutHit.type != 16 || aboutHit.action != QLatin1String("about")) {
        std::fprintf(stderr, "magic-dock: about close button missed (type=%d)\n", aboutHit.type);
        return 1;
    }
    pressAt(screen(1), aboutAt.x(), aboutAt.y(), false, false);
    releaseAt(screen(1), aboutAt.x(), aboutAt.y());
    if (screen(1).about) {
        std::fprintf(stderr, "magic-dock: about close button did not close it\n");
        return 1;
    }
    openMenu(screen(1), QStringLiteral("overflow"), {});
    relayout(screen(1));
    if (screen(1).menuClose.isEmpty() || !screen(1).menuRect.contains(screen(1).menuClose) ||
        screen(1).menuRect.top() < kScreenMargin - 0.5) {
        std::fprintf(stderr, "magic-dock: menu close button is outside the menu\n");
        return 1;
    }
    const QPointF menuAt = screen(1).menuClose.center();
    pressAt(screen(1), menuAt.x(), menuAt.y(), false, false);
    releaseAt(screen(1), menuAt.x(), menuAt.y());
    if (screen(1).menu) {
        std::fprintf(stderr, "magic-dock: menu close button did not close it\n");
        return 1;
    }

    // An open stack keeps the accent ring and the window count on its dock
    // icon, lists every open window (including a nested stack and a Qt app
    // id), and stays clear of the bar.
    {
        items_.clear();
        live_.clear();
        screens_.clear();
        margin_ = 0;
        pad_ = 4;
        radius_ = 0;
        iconSize_ = 40;
        expand_ = true;
        hideMode_ = Hide::Never;
        outputMode_ = QStringLiteral("All");
        edge_ = ShellSpec::Edge::Bottom;
        const QString deskDir = QDir::tempPath() + QStringLiteral("/magic-dock-stack-badge");
        QDir(deskDir).removeRecursively();
        QDir().mkpath(deskDir);
        const QString deskFile = deskDir + QStringLiteral("/demo-app.desktop");
        {
            QFile desk(deskFile);
            if (desk.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                desk.write("[Desktop Entry]\nType=Application\nName=Demo App\nExec=/bin/true\nStartupWMClass=demo-app\n");
            }
        }
        apps_->adoptDesktop(deskFile);
        auto cleanupDesk = [&]() { QDir(deskDir).removeRecursively(); };

        Item editors;
        editors.kind = Item::Kind::Stack;
        editors.id = QStringLiteral("editors");
        editors.name = QStringLiteral("Editors");
        Item demo;
        demo.kind = Item::Kind::App;
        demo.id = QStringLiteral("demo-app");
        editors.children.push_back(demo);
        Item browser;
        browser.kind = Item::Kind::App;
        browser.id = QStringLiteral("other-app");
        Item root;
        root.kind = Item::Kind::Stack;
        root.id = QStringLiteral("mz");
        root.name = QStringLiteral("Mz Apps");
        root.children.push_back(editors);
        root.children.push_back(browser);
        items_.push_back(root);

        auto addLive = [&](quint64 key, const QString &appId, const QString &title, bool active, bool minimized) {
            Live live;
            live.key = key;
            live.appId = appId;
            live.rawId = appId;
            live.title = title;
            live.activated = active;
            live.minimized = minimized;
            live.outputs = {1};
            live_.push_back(live);
        };
        addLive(101, QStringLiteral("demo-app"), QStringLiteral("Editor"), true, false);
        addLive(102, QStringLiteral("demo-app"), QStringLiteral("Notes"), false, true);
        addLive(103, QStringLiteral("other-app"), QStringLiteral("Browser"), false, false);
        addLive(104, QStringLiteral("local.demo.demo-app"), QStringLiteral("Alias"), false, false);

        OutputGeom geom;
        geom.id = 1;
        geom.logicalW = 960;
        geom.logicalH = 640;
        geom.scale = 1;
        geom.name = QStringLiteral("TEST");
        auto draw = [&](QImage &image) {
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing, true);
            paint(1, painter, geom);
        };
        QImage closed(960, 640, QImage::Format_ARGB32_Premultiplied);
        draw(closed);
        Screen &stackPage = screen(1);
        const Slot *rootSlot = nullptr;
        for (const Slot &slot : stackPage.slots)
            if (slot.uid == QLatin1String("stack:mz")) rootSlot = &slot;
        if (!rootSlot || rootSlot->running != 4 || !rootSlot->active) {
            std::fprintf(stderr, "magic-dock: stack badge count failed (%d)\n", rootSlot ? rootSlot->running : -1);
            cleanupDesk();
            return 1;
        }
        if (stackPage.pill.bottom() > stackPage.h - kEdgeGuard + 0.5) {
            std::fprintf(stderr, "magic-dock: dock bar is flush with the screen (%.1f)\n", stackPage.pill.bottom());
            cleanupDesk();
            return 1;
        }
        auto greenNear = [&](const QImage &image, const QRectF &icon) {
            int hits = 0;
            const int x0 = std::max(0, static_cast<int>(std::floor(icon.left())) - 4);
            const int y0 = std::max(0, static_cast<int>(std::floor(icon.top())) - 8);
            const int x1 = std::min(image.width(), static_cast<int>(std::ceil(icon.right())) + 10);
            const int y1 = std::min(image.height(), static_cast<int>(std::ceil(icon.bottom())) + 4);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const QColor color = image.pixelColor(x, y);
                    if (color.alpha() < 80) continue;
                    if (color.green() > 150 && color.red() < 140 && color.blue() < 180 && color.green() > color.red() + 30)
                        hits++;
                }
            }
            return hits;
        };
        auto ringHits = [&](const QImage &image, const QRectF &icon) {
            int hits = 0;
            const int y0 = std::max(0, static_cast<int>(std::floor(icon.top())));
            const int y1 = std::min(image.height(), static_cast<int>(std::ceil(icon.bottom())));
            auto column = [&](int x0, int x1) {
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        if (x < 0 || x >= image.width()) continue;
                        const QColor color = image.pixelColor(x, y);
                        if (color.alpha() < 80) continue;
                        if (color.red() > 170 && color.red() > color.green() + 40 && color.blue() < 80) hits++;
                    }
                }
            };
            column(static_cast<int>(std::floor(icon.left())) - 8, static_cast<int>(std::floor(icon.left())));
            column(static_cast<int>(std::ceil(icon.right())), static_cast<int>(std::ceil(icon.right())) + 8);
            return hits;
        };
        const int closedGreen = greenNear(closed, rootSlot->rect);
        const int closedRing = ringHits(closed, rootSlot->rect);
        if (closedGreen < 6 || closedRing > 4) {
            std::fprintf(stderr, "magic-dock: closed stack badge or ring failed (green=%d ring=%d)\n", closedGreen,
                         closedRing);
            cleanupDesk();
            return 1;
        }

        stackPage.openStack = QStringLiteral("mz");
        stackPage.shownStack = QStringLiteral("mz");
        stackPage.popup = 1;
        stackPage.sideStack = QStringLiteral("editors");
        stackPage.shownSide = QStringLiteral("editors");
        stackPage.side = 1;
        stackPage.menu = false;
        stackPage.pointer = false;
        QImage opened(960, 640, QImage::Format_ARGB32_Premultiplied);
        draw(opened);
        rootSlot = nullptr;
        for (const Slot &slot : stackPage.slots)
            if (slot.uid == QLatin1String("stack:mz")) rootSlot = &slot;
        bool sawAlias = false;
        bool sawBrowser = false;
        bool sawMinimized = false;
        bool sawActive = false;
        for (const Screen::Card &card : stackPage.stackWindows) {
            if (card.title.contains(QStringLiteral("Alias"))) sawAlias = true;
            if (card.title.contains(QStringLiteral("Browser"))) sawBrowser = true;
            if (card.minimized) sawMinimized = true;
            if (card.active) sawActive = true;
        }
        int sideDemo = 0;
        for (const Screen::Card &card : stackPage.sideWindows)
            if (card.title.contains(QStringLiteral("Alias")) || card.title.contains(QStringLiteral("Editor")) ||
                card.title.contains(QStringLiteral("Notes")))
                sideDemo++;
        const QRectF popupBox = stackPage.popupRect;
        const bool clearOfDock = !popupBox.isEmpty() && !popupBox.intersects(stackPage.pill) &&
                                 popupBox.bottom() <= stackPage.pill.top() - 8.0 && popupBox.top() >= kScreenMargin - 0.5 &&
                                 popupBox.left() >= kScreenMargin - 0.5;
        bool overlap = false;
        for (const Screen::Card &card : stackPage.stackWindows) {
            for (const Screen::Cell &cell : stackPage.cells)
                if (card.rect.intersects(cell.rect)) overlap = true;
        }
        const Screen::Cell *editorCell = nullptr;
        const Screen::Cell *browserCell = nullptr;
        for (const Screen::Cell &cell : stackPage.cells) {
            if (cell.nested && cell.stackId == QLatin1String("editors")) editorCell = &cell;
            if (!cell.nested && cell.appId == QLatin1String("other-app")) browserCell = &cell;
        }
        const int openRing = rootSlot ? ringHits(opened, rootSlot->rect) : 0;
        const int nestGreen = editorCell ? greenNear(opened, editorCell->icon) : 0;
        const int appGreen = browserCell ? greenNear(opened, browserCell->icon) : 0;
        if (stackPage.stackWindows.size() != 4 || stackPage.sideWindows.size() != 3 || sideDemo != 3 || !sawAlias ||
            !sawBrowser || !sawMinimized || !sawActive || !clearOfDock || overlap || openRing < 8 || nestGreen < 4 ||
            appGreen < 4 || !editorCell || !browserCell) {
            std::fprintf(stderr,
                         "magic-dock: open stack list failed (windows=%zu side=%zu ring=%d nest=%d app=%d clear=%d)\n",
                         stackPage.stackWindows.size(), stackPage.sideWindows.size(), openRing, nestGreen, appGreen,
                         clearOfDock ? 1 : 0);
            cleanupDesk();
            return 1;
        }

        const Screen::Card *closer = nullptr;
        const Screen::Card *focus = nullptr;
        for (const Screen::Card &card : stackPage.stackWindows) {
            if (!closer) closer = &card;
            if (card.key == 103) focus = &card;
        }
        quint64 closedKey = 0;
        quint64 focusedKey = 0;
        onClose = [&](quint64 key) { closedKey = key; };
        onActivate = [&](quint64 key) { focusedKey = key; };
        if (!closer || !focus) {
            std::fprintf(stderr, "magic-dock: stack window card missing\n");
            onClose = nullptr;
            onActivate = nullptr;
            cleanupDesk();
            return 1;
        }
        const QPointF closeAt = closer->closeRect.center();
        const QPointF focusAt = focus->rect.center();
        pressAt(stackPage, closeAt.x(), closeAt.y(), false, false);
        releaseAt(stackPage, closeAt.x(), closeAt.y());
        if (closedKey == 0 || stackPage.openStack != QLatin1String("mz")) {
            std::fprintf(stderr, "magic-dock: stack window close did not stay on the stack\n");
            onClose = nullptr;
            onActivate = nullptr;
            cleanupDesk();
            return 1;
        }
        pressAt(stackPage, focusAt.x(), focusAt.y(), false, false);
        releaseAt(stackPage, focusAt.x(), focusAt.y());
        onClose = nullptr;
        onActivate = nullptr;
        if (focusedKey != 103 || !stackPage.openStack.isEmpty() || !stackPage.sideStack.isEmpty()) {
            std::fprintf(stderr, "magic-dock: stack window pick failed (key=%llu open=%s)\n",
                         static_cast<unsigned long long>(focusedKey), qPrintable(stackPage.openStack));
            cleanupDesk();
            return 1;
        }
        cleanupDesk();
    }

    std::fprintf(stderr, "magic-dock: dock self-test ok\n");
    return 0;
}

