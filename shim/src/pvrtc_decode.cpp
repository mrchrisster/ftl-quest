#include "pvrtc_decode.h"
#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t GL_PVRTC_RGB_4BPPV1  = 0x8C00;
constexpr uint32_t GL_PVRTC_RGB_2BPPV1  = 0x8C01;
constexpr uint32_t GL_PVRTC_RGBA_4BPPV1 = 0x8C02;
constexpr uint32_t GL_PVRTC_RGBA_2BPPV1 = 0x8C03;

struct PVRTCWord {
    uint32_t mod_data;
    uint32_t color_data;
};

struct ColorRGBA {
    int32_t r;
    int32_t g;
    int32_t b;
    int32_t a;
};

uint32_t next_pow2(uint32_t v) {
    if (v <= 1) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

uint32_t twiddle_uv(uint32_t x_size, uint32_t y_size, uint32_t x_pos, uint32_t y_pos) {
    uint32_t min_dim = x_size;
    uint32_t max_val = y_pos;
    if (y_size < x_size) {
        min_dim = y_size;
        max_val = x_pos;
    }

    uint32_t twiddled = 0;
    uint32_t src_bit = 1;
    uint32_t dst_bit = 1;
    uint32_t shift_count = 0;

    while (src_bit < min_dim) {
        if (y_pos & src_bit) twiddled |= dst_bit;
        if (x_pos & src_bit) twiddled |= (dst_bit << 1);
        src_bit <<= 1;
        dst_bit <<= 2;
        shift_count++;
    }

    max_val >>= shift_count;
    twiddled |= (max_val << (2 * shift_count));
    return twiddled;
}

inline int32_t expand_5_to_8(uint32_t v) {
    return static_cast<int32_t>((v << 3) | (v >> 2));
}

inline int32_t expand_4_to_8(uint32_t v) {
    return static_cast<int32_t>((v << 4) | v);
}

inline int32_t expand_3_to_8(uint32_t v) {
    return static_cast<int32_t>((v << 5) | (v << 2) | (v >> 1));
}

ColorRGBA unpack_color_a(uint32_t color_data) {
    ColorRGBA c;
    if (color_data & 0x00008000U) {
        // Opaque RGB554
        c.r = expand_5_to_8((color_data >> 10) & 0x1F);
        c.g = expand_5_to_8((color_data >> 5) & 0x1F);
        c.b = expand_4_to_8((color_data >> 1) & 0xF);
        c.a = 255;
    } else {
        // Translucent ARGB3443
        c.a = expand_3_to_8((color_data >> 12) & 0x7);
        c.r = expand_4_to_8((color_data >> 8) & 0xF);
        c.g = expand_4_to_8((color_data >> 4) & 0xF);
        c.b = expand_3_to_8((color_data >> 1) & 0x7);
    }
    return c;
}

ColorRGBA unpack_color_b(uint32_t color_data) {
    ColorRGBA c;
    if (color_data & 0x80000000U) {
        // Opaque RGB555
        c.r = expand_5_to_8((color_data >> 26) & 0x1F);
        c.g = expand_5_to_8((color_data >> 21) & 0x1F);
        c.b = expand_5_to_8((color_data >> 16) & 0x1F);
        c.a = 255;
    } else {
        // Translucent ARGB3444
        c.a = expand_3_to_8((color_data >> 28) & 0x7);
        c.r = expand_4_to_8((color_data >> 24) & 0xF);
        c.g = expand_4_to_8((color_data >> 20) & 0xF);
        c.b = expand_4_to_8((color_data >> 16) & 0xF);
    }
    return c;
}

inline int32_t wrap_mod(int32_t v, int32_t m) {
    int32_t r = v % m;
    return (r < 0) ? (r + m) : r;
}

inline uint8_t clamp_u8(int32_t v) {
    if (v <= 0) return 0;
    if (v >= 255) return 255;
    return static_cast<uint8_t>(v);
}

inline ColorRGBA bilinear_color(
    const ColorRGBA &p, const ColorRGBA &q,
    const ColorRGBA &r, const ColorRGBA &s,
    int32_t u, int32_t v,
    int32_t word_w, int32_t word_h
) {
    const int32_t iu = word_w - u;
    const int32_t iv = word_h - v;
    const int32_t w00 = iu * iv;
    const int32_t w10 = u * iv;
    const int32_t w01 = iu * v;
    const int32_t w11 = u * v;
    const int32_t denom = word_w * word_h;

    return ColorRGBA{
        (p.r * w00 + q.r * w10 + r.r * w01 + s.r * w11) / denom,
        (p.g * w00 + q.g * w10 + r.g * w01 + s.g * w11) / denom,
        (p.b * w00 + q.b * w10 + r.b * w01 + s.b * w11) / denom,
        (p.a * w00 + q.a * w10 + r.a * w01 + s.a * w11) / denom
    };
}

// Compute 2bpp interpolated modulation weight (0..8) using pre-untwiddled linear_words.
void get_modulation_2bpp(
    const PVRTCWord *linear_words,
    uint32_t num_bx,
    uint32_t num_by,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    int32_t &out_weight,
    bool &out_punchthrough_zero
) {
    out_punchthrough_zero = false;
    const uint32_t w_mask = static_cast<uint32_t>(width - 1);
    const uint32_t h_mask = static_cast<uint32_t>(height - 1);

    auto read_code = [&](int32_t px, int32_t py, uint32_t &out_mode, uint32_t &out_mod_word, int32_t &lx, int32_t &ly) {
        px &= static_cast<int32_t>(w_mask);
        py &= static_cast<int32_t>(h_mask);
        uint32_t bx = static_cast<uint32_t>(px >> 3);
        uint32_t by = static_cast<uint32_t>(py >> 2);
        const PVRTCWord &w = linear_words[static_cast<size_t>(by) * num_bx + bx];
        out_mode = w.color_data & 1U;
        out_mod_word = w.mod_data;
        lx = px & 7;
        ly = py & 3;
    };

    uint32_t mode = 0, mod_word = 0;
    int32_t lx = 0, ly = 0;
    read_code(x, y, mode, mod_word, lx, ly);

    if (mode == 0) {
        uint32_t bit = (mod_word >> (ly * 8 + lx)) & 1U;
        out_weight = bit ? 8 : 0;
        return;
    }

    auto sample_stored = [&](int32_t px, int32_t py) -> int32_t {
        uint32_t m = 0, mw = 0;
        int32_t slx = 0, sly = 0;
        read_code(px, py, m, mw, slx, sly);
        if (m == 0) {
            return ((mw >> (sly * 8 + slx)) & 1U) ? 8 : 0;
        }
        uint32_t adj_mw = mw;
        if (adj_mw & 2U) adj_mw |= 1U;
        else adj_mw &= ~1U;
        if (adj_mw & (1U << 21)) adj_mw |= (1U << 20);
        else adj_mw &= ~(1U << 20);
        int32_t idx = sly * 4 + (slx >> 1);
        uint32_t bits = (adj_mw >> (idx * 2)) & 3U;
        static constexpr int32_t kTable[4] = { 0, 3, 5, 8 };
        return kTable[bits];
    };

    if (((x ^ y) & 1) == 0) {
        out_weight = sample_stored(x, y);
        return;
    }

    if (mod_word & 1U) {
        if (mod_word & (1U << 20)) {
            out_weight = (sample_stored(x - 1, y) + sample_stored(x + 1, y) + 1) / 2;
        } else {
            out_weight = (sample_stored(x, y - 1) + sample_stored(x, y + 1) + 1) / 2;
        }
    } else {
        out_weight = (sample_stored(x - 1, y) + sample_stored(x + 1, y) +
                      sample_stored(x, y - 1) + sample_stored(x, y + 1) + 2) / 4;
    }
}

} // namespace

bool pvrtc_is_supported_format(uint32_t gl_format) {
    return gl_format == GL_PVRTC_RGB_4BPPV1 ||
           gl_format == GL_PVRTC_RGB_2BPPV1 ||
           gl_format == GL_PVRTC_RGBA_4BPPV1 ||
           gl_format == GL_PVRTC_RGBA_2BPPV1;
}

bool pvrtc_is_2bpp(uint32_t gl_format) {
    return gl_format == GL_PVRTC_RGB_2BPPV1 || gl_format == GL_PVRTC_RGBA_2BPPV1;
}

bool pvrtc_has_alpha(uint32_t gl_format) {
    return gl_format == GL_PVRTC_RGBA_4BPPV1 || gl_format == GL_PVRTC_RGBA_2BPPV1;
}

bool pvrtc_decode_to_rgba8(
    const void *src_data,
    size_t src_bytes,
    int width,
    int height,
    uint32_t gl_format,
    std::vector<uint8_t> &out_rgba
) {
    if (!src_data || width <= 0 || height <= 0 || !pvrtc_is_supported_format(gl_format)) {
        return false;
    }

    const bool is_2bpp = pvrtc_is_2bpp(gl_format);
    const bool has_alpha = pvrtc_has_alpha(gl_format);
    const int32_t word_w = is_2bpp ? 8 : 4;
    const int32_t word_h = 4;

    const int32_t min_w = is_2bpp ? 16 : 8;
    const int32_t min_h = 8;
    const int32_t padded_w = static_cast<int32_t>(next_pow2(static_cast<uint32_t>(std::max(width, min_w))));
    const int32_t padded_h = static_cast<int32_t>(next_pow2(static_cast<uint32_t>(std::max(height, min_h))));

    const uint32_t num_bx = static_cast<uint32_t>(padded_w / word_w);
    const uint32_t num_by = static_cast<uint32_t>(padded_h / word_h);
    const size_t required_bytes = static_cast<size_t>(num_bx) * static_cast<size_t>(num_by) * sizeof(PVRTCWord);
    if (src_bytes > 0 && src_bytes < required_bytes) {
        // Still guard against reading past short buffers
        return false;
    }

    const auto *words = reinterpret_cast<const PVRTCWord *>(src_data);

    std::vector<ColorRGBA> col_a(static_cast<size_t>(num_bx) * num_by);
    std::vector<ColorRGBA> col_b(static_cast<size_t>(num_bx) * num_by);
    std::vector<PVRTCWord> linear_words(static_cast<size_t>(num_bx) * num_by);
    for (uint32_t by = 0; by < num_by; by++) {
        for (uint32_t bx = 0; bx < num_bx; bx++) {
            const uint32_t tw = twiddle_uv(num_bx, num_by, bx, by);
            const size_t linear = static_cast<size_t>(by) * num_bx + bx;
            linear_words[linear] = words[tw];
            col_a[linear] = unpack_color_a(words[tw].color_data);
            col_b[linear] = unpack_color_b(words[tw].color_data);
        }
    }

    out_rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const int32_t half_w = word_w / 2;
    const int32_t half_h = word_h / 2;

    for (int32_t y = 0; y < height; y++) {
        const int32_t sy = y - half_h;
        const int32_t by0 = wrap_mod((sy < 0) ? ((sy - word_h + 1) / word_h) : (sy / word_h), static_cast<int32_t>(num_by));
        const int32_t by1 = (by0 + 1) % static_cast<int32_t>(num_by);
        const int32_t fv = wrap_mod(sy, word_h);

        for (int32_t x = 0; x < width; x++) {
            const int32_t sx = x - half_w;
            const int32_t bx0 = wrap_mod((sx < 0) ? ((sx - word_w + 1) / word_w) : (sx / word_w), static_cast<int32_t>(num_bx));
            const int32_t bx1 = (bx0 + 1) % static_cast<int32_t>(num_bx);
            const int32_t fu = wrap_mod(sx, word_w);

            const size_t i00 = static_cast<size_t>(by0) * num_bx + bx0;
            const size_t i10 = static_cast<size_t>(by0) * num_bx + bx1;
            const size_t i01 = static_cast<size_t>(by1) * num_bx + bx0;
            const size_t i11 = static_cast<size_t>(by1) * num_bx + bx1;

            const ColorRGBA base_a = bilinear_color(col_a[i00], col_a[i10], col_a[i01], col_a[i11], fu, fv, word_w, word_h);
            const ColorRGBA base_b = bilinear_color(col_b[i00], col_b[i10], col_b[i01], col_b[i11], fu, fv, word_w, word_h);

            int32_t weight = 0;
            bool punchthrough_zero = false;
            if (!is_2bpp) {
                const uint32_t cbx = static_cast<uint32_t>(x >> 2);
                const uint32_t cby = static_cast<uint32_t>(y >> 2);
                const PVRTCWord &w = linear_words[static_cast<size_t>(cby) * num_bx + cbx];
                const int32_t pix_idx = (y & 3) * 4 + (x & 3);
                const uint32_t mod_bits = (w.mod_data >> (pix_idx * 2)) & 3U;
                const bool mode = (w.color_data & 1U) != 0;
                if (!mode) {
                    static constexpr int32_t kWeights4[4] = { 0, 3, 5, 8 };
                    weight = kWeights4[mod_bits];
                } else {
                    static constexpr int32_t kWeightsPT[4] = { 0, 4, 4, 8 };
                    weight = kWeightsPT[mod_bits];
                    if (mod_bits == 2) punchthrough_zero = true;
                }
            } else {
                get_modulation_2bpp(linear_words.data(), num_bx, num_by, x, y, padded_w, padded_h, weight, punchthrough_zero);
            }

            const int32_t inv_w = 8 - weight;
            const size_t dst = (static_cast<size_t>(y) * width + x) * 4;
            out_rgba[dst + 0] = clamp_u8((base_a.r * inv_w + base_b.r * weight) / 8);
            out_rgba[dst + 1] = clamp_u8((base_a.g * inv_w + base_b.g * weight) / 8);
            out_rgba[dst + 2] = clamp_u8((base_a.b * inv_w + base_b.b * weight) / 8);
            if (!has_alpha) {
                out_rgba[dst + 3] = 255;
            } else if (punchthrough_zero) {
                out_rgba[dst + 3] = 0;
            } else {
                out_rgba[dst + 3] = clamp_u8((base_a.a * inv_w + base_b.a * weight) / 8);
            }
        }
    }

    return true;
}
