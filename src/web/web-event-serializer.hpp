#pragma once

#include "core/event-types.hpp"
#include <QJsonObject>

class WebEventSerializer {
public:
    [[nodiscard]] static QJsonObject serialize(const PluginEvent &event);
};
