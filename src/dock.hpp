#pragma once

#include "apps.hpp"
#include "backend.hpp"
#include "settings.hpp"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QJsonValue>
#include <QRect>
#include <QRegion>

#include <QStringList>

#include <functional>
#include <vector>

class Dock {
public:
    explicit Dock(AppIndex *apps);
    ~Dock();

    void load();
    void setWindows(const std::vector<WindowState> &windows);
    void pointer(const PointerEv &event);
    void key(const KeyEv &event);
    bool wantsKeyboard() const;
    bool tick();
    QRegion paint(int outputId, QPainter &painter, const OutputGeom &geom);

    // Windows whose thumbnails should be captured while a preview strip is open.
    std::vector<quint64> previewKeys() const;
    // Stores a captured frame. Returns true when the image changed.
    bool noteThumbnail(quint64 key, const QImage &image, quint64 serial);

    // Draws sample states into PNGs and checks the default Social stack. Returns 0 on success.
    int preview(const QString &directory);
    // Checks edges, applets, trash, search, the menu, stacks, and the opening nudge. Returns 0 on success.
    int selfTest();

    ShellSpec shellSpec() const;
    bool takeShellChange();

    // File drag from another app. The trash can is the only drop target.
    bool acceptsFileDrop(int outputId, double x, double y) const;
    void fileDrag(int outputId, double x, double y, bool active);
    void dropFiles(const QStringList &uris);

    std::function<void(quint64 key)> onActivate;
    std::function<void(quint64 key)> onMinimize;
    std::function<void(quint64 key)> onUnminimize;
    std::function<void(quint64 key)> onClose;
    std::function<void(quint64 key, int outputId, QRect rect)> onRect;

private:
    struct Item;
    struct Live;
    struct Slot;
    struct Screen;

    AppIndex *apps_ = nullptr;
    std::vector<Item> items_;
    std::vector<Live> live_;
    QHash<int, Screen> screens_;
    int pointerOutput_ = -1;
    int iconSize_ = 40;
    int margin_ = 0;
    int pad_ = 4;
    int radius_ = 0;
    int handle_ = 4;
    int hideWaitMs_ = 1000;
    int unhideDelayMs_ = 200;
    double opacity_ = 1;
    int chromeR_ = 255;
    int chromeG_ = 149;
    int chromeB_ = 0;
    int hintPx_ = 3;
    QString chromeSig_;
    bool chromeDirty_ = false;
    bool magnify_ = false;
    bool exclusive_ = false;
    bool expand_ = true;
    bool shellDirty_ = true;
    ShellSpec::Edge edge_ = ShellSpec::Edge::Bottom;
    enum class Hide { Never, Overlap, Always } hideMode_ = Hide::Overlap;
    QString outputMode_ = QStringLiteral("All");
    QString settingsSig_;
    qint64 settingsPoll_ = 0;
    QHash<QString, QString> execs_;
    QHash<QString, qint64> opening_;
    static constexpr double kGap = 6;

    double barThickness() const;
    bool horizontal() const;
    QString taskId() const;
    QRectF popupRoom(const Screen &screen) const;
    QRectF popupNear(const Screen &screen, const QPointF &anchor, double width, double height) const;
    int screenGap() const;
    QPointF openingNudge(const QString &appId) const;
    bool outputWanted(int outputId, const QString &name) const;
    int activeOutputId() const;
    bool pulledToDock(const Screen &screen, double x, double y) const;
    void pollSettings();
    void refreshChrome();
    void applyCosmicSettings(const CosmicDockSettings &settings);
    void dismissPopup(Screen &screen, const QString &which);
    void noteOpening(const QString &appId);
    void captureExec(const QString &appId);
    QString canonical(const QString &appId) const;
    QHash<QString, QRect> sentRects_;
    QHash<quint64, QImage> thumbs_;
    QHash<quint64, quint64> thumbSerial_;
    qint64 now_ = 0;
    bool previewBackdrop_ = false;
    bool persist_ = true;
    // Self-test writes launchers here instead of the real desktop folder.
    QString desktopDirForTest_;
    double cpu_ = 0;
    double mem_ = 0;
    quint64 memUsed_ = 0;
    quint64 memTotal_ = 0;
    bool cpuReady_ = false;
    qint64 usagePoll_ = 0;
    quint64 cpuIdlePrev_ = 0;
    quint64 cpuTotalPrev_ = 0;
    int trashCount_ = 0;
    qint64 trashPoll_ = 0;
    qint64 trashFastUntil_ = 0;
    qint64 trashScan_ = 0;
    QStringList trashDirs_;
    QStringList trashNetDirs_;
    bool showTrash_ = true;
    bool showTask_ = true;
    bool showGrok_ = true;
    // The applet is drawn only when the Grok CLI is installed. Grogy launches from the
    // program remembered in dock.json, then from PATH.
    bool grokInstalled_ = false;
    bool grogyOnPath_ = false;
    QString grogyPath_;
    bool loaded_ = false;
    bool grogySave_ = false;
    qint64 grokProbe_ = 0;
    struct GrokBot {
        QString name;
        QString role;
        QString note;
        bool running = false;
    };
    int creditLeft_ = -1;
    bool creditOk_ = false;
    bool botsKnown_ = false;
    bool grokDirty_ = false;
    QString creditLine_;
    QString creditReset_;
    QString creditDetail_;
    QString creditPrepaid_;
    QStringList creditProducts_;
    QStringList runningBots_;
    QList<GrokBot> grokBots_;
    QString grokWriter_;
    qint64 creditPoll_ = 0;
    qint64 botPoll_ = 0;
    qint64 creditStarted_ = 0;
    qint64 botStarted_ = 0;
    class QProcess *creditProc_ = nullptr;
    class QProcess *botProc_ = nullptr;
    QImage previewWallpaper_;

    QString configPath() const;
    void save() const;
    void buildDefault();
    Screen &screen(int id);
    void relayout(Screen &screen);
    void retarget(Screen &screen);
    void updateHidden(Screen &screen);
    QRegion inputRegion(const Screen &screen) const;
    void pressAt(Screen &screen, double x, double y, bool right, bool middle);
    void releaseAt(Screen &screen, double x, double y);
    void dragMotion(Screen &screen, double x, double y);
    void activateApp(const QString &appId);
    void newWindow(const QString &appId);
    void runAction(Screen &screen, const QString &action, const QString &appId);
    void openMenu(Screen &screen, const QString &uid, const QString &appId);
    void launchId(const QString &appId);
    QString itemName(const QString &appId) const;
    QPixmap itemIcon(const QString &appId, int devicePx) const;
    void syncRects(int outputId, const Screen &screen);
    void paintScreen(QPainter &painter, Screen &screen);
    bool isApp(const Live &live, const QString &id) const;
    int itemIndex(const QString &uid) const;
    void moveItem(int from, int to);
    void mergeItems(const QString &dragUid, const QString &targetUid);
    void pullFromStack(Screen &screen, const QString &appId);
    void unpinApp(Screen &screen, const QString &id);
    void beginRename(Screen &screen, const QString &stackId = {});
    void commitRename(Screen &screen, bool apply);
    void applyKey(Screen &screen, int code, bool shift);
    QString desktopDirectory() const;
    void sendToDesktop(const QString &appId);
    QVector<quint64> keysFor(const QString &appId) const;
    void minimizeActive();
    void layoutPopup(Screen &screen);
    void layoutPreviews(Screen &screen);
    void layoutMenu(Screen &screen);
    void layoutSearch(Screen &screen);
    void layoutAbout(Screen &screen);
    void setSearch(Screen &screen, bool open);
    void clickSlot(Screen &screen, const QString &uid);
    bool pollMeters();
    bool pollGrok();
    bool probeGrokTools();
    QString resolveGrogy() const;
    bool nestDropTarget(const Screen &screen, double x, double y, QString *uid) const;
    QRectF tipZone(const Screen &screen, const QString &uid) const;
    bool keepsTip(const Screen &screen, double x, double y) const;
    void readCreditCache();
    void applyCreditBytes(const QByteArray &bytes);
    void applyBotBytes(const QByteArray &bytes);
    void openGrogy();
    void noteTrashChanged();
    void openTrash() const;
    void emptyTrash();
    void dropOntoTrash(Screen &screen);
    Item *findStack(const QString &id);
    QString parentStackId(const QString &id) const;
    std::vector<Item> *childList(const QString &stackId, int *index);
    QStringList appsIn(const Item &item) const;
    bool holdsApp(const Item &item, const QString &id) const;
    bool containsStack(const Item &item, const QString &stackId) const;
    QString topStackId(const QString &id) const;
    QString cleanAppId(const QString &raw);
    bool readNode(Item &item, const QJsonValue &value);
    QJsonValue writeChild(const Item &item) const;
    void pruneEmpty(std::vector<Item> &nodes, const QString &keep = {});
    void stripApp(std::vector<Item> &nodes, const QString &id, const QString &keepStack = {});
    void placeInStack(const QString &appId, const QString &stackId);
    void closeStackIfGone(Screen &screen);
    void openStackPage(Screen &screen, const QString &stackId, const QString &page);
    void pullNested(Screen &screen, const QString &stackId);
    void moveChild(const QString &stackId, int from, int to);
    void dropChildInto(Screen &screen, const QString &sourceId, const QString &targetId);
    void moveChildTo(Screen &screen, const QString &sourceId, const QString &targetId);
    void dragCell(Screen &screen, double x, double y);
    int childIndex(const Item &stack, const QString &appId, const QString &stackId) const;
    QString trashFilesDir() const;
    void refreshTrashDirs();
    QStringList trashBins() const;

    struct Hit {
        int type = 0;
        QString uid;
        QString appId;
        QString action;
        int index = -1;
        quint64 key = 0;
        bool side = false;
    };
    Hit hitTest(const Screen &screen, double x, double y) const;
};
