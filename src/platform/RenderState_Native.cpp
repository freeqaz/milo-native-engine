// RndRenderState — native no-op implementations
// On Xbox 360 these set D3D9 render states; on native they are no-ops
// since WebGPU manages its own pipeline state.

#include "rnddx9/RenderState.h"

RndRenderState TheRenderState;

D3DCMPFUNC RndRenderState::tf2cf[] = {
    // Normal Z
    D3DCMP_ALWAYS,       // 0
    D3DCMP_LESS,         // 1
    D3DCMP_EQUAL,        // 2
    D3DCMP_LESSEQUAL,    // 3
    D3DCMP_GREATER,      // 4
    D3DCMP_NOTEQUAL,     // 5
    D3DCMP_GREATEREQUAL, // 6
    D3DCMP_NEVER,        // 7
    // Reversed Z (flip less/greater)
    D3DCMP_ALWAYS,       // 8
    D3DCMP_GREATER,      // 9
    D3DCMP_EQUAL,        // 10
    D3DCMP_GREATEREQUAL, // 11
    D3DCMP_LESS,         // 12
    D3DCMP_NOTEQUAL,     // 13
    D3DCMP_LESSEQUAL,    // 14
    D3DCMP_NEVER,        // 15
};

void RndRenderState::SetBlendEnable(bool) {}
void RndRenderState::SetBlendOp(BlendOp) {}
void RndRenderState::SetBlend(Blend, Blend, Blend, Blend) {}
void RndRenderState::SetColorWriteMask(uint) {}
void RndRenderState::SetTextureFilter(uint, FilterMode, bool) {}
void RndRenderState::SetTextureClamp(uint, ClampMode) {}
void RndRenderState::SetBorderColor(uint, bool) {}
void RndRenderState::SetFillMode(FillMode) {}
void RndRenderState::SetCullMode(CullMode) {}
void RndRenderState::SetAlphaTestEnable(bool) {}
void RndRenderState::SetAlphaFunc(TestFunc, uint) {}
void RndRenderState::SetDepthTestEnable(bool) {}
void RndRenderState::SetDepthWriteEnable(bool) {}
void RndRenderState::SetDepthFunc(TestFunc) {}
void RndRenderState::SetStencilTestEnable(bool) {}
void RndRenderState::SetStencilFunc(TestFunc, u8) {}
void RndRenderState::SetStencilOp(StencilOp, StencilOp, StencilOp) {}
void RndRenderState::Init(void) {}
