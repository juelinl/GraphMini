#pragma once
#include "scalar.h"
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
namespace minigraph::set_ops {
template<bool Write>
inline size_t neon(const uint32_t* a, size_t na, const uint32_t* b, size_t nb,
                   uint32_t* out = nullptr) {
    size_t i = 0, j = 0, count = 0;
    while (na - i >= 4 && nb - j >= 4) {
        const auto va = vld1q_u32(a + i), vb = vld1q_u32(b + j);
        auto matches = vceqq_u32(va, vb);
        matches = vorrq_u32(matches, vceqq_u32(va, vextq_u32(vb, vb, 1)));
        matches = vorrq_u32(matches, vceqq_u32(va, vextq_u32(vb, vb, 2)));
        matches = vorrq_u32(matches, vceqq_u32(va, vextq_u32(vb, vb, 3)));
        const unsigned mask = (vgetq_lane_u32(matches, 0) & 1u) |
            (vgetq_lane_u32(matches, 1) & 2u) |
            (vgetq_lane_u32(matches, 2) & 4u) |
            (vgetq_lane_u32(matches, 3) & 8u);
        count += emit_mask<Write>(a + i, mask, Write && out ? out + count : nullptr);
        const auto am = a[i + 3], bm = b[j + 3];
        i += (am <= bm) * 4;
        j += (bm <= am) * 4;
    }
    if (i < na && j < nb)
        count += scalar<Write>(a + i, na - i, b + j, nb - j,
                               Write && out ? out + count : nullptr);
    return count;
}
}
#endif
