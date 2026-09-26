#pragma once

#include <QUrl>

enum class EventSubConnectionMode { Production, LocalTest, Invalid };

struct EventSubConnectionSettings {
    EventSubConnectionMode mode = EventSubConnectionMode::Production;
    QUrl websocketUrl{QStringLiteral("wss://eventsub.wss.twitch.tv/ws")};
    QUrl subscriptionUrl{QStringLiteral("https://api.twitch.tv/helix/eventsub/subscriptions")};
    QString channelId;
    QString error;
};

[[nodiscard]] EventSubConnectionSettings readEventSubConnectionSettings();
[[nodiscard]] bool isAllowedEventSubReconnectUrl(const QUrl &url, const EventSubConnectionSettings &settings);
