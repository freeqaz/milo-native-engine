#pragma once

#include "rndobj/BaseMaterial.h"

inline bool NativeShouldForceTextAlpha(
    bool isTextMesh, BaseMaterial::Blend blend, float alpha
) {
    return isTextMesh && blend == BaseMaterial::kBlendSrcAlpha && alpha < 0.01f;
}
