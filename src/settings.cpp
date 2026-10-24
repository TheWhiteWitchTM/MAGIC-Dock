#include "settings.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cstdio>

namespace {

QString clean(QString text) {
    text = text.trimmed();
    if (text.startsWith('"') && text.endsWith('"') && text.size() >= 2) text = text.mid(1, text.size() - 2);
    return text.trimmed();
}

QString readFile(const QString &directory, const QString &name) {
    QFile file(directory + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return clean(QString::fromUtf8(file.readAll()));
}

int iconForSize(const QString &size) {
    if (size == QLatin1String("XS")) return 24;
    if (size == QLatin1String("S")) return 32;
    if (size == QLatin1String("M") || size.isEmpty()) return 40;
    if (size == QLatin1String("L")) return 48;
    if (size == QLatin1String("XL")) return 56;
    static const QRegularExpression custom(QStringLiteral("^Custom\\((\\d+)\\)$"));
    const auto match = custom.match(size);
    if (match.hasMatch()) {
        const int raw = match.captured(1).toInt();
        const int snapped = std::max(16, raw) / 4 * 4;
        return std::max(16, snapped * 7 / 10);
    }
    return 40;
}

int field(const QString &text, const QString &key, int fallback) {
    const QRegularExpression re(key + QStringLiteral("\\s*:\\s*(\\d+)"));
    const auto match = re.match(text);
    if (!match.hasMatch()) return fallback;
    return match.captured(1).toInt();
}

}  // namespace

CosmicDockSettings readCosmicDockSettings(const QString &directory) {
    CosmicDockSettings settings;
    const QString dir = directory.isEmpty()
                            ? QDir::homePath() + QStringLiteral("/.config/cosmic/com.system76.CosmicPanel.Dock/v1")
                            : directory;
    if (!QFileInfo(dir).isDir()) return settings;
    settings.ok = true;

    const QString anchor = readFile(dir, QStringLiteral("anchor"));
    if (anchor == QLatin1String("Top") || anchor == QLatin1String("Bottom") || anchor == QLatin1String("Left") ||
        anchor == QLatin1String("Right")) {
        settings.anchor = anchor;
    }
    const QString margin = readFile(dir, QStringLiteral("margin"));
    if (!margin.isEmpty()) settings.margin = std::clamp(margin.toInt(), 0, 64);
    const QString gap = readFile(dir, QStringLiteral("anchor_gap"));
    settings.anchorGap = gap.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;

    const QString hide = readFile(dir, QStringLiteral("autohide"));
    if (hide == QLatin1String("Never") || hide == QLatin1String("OnOverlap") || hide == QLatin1String("Always"))
        settings.autohide = hide;
    const QString behavior = readFile(dir, QStringLiteral("autohide_behavior"));
    settings.hideWaitMs = std::clamp(field(behavior, QStringLiteral("wait_time"), 1000), 0, 10000);
    settings.unhideDelayMs = std::clamp(field(behavior, QStringLiteral("unhide_delay"), 200), 0, 5000);
    settings.handleSize = std::clamp(field(behavior, QStringLiteral("handle_size"), 4), 1, 32);

    settings.iconSize = std::clamp(iconForSize(readFile(dir, QStringLiteral("size"))), 16, 96);
    const QString padding = readFile(dir, QStringLiteral("padding"));
    if (!padding.isEmpty()) settings.padding = std::clamp(padding.toInt(), 0, 32);
    const QString radius = readFile(dir, QStringLiteral("border_radius"));
    if (!radius.isEmpty()) settings.borderRadius = std::clamp(radius.toInt(), 0, 48);

    settings.exclusiveZone = readFile(dir, QStringLiteral("exclusive_zone")).compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    const QString expand = readFile(dir, QStringLiteral("expand_to_edges"));
    settings.expandToEdges = expand.isEmpty() || expand.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    const QString opacity = readFile(dir, QStringLiteral("opacity"));
    if (!opacity.isEmpty()) settings.opacity = std::clamp(opacity.toDouble(), 0.2, 1.0);

    const QString output = readFile(dir, QStringLiteral("output"));
    if (output == QLatin1String("All") || output == QLatin1String("Active")) {
        settings.output = output;
    } else if (output.startsWith(QLatin1String("Name(")) && output.endsWith(')')) {
        settings.output = output.mid(5, output.size() - 6);
    } else if (!output.isEmpty()) {
        settings.output = output;
    }

    qint64 stamp = QFileInfo(dir).lastModified().toMSecsSinceEpoch();
    const QDir folder(dir);
    const auto files = folder.entryInfoList(QDir::Files);
    for (const QFileInfo &info : files) stamp ^= info.lastModified().toMSecsSinceEpoch() + info.size();
    settings.signature = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13")
                             .arg(settings.anchor)
                             .arg(settings.margin)
                             .arg(settings.anchorGap ? 1 : 0)
                             .arg(settings.autohide)
                             .arg(settings.hideWaitMs)
                             .arg(settings.unhideDelayMs)
                             .arg(settings.handleSize)
                             .arg(settings.iconSize)
                             .arg(settings.padding)
                             .arg(settings.borderRadius)
                             .arg(settings.exclusiveZone ? 1 : 0)
                             .arg(settings.expandToEdges ? 1 : 0)
                             .arg(settings.output) +
                         QStringLiteral("|%1|%2").arg(settings.opacity, 0, 'f', 2).arg(stamp);
    return settings;
}

int runSettingsSelfTest() {
    const QString dir = QDir::tempPath() + QStringLiteral("/magic-dock-settings-test");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    auto write = [&](const QString &name, const QString &body) {
        QFile file(dir + QLatin1Char('/') + name);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        file.write(body.toUtf8());
        return true;
    };
    write(QStringLiteral("anchor"), QStringLiteral("Left\n"));
    write(QStringLiteral("margin"), QStringLiteral("8\n"));
    write(QStringLiteral("anchor_gap"), QStringLiteral("true\n"));
    write(QStringLiteral("autohide"), QStringLiteral("Always\n"));
    write(QStringLiteral("autohide_behavior"),
          QStringLiteral("(\n    wait_time: 1500,\n    transition_time: 200,\n    handle_size: 6,\n    unhide_delay: 80,\n)\n"));
    write(QStringLiteral("size"), QStringLiteral("L\n"));
    write(QStringLiteral("padding"), QStringLiteral("2\n"));
    write(QStringLiteral("border_radius"), QStringLiteral("12\n"));
    write(QStringLiteral("exclusive_zone"), QStringLiteral("true\n"));
    write(QStringLiteral("expand_to_edges"), QStringLiteral("false\n"));
    write(QStringLiteral("opacity"), QStringLiteral("0.85\n"));
    write(QStringLiteral("output"), QStringLiteral("Name(DP-1)\n"));

    const CosmicDockSettings settings = readCosmicDockSettings(dir);
    if (!settings.ok || settings.anchor != QLatin1String("Left") || settings.margin != 8 || !settings.anchorGap ||
        settings.autohide != QLatin1String("Always") || settings.hideWaitMs != 1500 || settings.unhideDelayMs != 80 ||
        settings.handleSize != 6 || settings.iconSize != 48 || settings.padding != 2 || settings.borderRadius != 12 ||
        !settings.exclusiveZone || settings.expandToEdges || settings.output != QLatin1String("DP-1") ||
        settings.opacity < 0.84 || settings.opacity > 0.86 || settings.inset() != 8) {
        QDir(dir).removeRecursively();
        std::fprintf(stderr, "magic-dock: settings self-test failed (anchor=%s size=%d output=%s)\n",
                     qPrintable(settings.anchor), settings.iconSize, qPrintable(settings.output));
        return 1;
    }
    write(QStringLiteral("size"), QStringLiteral("Custom(40)\n"));
    const CosmicDockSettings custom = readCosmicDockSettings(dir);
    QDir(dir).removeRecursively();
    if (custom.iconSize != 28) {
        std::fprintf(stderr, "magic-dock: custom panel size parsed wrong (%d)\n", custom.iconSize);
        return 1;
    }
    std::fprintf(stderr, "magic-dock: settings self-test ok\n");
    return 0;
}
