/*
 * Geforce NV2A PGRAPH Vulkan hybrid family key codec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_HYBRID_FAMILY_CODEC_H
#define HW_XBOX_NV2A_PGRAPH_VK_HYBRID_FAMILY_CODEC_H

#include "hw/xbox/nv2a/pgraph/vk/renderer.h"

#define PGRAPH_VK_FAMILY_KEY_ABI 1U
#define PGRAPH_VK_FAMILY_KEY_MAX_SIZE (64U * 1024U)

typedef struct PGRAPHVkFamilyKeyBlob {
    uint8_t *data;
    size_t size;
} PGRAPHVkFamilyKeyBlob;

bool pgraph_vk_family_key_encode(const PipelineKey *key,
                                 PGRAPHVkFamilyKeyBlob *blob);
bool pgraph_vk_family_key_decode(const uint8_t *data, size_t size,
                                 PipelineKey *key);
void pgraph_vk_family_key_blob_destroy(PGRAPHVkFamilyKeyBlob *blob);

#endif
