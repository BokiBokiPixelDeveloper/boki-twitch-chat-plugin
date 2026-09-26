#include "twitch/chat-message-parser.hpp"

#include <QJsonArray>
#include <QRegularExpression>

ChatUser parseTwitchUser(const QJsonObject &object, const QString &prefix)
{
    return {object.value(prefix + QStringLiteral("user_id")).toString(),
            object.value(prefix + QStringLiteral("user_login")).toString(),
            object.value(prefix + QStringLiteral("user_name")).toString(), {}};
}

ChatMessage parseTwitchMessage(const QJsonObject &event)
{
    ChatMessage out;
    out.messageId = event.value(QStringLiteral("message_id")).toString();
    if (!event.value(QStringLiteral("chatter_is_anonymous")).toBool())
        out.user = parseTwitchUser(event, QStringLiteral("chatter_"));
    const auto rawColor = event.value(QStringLiteral("color")).toString();
    static const QRegularExpression twitchColor(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    if (twitchColor.match(rawColor).hasMatch())
        out.user.color = QColor(rawColor);
    out.metadata.messageType = event.value(QStringLiteral("message_type")).toString();
    out.metadata.systemText = event.value(QStringLiteral("system_message")).toString();
    out.metadata.rewardId = event.value(QStringLiteral("channel_points_custom_reward_id")).toString();
    const auto reply = event.value(QStringLiteral("reply"));
    if (reply.isObject()) {
        const auto object = reply.toObject();
        out.metadata.reply = ReplyMetadata{object.value(QStringLiteral("parent_message_id")).toString(),
                                          object.value(QStringLiteral("thread_message_id")).toString()};
    }
    const auto cheer = event.value(QStringLiteral("cheer")).toObject().value(QStringLiteral("bits"));
    if (cheer.isDouble() && cheer.toInt(-1) >= 0)
        out.metadata.cheerBits = cheer.toInt();
    for (const auto &value : event.value(QStringLiteral("badges")).toArray()) {
        const auto badge = value.toObject();
        const auto type = badge.value(QStringLiteral("set_id")).toString();
        const auto version = badge.value(QStringLiteral("id")).toString();
        if (!type.isEmpty() && !version.isEmpty())
            out.badges.push_back({BadgeProvider::Twitch, type, version,
                                 badge.value(QStringLiteral("info")).toString(), {}});
    }
    const auto message = event.value(QStringLiteral("message")).toObject();
    out.text = message.value(QStringLiteral("text")).toString();
    QString fragmentText;
    for (const auto &value : message.value(QStringLiteral("fragments")).toArray()) {
        const auto part = value.toObject();
        const auto type = part.value(QStringLiteral("type")).toString();
        ChatFragment fragment;
        fragment.text = part.value(QStringLiteral("text")).toString();
        fragment.sourceRange = TextRange{fragmentText.size(), fragment.text.size()};
        if (type == QStringLiteral("emote")) {
            const auto emote = part.value(QStringLiteral("emote")).toObject();
            fragment.emoteId = emote.value(QStringLiteral("id")).toString();
            static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9_-]+$"));
            if (validId.match(fragment.emoteId).hasMatch()) {
                fragment.type = ChatFragment::Type::Emote;
                fragment.provider = EmoteProvider::Twitch;
                const auto formats = emote.value(QStringLiteral("format")).toArray();
                fragment.twitch = TwitchEmoteMetadata{
                    emote.value(QStringLiteral("emote_set_id")).toString(),
                    emote.value(QStringLiteral("owner_id")).toString(),
                    formats.contains(QStringLiteral("static")), formats.contains(QStringLiteral("animated"))};
                const QString format = fragment.twitch->supportsAnimated ? QStringLiteral("animated")
                    : formats.isEmpty() ? QStringLiteral("default") : QStringLiteral("static");
                const QString base = QStringLiteral("https://static-cdn.jtvnw.net/emoticons/v2/%1/").arg(fragment.emoteId);
                fragment.imageUrl = QUrl(base + format + QStringLiteral("/dark/3.0"));
                fragment.fallbackUrl = QUrl(base + QStringLiteral("static/dark/3.0"));
            }
        } else if (type == QStringLiteral("mention")) {
            fragment.mention = parseTwitchUser(part.value(QStringLiteral("mention")).toObject(), {});
        } else if (type == QStringLiteral("cheermote")) {
            const auto cheerPart = part.value(QStringLiteral("cheermote")).toObject();
            fragment.cheermote = CheermoteMetadata{cheerPart.value(QStringLiteral("prefix")).toString(),
                cheerPart.value(QStringLiteral("bits")).toInt(), cheerPart.value(QStringLiteral("tier")).toInt()};
        } else if (type == QStringLiteral("gif")) {
            // Preserve the legacy extension as media, not a new Twitch event type.
            const auto rawUrl = part.value(QStringLiteral("gif")).toObject().value(QStringLiteral("url")).toString();
            const QUrl url(rawUrl, QUrl::StrictMode);
            if (!rawUrl.isEmpty() && url.isValid()) out.media.push_back({url, {}});
        }
        fragmentText += fragment.text;
        out.fragments.push_back(std::move(fragment));
    }
    // A partial/malformed fragments array must never silently discard message text.
    if (fragmentText != out.text || out.fragments.empty()) {
        out.fragments = {{ChatFragment::Type::Text, out.text}};
        out.fragments.front().sourceRange = TextRange{0, out.text.size()};
    }
    return out;
}
