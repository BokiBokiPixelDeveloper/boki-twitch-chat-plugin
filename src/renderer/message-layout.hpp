#pragma once

#include "chat/chat-types.hpp"
#include <QRectF>

struct InlineEmote {
    ImageAsset image;
    QRectF rect;
    int overlayBase = -1;
};

struct MessageLayout {
    QImage text;
    std::vector<InlineEmote> emotes;
};

MessageLayout layoutMessage(const ChatMessage &message, const QString &fontFamily,
                            int fontWeight, int fontPx, float outlineWidth);
