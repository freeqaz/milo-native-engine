// Override Emscripten's abort() to log instead of crash for missing function stubs.
// The engine has many Xbox/platform-specific functions that are not needed on web.
// This lets the engine continue running even when hitting unimplemented code paths.
var _originalAbort = null;

Module['onRuntimeInitialized'] = (function(prev) {
    return function() {
        if (prev) prev.call(this);
        // Patch abort to be lenient about missing functions
        if (typeof abort === 'function' && !_originalAbort) {
            _originalAbort = abort;
            abort = function(what) {
                if (typeof what === 'string' && what.startsWith('missing function:')) {
                    var funcName = what.substring('missing function: '.length);
                    if (!abort._warned) abort._warned = {};
                    if (!abort._warned[funcName]) {
                        console.warn('[stub] ' + funcName);
                        abort._warned[funcName] = true;
                    }
                    return 0;  // Return 0 instead of crashing
                }
                // For non-missing-function aborts, still crash
                return _originalAbort(what);
            };
        }
    };
})(Module['onRuntimeInitialized']);
