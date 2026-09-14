#include "renderer/chat-source.hpp"

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("OpenAI / Boki")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Native Twitch chat renderer (no CEF/browser source)";
}

static const char *sourceName(void *)
{
    return "Bokis Twitch Chat Plugin";
}

static void *sourceCreate(obs_data_t *settings, obs_source_t *source)
{
    return new ChatSource(settings, source);
}

static void sourceDestroy(void *data)
{
    delete static_cast<ChatSource *>(data);
}

static void sourceUpdate(void *data, obs_data_t *settings)
{
    static_cast<ChatSource *>(data)->update(settings);
}

static uint32_t sourceWidth(void *data)
{
    return static_cast<ChatSource *>(data)->width();
}

static uint32_t sourceHeight(void *data)
{
    return static_cast<ChatSource *>(data)->height();
}

static void sourceTick(void *data, float seconds)
{
    static_cast<ChatSource *>(data)->tick(seconds);
}

static void sourceRender(void *data, gs_effect_t *)
{
    static_cast<ChatSource *>(data)->render();
}

static obs_properties_t *sourceProperties(void *data)
{
    return static_cast<ChatSource *>(data)->properties();
}

static void sourceDefaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "canvas_width", 1920);
    obs_data_set_default_int(settings, "canvas_height", 1080);
    obs_data_set_default_int(settings, "lane_count", 6);
    obs_data_set_default_int(settings, "lane_jitter", 28);
    obs_data_set_default_string(settings, "font_family", "");
    obs_data_set_default_int(settings, "font_weight", 500);
    obs_data_set_default_double(settings, "outline_width", 2.5);
    obs_data_set_default_int(settings, "min_font", 28);
    obs_data_set_default_int(settings, "max_font", 52);
    obs_data_set_default_double(settings, "min_speed", 340.0);
    obs_data_set_default_double(settings, "max_speed", 1000.0);
    obs_data_set_default_int(settings, "max_messages", 80);
    obs_data_set_default_int(settings, "max_gifs", 6);
    obs_data_set_default_int(settings, "gif_size", 190);
    obs_data_set_default_double(settings, "gif_speed", 170.0);
    obs_data_set_default_double(settings, "gif_lifetime", 12.0);
    obs_data_set_default_bool(settings, "auto_update_check", true);
}

static obs_source_info sourceInfo = {};

bool obs_module_load(void)
{
    sourceInfo.id = "bokis_twitch_chat_plugin";
    sourceInfo.type = OBS_SOURCE_TYPE_INPUT;
    sourceInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
    sourceInfo.get_name = sourceName;
    sourceInfo.create = sourceCreate;
    sourceInfo.destroy = sourceDestroy;
    sourceInfo.get_width = sourceWidth;
    sourceInfo.get_height = sourceHeight;
    sourceInfo.get_defaults = sourceDefaults;
    sourceInfo.get_properties = sourceProperties;
    sourceInfo.update = sourceUpdate;
    sourceInfo.video_tick = sourceTick;
    sourceInfo.video_render = sourceRender;
    obs_register_source(&sourceInfo);
    blog(LOG_INFO, "[bokis-twitch-chat-plugin] Native plugin %s loaded", BOKIS_TWITCH_CHAT_VERSION);
    return true;
}
