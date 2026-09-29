#include "web/widget-status-ui.hpp"
#include <QApplication>
#include <QLabel>
#include <QTimer>
#include <QUuid>
#include <mutex>

struct WidgetStatusUi::Shared {
    std::mutex mutex;
    QString status = QStringLiteral("Web Widget loading");
    const QString marker = QStringLiteral("<span id=\"bokis-widget-%1\">").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool closed = false;
};

WidgetStatusUi::WidgetStatusUi() : shared_(std::make_shared<Shared>())
{
    auto *app = QCoreApplication::instance();
    if (!app) return;
    QMetaObject::invokeMethod(app, [shared = shared_] {
        if (!qobject_cast<QApplication *>(QCoreApplication::instance())) return;
        auto *timer = new QTimer(QCoreApplication::instance());
        QObject::connect(timer, &QTimer::timeout, timer, [shared, timer] {
            QString html;
            {
                std::lock_guard lock(shared->mutex);
                if (shared->closed) { timer->stop(); timer->deleteLater(); return; }
                html = shared->marker + shared->status.toHtmlEscaped() + "</span>";
            }
            // OBS exposes INFO properties as QLabels without stable object names.
            // Identify this source by its marker without retaining OBS property objects.
            for (auto *widget : QApplication::allWidgets()) {
                auto *label = qobject_cast<QLabel *>(widget);
                if (label && label->text().startsWith(shared->marker) && label->text() != html)
                    label->setText(html);
            }
        });
        timer->start(250);
    }, Qt::QueuedConnection);
}
WidgetStatusUi::~WidgetStatusUi()
{
    std::lock_guard lock(shared_->mutex);
    shared_->closed = true;
}
void WidgetStatusUi::publish(QString status)
{
    std::lock_guard lock(shared_->mutex);
    shared_->status = std::move(status);
}
QString WidgetStatusUi::html() const
{
    std::lock_guard lock(shared_->mutex);
    return shared_->marker + shared_->status.toHtmlEscaped() + "</span>";
}
