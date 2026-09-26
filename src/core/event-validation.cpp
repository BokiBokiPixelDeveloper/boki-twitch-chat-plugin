#include "core/event-validation.hpp"

#include <QRegularExpression>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <limits>

namespace {
constexpr qsizetype maxText = 8192;
constexpr qsizetype maxDisplayName = 128;
constexpr qsizetype maxIdBytes = 256;

bool hasInvalidUnicode(const QString &value)
{
    for (qsizetype i = 0; i < value.size(); ++i) {
        const auto c = value.at(i).unicode();
        if (QChar::isHighSurrogate(c)) {
            if (++i == value.size() || !QChar::isLowSurrogate(value.at(i).unicode())) return true;
        } else if (QChar::isLowSurrogate(c)) return true;
    }
    return false;
}

bool hasForbiddenIdCharacter(const QString &value)
{
    for (const auto c : value) {
        if (c.isSpace() || c.category() == QChar::Other_Control ||
            c.category() == QChar::Other_Format || c == QChar::ReplacementCharacter)
            return true;
    }
    return false;
}

bool validId(const QString &value, bool emptyAllowed = false)
{
    return (emptyAllowed || !value.isEmpty()) && value.toUtf8().size() <= maxIdBytes &&
        !hasInvalidUnicode(value) && !hasForbiddenIdCharacter(value);
}

bool validAsciiToken(const QString &value, qsizetype maximum)
{
    if (value.isEmpty() || value.size() > maximum) return false;
    for (const auto c : value)
        if (!(c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-')) || c.unicode() > 0x7f)
            return false;
    return true;
}

bool validProvider(EmoteProvider provider)
{
    switch (provider) {
    case EmoteProvider::Twitch:
    case EmoteProvider::FrankerFaceZ:
    case EmoteProvider::BetterTTV:
    case EmoteProvider::SevenTV: return true;
    }
    return false;
}

QString repairText(QString value, bool singleLine, bool &changed)
{
    if (hasInvalidUnicode(value)) return {};
    QString out;
    out.reserve(std::min(value.size(), maxText));
    for (qsizetype i = 0; i < value.size(); ++i) {
        const auto c = value.at(i);
        if (c == QLatin1Char('\r')) {
            if (i + 1 < value.size() && value.at(i + 1) == QLatin1Char('\n')) ++i;
            out += singleLine ? QLatin1Char(' ') : QLatin1Char('\n'); changed = true;
        } else if (c.category() == QChar::Other_Control || c == QChar(0x7f) ||
                   (c.unicode() >= 0x80 && c.unicode() <= 0x9f)) {
            if (c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += singleLine ? QLatin1Char(' ') : c;
            else out += QChar::ReplacementCharacter;
            changed = true;
        } else out += c;
    }
    if (out.size() <= maxText) return out;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, out);
    qsizetype last = 0;
    while (true) {
        const auto next = finder.toNextBoundary();
        if (next < 0 || next > maxText) break;
        last = next;
    }
    out.truncate(last); out.squeeze(); changed = true;
    return out;
}

bool repairUser(ChatUser &user, bool required, bool &changed)
{
    if (required && !validId(user.id)) return false;
    if (!required && !user.id.isEmpty() && !validId(user.id)) { user = {}; changed = true; return true; }
    if (!user.login.isEmpty()) {
        if (user.login.size() > 64 || !std::all_of(user.login.cbegin(), user.login.cend(), [](QChar c) {
                return c.unicode() <= 0x7f && (c.isLetterOrNumber() || c == QLatin1Char('_'));
            })) { user.login.clear(); changed = true; }
        else user.login = user.login.toLower();
    }
    bool textChanged = false;
    auto name = repairText(user.displayName, true, textChanged);
    if (hasInvalidUnicode(user.displayName)) name.clear();
    if (name.size() > maxDisplayName) { name.truncate(maxDisplayName); textChanged = true; }
    if (name != user.displayName) { user.displayName = std::move(name); changed = true; }
    if (user.displayName.isEmpty() && !user.login.isEmpty()) { user.displayName = user.login; changed = true; }
    if (user.color.isValid() && user.color.alpha() != 255) { user.color = {}; changed = true; }
    return true;
}

bool repairGiftActor(GiftActor &actor, bool &changed)
{
    if (actor.anonymous) {
        if (actor.user) { actor.user.reset(); changed = true; }
        return true;
    }
    return actor.user && repairUser(*actor.user, true, changed);
}

bool validUrlSyntax(const QUrl &url)
{
    const auto raw = url.toString(QUrl::FullyEncoded);
    if (raw.contains(QLatin1Char('\\'))) return false;
    for (qsizetype i = 0; i + 2 < raw.size(); ++i) {
        if (raw.at(i) != QLatin1Char('%')) continue;
        bool ok = false;
        const auto byte = raw.mid(i + 1, 2).toInt(&ok, 16);
        if (!ok || byte < 0x20 || byte == 0x7f) return false;
        i += 2;
    }
    return url.isValid() && raw.toUtf8().size() <= 2048 && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
        !url.host().isEmpty() && url.userInfo().isEmpty() && url.fragment().isEmpty() &&
        (url.port() == -1 || url.port() == 443);
}

bool providerHost(const QString &host, std::optional<EmoteProvider> provider)
{
    const auto h = host.toLower();
    if (!provider) return h == QStringLiteral("static-cdn.jtvnw.net") || h == QStringLiteral("cdn.7tv.app") ||
        h == QStringLiteral("cdn.betterttv.net") || h == QStringLiteral("cdn.frankerfacez.com");
    switch (*provider) {
    case EmoteProvider::Twitch: return h == QStringLiteral("static-cdn.jtvnw.net");
    case EmoteProvider::SevenTV: return h == QStringLiteral("cdn.7tv.app");
    case EmoteProvider::BetterTTV: return h == QStringLiteral("cdn.betterttv.net");
    case EmoteProvider::FrankerFaceZ: return h == QStringLiteral("cdn.frankerfacez.com");
    }
    return false;
}

bool repairMessage(ChatMessage &message, bool &changed)
{
    if (!validId(message.messageId)) return false;
    if (!repairUser(message.user, !message.user.id.isEmpty(), changed)) return false;
    bool textChanged = false;
    const auto text = repairText(message.text, false, textChanged);
    if (hasInvalidUnicode(message.text)) return false;
    if (text != message.text) { message.text = text; changed = true; }
    if (textChanged) {
        message.fragments = {{ChatFragment::Type::Text, message.text}};
        message.fragments.front().sourceRange = TextRange{0, message.text.size()};
        changed = true;
    }
    if (message.fragments.size() > 512) { message.fragments = {{ChatFragment::Type::Text, message.text}}; changed = true; }
    qsizetype position = 0;
    bool fragmentsValid = !message.fragments.empty();
    for (auto &fragment : message.fragments) {
        if (hasInvalidUnicode(fragment.text) || fragment.text.isEmpty() || position > message.text.size() ||
            fragment.text != message.text.mid(position, fragment.text.size())) { fragmentsValid = false; break; }
        fragment.sourceRange = TextRange{position, fragment.text.size()};
        position += fragment.text.size();
        if (fragment.type == ChatFragment::Type::Emote &&
            (!fragment.provider || !validProvider(*fragment.provider) || !validId(fragment.emoteId) ||
             (!isAllowedAssetUrl(fragment.imageUrl, fragment.provider) &&
              !isAllowedAssetUrl(fragment.fallbackUrl, fragment.provider)))) {
            fragment = ChatFragment{ChatFragment::Type::Text, fragment.text}; changed = true;
        }
        if (!fragment.imageUrl.isEmpty() && !isAllowedAssetUrl(fragment.imageUrl, fragment.provider)) { fragment.imageUrl = QUrl{}; changed = true; }
        if (!fragment.fallbackUrl.isEmpty() && !isAllowedAssetUrl(fragment.fallbackUrl, fragment.provider)) { fragment.fallbackUrl = QUrl{}; changed = true; }
        if (fragment.mention && !repairUser(*fragment.mention, true, changed)) { fragment.mention.reset(); changed = true; }
        if (fragment.cheermote && (!validAsciiToken(fragment.cheermote->prefix, 128) || fragment.cheermote->bits < 1 ||
                                   fragment.cheermote->tier < 1 || fragment.cheermote->tier > fragment.cheermote->bits)) {
            fragment.cheermote.reset(); changed = true;
        }
        if (fragment.twitch) {
            if (!fragment.provider || *fragment.provider != EmoteProvider::Twitch) { fragment.twitch.reset(); changed = true; }
            else {
                if (!fragment.twitch->setId.isEmpty() && !validId(fragment.twitch->setId)) { fragment.twitch->setId.clear(); changed = true; }
                if (!fragment.twitch->ownerId.isEmpty() && !validId(fragment.twitch->ownerId)) { fragment.twitch->ownerId.clear(); changed = true; }
            }
        }
    }
    if (!fragmentsValid || position != message.text.size()) {
        message.fragments = {{ChatFragment::Type::Text, message.text}};
        message.fragments.front().sourceRange = TextRange{0, message.text.size()}; changed = true;
    }
    if (message.badges.size() > 64) { message.badges.resize(64); changed = true; }
    std::erase_if(message.badges, [&](ChatBadge &badge) {
        const bool valid = badge.provider == BadgeProvider::Twitch && validAsciiToken(badge.type, 128) && validAsciiToken(badge.version, 128);
        if (!valid) changed = true;
        if (!badge.imageUrl.isEmpty() && (!validUrlSyntax(badge.imageUrl) ||
            badge.imageUrl.host().compare(QStringLiteral("static-cdn.jtvnw.net"), Qt::CaseInsensitive) != 0)) {
            badge.imageUrl = QUrl{}; changed = true;
        }
        bool ignored = false;
        const auto originalInfo = badge.info;
        badge.info = repairText(badge.info, true, ignored);
        changed |= ignored || badge.info != originalInfo;
        return !valid;
    });
    if (message.media.size() > 8) { message.media.resize(8); changed = true; }
    std::erase_if(message.media, [&](const ChatMedia &media) { const bool bad = !isAllowedAssetUrl(media.imageUrl); changed |= bad; return bad; });
    bool ignored = false;
    const auto originalSystemText = message.metadata.systemText;
    message.metadata.systemText = repairText(message.metadata.systemText, false, ignored);
    changed |= ignored || message.metadata.systemText != originalSystemText;
    if (!message.metadata.messageType.isEmpty() && !validAsciiToken(message.metadata.messageType, 64)) {
        message.metadata.messageType.clear(); changed = true;
    }
    if (!message.metadata.rewardId.isEmpty() && !validId(message.metadata.rewardId, true)) { message.metadata.rewardId.clear(); changed = true; }
    if (message.metadata.reply && (!validId(message.metadata.reply->parentMessageId) || !validId(message.metadata.reply->threadMessageId))) {
        message.metadata.reply.reset(); changed = true;
    }
    if (message.metadata.cheerBits && *message.metadata.cheerBits < 0) { message.metadata.cheerBits.reset(); changed = true; }
    return true;
}
} // namespace

bool isAllowedAssetUrl(const QUrl &url, std::optional<EmoteProvider> provider)
{
    return !url.isEmpty() && validUrlSyntax(url) && providerHost(url.host(), provider);
}

EventValidationResult validateEvent(PluginEvent &event)
{
    bool changed = false;
    switch (event.header.origin) {
    case EventOrigin::Production:
    case EventOrigin::LocalTransportTest:
    case EventOrigin::SyntheticTest: break;
    default: return {ValidationDisposition::Rejected, QStringLiteral("Origin")};
    }
    if (!validId(event.header.eventId) || !validId(event.header.channelId) ||
        !validId(event.header.originChannelId, true) || !validId(event.header.originMessageId, true) ||
        !event.header.timestamp.isValid() || !event.header.receivedAt.isValid())
        return {ValidationDisposition::Rejected, QStringLiteral("Identity")};
    bool valid = std::visit([&](auto &value) -> bool {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ChatMessage>) return repairMessage(value, changed);
        else if constexpr (std::is_same_v<T, MessageDeleted>) return validId(value.messageId) && repairUser(value.user, true, changed);
        else if constexpr (std::is_same_v<T, ChatCleared>) return !value.user || repairUser(*value.user, true, changed);
        else if constexpr (std::is_same_v<T, Follow>) return value.followedAt.isValid() && repairUser(value.user, true, changed);
        else if constexpr (std::is_same_v<T, Subscription>) return value.terms.durationMonths >= 1 && value.terms.durationMonths <= 12000 && repairMessage(value.notice, changed);
        else if constexpr (std::is_same_v<T, Resubscription>) {
            if (value.streakMonths && (*value.streakMonths < 0 || *value.streakMonths > value.cumulativeMonths)) { value.streakMonths.reset(); changed = true; }
            if (value.gifter && !repairGiftActor(*value.gifter, changed)) { value.gifter.reset(); changed = true; }
            return value.terms.durationMonths >= 1 && value.terms.durationMonths <= 12000 && value.cumulativeMonths >= 1 &&
                value.cumulativeMonths <= 12000 && repairMessage(value.notice, changed);
        }
        else if constexpr (std::is_same_v<T, GiftSubscription>) {
            if (value.cumulativeTotal && *value.cumulativeTotal < 0) { value.cumulativeTotal.reset(); changed = true; }
            return value.terms.durationMonths >= 1 && value.terms.durationMonths <= 12000 && validId(value.communityGiftId, true) &&
                repairGiftActor(value.gifter, changed) && repairUser(value.recipient, true, changed) && repairMessage(value.notice, changed);
        }
        else if constexpr (std::is_same_v<T, CommunityGiftSubscription>) {
            if (value.cumulativeTotal && *value.cumulativeTotal < 0) { value.cumulativeTotal.reset(); changed = true; }
            return value.count >= 1 && validId(value.communityGiftId) && repairGiftActor(value.gifter, changed) && repairMessage(value.notice, changed);
        }
        else if constexpr (std::is_same_v<T, Cheer>) {
            if (hasInvalidUnicode(value.text)) return false;
            bool text = false; value.text = repairText(value.text, false, text); changed |= text;
            return value.bits >= 1 && (!value.user || repairUser(*value.user, true, changed));
        }
        else if constexpr (std::is_same_v<T, Raid>) return value.viewers >= 0 && repairUser(value.from, true, changed) && repairUser(value.to, true, changed);
        return false;
    }, event.payload);
    return {valid ? (changed ? ValidationDisposition::Repaired : ValidationDisposition::Accepted) : ValidationDisposition::Rejected,
            valid ? QString{} : QStringLiteral("Event")};
}
