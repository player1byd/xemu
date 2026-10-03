/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "xemu-settings-migration.hh"
#include "xemu-tweaks.h"

void xemu_settings_apply_ubershader_migration(
    CNode &config_root, const toml::table &table)
{
    const toml::table *tweaks_table = table["tweaks"].as_table();
    bool mode_present = tweaks_table &&
        tweaks_table->contains("vk_ubershader_mode");
    bool legacy_present = tweaks_table &&
        tweaks_table->contains("vk_hybrid_ubershaders");

    config_root.update_from_table(table);

    CNode *tweaks = config_root.child("tweaks");
    CNode *mode = tweaks->child("vk_ubershader_mode");
    CNode *legacy = tweaks->child("vk_hybrid_ubershaders");
    if (mode_present || legacy_present) {
        mode->set_enum_by_index(static_cast<int>(
            xemu_vulkan_ubershader_migrate_mode(
                mode_present,
                static_cast<XemuVulkanUbershaderMode>(mode->data_enum.val),
                legacy->data.boolean.val)));
    }
    legacy->data.boolean.val = false;
}
