#pragma once

#include "apps.hpp"
#include "backend.hpp"
#include "settings.hpp"

#include <QHash>
#include <QImage>
#include <QRect>
#include <QRegion>

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
    // Checks edges, the far-end monitor button, and the opening nudge. Returns 0 on success.
    int selfTest();

    ShellSpec shellSpec() const;
    bool takeShellChange();

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
    QRectF popupNear(const Screen &screen, const QPointF &anchor, double width, double height) const;
    QPointF openingNudge(const QString &appId) const;
    bool outputWanted(int outputId, const QString &name) const;
    int activeOutputId() const;
    bool pulledToDock(const Screen &screen, double x, double y) const;
    void pollSettings();
    void applyCosmicSettings(const CosmicDockSettings &settings);
    void noteOpening(const QString &appId);
    void captureExec(const QString &appId);
    QString canonical(const QString &appId) const;
    QHash<QString, QRect> sentRects_;
    QHash<quint64, QImage> thumbs_;
    QHash<quint64, quint64> thumbSerial_;
    qint64 now_ = 0;
    bool previewBackdrop_ = false;

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
    void commitRename(Screen &screen, bool apply);
    void applyKey(Screen &screen, int code, bool shift);
    QVector<quint64> keysFor(const QString &appId) const;
    void minimizeActive();
    void layoutPopup(Screen &screen);
    void layoutPreviews(Screen &screen);
    void layoutMenu(Screen &screen);
    void clickSlot(Screen &screen, const QString &uid);

    struct Hit {
        int type = 0;
        QString uid;
        QString appId;
        QString action;
        int index = -1;
        quint64 key = 0;
    };
    Hit hitTest(const Screen &screen, double x, double y) const;
};
