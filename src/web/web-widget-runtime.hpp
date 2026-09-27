#pragma once

#include "core/plugin-runtime.hpp"
#include <QString>
#include <atomic>
#include <memory>

struct obs_source;
using obs_source_t = struct obs_source;

// OBS-facing methods are serialized by ChatSource. Qt bridge state owns a private
// event loop and never calls back into the parent or browser source.
class WebWidgetRuntime final {
public:
    WebWidgetRuntime(obs_source_t *parent, std::shared_ptr<PluginRuntime> runtime,
                     std::shared_ptr<std::atomic_bool> backendAccepted, uint32_t width, uint32_t height);
    ~WebWidgetRuntime();
    void shutdown();

    bool available() const;
    QString status() const;
    void resize(uint32_t width, uint32_t height);
    void render();
    obs_source_t *child() const;

private:
    class State;
    class Worker;
    std::unique_ptr<Worker> worker_;
    obs_source_t *parent_ = nullptr;
    obs_source_t *browser_ = nullptr;
    bool attached_ = false;
    bool stopped_ = false;
    QString status_ = QStringLiteral("Web Widget disabled");
};
