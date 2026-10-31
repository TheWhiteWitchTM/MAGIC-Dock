#pragma once

#include <QString>
#include <QStringList>

#include <vector>

// One emoji from the same set Mira's emoji and flag tools use.
struct Mark {
    QString id;
    QString name;
    QString keys;
    QString natives;
    int cat = 0;
    QString nativeAt(int skin) const;
};

const std::vector<Mark> &allMarks();
QString markCategory(int cat);
// terms are the words of one piece. A miss is "?".
QString findEmoji(const QStringList &terms);
// A country code, a name, or a keyword. A miss is "?". An empty term is empty.
QString findFlag(const QString &term);
// Skin index from words such as "dark". base receives the words that are not a skin.
int emojiSkin(const QStringList &terms, QStringList *base);
// cat < 0 is every category. query filters by the words in the text.
std::vector<int> filterMarks(int cat, const QString &query);
