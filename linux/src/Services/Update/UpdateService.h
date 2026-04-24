#pragma once

// UpdateService — lightweight AppImage auto-updater for Linux.
//
// Mirrors the Windows UpdateService API shape (Velopack-based). The Linux
// implementation is custom-built to avoid vendoring native libs:
//   1. Fetch a JSON feed at kUpdateFeedUrl via QNetworkAccessManager.
//   2. Compare `version` in the feed against `GRIDEX_VERSION` (set at build
//      time from the CMake PROJECT_VERSION).
//   3. If newer, caller can invoke downloadAndApply() — that downloads the
//      AppImage to a temp path, verifies SHA256, swaps it over the file
//      pointed at by the $APPIMAGE env var (set by the AppRun wrapper),
//      chmod +x, and restarts the process via QProcess::startDetached.
//
// If the app is not running from an AppImage (dev builds, packaged .deb),
// downloadAndApply() still downloads + verifies but reports an error
// instead of self-replacing — the user can install the downloaded file
// via their package manager.
//
// Feed JSON format (served from R2 bucket, matches Windows pattern):
//   {
//     "version":   "0.2.0",
//     "published": "2026-05-01T00:00:00Z",
//     "url":       "https://cdn.gridex.app/linux/Gridex-0.2.0-x86_64.AppImage",
//     "sha256":    "abc123...",
//     "notes":     "release notes markdown"
//   }

#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace gridex {

// Feed URL — single source of truth. Trailing filename included (not a
// directory). The feed is plain JSON, no signature yet (TODO: minisign).
extern const char* const kUpdateFeedUrl;

struct UpdateCheckResult {
    bool hasUpdate = false;
    QString currentVersion;  // always filled
    QString newVersion;      // set iff hasUpdate
    QString downloadUrl;     // set iff hasUpdate
    QString sha256;          // set iff hasUpdate
    QString notes;           // set iff hasUpdate
    QString errorMessage;    // set on network / parse error
};

class UpdateService : public QObject {
    Q_OBJECT
public:
    explicit UpdateService(QObject* parent = nullptr);

    // Returns the current app version (build-time constant).
    static QString currentVersion();

    // True when $APPIMAGE env var points at an existing file — i.e. the app
    // was launched via the AppImage AppRun wrapper and self-update is
    // possible. Otherwise downloadAndApply() will only verify + notify.
    static bool isAppImage();

    // Fire-and-forget check. Emits updateChecked() on completion.
    void checkForUpdate();

    // Download the file at `url`, verify `expectedSha256`, swap over the
    // current AppImage and restart. Emits statusChanged() for progress,
    // errorOccurred() on failure.
    void downloadAndApply(const QString& url, const QString& expectedSha256);

signals:
    void updateChecked(const UpdateCheckResult& result);
    void statusChanged(const QString& message);
    void errorOccurred(const QString& message);

private:
    QNetworkAccessManager* nam_;
};

// Semver-ish comparator: returns true if `remote` > `local`. Accepts
// `X.Y.Z[-suffix]`; suffix breaks ties alphabetically.
bool isNewerVersion(const QString& remote, const QString& local);

}  // namespace gridex
