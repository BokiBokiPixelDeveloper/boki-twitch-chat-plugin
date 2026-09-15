#pragma once

#include <QObject>
#include <QByteArray>
#include <QDir>
#include <QString>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

struct AvailableUpdate {
    QString version;
    QString downloadUrl;
    QString sha256;
    qint64 size = -1;
};

class UpdateChecker final : public QObject {
public:
    enum class State { Idle, Checking, Current, Available, Downloading, Ready, Error };

    using StateCallback = std::function<void()>;

    explicit UpdateChecker(QString currentVersion, StateCallback stateChanged, QObject *parent = nullptr);
    ~UpdateChecker() override;

    void checkForUpdates();
    void installAvailableUpdate();

    QString status() const { return status_; }
    QString availableVersion() const { return available_.version; }
    bool hasAvailableUpdate() const { return hasAvailable_; }
    bool busy() const { return state_ == State::Checking || state_ == State::Downloading; }
    State state() const { return state_; }

private:
    friend class UpdateCheckerTests;
    void setStatus(State state, QString status);
    void notifyStateChanged();
    void handleManifestReply(QNetworkReply *reply);
    void handleBinaryReply(QNetworkReply *reply);
    bool stageBinaryAtomically(const QByteArray &payload, QString *errorMessage);
    static bool isNewerVersion(const QString &candidate, const QString &current);

    QString pendingDirectory_{QDir::homePath() + QStringLiteral("/.cache/bokis-twitch-chat-plugin/pending")};
    QString currentVersion_;
    QString status_{QStringLiteral("Noch nicht geprüft")};
    AvailableUpdate available_;
    bool hasAvailable_ = false;
    State state_ = State::Idle;
    StateCallback stateChanged_;
    std::unique_ptr<QNetworkAccessManager> network_;
};
