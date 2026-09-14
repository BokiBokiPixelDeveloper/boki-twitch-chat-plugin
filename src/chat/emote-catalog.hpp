#pragma once

#include "chat/chat-types.hpp"
#include <QHash>
#include <QJsonDocument>
#include <array>

enum class EmoteProvider { FrankerFaceZ, BetterTTV, SevenTV };

ChatMessage parseTwitchMessage(const QJsonObject &event);
QHash<QString, ChatFragment> parseEmoteCatalog(EmoteProvider provider, const QJsonDocument &document);

class EmoteCatalog {
public:
    void clear();
    void replace(EmoteProvider provider, bool channel, QHash<QString, ChatFragment> emotes);
    void apply(ChatMessage &message) const;

private:
    // Channel emotes override global emotes; within a scope: 7TV > BTTV > FFZ.
    std::array<QHash<QString, ChatFragment>, 6> catalogs_;
};
