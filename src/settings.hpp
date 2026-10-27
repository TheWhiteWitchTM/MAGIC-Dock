#pragma once

#include <QString>

// Pop!_OS writes these as plain files under
// ~/.config/cosmic/com.system76.CosmicPanel.Dock/v1/
struct CosmicDockSettings {
    bool ok = false;
    QString anchor = QStringLiteral("Bottom");  // Top, Bottom, Left, Right
    int margin = 0;
    bool anchorGap = false;
    QString autohide = QStringLiteral("OnOverlap");  // Never, OnOverlap, Always
    int hideWaitMs = 1000;
    int unhideDelayMs = 200;
    int handleSize = 4;
    int iconSize = 40;
    int padding = 4;
    int borderRadius = 0;
    bool exclusiveZone = false;
    bool expandToEdges = true;
    double opacity = 1;
    QString output = QStringLiteral("All");  // All, Active, or a connector name
    QString signature;

    int inset() const { return anchorGap || margin > 0 ? margin : 0; }
};

CosmicDockSettings readCosmicDockSettings(const QString &directory = QString());

// Active window hint from Settings → Appearance. The color is window_hint
// when one is set, otherwise the theme accent. hintPx is the hint thickness.
struct CosmicChrome {
    bool ok = false;
    int red = 255;
    int green = 149;
    int blue = 0;
    int hintPx = 3;
    QString signature;
};

CosmicChrome readCosmicChrome(const QString &root = QString());
int runSettingsSelfTest();
