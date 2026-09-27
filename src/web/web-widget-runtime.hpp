#pragma once

#include "core/plugin-runtime.hpp"
#include <QObject>
#include <atomic>
#include <memory>

struct obs_source;
using obs_source_t = struct obs_source;

class WebWidgetRuntime final : public QObject {
public:
    WebWidgetRuntime(obs_source_t *parent, std::shared_ptr<PluginRuntime> runtime,
                     std::shared_ptr<std::atomic_bool> backendAccepted, uint32_t width, uint32_t height);
    ~WebWidgetRuntime() override;

    bool available() const;
    QString status() const;
    void resize(uint32_t width, uint32_t height);
    void render();
    obs_source_t *child() const;

private:
    class State;
    std::unique_ptr<State> state_;
};
