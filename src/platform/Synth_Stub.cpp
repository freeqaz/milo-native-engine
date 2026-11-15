// DC3 Native Port - NativeSynth
// Platform-specific Synth subclass for Linux/macOS/Windows.
// Replaces system/synth_xbox/Synth.cpp (Synth360).
//
// Responsibilities:
// - Register StreamReceiverNative factory for audio output
// - Initialize/terminate AudioDevice (miniaudio)
// - Create stream decoders for audio formats (Vorbis, FFmpeg/Bink)

#include "synth/Synth.h"
#include "synth/FxSend.h"
#include "math/Decibels.h"
#include "synth/StandardStream.h"
#include "os/System.h"
#include "synth/StreamReceiver.h"
#include "synth/StreamNull.h"
#include "synth/VorbisReader.h"
#include "os/File.h"
#include "os/BufFile.h"
#include "utl/Symbol.h"
#include "audio/AudioDevice.h"
#include "platform/StreamReceiver_Native.h"

#ifdef HX_FFMPEG
#include "platform/FFmpegAudioReader.h"
#endif

extern File *NewFile(const char *, int);

class NativeSynth : public Synth {
public:
    // Synth360::PreInit's meter buses, by the same names and in the order its
    // MeterEffect writes them; 7 and 8 are the stereo downmix Poll() derives,
    // 6 is unused. HamMaster::CheckLevels reads the last two.
    virtual void PreInit() override {
        const char *busNames[9] = { "  FL", "  FR", "   C", " LFE", "  SL",
                                    "  SR", "",     " DML", " DMR" };
        for (int i = 0; i < 9; i++) {
            mLevelData.push_back(LevelData(busNames[i]));
        }
        AudioDevice::GetInstance().MasterMeter().SetEnabled(true);
    }

    // Synth360::Poll: hand the meter its parameters (which resets its
    // running sums), then derive the downmix pair from the buses it wrote.
    virtual void Poll() override {
        UpdateMasterLevels();
        Synth::Poll();
    }

    void UpdateMasterLevels() {
        if (mLevelData.size() < 9) return;
        float rms[6], peak[6];
        for (int c = 0; c < 6; c++) {
            rms[c] = mLevelData[c].mRMS;
            peak[c] = mLevelData[c].mPeak;
        }
        AudioDevice &dev = AudioDevice::GetInstance();
        dev.MasterMeter().Drain(dev.MasterMeterHeardFrame(), [&](const MeterQuantum &q) {
            mMeterState.Apply(q, rms, peak);
        });
        mMeterState.Reset();
        for (int c = 0; c < 6; c++) {
            mLevelData[c].mRMS = rms[c];
            mLevelData[c].mPeak = peak[c];
        }
        static float gainCenter = DbToRatio(-3.0f);
        static float gainSide = DbToRatio(-1.2f);
        static float gainRear = DbToRatio(-6.2f);
        std::vector<LevelData> &l = mLevelData;
        l[7].mRMS = l[2].mRMS * gainCenter + (l[5].mRMS * gainRear + l[4].mRMS * gainSide) + l[0].mRMS;
        l[7].mPeak = l[2].mPeak * gainCenter + (l[5].mPeak * gainRear + l[4].mPeak * gainSide) + l[0].mPeak;
        l[8].mRMS = l[2].mRMS * gainCenter + (l[5].mRMS * gainSide + l[4].mRMS * gainRear) + l[1].mRMS;
        l[8].mPeak = l[2].mPeak * gainCenter + (l[5].mPeak * gainSide + l[4].mPeak * gainRear) + l[1].mPeak;
    }

    virtual void Init() override {
        Synth::Init();

        // Register native StreamReceiver factory (like StreamReceiver360::Init())
        StreamReceiver::sFactory = StreamReceiverNative::Create;

        // Initialize audio output device
        AudioDevice::GetInstance().Init(44100);

        // Read audio latency offset from config (synth { audio_offset_ms <float> })
        DataArray *synthCfg = SystemConfig("synth");
        float offsetMs = 0.0f;
        if (synthCfg->FindData("audio_offset_ms", offsetMs, false)) {
            StandardStream::sAudioOffsetMs = offsetMs;
        }
    }

    virtual void Terminate() override {
        AudioDevice::GetInstance().Terminate();
        Synth::Terminate();
    }

    virtual StreamReader *NewStreamDecoder(File *file, StandardStream *stream, Symbol type) override {
#ifdef HX_FFMPEG
        // "bink" symbol = Bink audio container (.bik files used for song previews)
        if (type == "bink") {
            return new FFmpegAudioReader(file, stream);
        }
#endif
        // Vorbis/OGG/MOGG — the primary song audio format
        if (type == "ogg" || type == "mogg") {
            bool expectMap = (type == "mogg"); // .mogg has HMX header with OggMap + encryption
            return new VorbisReader(file, expectMap, stream, false);
        }
        return nullptr;
    }

    virtual void NewStreamFile(const char *path, File *&file, Symbol &sym) override {
        // Determine codec type from file extension
        const char *ext = strrchr(path, '.');
        if (ext) {
            if (strcmp(ext, ".bik") == 0) {
                sym = "bink";
            } else if (strcmp(ext, ".mogg") == 0) {
                sym = "mogg";
            } else if (strcmp(ext, ".ogg") == 0) {
                sym = "ogg";
            } else {
                sym = "ogg"; // default to ogg
            }
            file = NewFile(path, 2);
        } else {
            // No extension — try .mogg first (HMX encrypted), then .ogg
            char buf[256];
            snprintf(buf, sizeof(buf), "%s.mogg", path);
            file = NewFile(buf, 2);
            if (file) {
                sym = "mogg";
            } else {
                snprintf(buf, sizeof(buf), "%s.ogg", path);
                file = NewFile(buf, 2);
                sym = "ogg";
            }
        }
    }

    virtual Stream *NewStream(const char *path, float vol, float pan, bool loop) override {
        File *file = nullptr;
        Symbol sym;
        NewStreamFile(path, file, sym);
        if (!file) {
            return new StreamNull(vol);
        }
        return new StandardStream(file, vol, pan, sym, loop, true, false);
    }

    virtual Stream *NewBufStream(const void *buf, int size, Symbol sym, float vol, bool loop) override {
        if (!buf || size <= 0) {
            return new StreamNull(vol);
        }
        // HamAudio passes sym="main" (mixer bus name), not a codec type.
        // Detect mogg vs ogg from header:
        //   OGG: starts with "OggS" (0x4F 0x67 0x67 0x53)
        //   MOGG: starts with version byte (0x0A-0x0F for Harmonix mogg variants)
        Symbol codecType = sym;
        if (sym != "ogg" && sym != "mogg" && sym != "bink" && size >= 4) {
            const unsigned char *hdr = (const unsigned char *)buf;
            if (hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S') {
                codecType = "ogg";
            } else {
                codecType = "mogg";
            }
        }
        File *file = new BufFile(buf, size);
        return new StandardStream(file, vol, 0.0f, codecType, loop, true, false);
    }

private:
    MeterEffectState mMeterState; // the master MeterEffect's running sums
};

// Called from SynthPreInit() via Synth::New()
// This replaces the Xbox360's "new Synth360()" path
Synth *CreateNativeSynth() {
    return new NativeSynth();
}
