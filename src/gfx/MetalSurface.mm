// MetalSurface.mm — Objective-C++ helper for creating a CAMetalLayer
// from an NSWindow. Required because CAMetalLayer is an ObjC class
// that can't be created from pure C++.

#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

extern "C" void* CreateMetalLayerForWindow(void* nsWindow) {
    NSWindow* window = (__bridge NSWindow*)nsWindow;
    CAMetalLayer* layer = [CAMetalLayer layer];
    // Dawn is configured in framebuffer pixels, including the Retina scale.
    layer.contentsScale = window.backingScaleFactor;
    layer.drawableSize = [window.contentView convertRectToBacking:window.contentView.bounds].size;
    [window.contentView setWantsLayer:YES];
    [window.contentView setLayer:layer];
    return (__bridge void*)layer;
}

extern "C" void UpdateMetalLayerScaleForWindow(void* nsWindow) {
    NSWindow* window = (__bridge NSWindow*)nsWindow;
    CAMetalLayer* layer = (CAMetalLayer*)window.contentView.layer;
    if ([layer isKindOfClass:[CAMetalLayer class]] &&
        layer.contentsScale != window.backingScaleFactor) {
        layer.contentsScale = window.backingScaleFactor;
    }
}
