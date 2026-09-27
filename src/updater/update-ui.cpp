#include "updater/update-ui.hpp"
#include "updater/update-checker.hpp"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <mutex>
#include <vector>

namespace {
template<typename Function> void onUi(Function function)
{
    auto *app = QCoreApplication::instance();
    if (!app || QCoreApplication::closingDown()) return;
    if (QThread::currentThread() == app->thread()) function();
    else QMetaObject::invokeMethod(app, std::move(function), Qt::QueuedConnection);
}
}

struct UpdateUi::Shared {
    mutable std::mutex mutex;
    Snapshot snapshot;
    const QString marker = QStringLiteral("<span id=\"bokis-updater-%1\">")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    std::atomic_bool closed{false};
    Presenter *presenter = nullptr; // Accessed only on the application thread.

    QString html(const Snapshot &value) const
    {
        return marker + QStringLiteral("Status: ") + value.status.toHtmlEscaped().replace('\n', "<br>") + "</span>";
    }
};

class UpdateUi::Presenter final : public QObject {
public:
    Presenter(std::shared_ptr<Shared> shared, QString version, bool automaticCheck, const Factory &factory)
        : QObject(QCoreApplication::instance()), shared_(std::move(shared))
    {
        checker_ = factory(std::move(version), [this] { refresh(); });
        shared_->presenter = this;
        if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
            app->installEventFilter(this);
            // A source can be created on another thread while a properties view
            // is already being shown. Recover labels that predate this filter.
            for (auto *widget : QApplication::allWidgets()) {
                if (auto *label = qobject_cast<QLabel *>(widget);
                    label && label->text().startsWith(shared_->marker))
                    views_.push_back(label);
            }
        }
        refresh();
        if (automaticCheck)
            QTimer::singleShot(2500, this, [this] { request(false); });
    }
    ~Presenter() override { shared_->presenter = nullptr; }

    void request(bool install)
    {
        if (shared_->closed) return;
        if (install) checker_->installAvailableUpdate();
        else checker_->checkForUpdates();
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        // OBS creates OBS_TEXT_INFO as a QLabel with no property objectName.
        // Identify only our per-source label, then its sibling action buttons.
        // Show also handles reopening and rebuilds requested by other settings.
        if (event->type() == QEvent::Show && !shared_->closed) {
            if (auto *label = qobject_cast<QLabel *>(object);
                label && label->text().startsWith(shared_->marker)) {
                bool known = false;
                for (const auto &view : views_) known |= view == label;
                if (!known) views_.push_back(label);
                refresh();
            }
        }
        return false;
    }

private:
    void refresh()
    {
        if (!checker_ || shared_->closed) return;
        const Snapshot value{checker_->status(), checker_->canCheck(), checker_->canInstall()};
        {
            std::lock_guard lock(shared_->mutex);
            shared_->snapshot = value;
        }
        std::erase_if(views_, [](const auto &label) { return label.isNull(); });
        for (const auto &label : views_) {
            label->setText(shared_->html(value));
            label->setAccessibleName(QStringLiteral("Updater status"));
            auto *group = label->parentWidget();
            if (!group) continue;
            for (auto *button : group->findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly)) {
                if (button->text() == QLatin1String(CheckLabel)) button->setEnabled(value.canCheck);
                if (button->text() == QLatin1String(InstallLabel)) button->setEnabled(value.canInstall);
            }
        }
    }

    std::shared_ptr<Shared> shared_;
    std::unique_ptr<UpdateChecker> checker_;
    std::vector<QPointer<QLabel>> views_;
};

UpdateUi::UpdateUi(QString version, bool automaticCheck)
    : UpdateUi(std::move(version), automaticCheck, [](QString current, std::function<void()> changed) {
        return std::make_unique<UpdateChecker>(std::move(current), std::move(changed));
    })
{
}

UpdateUi::UpdateUi(QString version, bool automaticCheck, Factory factory) : shared_(std::make_shared<Shared>())
{
    onUi([shared = shared_, version = std::move(version), automaticCheck, factory = std::move(factory)] {
        if (!shared->closed) new Presenter(shared, version, automaticCheck, factory);
    });
}

UpdateUi::~UpdateUi()
{
    shared_->closed = true;
    onUi([shared = shared_] { delete shared->presenter; });
}

UpdateUi::Snapshot UpdateUi::snapshot() const
{
    std::lock_guard lock(shared_->mutex);
    return shared_->snapshot;
}

QString UpdateUi::statusHtml(const Snapshot &value) const { return shared_->html(value); }
void UpdateUi::checkForUpdates() { request(false); }
void UpdateUi::installAvailableUpdate() { request(true); }
void UpdateUi::request(bool install)
{
    onUi([shared = shared_, install] {
        if (!shared->closed && shared->presenter) shared->presenter->request(install);
    });
}
