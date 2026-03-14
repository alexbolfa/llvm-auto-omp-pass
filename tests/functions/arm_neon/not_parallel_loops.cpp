#include <arm_neon.h>

void convergent_stride_smaller_than_width(int *data, int n) {
    //!                              2 < 4
    for (int i = 0; i <= n - 4; i += 2) {
        // Load 4 integers into a SIMD register
        int32x4_t x = vld1q_s32(&data[i]);

        // CONVERGENT OP: Rotate elements within the register
        // Lane 0 gets Lane 1, Lane 1 gets Lane 2, etc.
        int32x4_t y = vextq_s32(x, x, 1);

        vst1q_s32(&data[i], vaddq_s32(x, y));
    }
}
