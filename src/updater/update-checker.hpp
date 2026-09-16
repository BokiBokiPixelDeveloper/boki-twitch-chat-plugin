#pragma once

#include "updater/post-exit.hpp"
#include <QObject>
#include <QByteArray>
#include <QDir>
#include <QString>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

struct RemoteArtifact {
    QString downloadUrl, sha256;
    qint64 size = -1;
};
struct AvailableUpdate {
    QString version;
    QString downloadUrl;
    QString sha256;
    qint64 size = -1;
    std::optional<RemoteArtifact> helper;
};

class UpdateChecker final : public QObject {
public:
    enum class State { Idle, Checking, Current, Available, Downloading, Ready, Error };

    using StateCallback = std::function<void()>;

    explicit UpdateChecker(QString currentVersion, StateCallback stateChanged, QObject *parent = nullptr);
    ~UpdateChecker() override;

    void checkForUpdates();
    void installAvailableUpdate();

    QString status() const { return lastResult_.isEmpty() ? status_ : lastResult_ + "\n" + status_; }
    QString availableVersion() const { return available_.version; }
    bool hasAvailableUpdate() const { return hasAvailable_; }
    bool busy() const { return state_ == State::Checking || state_ == State::Downloading; }
    State state() const { return state_; }

private:
    friend class UpdateCheckerTests;
    void setStatus(State state, QString status);
    void notifyStateChanged();
    void handleManifestReply(QNetworkReply *reply);
    void handleBinaryReply(QNetworkReply *reply, bool helper = false);
    bool stageBinaryAtomically(const QByteArray &payload, QString *errorMessage);
    static bool isNewerVersion(const QString &candidate, const QString &current);

    bool resumePending();
    QString pendingDirectory_{postexit::cacheDirectory()};
    QString targetPath_{postexit::modulePath()};
    std::function<bool(const QString &, const QString &, int, QString &)> launcher_{postexit::launch};
    int updateLock_ = -1;
    QString lastResult_;
    QByteArray pluginPayload_, helperPayload_;
    QString currentVersion_;
    QString status_{QStringLiteral("Not checked yet")};
    AvailableUpdate available_;
    bool hasAvailable_ = false;
    State state_ = State::Idle;
    StateCallback stateChanged_;
    std::unique_ptr<QNetworkAccessManager> network_;
};
