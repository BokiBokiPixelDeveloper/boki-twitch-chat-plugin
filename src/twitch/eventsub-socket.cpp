#include "twitch/eventsub-socket.hpp"
#include <QWebSocket>

namespace {
class WebSocketTransport final : public EventSubSocket {
public:
    WebSocketTransport()
    {
        socket_.setMaxAllowedIncomingFrameSize(1024 * 1024);
        socket_.setMaxAllowedIncomingMessageSize(1024 * 1024);
        QObject::connect(&socket_, &QWebSocket::textMessageReceived, this, &EventSubSocket::messageReceived);
        QObject::connect(&socket_, &QWebSocket::disconnected, this, &EventSubSocket::connectionLost);
        QObject::connect(&socket_, &QWebSocket::errorOccurred, this, [this] { Q_EMIT connectionLost(); });
    }
    void open(const QUrl &url) override { socket_.open(url); }
    void close() override { socket_.abort(); }
private:
    QWebSocket socket_;
};
} // namespace

std::unique_ptr<EventSubSocket> makeEventSubSocket() { return std::make_unique<WebSocketTransport>(); }
