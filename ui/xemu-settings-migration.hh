/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XEMU_SETTINGS_MIGRATION_HH
#define XEMU_SETTINGS_MIGRATION_HH

#include <cnode.h>
#include <toml++/toml.h>

/* Apply the settings table and retire the legacy ubershader Boolean. */
void xemu_settings_apply_ubershader_migration(
    CNode &config_root, const toml::table &table);

#endif
