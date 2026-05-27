// MOGG decode pipeline test
//
// Decodes shellmusic_loop_01.mogg through the full engine pipeline
// (VorbisReader → StandardStream → StreamReceiverNative) and writes
// WAV files for each channel. Then analyzes the output to verify it
// contains music (high autocorrelation) rather than static noise.
//
// Usage:
//   ./milo-tests --gtest_filter='MoggDecode.*'
//
// Output:
//   archive/sound/mogg_ch0.wav, mogg_ch1.wav, ...
//   archive/sound/mogg_reference.wav  (interleaved stereo from ch0+ch1)

#include "test_helpers.h"
#include <gtest/gtest.h>

#include "synth/Synth.h"
#include "synth/StandardStream.h"
#include "synth/VorbisReader.h"
#include "synth/StreamReceiver.h"
#include "platform/StreamReceiver_Native.h"
#include "audio/AudioDevice.h"
#include "os/File.h"
#include "os/Timer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <sys/stat.h>

extern File *NewFile(const char *, int);

// ============================================================================
// WAV writing helper
// ============================================================================

static bool WriteWAV(const char *path, const int16_t *samples, int numSamples,
                     int channels, int sampleRate) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    int dataSize = numSamples * channels * 2;
    int byteRate = sampleRate * channels * 2;

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + dataSize;
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtSize = 16;
    fwrite(&fmtSize, 4, 1, f);
    uint16_t audioFmt = 1; // PCM
    fwrite(&audioFmt, 2, 1, f);
    uint16_t ch = channels;
    fwrite(&ch, 2, 1, f);
    uint32_t sr = sampleRate;
    fwrite(&sr, 4, 1, f);
    uint32_t br = byteRate;
    fwrite(&br, 4, 1, f);
    uint16_t blockAlign = channels * 2;
    fwrite(&blockAlign, 2, 1, f);
    uint16_t bps = 16;
    fwrite(&bps, 2, 1, f);

    // data chunk
    fwrite("data", 1, 4, f);
    uint32_t ds = dataSize;
    fwrite(&ds, 4, 1, f);
    fwrite(samples, 2, numSamples * channels, f);

    fclose(f);
    return true;
}

// ============================================================================
// Audio signal analysis
// ============================================================================

struct AudioStats {
    float mean;
    float stdev;
    float meanAbs;
    int peak;
    int zeroCrossings;
    float autocorrelation;  // lag-1 autocorrelation
    float meanDiff;         // mean sample-to-sample difference
    int numSamples;

    bool isMusic() const {
        // Music: high autocorrelation, low sample-to-sample diff relative to stdev
        // Static: autocorrelation near 0, high diff
        return autocorrelation > 0.3f && meanDiff < stdev * 1.5f;
    }

    void print(const char *label) const {
        printf("  %s: %d samples, mean=%.1f, stdev=%.1f, peak=%d\n",
               label, numSamples, mean, stdev, peak);
        printf("    autocorr=%.4f, meanDiff=%.1f, zeroCrossings=%d\n",
               autocorrelation, meanDiff, zeroCrossings);
        printf("    verdict: %s\n", isMusic() ? "MUSIC" : "STATIC/NOISE");
    }
};

static AudioStats AnalyzeSamples(const int16_t *samples, int count) {
    AudioStats s = {};
    s.numSamples = count;
    if (count == 0) return s;

    // Mean
    double sum = 0;
    for (int i = 0; i < count; i++) sum += samples[i];
    s.mean = (float)(sum / count);

    // Stdev, peak, meanAbs
    double varSum = 0;
    double absSum = 0;
    s.peak = 0;
    for (int i = 0; i < count; i++) {
        double diff = samples[i] - s.mean;
        varSum += diff * diff;
        absSum += abs(samples[i]);
        if (abs(samples[i]) > s.peak) s.peak = abs(samples[i]);
    }
    s.stdev = (float)sqrt(varSum / count);
    s.meanAbs = (float)(absSum / count);

    // Zero crossings
    s.zeroCrossings = 0;
    for (int i = 1; i < count; i++) {
        if ((samples[i-1] >= 0) != (samples[i] >= 0))
            s.zeroCrossings++;
    }

    // Autocorrelation at lag 1
    int n = std::min(count, 8192);
    double var = 0;
    double meanN = 0;
    for (int i = 0; i < n; i++) meanN += samples[i];
    meanN /= n;
    for (int i = 0; i < n; i++) {
        double d = samples[i] - meanN;
        var += d * d;
    }
    var /= n;
    if (var > 0) {
        double cov = 0;
        for (int i = 0; i < n - 1; i++) {
            cov += (samples[i] - meanN) * (samples[i+1] - meanN);
        }
        s.autocorrelation = (float)(cov / ((n-1) * var));
    }

    // Mean sample-to-sample diff
    double diffSum = 0;
    for (int i = 1; i < count; i++) {
        diffSum += abs(samples[i] - samples[i-1]);
    }
    s.meanDiff = (float)(diffSum / (count - 1));

    return s;
}

// ============================================================================
// Test fixture
// ============================================================================

class MoggDecodeTest : public EngineTestFixture {
protected:
    void SetUp() override {
        if (!getenv("DC3_AUDIO_TESTS"))
            GTEST_SKIP() << "Set DC3_AUDIO_TESTS=1 to enable (audio device contention under ctest -j)";
        // Ensure StreamReceiver factory is set (SynthInit not called in test harness)
        if (!StreamReceiver::sFactory) {
            StreamReceiver::sFactory = StreamReceiverNative::Create;
        }
        // Init AudioDevice if needed (for sample rate)
        if (!AudioDevice::GetInstance().IsInitialized()) {
            AudioDevice::GetInstance().Init(44100);
        }
    }
};

TEST_F(MoggDecodeTest, DecodeShellMusic) {
    // Find the mogg file
    const char *moggPath = "sfx/samples/shell/shellmusic_loop_01.mogg";
    File *file = NewFile(moggPath, 2); // 2 = read mode
    ASSERT_NE(file, nullptr) << "Could not open " << moggPath;

    printf("  Opened: %s\n", moggPath);

    // Create a StandardStream directly (bypass MetaMusic/HamAudio)
    StandardStream stream(file, 0.0f, 0.0f, "mogg", false, true, false);

    // Poll until headers are parsed and stream is ready
    printf("  Polling for header parse...\n");
    int polls = 0;
    const int maxPolls = 2000;
    while (!stream.IsReady() && polls < maxPolls) {
        stream.PollStream();
        polls++;
    }
    printf("  Stream ready after %d polls, channels=%d, sampleRate=%d\n",
           polls, stream.GetNumChannels(), (int)stream.GetSampleRate());
    ASSERT_TRUE(stream.IsReady()) << "Stream never became ready after " << maxPolls << " polls";
    ASSERT_GT(stream.GetNumChannels(), 0);

    int numChannels = stream.GetNumChannels();
    int sampleRate = (int)stream.GetSampleRate();

    // Decode ~2 seconds of audio by continuing to poll
    int targetSamples = sampleRate * 2;
    printf("  Decoding %d samples (%d channels)...\n", targetSamples, numChannels);

    // Poll more to fill buffers
    for (int i = 0; i < 5000 && !stream.IsFinished(); i++) {
        stream.PollStream();
    }

    // Now drain the ring buffers from each channel's StreamReceiverNative
    std::vector<std::vector<int16_t>> channelData(numChannels);

    for (int ch = 0; ch < numChannels; ch++) {
        StreamReceiverNative *rcvr = static_cast<StreamReceiverNative *>(
            stream.GetChannel(ch));
        if (!rcvr) continue;

        // Render audio from the ring buffer
        rcvr->PlayImpl(); // must be playing to render
        int maxFrames = targetSamples;
        std::vector<float> floatBuf(maxFrames * 2); // stereo output
        int rendered = rcvr->RenderAudio(floatBuf.data(), maxFrames);

        // Convert back to int16 (mono — take left channel only since pan=0)
        channelData[ch].resize(rendered);
        for (int i = 0; i < rendered; i++) {
            float v = floatBuf[i * 2]; // left channel
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            channelData[ch][i] = (int16_t)(v * 32767.0f);
        }
        printf("  Channel %d: rendered %d samples\n", ch, rendered);
    }

    // Create output directory (relative to repo root, not build dir)
    const char *repoRoot = getenv("DC3_REPO_ROOT");
    std::string outDir = repoRoot ? std::string(repoRoot) + "/archive/sound"
                                  : "../../archive/sound";
    mkdir(outDir.c_str(), 0755);

    // Write per-channel WAVs
    for (int ch = 0; ch < numChannels; ch++) {
        if (channelData[ch].empty()) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/mogg_ch%d.wav", outDir.c_str(), ch);
        bool ok = WriteWAV(path, channelData[ch].data(),
                          (int)channelData[ch].size(), 1, sampleRate);
        if (!ok)
            printf("  WARNING: Failed to write %s (non-fatal)\n", path);
        else
            printf("  Wrote: %s (%d samples)\n", path, (int)channelData[ch].size());
    }

    // Write interleaved stereo from channels 0+1
    if (numChannels >= 2 && !channelData[0].empty() && !channelData[1].empty()) {
        int len = std::min(channelData[0].size(), channelData[1].size());
        std::vector<int16_t> stereo(len * 2);
        for (size_t i = 0; i < (size_t)len; i++) {
            stereo[i * 2 + 0] = channelData[0][i];
            stereo[i * 2 + 1] = channelData[1][i];
        }
        std::string refPath = outDir + "/mogg_reference.wav";
        bool ok = WriteWAV(refPath.c_str(), stereo.data(), len, 2, sampleRate);
        if (!ok)
            printf("  WARNING: Failed to write %s (non-fatal)\n", refPath.c_str());
        else
            printf("  Wrote: %s (%d frames stereo)\n", refPath.c_str(), len);
    }

    // Analyze each channel
    printf("\n  === Audio Analysis ===\n");
    bool anyMusic = false;
    for (int ch = 0; ch < numChannels; ch++) {
        if (channelData[ch].empty()) continue;
        char label[32];
        snprintf(label, sizeof(label), "Channel %d", ch);
        AudioStats stats = AnalyzeSamples(channelData[ch].data(),
                                          std::min((int)channelData[ch].size(), 44100));
        stats.print(label);
        if (stats.isMusic()) anyMusic = true;
    }

    EXPECT_TRUE(anyMusic) << "No channel contained music — all static/noise!";
}

// Test the full pipeline: decode → ring buffer → MixSources → output
TEST_F(MoggDecodeTest, FullPipelineIntegrity) {
    const char *moggPath = "sfx/samples/shell/shellmusic_loop_01.mogg";
    File *file = NewFile(moggPath, 2);
    ASSERT_NE(file, nullptr);

    StandardStream stream(file, 0.0f, 0.0f, "mogg", false, true, false);

    // Poll to ready + pre-fill (mimics StandardStream::Play)
    for (int i = 0; i < 500 && !stream.IsReady(); i++)
        stream.PollStream();
    ASSERT_TRUE(stream.IsReady());

    // Pre-fill ring buffers
    for (int i = 0; i < 50; i++)
        stream.PollStream();

    // Now call Play — this calls PlayImpl on each channel (registers with AudioDevice)
    stream.Play();

    // Simulate what MixSources does: call RenderAudio on each source
    // But do it on the main thread (no concurrency) to isolate the issue
    const int testFrames = 4096;
    std::vector<float> mixOutput(testFrames * 2, 0.0f);
    std::vector<float> srcBuf(testFrames * 2);

    AudioDevice &dev = AudioDevice::GetInstance();
    // Call MixSources directly (this is what the audio callback does)
    dev.MixSources(mixOutput.data(), testFrames);

    // Analyze the mix output
    std::vector<int16_t> mixSamples(testFrames);
    for (int i = 0; i < testFrames; i++) {
        // Take left channel
        float v = mixOutput[i * 2];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        mixSamples[i] = (int16_t)(v * 32767.0f);
    }

    AudioStats mixStats = AnalyzeSamples(mixSamples.data(), testFrames);
    mixStats.print("MixSources output");

    EXPECT_TRUE(mixStats.isMusic())
        << "MixSources output should be music, not static!\n"
        << "  If this fails, the corruption is in the RenderAudio → MixSources path";

    // Also poll more and mix again to check ongoing playback
    for (int i = 0; i < 50; i++)
        stream.PollStream();

    std::fill(mixOutput.begin(), mixOutput.end(), 0.0f);
    dev.MixSources(mixOutput.data(), testFrames);

    for (int i = 0; i < testFrames; i++) {
        float v = mixOutput[i * 2];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        mixSamples[i] = (int16_t)(v * 32767.0f);
    }
    AudioStats mix2Stats = AnalyzeSamples(mixSamples.data(), testFrames);
    mix2Stats.print("MixSources output (2nd batch)");

    stream.Stop();
}

// Test concurrent ring buffer access — simulates audio callback pulling
// while main thread writes
TEST_F(MoggDecodeTest, ConcurrentRingBuffer) {
    const char *moggPath = "sfx/samples/shell/shellmusic_loop_01.mogg";
    File *file = NewFile(moggPath, 2);
    ASSERT_NE(file, nullptr);

    StandardStream stream(file, 0.0f, 0.0f, "mogg", false, true, false);

    // Let Play() do its thing — it pumps VorbisReader, fills ring buffer,
    // then calls PlayImpl which registers with AudioDevice
    for (int i = 0; i < 500 && !stream.IsReady(); i++)
        stream.PollStream();
    ASSERT_TRUE(stream.IsReady());

    stream.Play();

    // Now continuously poll the stream (main thread) and simultaneously
    // pull audio via MixSources (simulated audio thread)
    // This mirrors the real app behavior.
    const int kIterations = 200;
    const int kFramesPerMix = 512;
    std::vector<float> mixBuf(kFramesPerMix * 2);
    std::vector<int16_t> capturedSamples;

    for (int iter = 0; iter < kIterations; iter++) {
        // Main thread: feed more decoded data
        stream.PollStream();

        // Audio thread (simulated on main thread): pull from ring buffer
        std::fill(mixBuf.begin(), mixBuf.end(), 0.0f);
        AudioDevice::GetInstance().MixSources(mixBuf.data(), kFramesPerMix);

        // Capture left channel samples
        for (int i = 0; i < kFramesPerMix; i++) {
            float v = mixBuf[i * 2];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            capturedSamples.push_back((int16_t)(v * 32767.0f));
        }
    }

    printf("  Captured %d samples across %d iterations\n",
           (int)capturedSamples.size(), kIterations);

    // Analyze the first second (skip first few frames which may be startup noise)
    int skipSamples = 2048;
    int analyzeSamples = std::min((int)capturedSamples.size() - skipSamples, 44100);
    if (analyzeSamples > 0) {
        AudioStats stats = AnalyzeSamples(
            capturedSamples.data() + skipSamples, analyzeSamples);
        stats.print("Concurrent ring buffer output");
        EXPECT_TRUE(stats.isMusic())
            << "Concurrent access should produce music, not static";
    }

    // Write the captured audio for listening
    const char *repoRoot = getenv("DC3_REPO_ROOT");
    std::string outDir = repoRoot ? std::string(repoRoot) + "/archive/sound"
                                  : "../../archive/sound";
    std::string outPath = outDir + "/mogg_concurrent.wav";
    WriteWAV(outPath.c_str(), capturedSamples.data(),
             (int)capturedSamples.size(), 1, 44100);
    printf("  Wrote: %s\n", outPath.c_str());

    stream.Stop();
}

// Compare the engine-decoded output against the full-pipeline WAV dump
// to determine where corruption enters
TEST_F(MoggDecodeTest, CompareWithWavDump) {
    // If archive/sound/shell_music_10s.wav exists (from DC3_DUMP_AUDIO),
    // compare its signal characteristics with the direct-decode output
    const char *repoRoot = getenv("DC3_REPO_ROOT");
    std::string refWavPath = repoRoot ? std::string(repoRoot) + "/archive/sound/shell_music_10s.wav"
                                      : "../../archive/sound/shell_music_10s.wav";
    FILE *ref = fopen(refWavPath.c_str(), "rb");
    if (!ref) {
        printf("  No reference WAV found at %s\n", refWavPath.c_str());
        GTEST_SKIP() << "No reference WAV";
    }

    // Read reference WAV samples (skip 44-byte header)
    fseek(ref, 0, SEEK_END);
    int fileSize = ftell(ref);
    fseek(ref, 44, SEEK_SET);
    int dataBytes = fileSize - 44;
    int refSamples = dataBytes / 2;
    std::vector<int16_t> refData(refSamples);
    fread(refData.data(), 2, refSamples, ref);
    fclose(ref);

    // Analyze a chunk from the middle (skip boot SFX)
    int offset = std::min(44100 * 4, refSamples - 8192); // skip first 4s
    if (offset < 0) offset = 0;
    int count = std::min(8192, refSamples - offset);

    printf("  Reference WAV: %d total samples\n", refSamples);
    AudioStats refStats = AnalyzeSamples(refData.data() + offset, count);
    refStats.print("WAV dump (post-pipeline)");

    // The WAV dump captures the FINAL output after all processing.
    // If it's static, the problem is in the decode or mixing pipeline.
    if (!refStats.isMusic()) {
        printf("  WARNING: Reference WAV is static — problem is in decode pipeline!\n");
    }
}
