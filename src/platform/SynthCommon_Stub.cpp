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
// The third parameter must be IXAudioBatchAllocator*, not void*.
//
// Every consumer declares this function through synth/Common_Xbox.h --
//     void DspAllocate(float *&, int, IXAudioBatchAllocator *);
// -- in dc3-decomp and in rb3-xenon alike, so their call sites reference
// _Z11DspAllocateRPfiP21IXAudioBatchAllocator.  A `void *` parameter defines
// _Z11DspAllocateRPfiPv, which is a *different symbol*: nothing in this repo or
// any consumer ever referenced it, and the real reference stayed unresolved.
//
// In dc3-native that unresolved reference was picked up by the weak asm-label
// stub _stub_fn_8 in native/src/engine_stubs_generated.cpp, which returns 0
// without touching `buf`.  `nm` on the link inputs showed the split plainly:
//     SynthCommon_Stub.cpp.o :  T DspAllocate(float*&, int, void*)
//     DelayEffect.cpp.o      :  U DspAllocate(float*&, int, IXAudioBatchAllocator*)
// DelayEffect::mBuffer and FlangerEffect::mDelayBuffers[] are not in their
// constructors' initialiser lists, so the delay and flanger effects ran on an
// indeterminate pointer and ~DelayEffect passed it to delete[].  The native
// link's -Wl,--unresolved-symbols=ignore-all meant there was no diagnostic.
class IXAudioBatchAllocator;
void DspAllocate(float *&buf, int sizeSamps, IXAudioBatchAllocator *) {
    buf = new float[sizeSamps];
}
