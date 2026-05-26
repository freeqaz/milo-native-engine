// DC3 Native Port — Screenshot Utilities
// Shared PNG/PPM writing for milo-viewer and auto-capture in dc3-native.

#pragma once

#include <cstdint>
#include <vector>

// Write an RGBA8 image as PNG. Returns true on success.
bool WritePNG(const char* path, const uint8_t* rgba, int w, int h);

// Write an RGBA8 image as PPM (P6 binary). Returns true on success.
bool WritePPM(const char* path, const uint8_t* rgba, int w, int h);

// Write an RGBA8 image as PNG to memory (std::vector<uint8_t>). Returns true on success.
bool WritePNGToMemory(std::vector<uint8_t>& out, const uint8_t* rgba, int w, int h);

// Write a screenshot — currently uses PNG format.
bool WriteScreenshot(const char* path, const uint8_t* rgba, int w, int h);
