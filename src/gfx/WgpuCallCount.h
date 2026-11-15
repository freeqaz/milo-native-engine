// The wrapped wgpu create counters (gfx/WgpuCallCount.cpp).
#pragma once

#include <cstdint>

// Every wgpuDeviceCreateBindGroup the linked program made, by any caller; 0
// where the build does not wrap the entry points (framecost::RawCallsCounted()).
uint64_t RawBindGroupCreateCount();

// Referenced by GpuDevice so the wrappers are always linked from the static
// engine library, whatever order the consumer's link line puts it in.
bool WgpuCallCountLinked();
