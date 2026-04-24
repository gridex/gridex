#include "Services/Update/UpdateService.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <nlohmann/json.hpp>

#ifndef GRIDEX_VERSION
#  define GRIDEX_VERSION "0.0.0-dev"
#endif

namespace gridex {

const char* const kUpdateFeedUrl = "https://cdn.gridex.app/linux/releases.stable.json";

namespace {

// Split "0.1.2-rc1" into ([0,1,2], "rc1").
std::pair<std::vector<int>, QString> parseVersion(const QString& v) {
    QString s = v.trimmed();
    if (s.startsWith('v') || s.startsWith('V')) s = s.mid(1);
    QString suffix;
    int dash = s.indexOf('-');
    if (dash >= 0) { suffix = s.mid(dash + 1); s = s.left(dash); }
    std::vector<int> parts;
    for (const auto& p : s.split('.')) parts.push_back(p.toInt());
    while (parts.size() < 3) parts.push_back(0);
    return {std::move(parts), std::move(suffix)};
}

}  // namespace

bool isNewerVersion(const QString& remote, const QString& local) {
    auto [r, rs] = parseVersion(remote);
    auto [l, ls] = parseVersion(local);
    for (std::size_t i = 0; i < std::min(r.size(), l.size()); ++i) {
        if (r[i] != l[i]) return r[i] > l[i];
    }
    // Same numeric parts → a present suffix is treated as a pre-release,
    // so 0.1.0 > 0.1.0-rc1. Both-present: lexicographic.
    if (rs.isEmpty() && !ls.isEmpty()) return true;
    if (!rs.isEmpty() && ls.isEmpty()) return false;
    return rs > ls;
}

UpdateService::UpdateService(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

QString UpdateService::currentVersion() { return QStringLiteral(GRIDEX_VERSION); }

bool UpdateService::isAppImage() {
    const QByteArray env = qgetenv("APPIMAGE");
    return !env.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(env));
}

void UpdateService::checkForUpdate() {
    QNetworkRequest req{QUrl(QString::fromUtf8(kUpdateFeedUrl))};
    req.setRawHeader("User-Agent", QByteArrayLiteral("Gridex-Updater/") + GRIDEX_VERSION);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    auto* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        UpdateCheckResult r;
        r.currentVersion = currentVersion();

        if (reply->error() != QNetworkReply::NoError) {
            r.errorMessage = reply->errorString();
            emit updateChecked(r);
            return;
        }
        try {
            auto raw = reply->readAll().toStdString();
            auto j = nlohmann::json::parse(raw);
            QString remoteVer = QString::fromStdString(j.value("version", ""));
            if (remoteVer.isEmpty()) {
                r.errorMessage = tr("Update feed missing 'version' field");
                emit updateChecked(r);
                return;
            }
            if (isNewerVersion(remoteVer, r.currentVersion)) {
                r.hasUpdate    = true;
                r.newVersion   = remoteVer;
                r.downloadUrl  = QString::fromStdString(j.value("url", ""));
                r.sha256       = QString::fromStdString(j.value("sha256", ""));
                r.notes        = QString::fromStdString(j.value("notes", ""));
            }
        } catch (const std::exception& e) {
            r.errorMessage = QString::fromUtf8(e.what());
        }
        emit updateChecked(r);
    });
}

void UpdateService::downloadAndApply(const QString& url, const QString& expectedSha256) {
    if (url.isEmpty()) {
        emit errorOccurred(tr("Download URL is empty."));
        return;
    }

    emit statusChanged(tr("Downloading update…"));

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", QByteArrayLiteral("Gridex-Updater/") + GRIDEX_VERSION);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    auto* reply = nam_->get(req);

    // Write as we receive, to a temp file alongside the target AppImage so
    // the final rename stays atomic (same filesystem).
    const QString appImagePath = QString::fromLocal8Bit(qgetenv("APPIMAGE"));
    const QString tempPath = (isAppImage() ? appImagePath : QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/Gridex.AppImage")
                             + QStringLiteral(".download");

    auto* file = new QFile(tempPath, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->abort();
        reply->deleteLater();
        file->deleteLater();
        emit errorOccurred(tr("Cannot open %1 for writing.").arg(tempPath));
        return;
    }

    connect(reply, &QNetworkReply::readyRead, this, [reply, file]() {
        file->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 bytesReceived, qint64 bytesTotal) {
                if (bytesTotal > 0) {
                    int pct = static_cast<int>((bytesReceived * 100) / bytesTotal);
                    emit statusChanged(tr("Downloading update… %1%").arg(pct));
                }
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, file, tempPath, appImagePath, expectedSha256]() {
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();
        file->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(tempPath);
            emit errorOccurred(reply->errorString());
            return;
        }

        // Verify SHA256 before touching anything.
        emit statusChanged(tr("Verifying download…"));
        QFile verify(tempPath);
        if (!verify.open(QIODevice::ReadOnly)) {
            QFile::remove(tempPath);
            emit errorOccurred(tr("Cannot reopen downloaded file for verification."));
            return;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&verify);
        verify.close();
        const QString gotSha = QString::fromLatin1(hash.result().toHex());
        if (!expectedSha256.isEmpty() && gotSha.compare(expectedSha256, Qt::CaseInsensitive) != 0) {
            QFile::remove(tempPath);
            emit errorOccurred(tr("Checksum mismatch.\nExpected: %1\nGot: %2").arg(expectedSha256, gotSha));
            return;
        }

        if (!isAppImage()) {
            emit statusChanged(tr("Downloaded to %1. Install manually — this build was not launched from an AppImage.").arg(tempPath));
            return;
        }

        // Atomic swap: rename temp → AppImage path, chmod +x, relaunch.
        emit statusChanged(tr("Installing update…"));
        QFile::remove(appImagePath);
        if (!QFile::rename(tempPath, appImagePath)) {
            emit errorOccurred(tr("Could not replace %1.").arg(appImagePath));
            return;
        }
        QFile(appImagePath).setPermissions(
            QFile::ReadOwner  | QFile::WriteOwner | QFile::ExeOwner  |
            QFile::ReadGroup  | QFile::ExeGroup   |
            QFile::ReadOther  | QFile::ExeOther);

        emit statusChanged(tr("Restarting…"));
        QProcess::startDetached(appImagePath, {});
        QCoreApplication::quit();
    });
}

}  // namespace gridex
