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

QHash<QString, ChatFragment> parseEmoteCatalog(EmoteProvider provider, const QJsonDocument &document)
{
    QHash<QString, ChatFragment> out;
    auto add = [&](ChatFragment fragment) {
        if (!fragment.text.isEmpty() && !fragment.imageUrl.isEmpty() && out.size() < 10000) {
            fragment.type = ChatFragment::Type::Emote;
            fragment.provider = provider;
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
    } else if (provider == EmoteProvider::SevenTV) {
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
    size_t bucket = 0;
    switch (provider) {
    case EmoteProvider::FrankerFaceZ: bucket = 0; break;
    case EmoteProvider::BetterTTV: bucket = 1; break;
    case EmoteProvider::SevenTV: bucket = 2; break;
    default: return; // Native Twitch fragments never occupy third-party buckets.
    }
    for (auto &fragment : emotes) {
        fragment.provider = provider;
        fragment.sourceRange.reset(); // Catalog entries are not occurrences.
    }
    catalogs_[bucket + (channel ? 3 : 0)] = std::move(emotes);
}

void EmoteCatalog::apply(ChatMessage &message) const
{
    if (message.fragments.empty())
        message.fragments = {{ChatFragment::Type::Text, message.text}};
    std::vector<ChatFragment> fragments;
    static const QRegularExpression tokens(QStringLiteral("\\s+|\\S+"), QRegularExpression::UseUnicodePropertiesOption);
    qsizetype offset = 0;
    for (const auto &fragment : message.fragments) {
        const qsizetype fragmentOffset = offset;
        offset += fragment.text.size();
        if (fragment.type != ChatFragment::Type::Text || fragment.mention || fragment.cheermote) {
            fragments.push_back(fragment);
            fragments.back().sourceRange = TextRange{fragmentOffset, fragment.text.size()};
            continue;
        }
        auto matches = tokens.globalMatch(fragment.text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const QString token = match.captured();
            const TextRange range{fragmentOffset + match.capturedStart(), token.size()};
            const ChatFragment *emote = nullptr;
            for (auto it = catalogs_.rbegin(); it != catalogs_.rend(); ++it) {
                const auto found = it->constFind(token);
                if (found != it->cend()) {
                    emote = &found.value();
                    break;
                }
            }
            if (emote) {
                fragments.push_back(*emote);
                fragments.back().sourceRange = range;
            } else if (!fragments.empty() && fragments.back().type == ChatFragment::Type::Text &&
                       !fragments.back().mention && !fragments.back().cheermote) {
                fragments.back().text += token;
                fragments.back().sourceRange->length += token.size();
            } else {
                fragments.push_back({ChatFragment::Type::Text, token});
                fragments.back().sourceRange = range;
            }
        }
    }
    message.fragments = std::move(fragments);
}
