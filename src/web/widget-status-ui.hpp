#pragma once
#include <QString>
#include <memory>

// A source-independent, thread-safe status mailbox. The presenter only owns Qt
// labels, never a source/runtime pointer, and updates them without rebuilding OBS properties.
class WidgetStatusUi final {
public:
    WidgetStatusUi();
    ~WidgetStatusUi();
    void publish(QString status);
    QString html() const;
private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};
