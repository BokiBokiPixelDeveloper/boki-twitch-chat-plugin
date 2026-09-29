-- Optional isolated OBS integration script; see tests/README.md.
obs = obslua
local source, scene, second
local step = 0
local fixture = ""
function click(key)
    local props = obs.obs_source_properties(source)
    local property = obs.obs_properties_get(props, key)
    if property then obs.obs_property_button_clicked(property, nil) end
    obs.obs_properties_destroy(props)
end
function status()
    local props = obs.obs_source_properties(source)
    local property = obs.obs_properties_get(props, 'web_status_info')
    if property then obs.script_log(obs.LOG_INFO, 'SMOKE ' .. obs.obs_property_description(property)) end
    obs.obs_properties_destroy(props)
end
function mode(value)
    local settings = obs.obs_source_get_settings(source)
    obs.obs_data_set_string(settings, 'renderer_mode', value)
    obs.obs_source_update(source, settings)
    obs.obs_data_release(settings)
end
function run()
    step = step + 1
    if step == 1 then
        scene = obs.obs_scene_create('Isolated widget test')
        local settings = obs.obs_data_create()
        obs.obs_data_set_string(settings, 'client_id', 'isolated-widget-test')
        obs.obs_data_set_string(settings, 'channel', 'fixture')
        obs.obs_data_set_string(settings, 'renderer_mode', 'web_widget')
        obs.obs_data_set_string(settings, 'web_widget_compatibility', 'auto')
        obs.obs_data_set_bool(settings, 'event_test_enabled', true)
        obs.obs_data_set_bool(settings, 'auto_update_check', false)
        obs.obs_data_set_bool(settings, 'web_widget_trust_acknowledged', true)
        obs.obs_data_set_string(settings, 'web_widget_zip_path', fixture)
        source = obs.obs_source_create('bokis_twitch_chat_plugin', 'Scrapbook smoke', settings, nil)
        obs.obs_data_release(settings)
        obs.obs_scene_add(scene, source)
        obs.obs_frontend_set_current_scene(obs.obs_scene_get_source(scene))
    elseif step == 3 then status(); click('web_import_widget')
    elseif step == 5 then status(); click('event_test_chat')
    elseif step == 6 then obs.obs_frontend_take_source_screenshot(source)
    elseif step == 7 then status(); click('web_refresh'); click('web_refresh')
    elseif step == 9 then status(); click('event_test_chat')
    elseif step == 10 then mode('native')
    elseif step == 11 then mode('web_widget')
    elseif step == 13 then
        status()
        local settings = obs.obs_source_get_settings(source)
        second = obs.obs_source_create('bokis_twitch_chat_plugin', 'Second Scrapbook', settings, nil)
        obs.obs_data_release(settings)
        obs.obs_scene_add(scene, second)
    elseif step == 15 then status(); click('event_test_chat')
    elseif step == 17 then
        obs.obs_sceneitem_remove(obs.obs_scene_find_source(scene, 'Second Scrapbook'))
        obs.obs_source_release(second); second = nil
        obs.obs_sceneitem_remove(obs.obs_scene_find_source(scene, 'Scrapbook smoke'))
        obs.obs_source_release(source); source = nil
        obs.obs_scene_release(scene); scene = nil
        obs.script_log(obs.LOG_INFO, 'SMOKE complete')
        obs.timer_remove(run)
    end
end
function script_load(settings)
    fixture = obs.obs_data_get_string(settings, "fixture")
    obs.timer_add(run, 1000)
end
