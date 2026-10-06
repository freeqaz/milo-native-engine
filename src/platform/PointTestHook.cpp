// PointTestHook — engine-side storage for the point-test seam. See
// PointTestHook.h.

#include "platform/PointTestHook.h"

namespace {
NativePointTester* gPointTester = nullptr;
NativePointTestResultFn gPointTestResultFn = nullptr;
}

void SetNativePointTester(NativePointTester* tester) { gPointTester = tester; }
NativePointTester* GetNativePointTester() { return gPointTester; }

void SetNativePointTestResultFn(NativePointTestResultFn fn) { gPointTestResultFn = fn; }
NativePointTestResultFn GetNativePointTestResultFn() { return gPointTestResultFn; }
