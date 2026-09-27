#pragma once

#include <QByteArray>
#include <optional>

class WidgetResourceRequest {
public:
    [[nodiscard]] static std::optional<QByteArray> resourceName(
        const QByteArray &request, const QByteArray &capability, quint16 port);
};
