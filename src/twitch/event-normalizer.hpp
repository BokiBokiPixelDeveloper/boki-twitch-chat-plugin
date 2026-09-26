#pragma once

#include "core/event-types.hpp"
#include <QByteArray>

enum class NormalizationStatus { Event, Ignored, Invalid };
struct NormalizationResult {
    NormalizationStatus status = NormalizationStatus::Invalid;
    std::optional<PluginEvent> event;
    QString error; // Fixed diagnostic text; never the input JSON or credentials.
};

// Pure Twitch boundary: no network, OBS, timers, deduplication, or asset downloads.
// Controls/unknown types and versions are ignored. Invalid known events fail.
// The caller supplies ingress time/order; timestamps do not define delivery order.
NormalizationResult normalizeTwitchEvent(const QByteArray &envelope, QDateTime receivedAt,
                                       std::uint64_t sequence = 0, std::uint64_t generation = 0);
