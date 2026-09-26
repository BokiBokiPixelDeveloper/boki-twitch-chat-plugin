#pragma once

#include "chat/emote-catalog.hpp"
#include "chat/image-cache.hpp"
#include <QDeadlineTimer>
#include <QTimer>

class EmoteService : public QObject {
public:
    using MessageCallback = std::function<void(ChatMessage)>;
    explicit EmoteService(MessageCallback callback, QNetworkAccessManager *transport = nullptr);
    ~EmoteService() override;
    void setChannel(const QString &twitchId);
    void clear();
    void resolve(ChatMessage message);
    void resolve(ChatMessage message, MessageCallback completion);
    void loadImage(const QUrl &url, ImageCache::Callback callback);

private:
    struct Pending {
        ChatMessage message;
        MessageCallback completion;
        QDeadlineTimer deadline{2500};
        int remaining = 0;
        bool started = false;
        bool assembling = false;
        bool delivered = false;
    };
    void refresh();
    void fetchCatalog(EmoteProvider provider, bool channel, const QUrl &url, quint64 generation);
    void start(const std::shared_ptr<Pending> &pending);
    void flush();

    MessageCallback callback_;
    EmoteCatalog catalog_;
    ImageCache images_;
    QNetworkAccessManager network_;
    QNetworkAccessManager *transport_;
    QTimer refreshTimer_;
    QTimer flushTimer_;
    QString channelId_;
    quint64 generation_ = 0;
    int loading_ = 0;
    bool initialLoad_ = false;
    std::deque<std::shared_ptr<Pending>> pending_;
    QSet<QNetworkReply *> replies_;
};
