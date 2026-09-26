#pragma once

#include "core/event-types.hpp"

enum class ValidationDisposition { Accepted, Repaired, Rejected };

// Bounded, renderer-independent policy for every externally sourced event.
struct EventValidationResult {
    ValidationDisposition disposition = ValidationDisposition::Accepted;
    QString category;
};

// Repairs optional fields in place. A rejected result must never be published.
[[nodiscard]] EventValidationResult validateEvent(PluginEvent &event);
[[nodiscard]] bool isAllowedAssetUrl(const QUrl &url, std::optional<EmoteProvider> provider = {});
