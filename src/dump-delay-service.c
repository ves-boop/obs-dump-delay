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

#include "dump-delay.h"

/*
 * A streaming service whose preferred output is dump_delay_output, so the
 * normal "Start Streaming" button uses our output instead of rtmp_output.
 * Delay settings live here too, the output reads them from the service.
 */

struct dump_delay_service {
	char *server;
	char *key;
};

static const char *dds_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Service.Name");
}

static void dds_update(void *data, obs_data_t *settings)
{
	struct dump_delay_service *s = data;

	bfree(s->server);
	bfree(s->key);
	s->server = bstrdup(obs_data_get_string(settings, "server"));
	s->key = bstrdup(obs_data_get_string(settings, "key"));
}

static void *dds_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(service);

	struct dump_delay_service *s = bzalloc(sizeof(*s));
	dds_update(s, settings);
	return s;
}

static void dds_destroy(void *data)
{
	struct dump_delay_service *s = data;

	bfree(s->server);
	bfree(s->key);
	bfree(s);
}

static void dds_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "server",
				    "rtmp://live.twitch.tv/app");
	obs_data_set_default_int(settings, "delay_sec", 10);
	obs_data_set_default_string(settings, "slate_scene",
				    obs_module_text("Slate.DefaultName"));
	obs_data_set_default_int(settings, "cut_margin_ms", 1000);
}

static obs_properties_t *dds_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "server", obs_module_text("Props.Server"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "key", obs_module_text("Props.Key"),
				OBS_TEXT_PASSWORD);
	obs_properties_add_int(props, "delay_sec",
			       obs_module_text("Settings.Delay"), 3, 60, 1);
	obs_properties_add_text(props, "slate_scene",
				obs_module_text("Settings.Slate"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "cut_margin_ms",
			       obs_module_text("Settings.Margin"), 0, 5000,
			       100);
	return props;
}

/* Twitch requires a 2 s keyframe interval, and the keyframe interval also
 * bounds the freeze right after a dump (we resume on the first slate
 * keyframe). OBS's own Twitch service sets this through the same hook. */
static void dds_apply_encoder_settings(void *data, obs_data_t *video_settings,
				       obs_data_t *audio_settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(audio_settings);

	if (!video_settings)
		return;

	int64_t keyint = obs_data_get_int(video_settings, "keyint_sec");
	if (keyint <= 0 || keyint > 2)
		obs_data_set_int(video_settings, "keyint_sec", 2);
}

static const char *dds_get_output_type(void *data)
{
	UNUSED_PARAMETER(data);
	return "dump_delay_output";
}

static const char *dds_get_protocol(void *data)
{
	UNUSED_PARAMETER(data);
	return "RTMP";
}

static const char **dds_get_video_codecs(void *data)
{
	UNUSED_PARAMETER(data);
	static const char *codecs[] = {"h264", NULL};
	return codecs;
}

static const char **dds_get_audio_codecs(void *data)
{
	UNUSED_PARAMETER(data);
	static const char *codecs[] = {"aac", NULL};
	return codecs;
}

static const char *dds_get_connect_info(void *data, uint32_t type)
{
	struct dump_delay_service *s = data;

	switch ((enum obs_service_connect_info)type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return s->server;
	case OBS_SERVICE_CONNECT_INFO_STREAM_KEY:
		return s->key;
	default:
		return NULL;
	}
}

static bool dds_can_try_to_connect(void *data)
{
	struct dump_delay_service *s = data;
	return s->server && *s->server && s->key && *s->key;
}

static struct obs_service_info dump_delay_service = {
	.id = "dump_delay_service",
	.get_name = dds_get_name,
	.create = dds_create,
	.destroy = dds_destroy,
	.update = dds_update,
	.get_defaults = dds_get_defaults,
	.get_properties = dds_get_properties,
	.apply_encoder_settings = dds_apply_encoder_settings,
	.get_output_type = dds_get_output_type,
	.get_protocol = dds_get_protocol,
	.get_supported_video_codecs = dds_get_video_codecs,
	.get_supported_audio_codecs = dds_get_audio_codecs,
	.get_connect_info = dds_get_connect_info,
	.can_try_to_connect = dds_can_try_to_connect,
};

void dump_delay_register_service(void)
{
	obs_register_service(&dump_delay_service);
}
