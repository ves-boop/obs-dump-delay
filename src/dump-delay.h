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

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void dump_delay_register_output(void);
void dump_delay_register_service(void);

/* Drop the delay buffer of the active dump-delay stream (any thread). */
void dump_delay_trigger(void);

enum dump_delay_status {
	DUMP_DELAY_OFF,       /* no dump-delay stream running */
	DUMP_DELAY_BUFFERING, /* stream started, filling the delay buffer */
	DUMP_DELAY_LIVE,      /* connected, delay intact */
	DUMP_DELAY_DUMPING,   /* dumped, slate on air while the delay rebuilds */
};

enum dump_delay_status dump_delay_get_status(void);

/* Plugin settings, stored in the plugin's config folder (UI thread). */
struct dump_delay_config {
	int delay_sec;
	char slate_scene[256];
	int cut_margin_ms;
};

void dump_delay_config_load(void);
void dump_delay_config_get(struct dump_delay_config *cfg);
/* Saves and, if the dump-delay service is in use, applies to it. */
void dump_delay_config_set(const struct dump_delay_config *cfg);
/* True only the first time it is called after installation. */
bool dump_delay_config_take_first_run(void);

/* Switching streaming between the dump-delay service and the service
 * configured in Settings > Stream (UI thread). On failure *error is set to a
 * static, user-readable message. */
bool dump_delay_is_enabled(void);
/* OBS's own Stream Delay (Settings > Advanced) is on: incompatible. */
bool dump_delay_obs_delay_enabled(void);
bool dump_delay_enable(const char **error);
bool dump_delay_disable(const char **error);

/* Create a simple slate scene: dark background, centered text (UI thread). */
void dump_delay_create_slate_scene(const char *name);

void dump_delay_dock_create(void);
void dump_delay_open_settings(void);

#ifdef __cplusplus
}
#endif
