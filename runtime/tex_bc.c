// BC1-BC7 blocks decoded on the CPU, for GPUs that do not sample BC formats (Qualcomm's Adreno driver among them):
// vk_formats.c tex_vk_format then creates the image in the format bc_texel_bytes names and vk_texture.c uploads what
// bc_decode writes. The block decoders are bcdec's (third_party/bcdec, MIT).
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE   // the signed BC4/BC5 variants, bit-exact rather than the fast approximation
#include "third_party/bcdec/bcdec.h"

#include <string.h>

#include "tex.h"

int bc_texel_bytes(int kind) {
    switch (kind) {
    case 4: return 1;   // R8
    case 5: return 2;   // R8G8
    case 6: return 8;   // R16G16B16A16 half
    default: return 4;  // RGBA8: BC1, BC2, BC3, BC7
    }
}

static int block_bytes(int kind) { return kind == 1 || kind == 4 ? 8 : 16; }

void bc_decode(int kind, int is_signed, int opaque, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out) {
    int tb = bc_texel_bytes(kind), bb = block_bytes(kind);
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    uint8_t tmp[4 * 4 * 8];
    uint16_t rgb[4 * 4 * 3];
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++, blocks += bb) {
            switch (kind) {
            case 1: bcdec_bc1(blocks, tmp, 4 * 4); break;
            case 2: bcdec_bc2(blocks, tmp, 4 * 4); break;
            case 3: bcdec_bc3(blocks, tmp, 4 * 4); break;
            case 4: bcdec_bc4(blocks, tmp, 4, is_signed); break;
            case 5: bcdec_bc5(blocks, tmp, 4 * 2, is_signed); break;
            case 6:   // RGB halves; the image is RGBA, alpha 1.0
                bcdec_bc6h_half(blocks, rgb, 4 * 3, is_signed);
                for (int i = 0; i < 16; i++) {
                    uint16_t px[4] = {rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2], 0x3c00};
                    memcpy(tmp + 8 * i, px, 8);
                }
                break;
            default: bcdec_bc7(blocks, tmp, 4 * 4); break;
            }
            if (opaque)   // BC1 RGB: the punch-through texel is black with alpha 1, as Vulkan's BC1_RGB samples it
                for (int i = 0; i < 16; i++) tmp[4 * i + 3] = 255;
            uint32_t x0 = bx * 4, y0 = by * 4;
            uint32_t cw = width - x0 < 4 ? width - x0 : 4, ch = height - y0 < 4 ? height - y0 : 4;
            for (uint32_t y = 0; y < ch; y++)
                memcpy(out + ((size_t)(y0 + y) * width + x0) * tb, tmp + (size_t)y * 4 * tb, (size_t)cw * tb);
        }
}
