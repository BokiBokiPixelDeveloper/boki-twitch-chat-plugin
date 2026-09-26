#include "core/synthetic-event-producer.hpp"

namespace {
QString kindName(SyntheticEventKind kind)
{
    switch (kind) {
    case SyntheticEventKind::ChatMessage: return QStringLiteral("ChatMessage");
    case SyntheticEventKind::MessageDeleted: return QStringLiteral("MessageDeleted");
    case SyntheticEventKind::ChatCleared: return QStringLiteral("ChatCleared");
    case SyntheticEventKind::Follow: return QStringLiteral("Follow");
    case SyntheticEventKind::Subscription: return QStringLiteral("Subscription");
    case SyntheticEventKind::Resubscription: return QStringLiteral("Resubscription");
    case SyntheticEventKind::GiftSubscription: return QStringLiteral("GiftSubscription");
    case SyntheticEventKind::CommunityGiftSubscription: return QStringLiteral("CommunityGiftSubscription");
    case SyntheticEventKind::Cheer: return QStringLiteral("Cheer");
    case SyntheticEventKind::Raid: return QStringLiteral("Raid");
    }
    return QStringLiteral("Unknown");
}

ChatUser user(QString id, QString displayName, QColor color = QColor(QStringLiteral("#9147ff")))
{
    return {std::move(id), QStringLiteral("testuser"), std::move(displayName), std::move(color)};
}

ChatMessage notice(QString id, const ChatUser &actor, QString text, QString systemText)
{
    ChatMessage message;
    message.messageId = std::move(id);
    message.user = actor;
    message.text = std::move(text);
    message.fragments = {{ChatFragment::Type::Text, message.text}};
    message.metadata.systemText = std::move(systemText);
    return message;
}
}

SyntheticEventProducer::SyntheticEventProducer(EventDispatcher &dispatcher, LogCallback log, QNetworkAccessManager *assets)
    : pipeline_(dispatcher, log, assets), log_(std::move(log))
{
    pipeline_.setChannel(QStringLiteral("synthetic-test-channel"), 1, false);
}

void SyntheticEventProducer::setEnabled(bool enabled)
{
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (log_) log_(enabled ? QStringLiteral("[EventTest] Synthetic event testing enabled")
                           : QStringLiteral("[EventTest] Synthetic event testing disabled"));
}

IngestResult SyntheticEventProducer::inject(SyntheticEventKind kind, const SyntheticEventValues &values)
{
    if (!enabled_) return IngestResult::Stopped;
    auto event = makeEvent(kind, values);
    if (log_) log_(QStringLiteral("[EventTest] Injecting synthetic %1 event").arg(kindName(kind)));
    return pipeline_.ingest(std::move(event));
}

PluginEvent SyntheticEventProducer::makeEvent(SyntheticEventKind kind, const SyntheticEventValues &values)
{
    const auto now = QDateTime::currentDateTimeUtc();
    const auto suffix = QString::number(id_++);
    const auto eventId = QStringLiteral("synthetic-event-") + suffix;
    const auto actor = user(QStringLiteral("synthetic-user"), values.displayName);
    EventPayload payload;
    switch (kind) {
    case SyntheticEventKind::ChatMessage: {
        lastMessageId_ = QStringLiteral("synthetic-message-") + suffix;
        auto message = notice(lastMessageId_, actor, values.chatText, {});
        payload = std::move(message); break;
    }
    case SyntheticEventKind::MessageDeleted:
        payload = MessageDeleted{lastMessageId_.isEmpty() ? QStringLiteral("synthetic-message-none") : lastMessageId_, actor}; break;
    case SyntheticEventKind::ChatCleared: payload = ChatCleared{}; break;
    case SyntheticEventKind::Follow: payload = Follow{actor, now}; break;
    case SyntheticEventKind::Subscription:
        payload = Subscription{notice(QStringLiteral("synthetic-sub-") + suffix, actor, QStringLiteral("Subscribed at Tier 1"),
                                      QStringLiteral("Test User subscribed at Tier 1.")),
                               {SubscriptionTier::Tier1, false, 1}}; break;
    case SyntheticEventKind::Resubscription:
        payload = Resubscription{notice(QStringLiteral("synthetic-resub-") + suffix, actor, QStringLiteral("Glad to be back!"),
                                        QStringLiteral("Test User subscribed for %1 months.").arg(values.resubMonths)),
                                 {SubscriptionTier::Tier1, false, 1}, values.resubMonths, values.resubMonths, false, {}}; break;
    case SyntheticEventKind::GiftSubscription: {
        const auto recipient = user(QStringLiteral("synthetic-recipient"), QStringLiteral("Test Recipient"));
        payload = GiftSubscription{notice(QStringLiteral("synthetic-gift-") + suffix, actor, QStringLiteral("Enjoy the gift!"),
                                           QStringLiteral("Test User gifted a subscription to Test Recipient.")),
                                   {false, actor}, recipient, {SubscriptionTier::Tier1, false, 1}, {}, 1}; break;
    }
    case SyntheticEventKind::CommunityGiftSubscription:
        payload = CommunityGiftSubscription{notice(QStringLiteral("synthetic-community-") + suffix, actor, QStringLiteral("Community gifts!"),
                                                    QStringLiteral("Test User gifted %1 subscriptions.").arg(values.giftCount)),
                                            {false, actor}, SubscriptionTier::Tier1,
                                            QStringLiteral("synthetic-community-") + suffix, values.giftCount, values.giftCount}; break;
    case SyntheticEventKind::Cheer: payload = Cheer{actor, values.cheerBits, QStringLiteral("Cheer%1 Great stream!").arg(values.cheerBits)}; break;
    case SyntheticEventKind::Raid:
        payload = Raid{user(QStringLiteral("synthetic-raider"), QStringLiteral("Test Raider")),
                       user(QStringLiteral("synthetic-channel"), QStringLiteral("Test Channel")), values.raidViewers}; break;
    }
    return {{eventId, now, now, QStringLiteral("synthetic-test-channel"), {}, {}, 0, 0, EventOrigin::SyntheticTest},
            std::move(payload)};
}
