#include "updater/update-ui.hpp"
#include "updater/update-checker.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <thread>

namespace {
class Reply final : public QNetworkReply {
public:
    explicit Reply(QObject *parent) : QNetworkReply(parent) { open(ReadOnly); }
    void abort() override {}
    void finish()
    {
        setError(ConnectionRefusedError, QStringLiteral("Test connection failure"));
        setFinished(true);
        Q_EMIT finished();
    }
protected:
    qint64 readData(char *, qint64) override { return -1; }
};
class Network final : public QNetworkAccessManager {
public:
    int requests = 0;
    QPointer<Reply> reply;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &, QIODevice *) override
    {
        ++requests;
        reply = new Reply(this);
        return reply;
    }
};

// Mirrors OBS AddGroup/AddText(OBS_TEXT_INFO)/AddButton: unnamed sibling
// widgets in a QFormLayout. Production never depends on OBS private C++ types.
struct Properties : QGroupBox {
    QLabel *status;
    QPushButton *check;
    QPushButton *install;
    QCheckBox *unrelated;
    explicit Properties(UpdateUi &ui)
    {
        auto *layout = new QFormLayout(this);
        const auto value = ui.snapshot();
        status = new QLabel(ui.statusHtml(value));
        status->setWordWrap(true);
        check = new QPushButton(UpdateUi::CheckLabel);
        install = new QPushButton(UpdateUi::InstallLabel);
        unrelated = new QCheckBox("Automatically check for updates on startup");
        check->setEnabled(value.canCheck);
        install->setEnabled(value.canInstall);
        layout->addRow(unrelated);
        layout->addRow(status);
        layout->addRow(check);
        layout->addRow(install);
        QObject::connect(check, &QPushButton::clicked, &uiContext, [&ui] { ui.checkForUpdates(); });
        QObject::connect(install, &QPushButton::clicked, &uiContext, [&ui] { ui.installAvailableUpdate(); });
        show();
    }
    QObject uiContext;
};
}

extern "C" uint32_t obs_get_version() { return (32u << 24) | (2u << 16) | 2u; }

class UpdateUiTests : public QObject {
    Q_OBJECT
    UpdateChecker *checker_ = nullptr;
    Network *network_ = nullptr;
    QTemporaryDir directory_;
    UpdateUi::Factory factory()
    {
        return [this](QString version, std::function<void()> changed) {
            auto checker = std::make_unique<UpdateChecker>(std::move(version), std::move(changed));
            checker_ = checker.get();
            auto network = std::make_unique<Network>();
            network_ = network.get();
            checker->network_ = std::move(network);
            checker->pendingDirectory_ = directory_.path() + "/pending";
            return checker;
        };
    }
private Q_SLOTS:
    void immediateFeedbackAndRepeatedClicks()
    {
        UpdateUi ui("1.0.0", false, factory());
        Properties properties(ui);
        QPointer<QLabel> original = properties.status;
        QVERIFY(properties.check->isEnabled());
        QVERIFY(!properties.install->isEnabled());
        properties.check->click();
        QVERIFY(properties.status->text().contains("Checking for updates"));
        QVERIFY(!properties.check->isEnabled());
        QVERIFY(!properties.install->isEnabled());
        QVERIFY(properties.unrelated->isEnabled());
        for (int i = 0; i < 20; ++i) {
            properties.check->click();
            properties.install->click();
            ui.checkForUpdates(); // Exercise the secondary backend guard too.
            ui.installAvailableUpdate();
        }
        QCOMPARE(network_->requests, 1);
        network_->reply->finish();
        QVERIFY(properties.status->text().contains("Update failed"));
        QVERIFY(properties.check->isEnabled());
        QCOMPARE(original.data(), properties.status);
    }

    void installStatesAndHtmlEscaping()
    {
        UpdateUi ui("1.0.0", false, factory());
        Properties properties(ui);
        checker_->hasAvailable_ = true;
        checker_->available_.version = "2.0.0";
        checker_->available_.downloadUrl = "https://example.invalid/plugin";
        checker_->setStatus(UpdateChecker::State::Available, "Update available: 2.0.0");
        QVERIFY(properties.check->isEnabled());
        QVERIFY(properties.install->isEnabled());
        properties.install->click();
        QVERIFY(properties.status->text().contains("Downloading update 2.0.0"));
        QVERIFY(!properties.check->isEnabled());
        QVERIFY(!properties.install->isEnabled());
        for (int i = 0; i < 20; ++i) {
            ui.installAvailableUpdate();
            ui.checkForUpdates();
        }
        QCOMPARE(network_->requests, 1);
        network_->reply->finish();
        QVERIFY(properties.status->text().contains("download failed"));
        QVERIFY(properties.check->isEnabled());
        QVERIFY(properties.install->isEnabled());
        checker_->setStatus(UpdateChecker::State::Error, "Update failed: <b>file</b> & reason");
        QVERIFY(properties.status->text().contains("&lt;b&gt;file&lt;/b&gt; &amp; reason"));
        checker_->hasAvailable_ = false;
        checker_->setStatus(UpdateChecker::State::Ready, "Update ready – close OBS completely.");
        QVERIFY(!properties.check->isEnabled());
        QVERIFY(!properties.install->isEnabled());
    }

    void closeReopenAndExternalRebuild()
    {
        UpdateUi ui("1.0.0", false, factory());
        auto properties = std::make_unique<Properties>(ui);
        properties->check->click();
        properties.reset();
        // Reopening during a request restores the busy state.
        properties = std::make_unique<Properties>(ui);
        QVERIFY(properties->status->text().contains("Checking"));
        QVERIFY(!properties->check->isEnabled());
        properties.reset();
        network_->reply->finish(); // No live properties widgets.
        properties = std::make_unique<Properties>(ui);
        QVERIFY(properties->status->text().contains("Update failed"));
        QVERIFY(properties->check->isEnabled());
        checker_->setStatus(UpdateChecker::State::Current, "Plugin is up to date.");
        QVERIFY(properties->status->text().contains("up to date"));
        properties.reset();
        properties = std::make_unique<Properties>(ui);
        QVERIFY(properties->status->text().contains("up to date"));
    }

    void independentSourcesAndMultipleViews()
    {
        UpdateUi first("1.0.0", false, factory());
        auto *firstNetwork = network_;
        Properties a(first), b(first);
        UpdateUi second("1.0.0", false, factory());
        Properties c(second);
        a.check->click();
        QVERIFY(!a.check->isEnabled());
        QVERIFY(!b.check->isEnabled());
        QVERIFY(c.check->isEnabled());
        QCOMPARE(firstNetwork->requests, 1);
        QCOMPARE(network_->requests, 0);
    }

    void sourceDestroyedWithReplyQueued()
    {
        auto ui = std::unique_ptr<UpdateUi>(new UpdateUi("1.0.0", false, factory()));
        QPointer<UpdateChecker> checker = checker_;
        ui->checkForUpdates();
        QPointer<Reply> reply = network_->reply;
        QTimer::singleShot(0, reply, [reply] { reply->finish(); });
        // OBS may release its last source reference on a non-UI thread.
        std::thread worker([&] { ui.reset(); });
        worker.join();
        QTRY_VERIFY(checker.isNull());
        QVERIFY(reply.isNull());
    }

    void workerCreationAndDelayedInitialization()
    {
        std::unique_ptr<UpdateUi> ui;
        std::thread worker([&] { ui.reset(new UpdateUi("1.0.0", false, factory())); });
        worker.join();
        Properties properties(*ui); // Shown before the queued presenter exists.
        QVERIFY(!properties.check->isEnabled());
        QTRY_VERIFY(properties.check->isEnabled());
        QCOMPARE(checker_->thread(), qApp->thread());
        QCOMPARE(network_->thread(), qApp->thread());
        properties.check->click();
        QVERIFY(properties.status->text().contains("Checking"));
    }

    void createAndDestroyBeforeUiDelivery()
    {
        bool constructed = false;
        std::thread worker([&] {
            UpdateUi ui("1.0.0", false, [&](QString, std::function<void()>) {
                constructed = true;
                return std::unique_ptr<UpdateChecker>();
            });
            ui.checkForUpdates();
        });
        worker.join();
        QCoreApplication::processEvents();
        QVERIFY(!constructed);
    }
};
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir isolated;
    qputenv("XDG_CACHE_HOME", isolated.path().toUtf8());
    qputenv("XDG_STATE_HOME", isolated.path().toUtf8());
    UpdateUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "update-ui-tests.moc"
