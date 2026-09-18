#include "updater/update-checker.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QScopeGuard>
#include <QVersionNumber>
#include <QRegularExpression>

#include <obs-module.h>

#include <algorithm>

namespace {
constexpr auto UPDATE_MANIFEST_URL =
    "https://github.com/BokiBokiPixelDeveloper/boki-twitch-chat-plugin/releases/latest/download/update-manifest.json";

QString normalizeVersion(QString value)
{
    value = value.trimmed();
    if (value.startsWith(QLatin1Char('v')))
        value.remove(0, 1);
    return value;
}

struct SemVerParts {
    QVersionNumber core;
    QString pre;
    bool valid = false;
};

SemVerParts parseSemVer(QString value)
{
    value = normalizeVersion(std::move(value));
    const int plus = value.indexOf(QLatin1Char('+'));
    if (plus >= 0)
        value.truncate(plus);

    QString pre;
    const int dash = value.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        pre = value.mid(dash + 1);
        value.truncate(dash);
    }

    const auto core = QVersionNumber::fromString(value);
    return {core, pre, !core.isNull()};
}

int comparePrerelease(const QString &a, const QString &b)
{
    if (a.isEmpty() && b.isEmpty())
        return 0;
    if (a.isEmpty())
        return 1; // stable > prerelease
    if (b.isEmpty())
        return -1;

    const QStringList aa = a.split(QLatin1Char('.'));
    const QStringList bb = b.split(QLatin1Char('.'));
    const int count = std::max(aa.size(), bb.size());
    for (int i = 0; i < count; ++i) {
        if (i >= aa.size())
            return -1;
        if (i >= bb.size())
            return 1;

        bool aNumber = false;
        bool bNumber = false;
        const qlonglong ai = aa[i].toLongLong(&aNumber);
        const qlonglong bi = bb[i].toLongLong(&bNumber);
        if (aNumber && bNumber) {
            if (ai < bi)
                return -1;
            if (ai > bi)
                return 1;
            continue;
        }
        if (aNumber != bNumber)
            return aNumber ? -1 : 1; // numeric identifiers have lower precedence
        const int cmp = QString::compare(aa[i], bb[i], Qt::CaseSensitive);
        if (cmp < 0)
            return -1;
        if (cmp > 0)
            return 1;
    }
    return 0;
}
} // namespace

UpdateChecker::UpdateChecker(QString currentVersion, StateCallback stateChanged, QObject *parent)
    : QObject(parent), currentVersion_(normalizeVersion(std::move(currentVersion))), stateChanged_(std::move(stateChanged)),
      network_(std::make_unique<QNetworkAccessManager>())
{
    postexit::holdProcessUseLock();
    QFile result(postexit::stateDirectory() + "/last-update-result.json");
    if (result.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(result.readAll());
        if (doc.isObject() && doc.object().contains("success")) {
            const auto object = doc.object();
            lastResult_ = object["success"].toBool()
                ? QStringLiteral("Update to %1 installed successfully.").arg(object["toVersion"].toString())
                : (object["rollbackFailed"].toBool()
                    ? QStringLiteral("Update failed – manual recovery required. %1")
                    : QStringLiteral("Update could not be installed – the previous version was retained. %1"))
                    .arg(object["error"].toString());
            result.remove();
        }
    }
    resumePending();
}

UpdateChecker::~UpdateChecker()
{
    if (updateLock_ >= 0) postexit::releaseLock(updateLock_);
}

bool UpdateChecker::resumePending()
{
    if (!QFile::exists(pendingDirectory_ + "/pending.json")) return false;
    hasAvailable_ = false;
    const int lock = postexit::acquireLock(pendingDirectory_ + "/update.lock");
    if (lock < 0) {
        setStatus(State::Ready, QStringLiteral("Update ready – close OBS completely. Installation will start automatically after it exits."));
        return true;
    }
    QString error;
    bool started = false;
    try {
        const auto pending = postexit::readPending(pendingDirectory_);
        if (pending.target != targetPath_)
            error = QStringLiteral("Pending update belongs to a different plugin installation");
        else
            started = launcher_(pendingDirectory_, QFileInfo(targetPath_).absolutePath() + "/" + postexit::helperFileName(), lock, error);
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
    postexit::releaseLock(lock);
    setStatus(started ? State::Ready : State::Error, started
        ? QStringLiteral("Update ready – close OBS completely. Installation will start automatically after it exits.")
        : QStringLiteral("Pending update could not be started: %1").arg(error));
    return true;
}

void UpdateChecker::setStatus(State state, QString status)
{
    // Button callbacks request their rebuild by returning true. Never rebuild here:
    // Qt is still dispatching mouseReleaseEvent to the existing property button.
    state_ = state;
    status_ = std::move(status);
}

void UpdateChecker::notifyStateChanged()
{
    // Network completions must cross the OBS UI queue, even on the Qt main thread.
    // A queued completion must not retain the source or dereference a dead checker.
    auto *guard = new QPointer<UpdateChecker>(this);
    obs_queue_task(OBS_TASK_UI, [](void *data) {
        const std::unique_ptr<QPointer<UpdateChecker>> checker(static_cast<QPointer<UpdateChecker> *>(data));
        if (*checker && (*checker)->stateChanged_)
            (*checker)->stateChanged_();
    }, guard, false);
}

bool UpdateChecker::isNewerVersion(const QString &candidate, const QString &current)
{
    const auto c = parseSemVer(candidate);
    const auto i = parseSemVer(current);
    if (!c.valid || !i.valid)
        return false;

    const int coreCmp = QVersionNumber::compare(c.core, i.core);
    if (coreCmp != 0)
        return coreCmp > 0;
    return comparePrerelease(c.pre, i.pre) > 0;
}

void UpdateChecker::checkForUpdates()
{
    if (busy() || state_ == State::Ready || resumePending())
        return;

    hasAvailable_ = false;
    available_ = {};
    setStatus(State::Checking, QStringLiteral("Checking for updates…"));

    QNetworkRequest request(QUrl(QString::fromLatin1(UPDATE_MANIFEST_URL)));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Bokis-Twitch-Chat-Plugin/%1").arg(currentVersion_));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = network_->get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleManifestReply(reply); });
}

void UpdateChecker::handleManifestReply(QNetworkReply *reply)
{
    const auto guard = std::unique_ptr<QNetworkReply, void (*)(QNetworkReply *)>(reply, [](QNetworkReply *r) { r->deleteLater(); });
    const auto notify = qScopeGuard([this] { notifyStateChanged(); });

    if (reply->error() != QNetworkReply::NoError) {
        setStatus(State::Error, QStringLiteral("Update source unavailable: %1. Is the release repository still private?")
                      .arg(reply->errorString()));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        setStatus(State::Error, QStringLiteral("Invalid update manifest"));
        return;
    }

    const QJsonObject root = doc.object();
    const QString version = root.value(QStringLiteral("version")).toString();
    const QJsonObject platform = root.value(QStringLiteral("platforms")).toObject().value(postexit::platformKey()).toObject();
    if (!version.isEmpty() && platform.isEmpty()) {
        setStatus(State::Error, QStringLiteral("This release has no package for %1").arg(postexit::platformKey()));
        return;
    }
    // Platform-specific ABI requirements are optional for legacy manifests.
    if (platform.contains("obs")) {
        const auto compatibility = platform["obs"].toObject();
        const auto minimum = QVersionNumber::fromString(compatibility["minVersion"].toString());
        const int maximum = compatibility["maxMajorVersion"].toInt(-1);
        const auto api = obs_get_version();
        const QVersionNumber running(int(api >> 24), int((api >> 16) & 0xff), int(api & 0xffff));
        if (minimum.isNull() || maximum < minimum.majorVersion() ||
            running < minimum || running.majorVersion() > maximum) {
            setStatus(State::Error, QStringLiteral("This update requires a compatible OBS version (%1 through major %2)")
                          .arg(minimum.toString()).arg(maximum));
            return;
        }
    }
    const QString url = platform.value(QStringLiteral("url")).toString();
    const QString sha256 = platform.value(QStringLiteral("sha256")).toString().toLower();
    const qint64 size = platform.value(QStringLiteral("size")).toInteger(-1);

    const QUrl binaryUrl(url);
    if (version.isEmpty() || !binaryUrl.isValid() || binaryUrl.scheme() != "https" || binaryUrl.host().isEmpty() ||
        !QRegularExpression("^[0-9a-f]{64}$").match(sha256).hasMatch() || size <= 0) {
        setStatus(State::Error, QStringLiteral("Update manifest is incomplete"));
        return;
    }

    std::optional<RemoteArtifact> helper;
    if (platform.contains("helper")) {
        const auto object = platform["helper"].toObject();
        helper = RemoteArtifact{object["url"].toString(), object["sha256"].toString().toLower(), object["size"].toInteger(-1)};
        const QUrl helperUrl(helper->downloadUrl);
        if (!helperUrl.isValid() || helperUrl.scheme() != "https" || helperUrl.host().isEmpty() ||
            !QRegularExpression("^[0-9a-f]{64}$").match(helper->sha256).hasMatch() || helper->size <= 0) {
            setStatus(State::Error, QStringLiteral("Invalid helper entry in update manifest"));
            return;
        }
    }
    if (!isNewerVersion(version, currentVersion_)) {
        setStatus(State::Current, QStringLiteral("Up to date – installed: %1").arg(currentVersion_));
        return;
    }

    available_ = {version, url, sha256, size, helper};
    hasAvailable_ = true;
    setStatus(State::Available, QStringLiteral("Update available: %1 → %2").arg(currentVersion_, version));
}

void UpdateChecker::installAvailableUpdate()
{
    if (busy() || !hasAvailable_ || state_ == State::Ready)
        return;

    if (resumePending()) return;
    if (updateLock_ < 0) updateLock_ = postexit::acquireLock(pendingDirectory_ + "/update.lock");
    if (updateLock_ < 0) {
        setStatus(State::Error, QStringLiteral("An update is already being prepared or installed"));
        return;
    }
    // Recheck after acquiring the inter-process lock.
    if (QFile::exists(pendingDirectory_ + "/pending.json")) {
        postexit::releaseLock(updateLock_); updateLock_ = -1;
        resumePending();
        return;
    }
    pluginPayload_.clear();
    helperPayload_.clear();
    setStatus(State::Downloading, QStringLiteral("Downloading update %1…").arg(available_.version));

    QNetworkRequest request(QUrl(available_.downloadUrl));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Bokis-Twitch-Chat-Plugin/%1").arg(currentVersion_));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = network_->get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleBinaryReply(reply); });
}

void UpdateChecker::handleBinaryReply(QNetworkReply *reply, bool helper)
{
    const auto unlock = qScopeGuard([this] {
        if (state_ != State::Downloading) {
            if (updateLock_ >= 0) { postexit::releaseLock(updateLock_); updateLock_ = -1; }
            pluginPayload_.clear();
            helperPayload_.clear();
        }
    });
    const auto guard = std::unique_ptr<QNetworkReply, void (*)(QNetworkReply *)>(reply, [](QNetworkReply *r) { r->deleteLater(); });
    const auto notify = qScopeGuard([this] { notifyStateChanged(); });

    if (reply->error() != QNetworkReply::NoError) {
        setStatus(State::Error, QStringLiteral("Update download failed: %1").arg(reply->errorString()));
        return;
    }

    const QByteArray payload = reply->readAll();
    const qint64 expectedSize = helper ? available_.helper->size : available_.size;
    const QString expectedHash = helper ? available_.helper->sha256 : available_.sha256;
    if (payload.size() != expectedSize) {
        setStatus(State::Error, QStringLiteral("Update rejected: file size does not match"));
        return;
    }

    const QString actualHash = QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    if (actualHash.compare(expectedHash, Qt::CaseInsensitive) != 0) {
        setStatus(State::Error, QStringLiteral("Update rejected: SHA-256 does not match"));
        return;
    }

    if (!postexit::validBinary(payload)) {
        setStatus(State::Error, QStringLiteral("Update rejected: invalid platform binary format"));
        return;
    }
    if (!helper && available_.helper) {
        pluginPayload_ = payload;
        setStatus(State::Downloading, QStringLiteral("Downloading updater for update %1…").arg(available_.version));
        QNetworkRequest request(QUrl(available_.helper->downloadUrl));
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Bokis-Twitch-Chat-Plugin/%1").arg(currentVersion_));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        auto *next = network_->get(request);
        QObject::connect(next, &QNetworkReply::finished, this, [this, next] { handleBinaryReply(next, true); });
        return; // Retain the same lock until both downloads and staging finish.
    }
    if (helper) helperPayload_ = payload;
    QString error;
    if (!stageBinaryAtomically(helper ? pluginPayload_ : payload, &error)) {
        setStatus(State::Error, QStringLiteral("Update could not be staged: %1").arg(error));
        return;
    }

    hasAvailable_ = false;
    if (!launcher_(pendingDirectory_, QFileInfo(targetPath_).absolutePath() + "/" + postexit::helperFileName(), updateLock_, error)) {
        setStatus(State::Error, QStringLiteral("Update saved, but helper failed to start: %1").arg(error));
        return;
    }
    setStatus(State::Ready, QStringLiteral("Update ready – close OBS completely. Installation will start automatically after it exits."));
}

bool UpdateChecker::stageBinaryAtomically(const QByteArray &payload, QString *errorMessage)
{
    // Never touch the loaded module. A separate installer may consume this file
    // only after OBS exits. Keep the filename independent of manifest input.
    const QString pendingDir = pendingDirectory_;
    if (!QDir().mkpath(pendingDir)) {
        *errorMessage = QStringLiteral("Pending directory could not be created");
        return false;
    }
    const QString binaryPath = pendingDir + "/" + postexit::pluginFileName();
    const QString helperPath = pendingDir + "/" + postexit::helperFileName();
    QStringList savedPaths;
    auto save = [&](const QString &path, const QByteArray &bytes) {
        if (QFileInfo(path).isSymLink() || QFileInfo(path).canonicalFilePath() == targetPath_) {
            *errorMessage = QStringLiteral("Unsafe pending path");
            return false;
        }
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() ||
            !output.flush() || !postexit::flushFile(output.handle()) || !output.commit()) {
            *errorMessage = QStringLiteral("Atomic staging failed: %1").arg(path);
            return false;
        }
        savedPaths.append(path);
        return true;
    };
    // Metadata is the commit marker: no helper may consume either file before it exists.
    bool complete = false;
    const auto cleanup = qScopeGuard([&] {
        if (!complete) {
            for (const auto &path : savedPaths) QFile::remove(path);
            QFile::remove(pendingDir + "/pending.json");
        }
    });
    if (!save(binaryPath, payload)) return false;
    postexit::Pending pending{currentVersion_, available_.version, available_.sha256,
        binaryPath, targetPath_, payload.size(), std::nullopt};
    if (available_.helper) {
        if (!save(helperPath, helperPayload_)) return false;
        pending.helper = postexit::PendingFile{available_.helper->sha256, helperPath,
            QFileInfo(targetPath_).absolutePath() + "/" + postexit::helperFileName(), helperPayload_.size()};
    }
    complete = postexit::writePending(pendingDir, pending, *errorMessage);
    return complete;
}
