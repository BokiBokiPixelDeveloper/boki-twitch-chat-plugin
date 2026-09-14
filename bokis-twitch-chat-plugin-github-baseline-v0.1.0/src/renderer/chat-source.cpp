#include "renderer/chat-source.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>

#include <graphics/graphics.h>
#include <obs-module.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr const char *S_CLIENT_ID = "client_id";
constexpr const char *S_CHANNEL = "channel";
constexpr const char *S_ACCESS_TOKEN = "access_token";
constexpr const char *S_REFRESH_TOKEN = "refresh_token";
constexpr const char *S_CANVAS_W = "canvas_width";
constexpr const char *S_CANVAS_H = "canvas_height";
constexpr const char *S_LANES = "lane_count";
constexpr const char *S_JITTER = "lane_jitter";
constexpr const char *S_FONT_FAMILY = "font_family";
constexpr const char *S_FONT_WEIGHT = "font_weight";
constexpr const char *S_OUTLINE = "outline_width";
constexpr const char *S_MIN_FONT = "min_font";
constexpr const char *S_MAX_FONT = "max_font";
constexpr const char *S_MIN_SPEED = "min_speed";
constexpr const char *S_MAX_SPEED = "max_speed";
constexpr const char *S_MAX_MSG = "max_messages";
constexpr const char *S_MAX_GIFS = "max_gifs";
constexpr const char *S_GIF_SIZE = "gif_size";
constexpr const char *S_GIF_SPEED = "gif_speed";
constexpr const char *S_GIF_LIFETIME = "gif_lifetime";

bool buttonConnect(obs_properties_t *, obs_property_t *, void *data)
{
    static_cast<ChatSource *>(data)->connectTwitch();
    return true;
}

bool buttonTestMessage(obs_properties_t *, obs_property_t *, void *data)
{
    static_cast<ChatSource *>(data)->addTestMessage();
    return false;
}

bool buttonTestGif(obs_properties_t *, obs_property_t *, void *data)
{
    static_cast<ChatSource *>(data)->addTestGif();
    return false;
}

QColor safeColor(const QColor &c)
{
    return c.isValid() ? c : QColor(0x91, 0xC8, 0xFF);
}
} // namespace

ChatSource::ChatSource(obs_data_t *settings, obs_source_t *source) : source_(source)
{
    twitch_ = std::make_unique<TwitchClient>(
        [this](PendingChatMessage msg) { enqueueMessage(std::move(msg)); },
        [this](DecodedGif gif) { enqueueGif(std::move(gif)); },
        [this](QString state) { status_ = std::move(state); },
        [this](QString access, QString refresh) { persistTokens(access, refresh); });

    update(settings);
    twitch_->startOrResume();
}

ChatSource::~ChatSource()
{
    twitch_->disconnect();
    obs_enter_graphics();
    for (auto &m : messages_)
        destroyTexture(m.texture);
    for (auto &g : gifs_)
        destroyTexture(g.texture);
    obs_leave_graphics();
}

void ChatSource::update(obs_data_t *settings)
{
    canvasWidth_ = static_cast<uint32_t>(obs_data_get_int(settings, S_CANVAS_W));
    canvasHeight_ = static_cast<uint32_t>(obs_data_get_int(settings, S_CANVAS_H));
    laneCount_ = static_cast<int>(obs_data_get_int(settings, S_LANES));
    laneJitter_ = static_cast<int>(obs_data_get_int(settings, S_JITTER));
    fontFamily_ = QString::fromUtf8(obs_data_get_string(settings, S_FONT_FAMILY));
    fontWeight_ = static_cast<int>(obs_data_get_int(settings, S_FONT_WEIGHT));
    outlineWidthPx_ = static_cast<float>(obs_data_get_double(settings, S_OUTLINE));
    minFontPx_ = static_cast<int>(obs_data_get_int(settings, S_MIN_FONT));
    maxFontPx_ = static_cast<int>(obs_data_get_int(settings, S_MAX_FONT));
    minSpeed_ = static_cast<float>(obs_data_get_double(settings, S_MIN_SPEED));
    maxSpeed_ = static_cast<float>(obs_data_get_double(settings, S_MAX_SPEED));
    maxMessages_ = static_cast<int>(obs_data_get_int(settings, S_MAX_MSG));
    maxGifs_ = static_cast<int>(obs_data_get_int(settings, S_MAX_GIFS));
    gifSize_ = static_cast<int>(obs_data_get_int(settings, S_GIF_SIZE));
    gifSpeed_ = static_cast<float>(obs_data_get_double(settings, S_GIF_SPEED));
    gifLifetimeSeconds_ = static_cast<float>(obs_data_get_double(settings, S_GIF_LIFETIME));

    clientId_ = QString::fromUtf8(obs_data_get_string(settings, S_CLIENT_ID));
    channel_ = QString::fromUtf8(obs_data_get_string(settings, S_CHANNEL));
    accessToken_ = QString::fromUtf8(obs_data_get_string(settings, S_ACCESS_TOKEN));
    refreshToken_ = QString::fromUtf8(obs_data_get_string(settings, S_REFRESH_TOKEN));

    canvasWidth_ = std::clamp<uint32_t>(canvasWidth_, 320, 7680);
    canvasHeight_ = std::clamp<uint32_t>(canvasHeight_, 240, 4320);
    laneCount_ = std::clamp(laneCount_, 1, 20);
    fontWeight_ = std::clamp(fontWeight_, 100, 900);
    outlineWidthPx_ = std::clamp(outlineWidthPx_, 0.0f, 12.0f);
    minFontPx_ = std::clamp(minFontPx_, 12, 180);
    maxFontPx_ = std::max(minFontPx_, std::clamp(maxFontPx_, 12, 220));
    minSpeed_ = std::clamp(minSpeed_, 20.0f, 4000.0f);
    maxSpeed_ = std::max(minSpeed_, std::clamp(maxSpeed_, 20.0f, 6000.0f));
    maxMessages_ = std::clamp(maxMessages_, 4, 300);
    maxGifs_ = std::clamp(maxGifs_, 0, 30);
    gifLifetimeSeconds_ = std::clamp(gifLifetimeSeconds_, 2.0f, 120.0f);

    twitch_->configure(clientId_, channel_, accessToken_, refreshToken_);
}

void ChatSource::enqueueMessage(PendingChatMessage message)
{
    // Rasterize at the FINAL font size before the OBS video thread sees the message.
    // alpha2 rendered everything at maxFontPx_ and scaled the texture down, which made
    // small text look soft/warped. Keeping QPainter off video_tick also avoids a render hitch.
    int pendingCount = 0;
    {
        QMutexLocker lock(&pendingMutex_);
        pendingCount = static_cast<int>(pendingMessages_.size());
    }

    const float pressure = std::clamp((static_cast<float>(activeMessageCount_.load()) + pendingCount) / 18.0f, 0.0f, 1.0f);
    const float lengthPressure = std::clamp((message.text.size() - 70.0f) / 180.0f, 0.0f, 1.0f);
    const float density = std::max(pressure, lengthPressure * 0.85f);
    message.fontPx = static_cast<int>(std::round(maxFontPx_ - density * (maxFontPx_ - minFontPx_)));
    message.speed = minSpeed_ + std::max(pressure, lengthPressure) * (maxSpeed_ - minSpeed_);
    message.rasterized = rasterizeMessage(message, message.fontPx);

    QMutexLocker lock(&pendingMutex_);
    pendingMessages_.push_back(std::move(message));
}

void ChatSource::enqueueGif(DecodedGif gif)
{
    QMutexLocker lock(&pendingMutex_);
    pendingGifs_.push_back(std::move(gif));
}

void ChatSource::persistTokens(const QString &accessToken, const QString &refreshToken)
{
    accessToken_ = accessToken;
    refreshToken_ = refreshToken;
    obs_data_t *settings = obs_source_get_settings(source_);
    obs_data_set_string(settings, S_ACCESS_TOKEN, accessToken.toUtf8().constData());
    obs_data_set_string(settings, S_REFRESH_TOKEN, refreshToken.toUtf8().constData());
    obs_source_update(source_, settings);
    obs_data_release(settings);
}

void ChatSource::connectTwitch()
{
    obs_data_t *settings = obs_source_get_settings(source_);
    update(settings);
    obs_data_release(settings);
    twitch_->beginDeviceFlow();
}

void ChatSource::addTestMessage()
{
    blog(LOG_INFO, "[bokis-twitch-chat-plugin] Native test message requested");
    enqueueMessage(PendingChatMessage{QStringLiteral("Boki"),
                                      QStringLiteral("Native OBS rendering – no CEF, 1000 px/s works 👀"),
                                      QColor(QStringLiteral("#b68cff"))});
}

void ChatSource::addTestGif()
{
    blog(LOG_INFO, "[bokis-twitch-chat-plugin] Native test GIF requested");
    DecodedGif gif;
    constexpr int frames = 18;
    constexpr int size = 180;
    for (int i = 0; i < frames; ++i) {
        QImage image(size, size, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int inset = 10 + static_cast<int>(8.0 * std::sin(i * 0.35));
        QColor bg;
        bg.setHsv((i * 20) % 360, 150, 245, 230);
        p.setBrush(bg);
        p.setPen(QPen(QColor(255, 255, 255, 220), 4));
        p.drawRoundedRect(QRect(inset, inset, size - inset * 2, size - inset * 2), 28, 28);
        QFont font(QStringLiteral("DejaVu Sans"));
        font.setBold(true);
        font.setPixelSize(30);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("GIF"));
        p.end();
        gif.frames.push_back(std::move(image));
        gif.delaysMs.push_back(50);
    }
    enqueueGif(std::move(gif));
}

QString ChatSource::status() const
{
    return status_;
}

QImage ChatSource::rasterizeMessage(const PendingChatMessage &msg, int fontPx) const
{
    QFont font;
    if (!fontFamily_.isEmpty())
        font.setFamily(fontFamily_);
    font.setPixelSize(fontPx);
    font.setWeight(static_cast<QFont::Weight>(fontWeight_));
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setHintingPreference(QFont::PreferFullHinting);
    QFont bold = font;
    bold.setWeight(static_cast<QFont::Weight>(std::max(fontWeight_, 600)));

    const QString prefix = msg.userName + QStringLiteral(": ");
    const QFontMetrics nameMetrics(bold);
    const QFontMetrics textMetrics(font);
    const int nameW = nameMetrics.horizontalAdvance(prefix);
    const int textW = textMetrics.horizontalAdvance(msg.text);
    const int pad = std::max(6, fontPx / 5);
    const int width = std::max(1, nameW + textW + pad * 2 + 4);
    const int height = std::max(nameMetrics.height(), textMetrics.height()) + pad * 2 + 4;

    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const qreal baseline = pad + nameMetrics.ascent() + 2;
    QPainterPath namePath;
    namePath.addText(pad, baseline, bold, prefix);
    QPainterPath textPath;
    textPath.addText(pad + nameW, baseline, font, msg.text);

    QPen outline(QColor(0, 0, 0, 220));
    outline.setWidthF(outlineWidthPx_);
    outline.setJoinStyle(Qt::RoundJoin);
    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(namePath);
    painter.drawPath(textPath);

    painter.setPen(Qt::NoPen);
    painter.setBrush(safeColor(msg.userColor));
    painter.drawPath(namePath);
    painter.setBrush(Qt::white);
    painter.drawPath(textPath);
    painter.end();
    return image;
}

float ChatSource::collisionScore(float x, float y, float w, float h, float speed) const
{
    float score = 0.0f;
    for (const auto &m : messages_) {
        const bool verticalOverlap = !(y + h + 8.0f < m.y || m.y + m.height + 8.0f < y);
        if (!verticalOverlap)
            continue;

        const float gap = x - (m.x + m.width);
        if (gap < 120.0f)
            score += 1000.0f + (120.0f - gap);

        if (speed > m.speed && gap > 0.0f) {
            const float rel = speed - m.speed;
            const float catchSeconds = gap / std::max(1.0f, rel);
            const float timeVisible = (x + w) / std::max(1.0f, speed);
            if (catchSeconds < timeVisible)
                score += 500.0f;
        }
    }
    return score;
}

float ChatSource::chooseY(float messageHeight, float messageWidth, float speed)
{
    const float topMargin = 45.0f;
    const float bottomMargin = 45.0f;
    const float usable = std::max(1.0f, static_cast<float>(canvasHeight_) - topMargin - bottomMargin);
    const float laneStep = usable / static_cast<float>(laneCount_);
    std::uniform_real_distribution<float> jitter(-static_cast<float>(laneJitter_), static_cast<float>(laneJitter_));

    struct Candidate { float y; float score; };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<size_t>(laneCount_ * 3));

    const float spawnX = static_cast<float>(canvasWidth_) + 25.0f;
    for (int lane = 0; lane < laneCount_; ++lane) {
        const float center = topMargin + laneStep * (lane + 0.5f);
        const float primary = std::clamp(center - messageHeight * 0.5f + jitter(rng_), 0.0f,
                                         static_cast<float>(canvasHeight_) - messageHeight);
        candidates.push_back({primary, collisionScore(spawnX, primary, messageWidth, messageHeight, speed)});

        for (float sign : {-1.0f, 1.0f}) {
            const float sub = std::clamp(primary + sign * laneStep * 0.38f + jitter(rng_) * 0.4f, 0.0f,
                                         static_cast<float>(canvasHeight_) - messageHeight);
            candidates.push_back({sub, collisionScore(spawnX, sub, messageWidth, messageHeight, speed) + 30.0f});
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.score < b.score; });
    const int pool = std::min<int>(3, static_cast<int>(candidates.size()));
    std::uniform_int_distribution<int> pick(0, std::max(0, pool - 1));
    return candidates[static_cast<size_t>(pick(rng_))].y;
}

void ChatSource::consumePending()
{
    std::deque<PendingChatMessage> newMessages;
    std::deque<DecodedGif> newGifs;
    {
        QMutexLocker lock(&pendingMutex_);
        newMessages.swap(pendingMessages_);
        newGifs.swap(pendingGifs_);
    }

    while (!newMessages.empty() && static_cast<int>(messages_.size()) < maxMessages_) {
        PendingChatMessage pending = std::move(newMessages.front());
        newMessages.pop_front();

        if (pending.rasterized.isNull()) {
            if (pending.fontPx <= 0)
                pending.fontPx = maxFontPx_;
            pending.rasterized = rasterizeMessage(pending, pending.fontPx);
        }

        RenderMessage msg;
        msg.width = static_cast<float>(pending.rasterized.width());
        msg.height = static_cast<float>(pending.rasterized.height());
        msg.image = std::move(pending.rasterized);
        msg.speed = pending.speed;
        msg.x = static_cast<float>(canvasWidth_) + 25.0f;
        msg.y = chooseY(msg.height, msg.width, msg.speed);
        messages_.push_back(std::move(msg));
        blog(LOG_INFO, "[bokis-twitch-chat-plugin] Spawned message; active=%zu", messages_.size());
    }

    if (!newMessages.empty()) {
        QMutexLocker lock(&pendingMutex_);
        while (!newMessages.empty()) {
            pendingMessages_.push_front(std::move(newMessages.back()));
            newMessages.pop_back();
        }
    }

    while (!newGifs.empty() && static_cast<int>(gifs_.size()) < maxGifs_) {
        DecodedGif decoded = std::move(newGifs.front());
        newGifs.pop_front();
        if (decoded.frames.empty())
            continue;

        std::uniform_real_distribution<float> rx(20.0f, std::max(20.0f, static_cast<float>(canvasWidth_ - gifSize_ - 20)));
        std::uniform_real_distribution<float> ry(20.0f, std::max(20.0f, static_cast<float>(canvasHeight_ - gifSize_ - 20)));
        std::uniform_int_distribution<int> sign(0, 1);

        RenderGif gif;
        gif.decoded = std::move(decoded);
        gif.x = rx(rng_);
        gif.y = ry(rng_);
        gif.width = static_cast<float>(gifSize_);
        gif.height = static_cast<float>(gifSize_);
        gif.vx = gifSpeed_ * (sign(rng_) ? 1.0f : -1.0f);
        gif.vy = gifSpeed_ * 0.72f * (sign(rng_) ? 1.0f : -1.0f);
        std::uniform_real_distribution<float> lifeVariation(0.88f, 1.12f);
        gif.lifetimeSeconds = gifLifetimeSeconds_ * lifeVariation(rng_);
        gifs_.push_back(std::move(gif));
        blog(LOG_INFO, "[bokis-twitch-chat-plugin] Spawned GIF; active=%zu", gifs_.size());
    }

    if (!newGifs.empty()) {
        QMutexLocker lock(&pendingMutex_);
        while (!newGifs.empty()) {
            pendingGifs_.push_front(std::move(newGifs.back()));
            newGifs.pop_back();
        }
    }
}

void ChatSource::tick(float seconds)
{
    consumePending();

    // Native OBS timing: no browser clock and no catch-up clamp. Speed remains true px/s.
    const float dt = std::clamp(seconds, 0.0f, 0.10f);

    for (auto &m : messages_)
        m.x -= m.speed * dt;

    for (auto &g : gifs_) {
        g.ageSeconds += dt;
        g.x += g.vx * dt;
        g.y += g.vy * dt;
        if (g.x <= 0.0f) { g.x = 0.0f; g.vx = std::abs(g.vx); }
        if (g.y <= 0.0f) { g.y = 0.0f; g.vy = std::abs(g.vy); }
        if (g.x + g.width >= canvasWidth_) { g.x = canvasWidth_ - g.width; g.vx = -std::abs(g.vx); }
        if (g.y + g.height >= canvasHeight_) { g.y = canvasHeight_ - g.height; g.vy = -std::abs(g.vy); }

        if (g.decoded.frames.size() > 1) {
            g.accumulatedMs += static_cast<int>(std::round(dt * 1000.0f));
            const int delay = g.decoded.delaysMs.empty() ? 100 : g.decoded.delaysMs[static_cast<size_t>(g.currentFrame)];
            while (g.accumulatedMs >= delay) {
                g.accumulatedMs -= delay;
                g.currentFrame = (g.currentFrame + 1) % static_cast<int>(g.decoded.frames.size());
                g.textureDirty = true;
            }
        }
    }

    auto msgIt = std::remove_if(messages_.begin(), messages_.end(), [this](const RenderMessage &m) {
        if (m.x + m.width >= -40.0f)
            return false;
        if (m.texture)
            deferredDestroy_.push_back(m.texture);
        return true;
    });
    messages_.erase(msgIt, messages_.end());
    activeMessageCount_.store(static_cast<int>(messages_.size()));

    auto gifIt = std::remove_if(gifs_.begin(), gifs_.end(), [this](const RenderGif &g) {
        if (g.ageSeconds < g.lifetimeSeconds)
            return false;
        if (g.texture)
            deferredDestroy_.push_back(g.texture);
        return true;
    });
    gifs_.erase(gifIt, gifs_.end());
}

void *ChatSource::createTexture(const QImage &source)
{
    if (source.isNull())
        return nullptr;
    QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    const uint8_t *data = image.constBits();
    return gs_texture_create(static_cast<uint32_t>(image.width()), static_cast<uint32_t>(image.height()),
                             GS_RGBA, 1, &data, GS_DYNAMIC);
}

void ChatSource::updateTexture(void *texture, const QImage &source)
{
    if (!texture || source.isNull())
        return;
    QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    gs_texture_set_image(static_cast<gs_texture_t *>(texture), image.constBits(), static_cast<uint32_t>(image.bytesPerLine()), false);
}

void ChatSource::destroyTexture(void *&texture)
{
    if (texture) {
        gs_texture_destroy(static_cast<gs_texture_t *>(texture));
        texture = nullptr;
    }
}

void ChatSource::render()
{
    for (void *texture : deferredDestroy_) {
        if (texture)
            gs_texture_destroy(static_cast<gs_texture_t *>(texture));
    }
    deferredDestroy_.clear();

    // OBS_SOURCE_CUSTOM_DRAW means libobs does not wrap us in the standard
    // texture effect. alpha1 called obs_source_draw() without an active effect,
    // which can result in a completely invisible source. Drive the standard
    // OBS effect explicitly and draw our textures as sprites.
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    if (!effect)
        return;
    gs_eparam_t *imageParam = gs_effect_get_param_by_name(effect, "image");
    if (!imageParam)
        return;

    const bool previousSrgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_blend_state_push();
    gs_reset_blend_state();

    auto drawTexture = [&](gs_texture_t *texture, float x, float y, float w, float h) {
        if (!texture)
            return;
        gs_matrix_push();
        gs_matrix_translate3f(x, y, 0.0f);
        gs_effect_set_texture_srgb(imageParam, texture);
        while (gs_effect_loop(effect, "Draw"))
            gs_draw_sprite(texture, 0, static_cast<uint32_t>(std::max(1.0f, w)),
                           static_cast<uint32_t>(std::max(1.0f, h)));
        gs_matrix_pop();
    };

    for (auto &m : messages_) {
        if (!m.texture) {
            m.texture = createTexture(m.image);
            blog(LOG_DEBUG, "[bokis-twitch-chat-plugin] message texture create: %s (%ux%u)",
                 m.texture ? "ok" : "FAILED",
                 static_cast<unsigned>(m.image.width()), static_cast<unsigned>(m.image.height()));
        }
        drawTexture(static_cast<gs_texture_t *>(m.texture), m.x, m.y, m.width, m.height);
    }

    for (auto &g : gifs_) {
        if (g.decoded.frames.empty())
            continue;
        const QImage &frame = g.decoded.frames[static_cast<size_t>(g.currentFrame)];
        if (!g.texture) {
            g.texture = createTexture(frame);
            g.textureDirty = false;
            blog(LOG_DEBUG, "[bokis-twitch-chat-plugin] gif texture create: %s (%ux%u)",
                 g.texture ? "ok" : "FAILED",
                 static_cast<unsigned>(frame.width()), static_cast<unsigned>(frame.height()));
        } else if (g.textureDirty) {
            updateTexture(g.texture, frame);
            g.textureDirty = false;
        }
        drawTexture(static_cast<gs_texture_t *>(g.texture), g.x, g.y, g.width, g.height);
    }

    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previousSrgb);
}

obs_properties_t *ChatSource::properties()
{
    obs_properties_t *props = obs_properties_create();

    obs_properties_t *twitch = obs_properties_create();
    obs_properties_add_text(twitch, S_CLIENT_ID, "Twitch Client ID", OBS_TEXT_DEFAULT);
    obs_properties_add_text(twitch, S_CHANNEL, "Kanal (Loginname)", OBS_TEXT_DEFAULT);
    const QByteArray statusUtf8 = QStringLiteral("Status: %1").arg(status_).toUtf8();
    obs_properties_add_text(twitch, "status_info", statusUtf8.constData(), OBS_TEXT_INFO);
    obs_properties_add_button2(twitch, "connect_twitch", "Mit Twitch verbinden (Device Flow)", buttonConnect, this);
    obs_properties_add_group(props, "twitch_group", "Twitch", OBS_GROUP_NORMAL, twitch);

    obs_properties_t *layout = obs_properties_create();
    obs_properties_add_int(layout, S_CANVAS_W, "Breite", 320, 7680, 1);
    obs_properties_add_int(layout, S_CANVAS_H, "Höhe", 240, 4320, 1);
    obs_properties_add_int(layout, S_LANES, "Primary Lanes", 1, 20, 1);
    obs_properties_add_int_slider(layout, S_JITTER, "Y-Jitter (px)", 0, 150, 1);

    obs_property_t *fontList = obs_properties_add_list(layout, S_FONT_FAMILY, "Schriftart",
                                                       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(fontList, "Systemstandard", "");
    const QStringList families = QFontDatabase::families();
    for (const QString &family : families) {
        const QByteArray utf8 = family.toUtf8();
        obs_property_list_add_string(fontList, utf8.constData(), utf8.constData());
    }

    obs_property_t *weightList = obs_properties_add_list(layout, S_FONT_WEIGHT, "Schriftgewicht",
                                                         OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(weightList, "Light (300)", 300);
    obs_property_list_add_int(weightList, "Regular (400)", 400);
    obs_property_list_add_int(weightList, "Medium (500)", 500);
    obs_property_list_add_int(weightList, "SemiBold (600)", 600);
    obs_property_list_add_int(weightList, "Bold (700)", 700);
    obs_property_list_add_int(weightList, "ExtraBold (800)", 800);
    obs_properties_add_float_slider(layout, S_OUTLINE, "Textumrandung (px)", 0.0, 10.0, 0.25);

    obs_properties_add_int_slider(layout, S_MIN_FONT, "Min. Schriftgröße", 12, 120, 1);
    obs_properties_add_int_slider(layout, S_MAX_FONT, "Max. Schriftgröße", 18, 180, 1);
    obs_properties_add_float_slider(layout, S_MIN_SPEED, "Min. Geschwindigkeit (px/s)", 20.0, 2000.0, 10.0);
    obs_properties_add_float_slider(layout, S_MAX_SPEED, "Max. Geschwindigkeit (px/s)", 100.0, 6000.0, 25.0);
    obs_properties_add_int(layout, S_MAX_MSG, "Max. aktive Nachrichten", 4, 300, 1);
    obs_properties_add_button2(layout, "test_message", "Native Testnachricht", buttonTestMessage, this);
    obs_properties_add_group(props, "layout_group", "Nachrichten", OBS_GROUP_NORMAL, layout);

    obs_properties_t *gifs = obs_properties_create();
    obs_properties_add_int(gifs, S_MAX_GIFS, "Max. GIFs gleichzeitig", 0, 30, 1);
    obs_properties_add_int_slider(gifs, S_GIF_SIZE, "GIF-Größe", 64, 600, 1);
    obs_properties_add_float_slider(gifs, S_GIF_SPEED, "GIF-Bounce-Speed (px/s)", 20.0, 1200.0, 10.0);
    obs_properties_add_float_slider(gifs, S_GIF_LIFETIME, "GIF-Lebensdauer (Sekunden)", 2.0, 60.0, 0.5);
    obs_properties_add_button2(gifs, "test_gif", "Native GIF-Animation testen", buttonTestGif, this);
    obs_properties_add_group(props, "gif_group", "GIFs", OBS_GROUP_NORMAL, gifs);

    return props;
}
