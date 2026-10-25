#pragma once

#include <dxgiformat.h>
#include <cstdint>

// Packed P010 retained-reservoir slot (llm-wiki/wgc-reservoir-vram-design.md,
// "Packed P010 slots").
//
// P010 keeps each 10-bit code in the high bits of a 16-bit sample, so a 4:2:0
// frame takes 24 bits per pixel for 15 bits of codes. A packed slot is one
// frame-sized R16_UINT texture, 16 bits per pixel, from which every P010 code
// is reconstructed exactly:
//   texel(x, y)  = Y(x, y) | piece(k) << 10,  k = (x & 1) | (y & 1) << 1
//   block word   = (Cb ^ 512) | (Cr ^ 512) << 10   (the 2x2 block's chroma)
//   piece(k)     = (block word >> 5k) & 31
// Bit 15 is always zero. Chroma is stored relative to neutral (XOR 512 is the
// 10-bit two's-complement offset), so neutral chroma has zero pieces and a
// frame of one luma code with neutral chroma (privacy black) is one uniform
// texel value that a clear can write.
//
// kPackedP010ShaderSource (mediaengine/video/packed_p010_shader.h) implements
// this layout in HLSL; PackedP010*Test pins both to these functions.

namespace ce::packed_p010 {

inline constexpr DXGI_FORMAT kSlotFormat = DXGI_FORMAT_R16_UINT;
inline constexpr uint32_t kBitsPerPixel = 16;
inline constexpr uint32_t kCodeMask = 0x3FFu;
inline constexpr uint32_t kNeutralChroma = 512u;
inline constexpr uint32_t kPieceBits = 5;
inline constexpr uint32_t kPieceMask = (1u << kPieceBits) - 1u;

inline bool IsSlotFormat(DXGI_FORMAT format) {
    return format == kSlotFormat;
}

// Which fifth of the block word pixel (x, y) carries.
inline uint32_t PieceIndex(uint32_t x, uint32_t y) {
    return (x & 1u) | ((y & 1u) << 1);
}

inline uint32_t BlockWord(uint32_t cb, uint32_t cr) {
    return ((cb & kCodeMask) ^ kNeutralChroma) | (((cr & kCodeMask) ^ kNeutralChroma) << 10);
}

inline uint16_t PackTexel(uint32_t luma, uint32_t blockWord, uint32_t piece) {
    const uint32_t bits = (blockWord >> (kPieceBits * (piece & 3u))) & kPieceMask;
    return static_cast<uint16_t>((luma & kCodeMask) | (bits << 10));
}

inline uint32_t LumaCode(uint16_t texel) {
    return texel & kCodeMask;
}

// Cb/Cr of a 2x2 block from its four texels, indexed by PieceIndex.
inline void ChromaCodes(const uint16_t (&blockTexels)[4], uint32_t* cb, uint32_t* cr) {
    uint32_t word = 0;
    for (uint32_t piece = 0; piece < 4; ++piece)
        word |= ((static_cast<uint32_t>(blockTexels[piece]) >> 10) & kPieceMask) << (kPieceBits * piece);
    *cb = (word & kCodeMask) ^ kNeutralChroma;
    *cr = ((word >> 10) & kCodeMask) ^ kNeutralChroma;
}

// Every texel of a frame with this luma code and neutral chroma.
inline uint16_t NeutralTexel(uint32_t luma) {
    return static_cast<uint16_t>(luma & kCodeMask);
}

}  // namespace ce::packed_p010
