#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

struct DecodedImage {
    std::vector<QImage> frames;
    std::vector<int> delaysMs;
};

using ImageAsset = std::shared_ptr<const DecodedImage>;
using DecodedGif = DecodedImage;

struct ChatUser {
    QString id;
    QString login;
    QString displayName;
    QColor color; // Invalid when unavailable; renderers choose their fallback.
};

enum class EmoteProvider { Twitch, FrankerFaceZ, BetterTTV, SevenTV };
enum class BadgeProvider { Twitch };

// Half-open UTF-16 code-unit range into ChatMessage::text (QString indexing).
struct TextRange {
    qsizetype offset = 0;
    qsizetype length = 0;
};

struct ChatBadge {
    BadgeProvider provider = BadgeProvider::Twitch;
    QString type;
    QString version;
    QString info;
    QUrl imageUrl; // Empty until resolved; type/version are the asset key.
};

struct TwitchEmoteMetadata {
    QString setId;
    QString ownerId;
    bool supportsStatic = false;
    bool supportsAnimated = false;
};

struct CheermoteMetadata {
    QString prefix;
    int bits = 0;
    int tier = 0;
};

struct ReplyMetadata {
    QString parentMessageId;
    QString threadMessageId;
};

struct ChatMetadata {
    QString messageType;
    QString systemText;
    std::optional<ReplyMetadata> reply;
    std::optional<int> cheerBits;
    QString rewardId;
};

struct ChatMedia {
    QUrl imageUrl;
    ImageAsset image;
};

struct ChatFragment {
    enum class Type { Text, Emote };
    Type type = Type::Text;
    QString text;
    QString emoteId;
    QUrl imageUrl;
    QUrl fallbackUrl;
    bool zeroWidth = false;
    ImageAsset image;
    std::optional<EmoteProvider> provider;
    std::optional<TextRange> sourceRange;
    std::optional<TwitchEmoteMetadata> twitch;
    std::optional<ChatUser> mention;
    std::optional<CheermoteMetadata> cheermote;

    ChatFragment() = default;
    ChatFragment(Type kind, QString value) : type(kind), text(std::move(value)) {}
};

// Normalized data only. Layout, movement and GPU state belong to the renderer.
struct ChatMessage {
    QString messageId;
    ChatUser user;
    QString text;
    std::vector<ChatFragment> fragments;
    std::vector<ChatBadge> badges;
    std::vector<ChatMedia> media;
    ChatMetadata metadata;

    ChatMessage() = default;
    ChatMessage(QString name, QString value, QColor color = QColor(0x91, 0xC8, 0xFF))
        : user{{}, {}, std::move(name), std::move(color)}, text(std::move(value)) {}
};
