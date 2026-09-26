#pragma once

#include "core/event-dispatcher.hpp"
#include "chat/emote-service.hpp"
#include <QElapsedTimer>
#include <QTimer>

enum class IngestResult { Accepted, Duplicate, Ignored, Invalid, WrongChannel, Stopped };

// One producer-thread pipeline for all event types. Assets cannot reorder events.
class OrderedEventPipeline : public QObject {
public:
    using LogCallback = std::function<void(QString)>;
    explicit OrderedEventPipeline(EventDispatcher &dispatcher, LogCallback log = {},
                                  QNetworkAccessManager *assets = nullptr);
    ~OrderedEventPipeline() override;
    void setChannel(QString channelId, std::uint64_t generation);
    void stop();
    IngestResult ingest(const QByteArray &envelope);

private:
    struct Pending {
        PluginEvent event;
        QDeadlineTimer deadline{2500};
        bool ready = false;
    };
    void flush();
    void enforceBudget();
    EventDispatcher &dispatcher_;
    LogCallback log_;
    EmoteService emotes_;
    QTimer flushTimer_;
    QElapsedTimer clock_;
    QString channelId_;
    std::uint64_t generation_ = 0;
    std::uint64_t nextSequence_ = 1;
    std::deque<std::shared_ptr<Pending>> pending_;
    QHash<QString, qint64> seen_;
    std::deque<std::pair<QString, qint64>> seenOrder_;
};
