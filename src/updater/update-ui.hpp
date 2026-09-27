#pragma once

#include <QString>
#include <memory>
#include <functional>

class UpdateChecker;

// Thread-safe source-facing handle. The checker and Qt widgets live on the UI
// thread; neither queued work nor network completions retain a ChatSource.
class UpdateUi final {
public:
    struct Snapshot {
        QString status = QStringLiteral("Initializing updater…");
        bool canCheck = false;
        bool canInstall = false;
    };
    UpdateUi(QString version, bool automaticCheck);
    ~UpdateUi();
    UpdateUi(const UpdateUi &) = delete;
    UpdateUi &operator=(const UpdateUi &) = delete;

    Snapshot snapshot() const;
    QString statusHtml(const Snapshot &snapshot) const;
    void checkForUpdates();
    void installAvailableUpdate();

    static constexpr auto CheckLabel = "Check for updates";
    static constexpr auto InstallLabel = "Install update";

private:
    friend class UpdateUiTests;
    using Factory = std::function<std::unique_ptr<UpdateChecker>(QString, std::function<void()>)>;
    UpdateUi(QString version, bool automaticCheck, Factory factory);
    struct Shared;
    class Presenter;
    std::shared_ptr<Shared> shared_;
    void request(bool install);
};
