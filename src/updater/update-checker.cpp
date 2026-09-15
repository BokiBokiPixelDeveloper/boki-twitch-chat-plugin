#include "updater/update-checker.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QScopeGuard>
#include <QVersionNumber>

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
}

UpdateChecker::~UpdateChecker() = default;

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
    if (busy() || state_ == State::Ready)
        return;

    hasAvailable_ = false;
    available_ = {};
    setStatus(State::Checking, QStringLiteral("Suche nach Updates …"));

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
        setStatus(State::Error, QStringLiteral("Update-Quelle nicht erreichbar: %1. Ist das Release-Repo noch privat?")
                      .arg(reply->errorString()));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        setStatus(State::Error, QStringLiteral("Ungültiges Update-Manifest"));
        return;
    }

    const QJsonObject root = doc.object();
    const QString version = root.value(QStringLiteral("version")).toString();
    const QJsonObject platform = root.value(QStringLiteral("platforms")).toObject().value(QStringLiteral("linux-x86_64")).toObject();
    const QString url = platform.value(QStringLiteral("url")).toString();
    const QString sha256 = platform.value(QStringLiteral("sha256")).toString().toLower();
    const qint64 size = static_cast<qint64>(platform.value(QStringLiteral("size")).toDouble(-1));

    if (version.isEmpty() || url.isEmpty() || sha256.size() != 64) {
        setStatus(State::Error, QStringLiteral("Update-Manifest ist unvollständig"));
        return;
    }

    if (!isNewerVersion(version, currentVersion_)) {
        setStatus(State::Current, QStringLiteral("Aktuell – installiert: %1").arg(currentVersion_));
        return;
    }

    available_ = {version, url, sha256, size};
    hasAvailable_ = true;
    setStatus(State::Available, QStringLiteral("Update verfügbar: %1 → %2").arg(currentVersion_, version));
}

void UpdateChecker::installAvailableUpdate()
{
    if (busy() || !hasAvailable_ || state_ == State::Ready)
        return;

    setStatus(State::Downloading, QStringLiteral("Lade Update %1 …").arg(available_.version));

    QNetworkRequest request(QUrl(available_.downloadUrl));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Bokis-Twitch-Chat-Plugin/%1").arg(currentVersion_));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = network_->get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleBinaryReply(reply); });
}

void UpdateChecker::handleBinaryReply(QNetworkReply *reply)
{
    const auto guard = std::unique_ptr<QNetworkReply, void (*)(QNetworkReply *)>(reply, [](QNetworkReply *r) { r->deleteLater(); });
    const auto notify = qScopeGuard([this] { notifyStateChanged(); });

    if (reply->error() != QNetworkReply::NoError) {
        setStatus(State::Error, QStringLiteral("Update-Download fehlgeschlagen: %1").arg(reply->errorString()));
        return;
    }

    const QByteArray payload = reply->readAll();
    if (available_.size >= 0 && payload.size() != available_.size) {
        setStatus(State::Error, QStringLiteral("Update verworfen: Dateigröße stimmt nicht"));
        return;
    }

    const QString actualHash = QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    if (actualHash.compare(available_.sha256, Qt::CaseInsensitive) != 0) {
        setStatus(State::Error, QStringLiteral("Update verworfen: SHA-256 stimmt nicht"));
        return;
    }

    QString error;
    if (!stageBinaryAtomically(payload, &error)) {
        setStatus(State::Error, QStringLiteral("Update konnte nicht bereitgestellt werden: %1").arg(error));
        return;
    }

    hasAvailable_ = false;
    setStatus(State::Ready, QStringLiteral("Update bereit – OBS neu starten"));
}

bool UpdateChecker::stageBinaryAtomically(const QByteArray &payload, QString *errorMessage)
{
    // Never touch the loaded module. A separate installer may consume this file
    // only after OBS exits. Keep the filename independent of manifest input.
    const QString pendingDir = pendingDirectory_;
    if (!QDir().mkpath(pendingDir)) {
        *errorMessage = QStringLiteral("Pending-Verzeichnis konnte nicht erstellt werden");
        return false;
    }
    const QString binaryPath = pendingDir + QStringLiteral("/bokis-twitch-chat-plugin.so");

    QSaveFile output(binaryPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        *errorMessage = QStringLiteral("Keine Schreibrechte auf %1").arg(binaryPath);
        return false;
    }
    if (output.write(payload) != payload.size()) {
        output.cancelWriting();
        *errorMessage = QStringLiteral("Update konnte nicht vollständig geschrieben werden");
        return false;
    }
    if (!output.commit()) {
        *errorMessage = QStringLiteral("Atomare Bereitstellung ist fehlgeschlagen");
        return false;
    }

    QFile::setPermissions(binaryPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                         QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                         QFileDevice::ExeOther);
    return true;
}
