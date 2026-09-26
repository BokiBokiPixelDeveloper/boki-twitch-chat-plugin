#pragma once

#include "chat/chat-types.hpp"
#include <QJsonObject>

// Twitch-only JSON boundary. The application model contains no JSON objects.
ChatUser parseTwitchUser(const QJsonObject &object, const QString &prefix);
ChatMessage parseTwitchMessage(const QJsonObject &event);
