#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Returns true for GL_COMPRESSED_{RGB,RGBA}_PVRTC_{4,2}BPPV1_IMG (0x8C00..0x8C03).
bool pvrtc_is_supported_format(uint32_t gl_format);

// Returns true for 2bpp PVRTC formats (0x8C01, 0x8C03).
bool pvrtc_is_2bpp(uint32_t gl_format);

// Returns true for RGBA PVRTC formats (0x8C02, 0x8C03).
bool pvrtc_has_alpha(uint32_t gl_format);

// Decodes a PVRTC 4bpp or 2bpp compressed buffer into tightly packed RGBA8888 pixels.
// Handles minimum block padding and non-square power-of-two Morton twiddling.
bool pvrtc_decode_to_rgba8(
    const void *src_data,
    size_t src_bytes,
    int width,
    int height,
    uint32_t gl_format,
    std::vector<uint8_t> &out_rgba
);
