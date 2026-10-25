#pragma once

#include <QHash>
#include <QPixmap>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <vector>

struct AppHit {
    QString id;
    QString name;
};

struct DesktopAction {
    QString id;
    QString name;
    QString exec;
};

struct DesktopApp {
    QString id;
    QString name;
    QString icon;
    QString exec;
    QString wmClass;
    QString path;
    QString workdir;
    bool noDisplay = false;
    std::vector<DesktopAction> actions;
};

class AppIndex {
public:
    void scan(const QStringList &extraDirs = {});
    // Reload desktop files and icons when an app or a web app was just installed.
    bool rescanIfChanged();
    // Teach the index about a desktop file that is not in an applications directory.
    void adoptDesktop(const QString &filePath);
    // Qt app ids look like local.cinder.cinder. The desktop file often lives in the project tree.
    QString adoptProject(const QString &appId);
    // Extra icon theme root. Tests use this. Production leaves it empty.
    void setIconRoot(const QString &root);

    const DesktopApp *byId(const QString &id) const;
    // Map a window app_id or a favorite id onto a desktop id.
    QString resolve(const QString &appId) const;
    QStringList cosmicFavorites() const;
    QPixmap iconFor(const QString &desktopId, int devicePx) const;
    QPixmap iconNamed(const QString &nameOrPath, int devicePx) const;

    // Absolute program to run when the desktop file is missing or gio cannot start it.
    QString programFor(const QString &appId) const;
    // Installed apps whose name or id matches. An empty query returns nothing.
    std::vector<AppHit> search(const QString &query, int limit) const;
    // Starts the app with a clean environment. Returns false when nothing could be started.
    bool launch(const QString &appId, const QString &fallbackExec = {}, const QString &fallbackDir = {});

private:
    void scanDir(const QString &dir);
    void parseDesktop(const QString &filePath);
    QString findIconFile(const QString &name) const;
    qint64 treeStamp() const;

    QHash<QString, DesktopApp> apps_;
    QHash<QString, QString> lower_;
    QHash<QString, QString> byClass_;
    QHash<QString, QString> byExec_;
    QHash<QString, QString> byWeb_;
    QString iconRoot_;
    QStringList extraDirs_;
    qint64 stamp_ = 0;
    mutable QHash<QString, QPixmap> iconCache_;
    mutable QHash<QString, QString> iconFileCache_;
};

QStringList splitExec(const QString &exec, const DesktopApp *app);
QProcessEnvironment launchEnvironment();
// Home when the desktop file has no Path. COSMIC Terminal also gets --working-directory.
bool startDetachedApp(const QString &program, const QStringList &args, const QString &workdir, const char *how = "app");
int runAppsSelfTest();
