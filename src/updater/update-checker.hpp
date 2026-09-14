#pragma once

#include <QObject>
#include <QByteArray>
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
    using StateCallback = std::function<void()>;

    explicit UpdateChecker(QString currentVersion, StateCallback stateChanged, QObject *parent = nullptr);
    ~UpdateChecker() override;

    void checkForUpdates();
    void installAvailableUpdate();

    QString status() const { return status_; }
    QString availableVersion() const { return available_.version; }
    bool hasAvailableUpdate() const { return hasAvailable_; }
    bool busy() const { return busy_; }

private:
    void setStatus(QString status);
    void handleManifestReply(QNetworkReply *reply);
    void handleBinaryReply(QNetworkReply *reply);
    bool installBinaryAtomically(const QByteArray &payload, QString *errorMessage);
    static bool isNewerVersion(const QString &candidate, const QString &current);

    QString currentVersion_;
    QString status_{QStringLiteral("Noch nicht geprüft")};
    AvailableUpdate available_;
    bool hasAvailable_ = false;
    bool busy_ = false;
    StateCallback stateChanged_;
    std::unique_ptr<QNetworkAccessManager> network_;
};
