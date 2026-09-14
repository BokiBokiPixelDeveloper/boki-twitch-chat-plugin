#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QVector>

#include <cstdint>
#include <vector>

struct PendingChatMessage {
    QString userName;
    QString text;
    QColor userColor{0x91, 0xC8, 0xFF};
    QImage rasterized; // prepared at the final font size on the Qt/network thread
    int fontPx = 0;
    float speed = 500.0f;
};

struct DecodedGif {
    std::vector<QImage> frames;
    std::vector<int> delaysMs;
};

struct RenderMessage {
    QImage image;
    void *texture = nullptr; // gs_texture_t*, kept opaque in this header
    float x = 0.0f;
    float y = 0.0f;
    float speed = 500.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct RenderGif {
    DecodedGif decoded;
    void *texture = nullptr; // gs_texture_t*
    float x = 0.0f;
    float y = 0.0f;
    float vx = 150.0f;
    float vy = 110.0f;
    float width = 180.0f;
    float height = 180.0f;
    int currentFrame = 0;
    int accumulatedMs = 0;
    bool textureDirty = true;
    float ageSeconds = 0.0f;
    float lifetimeSeconds = 12.0f;
};
