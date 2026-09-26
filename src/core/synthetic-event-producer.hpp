#pragma once

#include "core/ordered-event-pipeline.hpp"

enum class SyntheticEventKind {
    ChatMessage, MessageDeleted, ChatCleared, Follow, Subscription,
    Resubscription, GiftSubscription, CommunityGiftSubscription, Cheer, Raid
};

struct SyntheticEventValues {
    QString displayName{QStringLiteral("Test User")};
    QString chatText{QStringLiteral("This is a test message.")};
    int cheerBits = 100;
    int raidViewers = 25;
    int giftCount = 5;
    int resubMonths = 6;
};

class SyntheticEventProducer final : public QObject {
public:
    using LogCallback = OrderedEventPipeline::LogCallback;
    SyntheticEventProducer(EventDispatcher &dispatcher, LogCallback log = {}, QNetworkAccessManager *assets = nullptr);
    void setEnabled(bool enabled);
    [[nodiscard]] bool isEnabled() const { return enabled_; }
    IngestResult inject(SyntheticEventKind kind, const SyntheticEventValues &values = {});

private:
    PluginEvent makeEvent(SyntheticEventKind kind, const SyntheticEventValues &values);
    OrderedEventPipeline pipeline_;
    LogCallback log_;
    bool enabled_ = false;
    std::uint64_t id_ = 1;
    QString lastMessageId_;
};
