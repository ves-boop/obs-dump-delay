/*
 * Dump Delay for OBS Studio
 * Copyright (C) 2026 Valerio Enrico Salvo (Ves)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version. See LICENSE.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include "dump-delay.h"

/*
 * Plugin settings and service switching. The settings live in the plugin's
 * own config file (OBS's Settings > Stream page can't show third-party
 * services) and are copied into the dump-delay service when it is in use.
 */

static obs_data_t *config;

static char *config_file(void)
{
	return obs_module_config_path("config.json");
}

static void apply_defaults(obs_data_t *data)
{
	obs_data_set_default_int(data, "delay_sec", 10);
	obs_data_set_default_string(data, "slate_scene",
				    obs_module_text("Slate.DefaultName"));
	obs_data_set_default_int(data, "cut_margin_ms", 1000);
}

void dump_delay_config_load(void)
{
	char *path = config_file();

	obs_data_release(config);
	config = obs_data_create_from_json_file_safe(path, "bak");
	if (!config)
		config = obs_data_create();
	apply_defaults(config);
	bfree(path);
}

void dump_delay_config_get(struct dump_delay_config *cfg)
{
	cfg->delay_sec = (int)obs_data_get_int(config, "delay_sec");
	snprintf(cfg->slate_scene, sizeof(cfg->slate_scene), "%s",
		 obs_data_get_string(config, "slate_scene"));
	cfg->cut_margin_ms = (int)obs_data_get_int(config, "cut_margin_ms");
}

static bool is_dump_delay_service(obs_service_t *service)
{
	return service &&
	       strcmp(obs_service_get_id(service), "dump_delay_service") == 0;
}

static void copy_config(obs_data_t *dst)
{
	obs_data_set_int(dst, "delay_sec", obs_data_get_int(config, "delay_sec"));
	obs_data_set_string(dst, "slate_scene",
			    obs_data_get_string(config, "slate_scene"));
	obs_data_set_int(dst, "cut_margin_ms",
			 obs_data_get_int(config, "cut_margin_ms"));
}

/* Push the plugin settings into the dump-delay service, if it is in use.
 * The output reads them when a stream starts. */
static void sync_service(void)
{
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (!is_dump_delay_service(service))
		return;

	obs_data_t *settings = obs_data_create();
	copy_config(settings);
	obs_service_update(service, settings);
	obs_data_release(settings);
	obs_frontend_save_streaming_service();
}

static void save_config(void)
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);

	char *path = config_file();
	if (!obs_data_save_json_safe(config, path, "tmp", "bak"))
		blog(LOG_WARNING, "[dump-delay] could not save %s", path);
	bfree(path);
}

void dump_delay_config_set(const struct dump_delay_config *cfg)
{
	obs_data_set_int(config, "delay_sec", cfg->delay_sec);
	obs_data_set_string(config, "slate_scene", cfg->slate_scene);
	obs_data_set_int(config, "cut_margin_ms", cfg->cut_margin_ms);
	save_config();
	sync_service();
}

bool dump_delay_config_take_first_run(void)
{
	if (obs_data_get_bool(config, "dock_shown"))
		return false;

	obs_data_set_bool(config, "dock_shown", true);
	save_config();
	return true;
}

/* OBS's own stream delay sits in front of our output: what it holds back
 * can't be dumped. Same setting the Simple/Advanced output handlers read. */
bool dump_delay_obs_delay_enabled(void)
{
	config_t *profile = obs_frontend_get_profile_config();
	return profile && config_get_bool(profile, "Output", "DelayEnable");
}

bool dump_delay_is_enabled(void)
{
	return is_dump_delay_service(obs_frontend_get_streaming_service());
}

static void set_streaming_service(obs_service_t *service)
{
	obs_frontend_set_streaming_service(service);
	obs_frontend_save_streaming_service();
	obs_service_release(service);
}

/* Takes the server and key of the service configured in Settings (Twitch
 * included) and keeps that service's settings to restore it later. */
bool dump_delay_enable(const char **error)
{
	obs_service_t *current = obs_frontend_get_streaming_service();

	if (is_dump_delay_service(current))
		return true;
	if (obs_frontend_streaming_active()) {
		*error = obs_module_text("Error.StopStreaming");
		return false;
	}

	const char *server = obs_service_get_connect_info(
		current, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const char *key = obs_service_get_connect_info(
		current, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	if (!server || !*server || !key || !*key) {
		*error = obs_module_text("Error.NoServerKey");
		return false;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_t *prev_settings = obs_service_get_settings(current);
	obs_data_set_string(settings, "server", server);
	obs_data_set_string(settings, "key", key);
	obs_data_set_string(settings, "prev_type", obs_service_get_id(current));
	obs_data_set_obj(settings, "prev_settings", prev_settings);
	copy_config(settings);

	obs_service_t *service = obs_service_create(
		"dump_delay_service", "default_service", settings, NULL);
	obs_data_release(prev_settings);
	obs_data_release(settings);

	if (!service) {
		*error = obs_module_text("Error.CreateService");
		return false;
	}
	set_streaming_service(service);
	blog(LOG_INFO, "[dump-delay] enabled: streaming now goes through the dump-delay output");
	return true;
}

static obs_sceneitem_t *add_source(obs_scene_t *scene, const char *unversioned_id,
				   const char *name, obs_data_t *settings)
{
	const char *id = obs_get_latest_input_type_id(unversioned_id);
	if (!id)
		return NULL;

	obs_source_t *source = obs_source_create(id, name, settings, NULL);
	if (!source)
		return NULL;

	obs_sceneitem_t *item = obs_scene_add(scene, source);
	obs_source_release(source);
	return item;
}

void dump_delay_create_slate_scene(const char *name)
{
	obs_source_t *existing = obs_get_source_by_name(name);
	if (existing) {
		obs_source_release(existing);
		return;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

	obs_scene_t *scene = obs_scene_create(name);

	obs_data_t *bg = obs_data_create();
	obs_data_set_int(bg, "color", 0xFF201C1C); /* ABGR: near-black */
	obs_data_set_int(bg, "width", ovi.base_width);
	obs_data_set_int(bg, "height", ovi.base_height);
	add_source(scene, "color_source", obs_module_text("Slate.Background"), bg);
	obs_data_release(bg);

	obs_data_t *text = obs_data_create();
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Arial");
	obs_data_set_int(font, "size", (int)ovi.base_height / 10);
	obs_data_set_string(font, "style", "Bold");
	obs_data_set_obj(text, "font", font);
	obs_data_set_string(text, "text", obs_module_text("Slate.Text"));
	obs_data_set_string(text, "align", "center");
	obs_sceneitem_t *item = add_source(scene, "text_gdiplus",
					   obs_module_text("Slate.TextSource"), text);
	obs_data_release(font);
	obs_data_release(text);

	if (item) {
		struct vec2 pos = {ovi.base_width / 2.0f, ovi.base_height / 2.0f};
		obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
		obs_sceneitem_set_pos(item, &pos);
	}

	obs_scene_release(scene);
	blog(LOG_INFO, "[dump-delay] created slate scene '%s'", name);
}

bool dump_delay_disable(const char **error)
{
	obs_service_t *current = obs_frontend_get_streaming_service();

	if (!is_dump_delay_service(current))
		return true;
	if (obs_frontend_streaming_active()) {
		*error = obs_module_text("Error.StopStreaming");
		return false;
	}

	obs_data_t *settings = obs_service_get_settings(current);
	const char *prev_type = obs_data_get_string(settings, "prev_type");
	obs_data_t *prev_settings = obs_data_get_obj(settings, "prev_settings");

	obs_service_t *service = obs_service_create(
		*prev_type ? prev_type : "rtmp_common", "default_service",
		prev_settings, NULL);
	obs_data_release(prev_settings);
	obs_data_release(settings);

	if (!service) {
		*error = obs_module_text("Error.RestoreService");
		return false;
	}
	set_streaming_service(service);
	blog(LOG_INFO, "[dump-delay] disabled: previous streaming service restored");
	return true;
}
