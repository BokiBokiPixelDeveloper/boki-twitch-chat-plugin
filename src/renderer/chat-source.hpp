#pragma once

#include "renderer/render-types.hpp"
#include "twitch/twitch-client.hpp"
#include "updater/update-checker.hpp"

#include <QMutex>
#include <QString>

#include <atomic>
#include <deque>
#include <memory>
#include <random>
#include <vector>

#include <obs.h>

class ChatSource {
public:
    ChatSource(obs_data_t *settings, obs_source_t *source);
    ~ChatSource();

    void update(obs_data_t *settings);
    void tick(float seconds);
    void render();
    uint32_t width() const { return canvasWidth_; }
    uint32_t height() const { return canvasHeight_; }

    obs_properties_t *properties();
    void connectTwitch();
    void addTestMessage();
    void addTestGif();
    void checkForUpdates();
    void installUpdate();

    QString status() const;

private:
    void enqueueMessage(ChatMessage message);
    void enqueueGif(DecodedGif gif);
    void persistTokens(const QString &accessToken, const QString &refreshToken);

    void consumePending();
    float chooseY(float messageHeight, float messageWidth, float speed);
    float collisionScore(float x, float y, float w, float h, float speed) const;

    static void destroyTexture(void *&texture);
    static void *createTexture(const QImage &image);
    static void updateTexture(void *texture, const QImage &image);

    obs_source_t *source_ = nullptr;
    std::unique_ptr<TwitchClient> twitch_;
    std::unique_ptr<UpdateChecker> updater_;

    mutable QMutex pendingMutex_;
    std::deque<PreparedMessage> pendingMessages_;
    std::deque<DecodedGif> pendingGifs_;

    std::vector<RenderMessage> messages_;
    std::vector<RenderGif> gifs_;
    std::vector<void *> deferredDestroy_;

    uint32_t canvasWidth_ = 1920;
    uint32_t canvasHeight_ = 1080;
    int laneCount_ = 6;
    int laneJitter_ = 28;
    QString fontFamily_;
    int fontWeight_ = 400;
    float outlineWidthPx_ = 2.5f;
    int minFontPx_ = 28;
    int maxFontPx_ = 52;
    float minSpeed_ = 340.0f;
    float maxSpeed_ = 1000.0f;
    int maxMessages_ = 80;
    int maxGifs_ = 6;
    int gifSize_ = 190;
    float gifSpeed_ = 170.0f;
    float gifLifetimeSeconds_ = 12.0f;

    std::atomic<int> activeMessageCount_{0};

    QString clientId_;
    QString channel_;
    QString accessToken_;
    QString refreshToken_;
    QString status_{QStringLiteral("Nicht verbunden")};

    mutable std::mt19937 rng_{std::random_device{}()};
};
