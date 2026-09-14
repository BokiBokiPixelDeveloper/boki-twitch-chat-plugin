#pragma once

#include "renderer/message-layout.hpp"

struct PreparedMessage {
    MessageLayout layout;
    float speed = 500.0f;
};

struct RenderEmote {
    InlineEmote layout;
    void *texture = nullptr; // gs_texture_t*, owned by the OBS graphics path
    int currentFrame = 0;
    double accumulatedMs = 0;
    bool textureDirty = true;
};

struct RenderMessage {
    QImage image;
    std::vector<RenderEmote> emotes;
    void *texture = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    float speed = 500.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct RenderGif {
    DecodedGif decoded;
    void *texture = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    float vx = 150.0f;
    float vy = 110.0f;
    float width = 180.0f;
    float height = 180.0f;
    int currentFrame = 0;
    double accumulatedMs = 0;
    bool textureDirty = true;
    float ageSeconds = 0.0f;
    float lifetimeSeconds = 12.0f;
};
