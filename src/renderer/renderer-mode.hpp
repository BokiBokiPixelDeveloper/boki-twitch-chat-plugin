#pragma once

#include <QString>

enum class RendererMode { Native, WebWidget };

inline RendererMode rendererModeFromSetting(const QString &value)
{
    return value == QStringLiteral("web_widget") ? RendererMode::WebWidget : RendererMode::Native;
}

inline QString rendererModeSetting(RendererMode mode)
{
    return mode == RendererMode::WebWidget ? QStringLiteral("web_widget") : QStringLiteral("native");
}
