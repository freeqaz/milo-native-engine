// MetalSurface.mm — Objective-C++ helper for creating a CAMetalLayer
// from an NSWindow. Required because CAMetalLayer is an ObjC class
// that can't be created from pure C++.

#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

extern "C" void* CreateMetalLayerForWindow(void* nsWindow) {
    NSWindow* window = (__bridge NSWindow*)nsWindow;
    CAMetalLayer* layer = [CAMetalLayer layer];
    // Leave contentsScale at 1.0 — Dawn's surface configuration uses window
    // point dimensions (e.g. 1280x720), and macOS composites the result to
    // fill the Retina framebuffer. Setting contentsScale=2 would create a
    // 2x drawable that mismatches Dawn's configured surface size.
    [window.contentView setWantsLayer:YES];
    [window.contentView setLayer:layer];
    return (__bridge void*)layer;
}
