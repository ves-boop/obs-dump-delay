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

#include "dump-delay.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-dump-delay", "en-US")
OBS_MODULE_AUTHOR("Valerio Enrico Salvo (Ves)")

static obs_hotkey_id dump_hotkey = OBS_INVALID_HOTKEY_ID;

static void on_dump_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			   bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		dump_delay_trigger();
}

static void on_settings_menu(void *data)
{
	UNUSED_PARAMETER(data);
	dump_delay_open_settings();
}

/* Frontend hotkeys registered by plugins are not persisted by OBS itself. */
static void on_save(obs_data_t *save_data, bool saving, void *data)
{
	UNUSED_PARAMETER(data);

	if (saving) {
		obs_data_array_t *keys = obs_hotkey_save(dump_hotkey);
		obs_data_set_array(save_data, "dump_delay_hotkey", keys);
		obs_data_array_release(keys);
	} else {
		obs_data_array_t *keys =
			obs_data_get_array(save_data, "dump_delay_hotkey");
		obs_hotkey_load(dump_hotkey, keys);
		obs_data_array_release(keys);
	}
}

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);

	/* the streaming service is loaded by now: make it match our settings */
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		struct dump_delay_config cfg;
		dump_delay_config_get(&cfg);
		dump_delay_config_set(&cfg);
	}
}

const char *obs_module_name(void)
{
	return obs_module_text("DumpDelay");
}

const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

bool obs_module_load(void)
{
	dump_delay_config_load();
	dump_delay_register_output();
	dump_delay_register_service();

	dump_hotkey = obs_hotkey_register_frontend("dump_delay.dump",
						   obs_module_text("Hotkey.Dump"),
						   on_dump_hotkey, NULL);
	obs_frontend_add_save_callback(on_save, NULL);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("Menu.Settings"),
					 on_settings_menu, NULL);

	dump_delay_dock_create();
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	obs_frontend_remove_save_callback(on_save, NULL);
	obs_hotkey_unregister(dump_hotkey);
}
