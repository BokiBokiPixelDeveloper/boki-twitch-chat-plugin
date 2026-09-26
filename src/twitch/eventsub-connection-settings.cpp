#include "twitch/eventsub-connection-settings.hpp"

namespace {
bool validChannelId(const QString &value)
{
    if (value.isEmpty() || value.toUtf8().size() > 256) return false;
    for (const auto c : value)
        if (c.isSpace() || c.category() == QChar::Other_Control || c.category() == QChar::Other_Format)
            return false;
    return true;
}

bool loopbackHost(const QString &host)
{
    return host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1");
}

bool validLocalUrl(const QUrl &url)
{
    return url.isValid() && url.scheme() == QStringLiteral("ws") && loopbackHost(url.host()) &&
        url.port() >= 1 && url.port() <= 65535 && url.path() == QStringLiteral("/ws") &&
        url.userInfo().isEmpty() && url.query().isEmpty() && url.fragment().isEmpty();
}
} // namespace

EventSubConnectionSettings readEventSubConnectionSettings()
{
    EventSubConnectionSettings settings;
    const auto endpoint = qEnvironmentVariable("BOKIS_EVENTSUB_TEST_URL");
    const auto channel = qEnvironmentVariable("BOKIS_EVENTSUB_TEST_CHANNEL_ID");
    if (endpoint.isEmpty() && channel.isEmpty()) return settings;
    settings.mode = EventSubConnectionMode::Invalid;
    if (endpoint.isEmpty() || channel.isEmpty()) {
        settings.error = QStringLiteral("[EventSub] Local test mode requires BOKIS_EVENTSUB_TEST_URL and BOKIS_EVENTSUB_TEST_CHANNEL_ID");
        return settings;
    }
    const QUrl url(endpoint, QUrl::StrictMode);
    if (endpoint != endpoint.trimmed()) {
        settings.error = QStringLiteral("[EventSub] Local test endpoint must not contain surrounding whitespace");
        return settings;
    }
    if (!validLocalUrl(url) || !validChannelId(channel)) {
        settings.error = QStringLiteral("[EventSub] Local test endpoint must be ws://127.0.0.1:<port>/ws or ws://[::1]:<port>/ws with a valid test channel ID");
        return settings;
    }
    settings.mode = EventSubConnectionMode::LocalTest;
    settings.websocketUrl = url;
    settings.subscriptionUrl = url;
    settings.subscriptionUrl.setScheme(QStringLiteral("http"));
    settings.subscriptionUrl.setPath(QStringLiteral("/eventsub/subscriptions"));
    settings.channelId = channel;
    return settings;
}

bool isAllowedEventSubReconnectUrl(const QUrl &url, const EventSubConnectionSettings &settings)
{
    if (settings.mode == EventSubConnectionMode::Production)
        return url.isValid() && url.scheme() == QStringLiteral("wss") &&
            url.host().compare(QStringLiteral("eventsub.wss.twitch.tv"), Qt::CaseInsensitive) == 0 &&
            (url.port() == -1 || url.port() == 443) && url.userInfo().isEmpty() && url.fragment().isEmpty();
    if (settings.mode != EventSubConnectionMode::LocalTest) return false;
    return url.isValid() && url.scheme() == QStringLiteral("ws") && url.host() == settings.websocketUrl.host() &&
        url.port() == settings.websocketUrl.port() && url.path() == QStringLiteral("/ws") &&
        url.userInfo().isEmpty() && url.fragment().isEmpty() && url.toString().toUtf8().size() <= 2048;
}
