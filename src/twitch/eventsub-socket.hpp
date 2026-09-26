#pragma once
#include <QObject>
#include <QUrl>
#include <memory>

// Transport seam for offline tests. No application events bypass normalization.
class EventSubSocket : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void open(const QUrl &url) = 0;
    virtual void close() = 0;
Q_SIGNALS:
    void messageReceived(QString message);
    void connectionLost();
};
std::unique_ptr<EventSubSocket> makeEventSubSocket();
