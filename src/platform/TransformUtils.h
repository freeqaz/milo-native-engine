#pragma once

#include "platform/Rnd_Wgpu.h"
#include "math/Mtx.h"
#include <cmath>
#include <cstring>

// ============================================================================
// Helper: Convert Transform to ObjectUniforms
// ============================================================================

inline void FillObjectUniforms(const Transform& worldXfm, ObjectUniforms& obj) {
    // World matrix — row-major, WGSL reads as column-major (correct transpose)
    obj.world[0]  = worldXfm.m.x.x; obj.world[1]  = worldXfm.m.x.y; obj.world[2]  = worldXfm.m.x.z; obj.world[3]  = 0;
    obj.world[4]  = worldXfm.m.y.x; obj.world[5]  = worldXfm.m.y.y; obj.world[6]  = worldXfm.m.y.z; obj.world[7]  = 0;
    obj.world[8]  = worldXfm.m.z.x; obj.world[9]  = worldXfm.m.z.y; obj.world[10] = worldXfm.m.z.z; obj.world[11] = 0;
    obj.world[12] = worldXfm.v.x;   obj.world[13] = worldXfm.v.y;   obj.world[14] = worldXfm.v.z;   obj.world[15] = 1;

    // Compute inverse-transpose of the upper 3x3 for correct normal transformation
    // under non-uniform scale. For pure rotation this equals the rotation matrix.
    const Hmx::Matrix3& m = worldXfm.m;
    float det = m.x.x * (m.y.y * m.z.z - m.y.z * m.z.y)
              - m.x.y * (m.y.x * m.z.z - m.y.z * m.z.x)
              + m.x.z * (m.y.x * m.z.y - m.y.y * m.z.x);
    if (fabsf(det) > 1e-12f) {
        float invDet = 1.0f / det;
        // Inverse of 3x3, then transposed — stored row-major
        obj.worldInvTranspose[0]  = (m.y.y * m.z.z - m.y.z * m.z.y) * invDet;
        obj.worldInvTranspose[1]  = (m.y.z * m.z.x - m.y.x * m.z.z) * invDet;
        obj.worldInvTranspose[2]  = (m.y.x * m.z.y - m.y.y * m.z.x) * invDet;
        obj.worldInvTranspose[3]  = 0;
        obj.worldInvTranspose[4]  = (m.x.z * m.z.y - m.x.y * m.z.z) * invDet;
        obj.worldInvTranspose[5]  = (m.x.x * m.z.z - m.x.z * m.z.x) * invDet;
        obj.worldInvTranspose[6]  = (m.x.y * m.z.x - m.x.x * m.z.y) * invDet;
        obj.worldInvTranspose[7]  = 0;
        obj.worldInvTranspose[8]  = (m.x.y * m.y.z - m.x.z * m.y.y) * invDet;
        obj.worldInvTranspose[9]  = (m.x.z * m.y.x - m.x.x * m.y.z) * invDet;
        obj.worldInvTranspose[10] = (m.x.x * m.y.y - m.x.y * m.y.x) * invDet;
        obj.worldInvTranspose[11] = 0;
        obj.worldInvTranspose[12] = 0;
        obj.worldInvTranspose[13] = 0;
        obj.worldInvTranspose[14] = 0;
        obj.worldInvTranspose[15] = 1;
    } else {
        // Degenerate — fall back to world matrix
        memcpy(obj.worldInvTranspose, obj.world, 64);
    }
}

// ============================================================================
// Helper: Convert Transform to a row-major 4x4 float array
// ============================================================================

inline void TransformToMat4(const Transform& xfm, float* out) {
    out[0]  = xfm.m.x.x; out[1]  = xfm.m.x.y; out[2]  = xfm.m.x.z; out[3]  = 0;
    out[4]  = xfm.m.y.x; out[5]  = xfm.m.y.y; out[6]  = xfm.m.y.z; out[7]  = 0;
    out[8]  = xfm.m.z.x; out[9]  = xfm.m.z.y; out[10] = xfm.m.z.z; out[11] = 0;
    out[12] = xfm.v.x;   out[13] = xfm.v.y;   out[14] = xfm.v.z;   out[15] = 1;
}
