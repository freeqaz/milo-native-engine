// DC3 Web Port - WebMovieImpl
// Uses browser's native <video> element for hardware-accelerated video decoding.
// Pre-transcoded videos (BINK → WebM) are served from the web server.
//
// Flow: BeginFromFile → JS creates <video> → Poll checks for new frames →
//       Draw extracts RGBA pixels via canvas → UploadRGBAToRndTex
//
// The .bik path is rewritten to .webm for the browser-native decoder.

#ifdef __EMSCRIPTEN__

#include "platform/WebMovieImpl.h"
#include "os/Debug.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <cstring>

// ---- JavaScript interop for <video> element management ----

// Create a <video> element and start loading. Returns a handle ID.
EM_JS(int, web_movie_create, (const char* url, int loop), {
    if (!Module._webMovies) Module._webMovies = {};
    if (!Module._webMovieNextId) Module._webMovieNextId = 1;

    var id = Module._webMovieNextId++;
    var video = document.createElement('video');
    // Don't set crossOrigin — videos are same-origin (/api/file/...) and
    // setting 'anonymous' forces CORS mode, which taints the canvas for
    // getImageData() pixel readback unless the server sends ACAO headers.
    video.playsInline = true;
    video.muted = false;  // Will be set by SetVolume
    video.loop = !!loop;
    video.preload = 'auto';
    // Prepend /api/file/ so the browser fetches from the asset server
    var rawUrl = UTF8ToString(url);
    video.src = rawUrl.startsWith('/') ? '/api/file' + rawUrl : '/api/file/' + rawUrl;

    // Canvas for pixel readback
    var canvas = document.createElement('canvas');
    var ctx = canvas.getContext('2d');

    Module._webMovies[id] = {
        video: video,
        canvas: canvas,
        ctx: ctx,
        hasNewFrame: false,
        ready: false,
        ended: false,
        width: 0,
        height: 0
    };

    video.addEventListener('loadedmetadata', function() {
        var m = Module._webMovies[id];
        m.width = video.videoWidth;
        m.height = video.videoHeight;
        m.canvas.width = video.videoWidth;
        m.canvas.height = video.videoHeight;
        m.ready = true;
        console.log('WebMovie[' + id + ']: loaded ' + video.videoWidth + 'x' + video.videoHeight);
    });

    video.addEventListener('ended', function() {
        Module._webMovies[id].ended = true;
    });

    video.addEventListener('error', function(e) {
        console.warn('WebMovie[' + id + ']: error loading video', e);
        Module._webMovies[id].ended = true;
    });

    // Use requestVideoFrameCallback for frame-accurate updates
    if ('requestVideoFrameCallback' in video) {
        function onFrame() {
            if (Module._webMovies[id]) {
                Module._webMovies[id].hasNewFrame = true;
                video.requestVideoFrameCallback(onFrame);
            }
        }
        video.requestVideoFrameCallback(onFrame);
    }

    // Start playback (may need user gesture on some browsers)
    video.play().catch(function(e) {
        console.warn('WebMovie[' + id + ']: autoplay blocked, trying muted', e);
        video.muted = true;
        video.play().catch(function(e2) {
            console.warn('WebMovie[' + id + ']: muted autoplay also blocked', e2);
        });
    });

    return id;
});

// Check if video is ready (metadata loaded)
EM_JS(int, web_movie_is_ready, (int id), {
    var m = Module._webMovies && Module._webMovies[id];
    return m ? (m.ready ? 1 : 0) : 0;
});

// Check if a new frame is available
EM_JS(int, web_movie_has_new_frame, (int id), {
    var m = Module._webMovies && Module._webMovies[id];
    if (!m || !m.ready) return 0;

    // If requestVideoFrameCallback isn't supported, fall back to time-based check
    if (!('requestVideoFrameCallback' in m.video)) {
        return (!m.video.paused && !m.video.ended && m.video.currentTime > 0) ? 1 : 0;
    }
    return m.hasNewFrame ? 1 : 0;
});

// Get video width
EM_JS(int, web_movie_width, (int id), {
    var m = Module._webMovies && Module._webMovies[id];
    return m ? m.width : 0;
});

// Get video height
EM_JS(int, web_movie_height, (int id), {
    var m = Module._webMovies && Module._webMovies[id];
    return m ? m.height : 0;
});

// Extract current frame as RGBA pixels into the provided buffer.
// Returns 1 on success, 0 if no frame available.
EM_JS(int, web_movie_read_frame, (int id, uint8_t* outBuf, int bufSize), {
    var m = Module._webMovies && Module._webMovies[id];
    if (!m || !m.ready) return 0;

    try {
        m.ctx.drawImage(m.video, 0, 0);
        var imageData = m.ctx.getImageData(0, 0, m.width, m.height);
        var len = Math.min(imageData.data.length, bufSize);
        HEAPU8.set(imageData.data.subarray(0, len), outBuf);
        m.hasNewFrame = false;
        return 1;
    } catch(e) {
        // CORS or other errors
        return 0;
    }
});

// Check if video ended
EM_JS(int, web_movie_ended, (int id), {
    var m = Module._webMovies && Module._webMovies[id];
    return (m && m.ended) ? 1 : 0;
});

// Set volume (dB scale: 0 = full, negative = quieter, <= -96 = silence)
// HTML <video>.volume expects linear 0.0-1.0, so convert from dB.
EM_JS(void, web_movie_set_volume, (int id, float volDb), {
    var m = Module._webMovies && Module._webMovies[id];
    if (m) {
        // Convert dB to linear: 10^(dB/20), clamped to [0, 1]
        var linear;
        if (volDb <= -96.0) {
            linear = 0.0;
        } else if (volDb >= 0.0) {
            linear = 1.0;
        } else {
            linear = Math.pow(10.0, volDb / 20.0);
        }
        m.video.volume = Math.min(1.0, Math.max(0.0, linear));
        m.video.muted = (linear <= 0);
    }
});

// Pause/unpause
EM_JS(void, web_movie_set_paused, (int id, int paused), {
    var m = Module._webMovies && Module._webMovies[id];
    if (!m) return;
    if (paused) {
        m.video.pause();
    } else {
        m.video.play().catch(function(){});
    }
});

// Get current frame number (approximation based on currentTime * fps)
EM_JS(int, web_movie_current_frame, (int id, float fps), {
    var m = Module._webMovies && Module._webMovies[id];
    if (!m) return 0;
    return Math.floor(m.video.currentTime * fps);
});

// Get duration in seconds
EM_JS(float, web_movie_duration, (int id), {
    var m = Module._webMovies && Module._webMovies[id];
    return (m && m.video.duration && isFinite(m.video.duration)) ? m.video.duration : 0;
});

// Show/hide the video element as a fullscreen overlay on the canvas.
// Used by MoviePanel for intro/attract videos that render directly.
EM_JS(void, web_movie_set_overlay, (int id, int show), {
    var m = Module._webMovies && Module._webMovies[id];
    if (!m) return;
    var container = document.getElementById('canvas-container');
    if (show) {
        m.video.style.cssText = 'position:absolute;top:0;left:0;width:100%;height:100%;' +
            'object-fit:contain;z-index:2147483647;background:#000;transform:translateZ(0);';
        if (!m.video.parentNode) container.appendChild(m.video);
    } else {
        if (m.video.parentNode) m.video.parentNode.removeChild(m.video);
    }
});

// Destroy the video element and free resources
EM_JS(void, web_movie_destroy, (int id), {
    var m = Module._webMovies && Module._webMovies[id];
    if (m) {
        m.video.pause();
        m.video.removeAttribute('src');
        m.video.load();
        delete Module._webMovies[id];
    }
});

// ---- WebMovieImpl implementation ----

WebMovieImpl::WebMovieImpl()
    : mVideoHandle(0),
      mVideoWidth(0), mVideoHeight(0),
      mOpen(false), mLoop(false), mPaused(false), mReady(false),
      mCurrentFrame(0), mNumFrames(0), mFrameRate(30.0f),
      mDisplayWidth(0), mDisplayHeight(0), mFrameDecoded(false) {}

WebMovieImpl::~WebMovieImpl() {
    End();
}

void WebMovieImpl::SetWidthHeight(int w, int h) {
    mDisplayWidth = w;
    mDisplayHeight = h;
}

bool WebMovieImpl::Ready() const {
    return mReady;
}

bool WebMovieImpl::BeginFromFile(
    char const *path, float volume, bool loop, bool /*unk1*/,
    bool /*unk2*/, bool /*unk3*/, int /*unk4*/, BinStream * /*bs*/, LoaderPos /*pos*/
) {
    End();

    if (!path || !path[0]) return false;

    mFilename = path;
    mLoop = loop;

    // Rewrite .bik path to .webm for browser-native decoding
    // e.g., "/videos/intro.bik" → "/videos/intro.webm"
    String webPath(path);
    const char *ext = strrchr(webPath.c_str(), '.');
    if (ext) {
        int extPos = ext - webPath.c_str();
        String newPath;
        // Copy everything before the extension
        for (int i = 0; i < extPos; i++) {
            newPath += webPath.c_str()[i];
        }
        newPath += ".webm";
        webPath = newPath;
    }

    printf("WebMovieImpl: opening %s (from %s)\n", webPath.c_str(), path);

    mVideoHandle = web_movie_create(webPath.c_str(), loop ? 1 : 0);
    if (mVideoHandle <= 0) {
        MILO_WARN("WebMovieImpl: failed to create video element for %s", webPath.c_str());
        return false;
    }

    mOpen = true;
    mPaused = false;
    mCurrentFrame = 0;
    mFrameRate = 30.0f; // Default, updated when metadata loads

    if (volume >= 0.0f) {
        web_movie_set_volume(mVideoHandle, volume);
    }

    return true;
}

bool WebMovieImpl::Poll() {
    // Convention: return true = still playing, false = done/ended
    // (TexMovie::Poll checks `if (!mMovie.Poll()) mMovie.End()`)
    if (!mOpen || mPaused || mVideoHandle <= 0) return true;

    // Check if metadata is loaded
    if (!mReady && web_movie_is_ready(mVideoHandle)) {
        mReady = true;
        mVideoWidth = web_movie_width(mVideoHandle);
        mVideoHeight = web_movie_height(mVideoHandle);
        mRGBABuffer.resize(mVideoWidth * mVideoHeight * 4);

        float duration = web_movie_duration(mVideoHandle);
        if (duration > 0 && mFrameRate > 0) {
            mNumFrames = (int)(duration * mFrameRate);
        }
    }

    if (!mReady) return true;

    // Check for new frame
    if (web_movie_has_new_frame(mVideoHandle)) {
        // Read the frame pixels
        int bufSize = mVideoWidth * mVideoHeight * 4;
        if (web_movie_read_frame(mVideoHandle, mRGBABuffer.data(), bufSize)) {
            mFrameDecoded = true;
            mCurrentFrame = web_movie_current_frame(mVideoHandle, mFrameRate);
        }
    }

    // Check if ended
    if (web_movie_ended(mVideoHandle)) {
        if (!mLoop) {
            return false; // Video ended
        }
    }

    return true; // Still playing
}

void WebMovieImpl::Draw() {
    if (!mFrameDecoded) return;
    // Pixel data is available in mRGBABuffer for upload.
    // TexMovie::DrawToTexture will call UploadRGBAToRndTex with this data.
    mFrameDecoded = false;
}

void WebMovieImpl::SetOverlay(bool show) {
    if (mVideoHandle > 0) {
        web_movie_set_overlay(mVideoHandle, show ? 1 : 0);
    }
}

void WebMovieImpl::End() {
    if (mVideoHandle > 0) {
        web_movie_set_overlay(mVideoHandle, 0); // hide overlay before destroy
        web_movie_destroy(mVideoHandle);
        mVideoHandle = 0;
    }
    mRGBABuffer.clear();
    mOpen = false;
    mReady = false;
    mFrameDecoded = false;
    mCurrentFrame = 0;
}

bool WebMovieImpl::CheckOpen(bool) {
    return mOpen;
}

bool WebMovieImpl::SetPaused(bool paused) {
    mPaused = paused;
    if (mVideoHandle > 0) {
        web_movie_set_paused(mVideoHandle, paused ? 1 : 0);
    }
    return true;
}

void WebMovieImpl::SetVolume(float vol) {
    if (mVideoHandle > 0) {
        web_movie_set_volume(mVideoHandle, vol);
    }
}

float WebMovieImpl::MsPerFrame() const {
    if (mFrameRate > 0.0f)
        return 1000.0f / mFrameRate;
    return 33.33f; // default ~30fps
}

void WebMovieImpl::Terminate() {
    End();
}

#endif // __EMSCRIPTEN__
