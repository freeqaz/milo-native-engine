// DC3 Native Port - XMA sample decoder
// Decodes Xbox 360 XMA2 audio to PCM via FFmpeg's xma2 codec.
// Used by SampleData::Load to convert XMA samples at load time.
#pragma once

#ifdef HX_FFMPEG

// Returns true on success, false on failure.
// On success, *outPCM is allocated with malloc() and must be freed by caller.
// outPCMSize is the total byte size of the PCM data.
bool DecodeXMAToPCM(
    const void* xmaData, int xmaSize,
    int numSamples, int sampleRate, int numChannels,
    void** outPCM, int* outPCMSize
);

#endif
