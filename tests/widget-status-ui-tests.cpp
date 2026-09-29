#include "web/widget-status-ui.hpp"
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <thread>

class WidgetStatusUiTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void updatesWithoutReplacingProperties()
    {
        auto status = std::make_unique<WidgetStatusUi>();
        WidgetStatusUi other;
        QScrollArea properties;
        auto *label = new QLabel(status->html());
        label->setMinimumHeight(2000);
        properties.setWidget(label); properties.resize(400, 200); properties.show();
        QLabel unrelated(other.html()); unrelated.show();
        properties.verticalScrollBar()->setValue(300);
        const auto initialPosition = properties.verticalScrollBar()->value();
        std::thread worker([&] { status->publish("Web Widget ready"); }); worker.join();
        QTRY_VERIFY(label->text().contains("ready"));
        QCOMPARE(properties.widget(), label);
        QCOMPARE(properties.verticalScrollBar()->value(), initialPosition);
        QVERIFY(!unrelated.text().contains("ready"));
        status->publish("Web Widget failed: <script>");
        QTRY_VERIFY(label->text().contains("&lt;script&gt;"));
        status.reset();
        QTest::qWait(300); // Pending presenter callbacks cannot access the destroyed owner.
    }
};
QTEST_MAIN(WidgetStatusUiTests)
#include "widget-status-ui-tests.moc"
