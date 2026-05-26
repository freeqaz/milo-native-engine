#pragma once

class RndMesh;
class RndCam;
class RndEnviron;

// Transparent draw queue — meshes with alpha blend are deferred and sorted back-to-front
bool HasTransparentDraws();
bool IsFlushingTransparentDraws();
void FlushTransparentDraws();
void QueueTransparentDraw(RndMesh* mesh, float distSq, RndCam* cam, RndEnviron* env);

// Text draw queue — text meshes drawn last so they appear on top of UI elements
void FlushTextDraws();

// Blend classification
bool IsTransparentBlend(int blend);

// Env var: MILO_NO_TRANSPARENT_DEFER disables transparent deferral
bool NoTransparentDefer();
