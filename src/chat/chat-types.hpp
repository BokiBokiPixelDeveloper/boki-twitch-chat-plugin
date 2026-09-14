#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <memory>
#include <vector>

struct DecodedImage {
    std::vector<QImage> frames;
    std::vector<int> delaysMs;
};

using ImageAsset = std::shared_ptr<const DecodedImage>;
using DecodedGif = DecodedImage;

struct ChatFragment {
    enum class Type { Text, Emote };
    Type type = Type::Text;
    QString text;
    QString emoteId;
    QUrl imageUrl;
    QUrl fallbackUrl;
    bool zeroWidth = false;
    ImageAsset image;

    ChatFragment() = default;
    ChatFragment(Type kind, QString value) : type(kind), text(std::move(value)) {}
};

// Normalized data only. Layout, movement and GPU state belong to the renderer.
struct ChatMessage {
    QString userName;
    QString text;
    QColor userColor{0x91, 0xC8, 0xFF};
    std::vector<ChatFragment> fragments;

    ChatMessage() = default;
    ChatMessage(QString name, QString value, QColor color = QColor(0x91, 0xC8, 0xFF))
        : userName(std::move(name)), text(std::move(value)), userColor(std::move(color)) {}
};
