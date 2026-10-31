#include "marks.hpp"

namespace {

struct MarkRec {
    const char *id;
    const char *name;
    const char *keys;
    const char *natives;
    unsigned char cat;
};

#include "marks_data.inc"

const std::vector<Mark> &table() {
    static std::vector<Mark> all;
    if (!all.empty()) return all;
    all.reserve(sizeof(kMarks) / sizeof(kMarks[0]));
    for (const MarkRec &rec : kMarks) {
        Mark mark;
        mark.id = QString::fromUtf8(rec.id);
        mark.name = QString::fromUtf8(rec.name);
        mark.keys = QString::fromUtf8(rec.keys);
        mark.natives = QString::fromUtf8(rec.natives);
        mark.cat = rec.cat;
        all.push_back(mark);
    }
    return all;
}

const QString skinWords[] = {QStringLiteral("light"), QStringLiteral("medium-light"), QStringLiteral("medium"),
                             QStringLiteral("medium-dark"), QStringLiteral("dark"), QStringLiteral("black")};
const int skinSlots[] = {1, 2, 3, 4, 5, 5};

QString blobOf(const Mark &mark) {
    return (mark.name + QLatin1Char(' ') + mark.keys + QLatin1Char(' ') + mark.id).toLower();
}

}  // namespace

QString Mark::nativeAt(int skin) const {
    const QStringList parts = natives.split(QLatin1Char('|'));
    if (parts.isEmpty()) return {};
    if (skin < 0 || skin >= parts.size() || parts.at(skin).isEmpty()) return parts.at(0);
    return parts.at(skin);
}

const std::vector<Mark> &allMarks() { return table(); }

QString markCategory(int cat) {
    switch (cat) {
    case 0: return QStringLiteral("People");
    case 1: return QStringLiteral("Nature");
    case 2: return QStringLiteral("Food");
    case 3: return QStringLiteral("Activity");
    case 4: return QStringLiteral("Places");
    case 5: return QStringLiteral("Objects");
    case 6: return QStringLiteral("Symbols");
    case 7: return QStringLiteral("Flags");
    default: return QStringLiteral("All");
    }
}

int emojiSkin(const QStringList &terms, QStringList *base) {
    int skin = 0;
    if (base) base->clear();
    for (const QString &term : terms) {
        const QString lower = term.toLower();
        bool found = false;
        for (int i = 0; i < 6; ++i) {
            if (lower == skinWords[i]) {
                skin = skinSlots[i];
                found = true;
                break;
            }
        }
        if (!found && base) base->push_back(lower);
    }
    return skin;
}

QString findEmoji(const QStringList &terms) {
    if (terms.isEmpty()) return {};
    QStringList base;
    const int skin = emojiSkin(terms, &base);
    const QStringList &use = base.isEmpty() ? terms : base;
    QStringList lower;
    for (const QString &term : use) {
        const QString text = term.toLower();
        if (!text.isEmpty()) lower.push_back(text);
    }
    if (lower.isEmpty()) return QStringLiteral("?");
    const std::vector<Mark> &marks = table();
    int best = -1;
    int bestScore = 0;
    for (int i = 0; i < static_cast<int>(marks.size()); ++i) {
        const QString fields = blobOf(marks[static_cast<size_t>(i)]);
        int score = 0;
        for (const QString &term : lower) {
            if (fields.contains(term)) score++;
        }
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best < 0) return QStringLiteral("?");
    return marks[static_cast<size_t>(best)].nativeAt(skin);
}

QString findFlag(const QString &term) {
    const QString clean = term.trimmed().toLower();
    if (clean.isEmpty()) return {};
    const std::vector<Mark> &marks = table();
    for (const Mark &mark : marks) {
        if (mark.cat != 7) continue;
        if (mark.id.compare(clean, Qt::CaseInsensitive) == 0) return mark.nativeAt(0);
        if (mark.id == QLatin1String("flag-") + clean) return mark.nativeAt(0);
    }
    for (const Mark &mark : marks) {
        if (mark.cat != 7) continue;
        if (blobOf(mark).contains(clean)) return mark.nativeAt(0);
    }
    return QStringLiteral("?");
}

std::vector<int> filterMarks(int cat, const QString &query) {
    QStringList words;
    const QString text = query.trimmed().toLower();
    if (!text.isEmpty()) {
        const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
        const QString last = parts.isEmpty() ? text : parts.last().trimmed();
        for (const QString &word : last.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (!word.isEmpty()) words.push_back(word);
        }
    }
    std::vector<int> hits;
    const std::vector<Mark> &marks = table();
    hits.reserve(marks.size());
    for (int i = 0; i < static_cast<int>(marks.size()); ++i) {
        const Mark &mark = marks[static_cast<size_t>(i)];
        if (cat >= 0 && mark.cat != cat) continue;
        if (!words.isEmpty()) {
            const QString fields = blobOf(mark);
            bool ok = true;
            for (const QString &word : words) {
                if (!fields.contains(word)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
        }
        hits.push_back(i);
    }
    return hits;
}
