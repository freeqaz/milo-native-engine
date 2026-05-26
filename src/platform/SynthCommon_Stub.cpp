// DC3 Native Port - Synth Common Xbox Stub
// Replaces system/synth/Common_Xbox.cpp

// DSP functions that were Xbox-specific
void DspClearBuffer(float *&buf, int sizeSamps) {
    if (buf) {
        for (int i = 0; i < sizeSamps; i++) buf[i] = 0.0f;
    }
}
void DspFree(float *&f) {
    delete[] f;
    f = nullptr;
}
void DspAllocate(float *&buf, int sizeSamps, void *) {
    buf = new float[sizeSamps];
}
