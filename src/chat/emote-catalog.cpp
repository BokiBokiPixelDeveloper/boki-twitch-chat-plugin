#include "chat/emote-catalog.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <limits>

namespace {
QUrl httpsUrl(QString value)
{
    if (value.startsWith(QStringLiteral("//")))
        value.prepend(QStringLiteral("https:"));
    const QUrl url(value);
    return url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty() ? url : QUrl{};
}

QString largestUrl(const QJsonObject &urls)
{
    for (const auto *scale : {"4", "3", "2", "1"}) {
        const auto value = urls.value(QLatin1String(scale)).toString();
        if (!value.isEmpty())
            return value;
    }
    return {};
}
} // namespace

ChatMessage parseTwitchMessage(const QJsonObject &event)
{
    ChatMessage out;
    out.userName = event.value(QStringLiteral("chatter_user_name")).toString();
    const QColor color(event.value(QStringLiteral("color")).toString());
    if (color.isValid())
        out.userColor = color;
    const auto message = event.value(QStringLiteral("message")).toObject();
    out.text = message.value(QStringLiteral("text")).toString();
    QString fragmentText;
    for (const auto &value : message.value(QStringLiteral("fragments")).toArray()) {
        const auto part = value.toObject();
        ChatFragment fragment;
        fragment.text = part.value(QStringLiteral("text")).toString();
        if (part.value(QStringLiteral("type")).toString() == QStringLiteral("emote")) {
            const auto emote = part.value(QStringLiteral("emote")).toObject();
            fragment.emoteId = emote.value(QStringLiteral("id")).toString();
            static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9_-]+$"));
            if (validId.match(fragment.emoteId).hasMatch()) {
                fragment.type = ChatFragment::Type::Emote;
                const auto formats = emote.value(QStringLiteral("format")).toArray();
                const QString format = formats.contains(QStringLiteral("animated")) ? QStringLiteral("animated")
                    : formats.isEmpty() ? QStringLiteral("default") : QStringLiteral("static");
                const QString base = QStringLiteral("https://static-cdn.jtvnw.net/emoticons/v2/%1/").arg(fragment.emoteId);
                fragment.imageUrl = QUrl(base + format + QStringLiteral("/dark/3.0"));
                fragment.fallbackUrl = QUrl(base + QStringLiteral("static/dark/3.0"));
            }
        }
        fragmentText += fragment.text;
        out.fragments.push_back(std::move(fragment));
    }
    // A malformed/partial fragments array must never silently discard message text.
    if (fragmentText != out.text || out.fragments.empty())
        out.fragments = {{ChatFragment::Type::Text, out.text}};
    return out;
}

QHash<QString, ChatFragment> parseEmoteCatalog(EmoteProvider provider, const QJsonDocument &document)
{
    QHash<QString, ChatFragment> out;
    auto add = [&](ChatFragment fragment) {
        if (!fragment.text.isEmpty() && !fragment.imageUrl.isEmpty() && out.size() < 10000) {
            fragment.type = ChatFragment::Type::Emote;
            out.insert(fragment.text, std::move(fragment));
        }
    };
    const auto root = document.object();
    if (provider == EmoteProvider::BetterTTV) {
        QJsonArray emotes = document.isArray() ? document.array() : root.value(QStringLiteral("channelEmotes")).toArray();
        for (const auto &value : root.value(QStringLiteral("sharedEmotes")).toArray())
            emotes.append(value);
        for (const auto &value : emotes) {
            const auto item = value.toObject();
            ChatFragment fragment;
            fragment.text = item.value(QStringLiteral("code")).toString();
            fragment.emoteId = item.value(QStringLiteral("id")).toString();
            fragment.imageUrl = httpsUrl(QStringLiteral("https://cdn.betterttv.net/emote/%1/3x").arg(
                QString::fromLatin1(QUrl::toPercentEncoding(fragment.emoteId))));
            // Legacy BTTV overlay emotes are not marked in the cached API.
            fragment.zeroWidth = item.value(QStringLiteral("modifier")).toBool() || QStringList{QStringLiteral("SoSnowy"), QStringLiteral("IceCold"),
                QStringLiteral("SantaHat"), QStringLiteral("TopHat"), QStringLiteral("ReinDeer"),
                QStringLiteral("CandyCane"), QStringLiteral("cvMask"), QStringLiteral("cvHazmat")}.contains(fragment.text);
            add(std::move(fragment));
        }
    } else if (provider == EmoteProvider::FrankerFaceZ) {
        const auto sets = root.value(QStringLiteral("sets")).toObject();
        const auto defaults = root.value(QStringLiteral("default_sets")).toArray();
        const int roomSet = root.value(QStringLiteral("room")).toObject().value(QStringLiteral("set")).toInt(-1);
        for (auto it = sets.begin(); it != sets.end(); ++it) {
            if ((!defaults.isEmpty() && !defaults.contains(it.key().toInt())) ||
                (roomSet >= 0 && it.key().toInt() != roomSet))
                continue;
            for (const auto &value : it.value().toObject().value(QStringLiteral("emoticons")).toArray()) {
                const auto item = value.toObject();
                ChatFragment fragment;
                fragment.text = item.value(QStringLiteral("name")).toString();
                fragment.emoteId = QString::number(item.value(QStringLiteral("id")).toInteger());
                fragment.fallbackUrl = httpsUrl(largestUrl(item.value(QStringLiteral("urls")).toObject()));
                fragment.imageUrl = httpsUrl(largestUrl(item.value(QStringLiteral("animated")).toObject()));
                if (fragment.imageUrl.isEmpty())
                    fragment.imageUrl = fragment.fallbackUrl;
                add(std::move(fragment));
            }
        }
    } else {
        const auto set = root.contains(QStringLiteral("emote_set")) ? root.value(QStringLiteral("emote_set")).toObject() : root;
        for (const auto &value : set.value(QStringLiteral("emotes")).toArray()) {
            const auto item = value.toObject();
            const auto data = item.value(QStringLiteral("data")).toObject();
            const auto host = data.value(QStringLiteral("host")).toObject();
            ChatFragment fragment;
            fragment.text = item.value(QStringLiteral("name")).toString(); // includes channel aliases
            fragment.emoteId = item.value(QStringLiteral("id")).toString();
            fragment.zeroWidth = (data.value(QStringLiteral("flags")).toInt() & (1 << 8)) != 0;
            int bestScore = std::numeric_limits<int>::min();
            for (const auto &fileValue : host.value(QStringLiteral("files")).toArray()) {
                const auto file = fileValue.toObject();
                const QString format = file.value(QStringLiteral("format")).toString();
                if (format != QStringLiteral("WEBP") && format != QStringLiteral("GIF") && format != QStringLiteral("PNG"))
                    continue;
                const int height = file.value(QStringLiteral("height")).toInt();
                const int score = (format == QStringLiteral("WEBP") ? 10000 : 0) - std::abs(height - 112);
                if (score <= bestScore)
                    continue;
                bestScore = score;
                const QString base = host.value(QStringLiteral("url")).toString() + QLatin1Char('/');
                fragment.imageUrl = httpsUrl(base + file.value(QStringLiteral("name")).toString());
                const QString staticName = file.value(QStringLiteral("static_name")).toString();
                fragment.fallbackUrl = staticName.isEmpty() ? QUrl{} : httpsUrl(base + staticName);
            }
            add(std::move(fragment));
        }
    }
    return out;
}

void EmoteCatalog::clear()
{
    for (auto &catalog : catalogs_)
        catalog.clear();
}

void EmoteCatalog::replace(EmoteProvider provider, bool channel, QHash<QString, ChatFragment> emotes)
{
    catalogs_[static_cast<size_t>(provider) + (channel ? 3 : 0)] = std::move(emotes);
}

void EmoteCatalog::apply(ChatMessage &message) const
{
    if (message.fragments.empty())
        message.fragments = {{ChatFragment::Type::Text, message.text}};
    std::vector<ChatFragment> fragments;
    static const QRegularExpression tokens(QStringLiteral("\\s+|\\S+"), QRegularExpression::UseUnicodePropertiesOption);
    for (const auto &fragment : message.fragments) {
        if (fragment.type != ChatFragment::Type::Text) {
            fragments.push_back(fragment);
            continue;
        }
        auto matches = tokens.globalMatch(fragment.text);
        while (matches.hasNext()) {
            const QString token = matches.next().captured();
            const ChatFragment *emote = nullptr;
            for (auto it = catalogs_.rbegin(); it != catalogs_.rend(); ++it) {
                const auto found = it->constFind(token);
                if (found != it->cend()) {
                    emote = &found.value();
                    break;
                }
            }
            if (emote)
                fragments.push_back(*emote);
            else if (!fragments.empty() && fragments.back().type == ChatFragment::Type::Text)
                fragments.back().text += token;
            else
                fragments.push_back({ChatFragment::Type::Text, token});
        }
    }
    message.fragments = std::move(fragments);
}
