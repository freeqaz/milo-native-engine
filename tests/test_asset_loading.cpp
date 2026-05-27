// Asset loading tests — verify that all game asset types can be loaded
// without crashes, stream desync, or assertion failures.
//
// Archive-backed assets require DC3_DATA pointing at extracted ark files.
// Standalone .milo_xbox assets use the pre-extracted library at MILO_LIB.
//
// Run the bulk loading sweep (tests every .milo_xbox in the library):
//   cd native/build && ctest -R BulkLoad --output-on-failure
//   MILO_BULK_CATEGORY=ui ctest -R BulkLoad --output-on-failure

#include "test_helpers.h"
#include "char/CharClip.h"
#include "char/CharServoBone.h"
#include "char/FileMerger.h"
#include "char/CharUtl.h"
#include "gfx/VertexFormats.h"
#include "platform/TransformUtils.h"
#include "char/CharTwistSolver.h"
#include "gesture/SkeletonViz.h"
#include "hamobj/Difficulty.h"
#include "hamobj/HamCharacter.h"
#include "hamobj/MoveGraph.h"
#include "math/Rot.h"
#include "obj/Dir.h"
#include "obj/DirLoader.h"
#include "obj/Object.h"
#include "os/File.h"
#include "rndobj/PropAnim.h"
#include "rndobj/PropKeys.h"
#include "utl/FilePath.h"

#include <sys/stat.h>
#include <dirent.h>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

static std::string GetMiloLibRoot() {
    const char *env = getenv("MILO_LIB");
    if (env && env[0])
        return env;
    const char *home = getenv("HOME");
    if (home && home[0])
        return std::string(home)
            + "/code/milohax/milo-engine-libs/harmonix-repos/milo-rnd-library/dc3";
    return "";
}

static bool FileExists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static ObjectDir *LoadMainCharacterFromLibrary() {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        return nullptr;

    std::string path = root + "/char/main/gen/main.milo_xbox";
    if (!FileExists(path))
        return nullptr;

    return DirLoader::LoadObjects(FilePath(path.c_str()), nullptr, nullptr);
}

static HamCharacter *FindMainCharacter(ObjectDir *dir) {
    if (!dir)
        return nullptr;

    HamCharacter *character = dynamic_cast<HamCharacter *>(dir);
    if (character)
        return character;

    ObjDirItr<HamCharacter> it(dir, true);
    return it ? it : nullptr;
}

static FileMerger *FindCharacterFileMerger(HamCharacter *character) {
    return character ? character->Find<FileMerger>("char.fm", false) : nullptr;
}

static ObjectDir *LoadCrowdClipLibrary() {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        return nullptr;

    std::string path = root + "/char/crowd/anim/gen/female_base.milo_xbox";
    if (!FileExists(path))
        return nullptr;

    return DirLoader::LoadObjects(FilePath(path.c_str()), nullptr, nullptr);
}

static CharClip *FindClipByName(ObjectDir *dir, const char *name) {
    if (!dir || !name)
        return nullptr;
    for (ObjDirItr<CharClip> it(dir, true); it != nullptr; ++it) {
        if (strcmp(it->Name(), name) == 0)
            return it;
    }
    return nullptr;
}

static int CountSkinnedMeshes(ObjectDir *dir) {
    int count = 0;
    for (ObjDirItr<RndMesh> it(dir, true); it != nullptr; ++it) {
        if (it->NumBones() > 0)
            count++;
    }
    return count;
}

// Try to load an archive-backed milo path. Returns dir or nullptr.
static ObjectDir *TryLoadArchive(const char *path) {
    FilePath fp(path);
    return DirLoader::LoadObjects(fp, nullptr, nullptr);
}

struct ArmPollableInventory {
    std::map<std::string, std::vector<std::string> > byClass;
};

static bool IsArmPollableClass(const char *className) {
    static const char *kClasses[] = {
        "CharForeTwist",
        "CharUpperTwist",
        "CharIKHand",
        "CharIKFingers",
        "CharBlendBone",
        "CharBoneTwist",
        "CharBoneOffset",
        "CharPosConstraint",
        "CharSleeve",
        nullptr
    };

    for (int i = 0; kClasses[i]; i++) {
        if (strcmp(className, kClasses[i]) == 0)
            return true;
    }
    return false;
}

static ArmPollableInventory CollectArmPollableInventory(ObjectDir *dir) {
    ArmPollableInventory inv;
    for (ObjDirItr<Hmx::Object> it(dir, true); it != nullptr; ++it) {
        const char *className = it->ClassName().Str();
        if (!className || !IsArmPollableClass(className))
            continue;
        inv.byClass[className].push_back(it->Name());
    }

    for (std::map<std::string, std::vector<std::string> >::iterator it = inv.byClass.begin();
         it != inv.byClass.end();
         ++it) {
        std::sort(it->second.begin(), it->second.end());
    }
    return inv;
}

// ============================================================================
// Fixture: full engine init
// ============================================================================

class AssetLoadingTest : public EngineTestFixture {};

// ============================================================================
// Archive-backed asset loading — these use paths resolved through TheArchive
// ============================================================================

// Core character resources — small, should always work
static const char *kCharResources[] = {
    "char/shared/main_resource.milo",
    "char/shared/viseme_resource.milo",
    "char/shared/skeleton_bones_resource.milo",
    nullptr
};

TEST_F(AssetLoadingTest, LoadCharResources) {
    int loaded = 0, skipped = 0;
    for (int i = 0; kCharResources[i]; i++) {
        ObjectDir *dir = TryLoadArchive(kCharResources[i]);
        if (dir) {
            printf("  OK: %s -> '%s' class='%s'\n",
                   kCharResources[i], dir->Name(), dir->ClassName().Str());
            EXPECT_NE(strlen(dir->Name()), 0u);
            loaded++;
        } else {
            skipped++;
        }
    }
    if (loaded == 0) {
        GTEST_SKIP() << "No archive assets available (set DC3_DATA)";
    }
    printf("CharResources: loaded=%d skipped=%d\n", loaded, skipped);
}

// UI panel dirs — complex hierarchies with many object types
static const char *kUIAssets[] = {
    "ui/gen/cheat.milo_xbox",
    "ui/gen/common.milo_xbox",
    "ui/gen/locale.milo_xbox",
    "ui/gen/panel_select.milo_xbox",
    "ui/resource/fonts/gen/default.milo_xbox",
    "ui/resource/lists/gen/default.milo_xbox",
    nullptr
};

TEST_F(AssetLoadingTest, LoadUIAssets) {
    int loaded = 0, skipped = 0;
    for (int i = 0; kUIAssets[i]; i++) {
        ObjectDir *dir = TryLoadArchive(kUIAssets[i]);
        if (dir) {
            // Count objects
            int count = 0;
            for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it)
                count++;
            printf("  OK: %s -> '%s' class='%s' objects=%d\n",
                   kUIAssets[i], dir->Name(), dir->ClassName().Str(), count);
            EXPECT_GT(count, 0) << "Dir should have objects: " << kUIAssets[i];
            loaded++;
        } else {
            skipped++;
        }
    }
    if (loaded == 0)
        GTEST_SKIP() << "No archive UI assets available";
    printf("UIAssets: loaded=%d skipped=%d\n", loaded, skipped);
}

// SFX dirs — ObjectDir containers for sound objects
static const char *kSFXAssets[] = {
    "sfx/gen/common_bank.milo_xbox",
    "sfx/gen/shell_fx.milo_xbox",
    "sfx/gen/ingame_bank.milo_xbox",
    nullptr
};

TEST_F(AssetLoadingTest, LoadSFXAssets) {
    int loaded = 0, skipped = 0;
    for (int i = 0; kSFXAssets[i]; i++) {
        ObjectDir *dir = TryLoadArchive(kSFXAssets[i]);
        if (dir) {
            printf("  OK: %s -> '%s' class='%s'\n",
                   kSFXAssets[i], dir->Name(), dir->ClassName().Str());
            loaded++;
        } else {
            skipped++;
        }
    }
    if (loaded == 0)
        GTEST_SKIP() << "No archive SFX assets available";
    printf("SFXAssets: loaded=%d skipped=%d\n", loaded, skipped);
}

// Flow dirs — game logic containers
static const char *kFlowAssets[] = {
    "flow/gen/crowd_audio_proxy.milo_xbox",
    "flow/gen/nav_player.milo_xbox",
    "flow/gen/spawner.milo_xbox",
    nullptr
};

TEST_F(AssetLoadingTest, LoadFlowAssets) {
    int loaded = 0, skipped = 0;
    for (int i = 0; kFlowAssets[i]; i++) {
        ObjectDir *dir = TryLoadArchive(kFlowAssets[i]);
        if (dir) {
            printf("  OK: %s -> '%s' class='%s'\n",
                   kFlowAssets[i], dir->Name(), dir->ClassName().Str());
            loaded++;
        } else {
            skipped++;
        }
    }
    if (loaded == 0)
        GTEST_SKIP() << "No archive flow assets available";
    printf("FlowAssets: loaded=%d skipped=%d\n", loaded, skipped);
}

// World dirs — venues with meshes, lights, cameras
// NOTE: world/gen/world.milo_xbox crashes due to nested subdir type mismatch
// (iconmandir interpreted as RndDir with mRev 32 > INIT_REVS 10).
// Use specific venue files that are known to load correctly.
static const char *kWorldAssets[] = {
    "world/default/gen/default.milo_xbox",
    "world/shared/camshots/gen/angel.milo_xbox",
    nullptr
};

TEST_F(AssetLoadingTest, LoadWorldAssets) {
    int loaded = 0, skipped = 0;
    for (int i = 0; kWorldAssets[i]; i++) {
        ObjectDir *dir = TryLoadArchive(kWorldAssets[i]);
        if (dir) {
            int count = 0;
            for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it)
                count++;
            printf("  OK: %s -> '%s' class='%s' objects=%d\n",
                   kWorldAssets[i], dir->Name(), dir->ClassName().Str(), count);
            loaded++;
        } else {
            skipped++;
        }
    }
    if (loaded == 0)
        GTEST_SKIP() << "No archive world assets available";
    printf("WorldAssets: loaded=%d skipped=%d\n", loaded, skipped);
}

TEST_F(AssetLoadingTest, LoadSystemRunHamSkeletonResource) {
    FilePath fp(FileSystemRoot(), "ham/skeleton.milo");
    ObjectDir *dir = DirLoader::LoadObjects(fp, nullptr, nullptr);
    ASSERT_NE(dir, nullptr) << "system-run ham/skeleton.milo failed to load from "
                            << FileSystemRoot();

    int count = 0;
    for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it)
        count++;
    EXPECT_GT(count, 0) << "skeleton resource dir should contain objects";
    printf("  OK: %s -> '%s' class='%s' objects=%d\n",
           fp.c_str(), dir->Name(), dir->ClassName().Str(), count);
}

TEST_F(AssetLoadingTest, SkeletonVizInitLoadsSystemResource) {
    SkeletonViz *viz = Hmx::Object::New<SkeletonViz>();
    ASSERT_NE(viz, nullptr);
    viz->Init();
    delete viz;
}

// ============================================================================
// Standalone .milo_xbox loading — uses pre-extracted library at MILO_LIB
// ============================================================================

struct StandaloneMiloEntry {
    const char *relPath;
    const char *category;
};

static const StandaloneMiloEntry kStandaloneMiloFiles[] = {
    // Flow
    {"flow/gen/crowd_audio_proxy.milo_xbox", "flow"},
    {"flow/gen/nav_player.milo_xbox", "flow"},
    // UI (small files)
    {"ui/resource/fonts/gen/default.milo_xbox", "ui-font"},
    {"ui/resource/lists/gen/default.milo_xbox", "ui-list"},
    // SFX
    {"sfx/gen/shell_fx.milo_xbox", "sfx"},
    // World (camshots are small, safe)
    {"world/shared/camshots/gen/angel.milo_xbox", "world-camshot"},
    {nullptr, nullptr}
};

TEST_F(AssetLoadingTest, LoadStandaloneMiloFiles) {
    std::string root = GetMiloLibRoot();
    if (root.empty()) {
        GTEST_SKIP() << "MILO_LIB not set and default path not found";
    }

    int loaded = 0, skipped = 0;
    for (int i = 0; kStandaloneMiloFiles[i].relPath; i++) {
        std::string full = root + "/" + kStandaloneMiloFiles[i].relPath;
        if (!FileExists(full)) {
            skipped++;
            continue;
        }

        FilePath fp(full.c_str());
        ObjectDir *dir = DirLoader::LoadObjects(fp, nullptr, nullptr);
        ASSERT_NE(dir, nullptr) << "Failed to load: " << full;

        int count = 0;
        for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it)
            count++;

        printf("  OK [%s]: '%s' class='%s' objects=%d\n",
               kStandaloneMiloFiles[i].category, dir->Name(),
               dir->ClassName().Str(), count);

        EXPECT_NE(strlen(dir->Name()), 0u);
        EXPECT_GT(count, 0) << "Dir has no objects: " << full;
        loaded++;
    }

    if (loaded == 0)
        GTEST_SKIP() << "No standalone .milo_xbox files found at " << root;
    printf("LoadStandaloneMiloFiles: loaded=%d skipped=%d\n", loaded, skipped);
}

// ============================================================================
// Known-failing loads — regression targets for decomp bugs
// ============================================================================
// These test assets that exercise code paths through incomplete decomp
// functions (ObjectDir::PreLoad 89.6%, ObjectDir::PostLoad 85.8%).
// They document known loading failures as targets for fixing.

// Try loading a standalone milo file. Returns dir or nullptr on failure.
// Catches MILO_FAIL crashes via the native port's longjmp handler.
static ObjectDir *TryLoadStandalone(const std::string &path) {
    if (!FileExists(path))
        return nullptr;
    FilePath fp(path.c_str());
    return DirLoader::LoadObjects(fp, nullptr, nullptr);
}

static ObjectDir *FindDirectObjectDirByType(ObjectDir *dir, const char *type) {
    if (!dir || !type)
        return nullptr;

    for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it) {
        Hmx::Object *obj = it;
        ObjectDir *objDir = dynamic_cast<ObjectDir *>(obj);
        if (objDir && obj->Type() == type)
            return objDir;
    }
    return nullptr;
}

static int CountMoveGraphVariants(const MoveGraph *graph) {
    if (!graph)
        return 0;

    int count = 0;
    FOREACH (it, graph->MoveParents()) {
        count += (int)it->second->Variants().size();
    }
    return count;
}

struct LayoutResolutionStats {
    int total = 0;
    int found = 0;
    int missing = 0;
    int rest = 0;
    int dance = 0;
};

static LayoutResolutionStats AnalyzeMoveGraphLayout(const MoveGraph *graph) {
    LayoutResolutionStats stats;
    if (!graph || !graph->Layout())
        return stats;

    DataArray *layout = graph->Layout();
    for (int i = 0; i < kNumDifficultiesDC2; i++) {
        DataArray *diff = layout->FindArray(DifficultyToSym((Difficulty)i), false);
        if (!diff || diff->Size() < 2)
            continue;

        DataArray *moves = diff->Array(1);
        if (!moves)
            continue;

        for (int j = 0; j < moves->Size(); j++) {
            Symbol variantName = moves->Sym(j);
            const MoveVariant *variant = graph->FindMoveByVariantName(variantName);
            stats.total++;
            if (!variant) {
                stats.missing++;
                continue;
            }

            stats.found++;
            if (variant->IsRest())
                stats.rest++;
            else
                stats.dance++;
        }
    }

    return stats;
}

static void PrintMoveGraphLayoutSample(const MoveGraph *graph, const char *tag, int count) {
    if (!graph || !graph->Layout())
        return;

    DataArray *layout = graph->Layout();
    DataArray *easy = layout->FindArray("easy", false);
    if (!easy || easy->Size() < 2)
        return;

    DataArray *moves = easy->Array(1);
    if (!moves)
        return;

    int limit = Min(count, moves->Size());
    for (int i = 0; i < limit; i++) {
        Symbol variantName = moves->Sym(i);
        const MoveVariant *variant = graph->FindMoveByVariantName(variantName);
        printf(
            "  %s easy[%d] = '%s' -> %s (%s)\n",
            tag,
            i,
            variantName.Str(),
            variant ? "FOUND" : "NULL",
            variant ? variant->HamMoveName().Str() : "missing"
        );
    }
}

struct MoveTrackStats {
    int total;
    int nulls;
    int rests;
    int nonRests;
};

static MoveTrackStats AnalyzeMoveTrack(ObjectDir *dir, RndPropAnim *anim) {
    MoveTrackStats stats = {0, 0, 0, 0};
    if (!dir || !anim)
        return stats;

    static Symbol sMove("move");
    PropKeys *keys = nullptr;
    for (ObjDirItr<Hmx::Object> it(dir, true); it != nullptr; ++it) {
        keys = anim->GetKeys(it, DataArrayPtr(sMove));
        if (keys)
            break;
    }
    if (!keys)
        return stats;

    Keys<Symbol, Symbol> *symKeys = keys->AsSymbolKeys();
    if (!symKeys)
        return stats;

    for (int i = 0; i < (int)symKeys->size(); i++) {
        Symbol value = (*symKeys)[i].value;
        stats.total++;
        if (value.Null()) {
            stats.nulls++;
        } else if (strncmp(value.Str(), "Rest.move", 9) == 0) {
            stats.rests++;
        } else {
            stats.nonRests++;
        }
    }
    return stats;
}

// Complex venues — these exercise deep subdir chains with many object types.
// The full loading chain goes:
//   DirLoader::LoadDir -> WorldDir::PreLoad -> PanelDir::PreLoad ->
//   RndDir::PreLoad -> ObjectDir::PreLoad (89.6%)
// then ObjectDir::PostLoad (85.8%) for inlined subdirs.
//
// KNOWN BUG: world/gen/world.milo_xbox contains inlined subdirs including
// 'director' (which itself inlines 'iconmandir'). Same root cause as
// World master file — contains inlined subdirs (director, iconmandir, etc.)
// Previously crashed with "String chars N > 512" due to missing PanelDir
// factory registration causing stream desync during inlined subdir loading.
TEST_F(AssetLoadingTest, LoadWorldMasterFile) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set";
    std::string path = root + "/world/gen/world.milo_xbox";
    if (!FileExists(path))
        GTEST_SKIP() << "world.milo_xbox not found";

    ObjectDir *dir = TryLoadStandalone(path);
    ASSERT_NE(dir, nullptr) << "world.milo_xbox failed to load";
    EXPECT_STREQ(dir->ClassName().Str(), "WorldDir");
}

// Full venue worlds — large files with meshes, lights, cameras, animations.
// These exercise the complete loading pipeline including nested subdirs.
struct VenueEntry {
    const char *relPath;
    const char *name;
};

static const VenueEntry kVenueWorlds[] = {
    {"world/glitterati/gen/glitterati.milo_xbox", "glitterati"},
    {"world/dclive/gen/dclive.milo_xbox", "dclive"},
    {"world/houseparty/gen/houseparty.milo_xbox", "houseparty"},
    {"world/rollerrink/gen/rollerrink.milo_xbox", "rollerrink"},
    {"world/bid/gen/bid.milo_xbox", "bid"},
    {"world/dci/gen/dci.milo_xbox", "dci"},
    {"world/throneroom/gen/throneroom.milo_xbox", "throneroom"},
    {"world/streetside/gen/streetside.milo_xbox", "streetside"},
    {nullptr, nullptr}
};

TEST_F(AssetLoadingTest, LoadFullVenueWorlds) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set";

    int loaded = 0, failed = 0, skipped = 0;
    for (int i = 0; kVenueWorlds[i].relPath; i++) {
        std::string path = root + "/" + kVenueWorlds[i].relPath;
        if (!FileExists(path)) {
            skipped++;
            continue;
        }

        printf("  Loading %s...\n", kVenueWorlds[i].name);
        fflush(stdout);
        ObjectDir *dir = TryLoadStandalone(path);
        if (!dir) {
            printf("  FAIL: %s returned nullptr\n", kVenueWorlds[i].name);
            failed++;
            ADD_FAILURE() << "Failed to load venue: " << kVenueWorlds[i].name
                << " (" << kVenueWorlds[i].relPath << ")";
            continue;
        }

        // Count objects both flat and recursive
        int flatCount = 0, recursiveCount = 0;
        for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it)
            flatCount++;
        for (ObjDirItr<Hmx::Object> it(dir, true); it != nullptr; ++it)
            recursiveCount++;
        int subdirCount = (int)dir->SubDirs().size();
        printf("  OK: %s -> '%s' class='%s' flat=%d recursive=%d subdirs=%d\n",
               kVenueWorlds[i].name, dir->Name(), dir->ClassName().Str(),
               flatCount, recursiveCount, subdirCount);
        // Complex venue worlds should have substantial content
        EXPECT_GT(recursiveCount, 10) << "Venue " << kVenueWorlds[i].name
            << " has suspiciously few objects — subdirs may not be loading";
        loaded++;
    }

    if (loaded == 0 && skipped > 0)
        GTEST_SKIP() << "No venue worlds found at " << root;
    printf("VenueWorlds: loaded=%d failed=%d skipped=%d\n", loaded, failed, skipped);
}

// Shared world subdirs — icon manager, director, phrase meter, etc.
// These contain types like HamCharacter that may not be registered.
static const StandaloneMiloEntry kSharedWorldSubdirs[] = {
    {"world/shared/gen/iconman.milo_xbox", "iconman"},
    {"world/shared/gen/peak_spiral.milo_xbox", "peak-spiral"},
    {"world/shared/gen/phrase_meter.milo_xbox", "phrase-meter"},
    {"world/shared/gen/move_feedback.milo_xbox", "move-feedback"},
    {"world/shared/gen/chars_base.milo_xbox", "chars-base"},
    {nullptr, nullptr}
};

// Director contains inlined subdirs (PanelDir 'hud', RndDir 'iconmandir',
// HamDirector, PracticeSection, PropAnims). Loading fails with stream desync:
// ObjectDir::PreLoad (88.9%) reads the wrong bytes for inlined subdir revisions.
// The 'iconmandir' inlined subdir gets mRev=32 (the outer file rev) instead
// of the actual RndDir rev, causing ASSERT_REVS WARNING then String overflow.
//
// Director subdir — contains inlined 'hud' (PanelDir) and 'iconmandir' subdirs.
// Previously crashed with "String chars N > 512" due to missing PanelDir
// factory registration causing NULL object and ReadDead stream desync.
TEST_F(AssetLoadingTest, LoadDirectorSubdir) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set";
    std::string path = root + "/world/shared/gen/director.milo_xbox";
    if (!FileExists(path))
        GTEST_SKIP() << "director.milo_xbox not found";

    ObjectDir *dir = TryLoadStandalone(path);
    ASSERT_NE(dir, nullptr) << "director.milo_xbox failed to load";
    EXPECT_STREQ(dir->ClassName().Str(), "RndDir");
}

TEST_F(AssetLoadingTest, LoadSharedWorldSubdirs) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set";

    int loaded = 0, failed = 0, skipped = 0;
    for (int i = 0; kSharedWorldSubdirs[i].relPath; i++) {
        std::string path = root + "/" + kSharedWorldSubdirs[i].relPath;
        if (!FileExists(path)) {
            skipped++;
            continue;
        }

        printf("  Loading %s...\n", kSharedWorldSubdirs[i].category);
        fflush(stdout);
        ObjectDir *dir = TryLoadStandalone(path);
        if (!dir) {
            printf("  FAIL: %s returned nullptr\n", kSharedWorldSubdirs[i].category);
            failed++;
            ADD_FAILURE() << "Failed to load shared subdir: "
                << kSharedWorldSubdirs[i].category
                << " (" << kSharedWorldSubdirs[i].relPath << ")";
            continue;
        }

        int count = 0;
        for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it)
            count++;
        printf("  OK [%s]: '%s' class='%s' objects=%d\n",
               kSharedWorldSubdirs[i].category, dir->Name(),
               dir->ClassName().Str(), count);
        EXPECT_GT(count, 0);
        loaded++;
    }

    if (loaded == 0 && skipped > 0)
        GTEST_SKIP() << "No shared world subdirs found";
    printf("SharedWorldSubdirs: loaded=%d failed=%d skipped=%d\n",
           loaded, failed, skipped);
}

TEST_F(AssetLoadingTest, CharacterVoiceBanksResolveRuntimeSoundScope) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set";

    std::string camshotPath =
        root + "/world/shared/camshots/gen/mini_blue.milo_xbox";
    std::string voiceBankPath =
        root + "/sfx/loc/eng/gen/vo_bank_iconmanblue.milo_xbox";
    if (!FileExists(camshotPath) || !FileExists(voiceBankPath)) {
        GTEST_SKIP() << "mini_blue or vo_bank_iconmanblue asset not found";
    }

    ObjectDir *camshotDir = TryLoadStandalone(camshotPath);
    ASSERT_NE(camshotDir, nullptr) << "mini_blue.milo_xbox failed to load";

    Hmx::Object *camshotSound =
        camshotDir->FindObject("win_blue_P2_low_mov.snd", false, true);
    ASSERT_NE(camshotSound, nullptr)
        << "mini_blue root should resolve referenced VO sounds recursively";

    ObjectDir *voiceBankRoot = TryLoadStandalone(voiceBankPath);
    ASSERT_NE(voiceBankRoot, nullptr) << "vo_bank_iconmanblue.milo_xbox failed to load";

    ObjectDir *characterVo = nullptr;
    if (voiceBankRoot->Type() == "character_vo")
        characterVo = voiceBankRoot;
    else
        characterVo = FindDirectObjectDirByType(voiceBankRoot, "character_vo");
    ASSERT_NE(characterVo, nullptr)
        << "voice bank should expose a character_vo runtime object";

    Hmx::Object *voiceBankFlow = characterVo->FindObject("vo.flow", false, true);
    Hmx::Object *voiceBankSound =
        characterVo->FindObject("win_blue_low_01.snd", false, true);

    printf(
        "  character_vo scope: root='%s' bank='%s' flow=%p sound=%p soundDir='%s'\n",
        voiceBankRoot->Name(),
        characterVo->Name(),
        (void *)voiceBankFlow,
        (void *)voiceBankSound,
        voiceBankSound && voiceBankSound->Dir() ? voiceBankSound->Dir()->Name() : "(null)"
    );

    EXPECT_NE(voiceBankFlow, nullptr)
        << "character_vo should resolve vo.flow for runtime play_vo";
    EXPECT_NE(voiceBankSound, nullptr)
        << "character_vo should resolve runtime foley sounds recursively";
    EXPECT_EQ(voiceBankSound ? voiceBankSound->Dir() : nullptr, characterVo)
        << "vo_bank_iconmanblue runtime foley should resolve in character_vo scope";
}

TEST_F(AssetLoadingTest, BetterOffAloneMoveGraphCopyPreservesLayoutResolution) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set";

    std::string path = root + "/songs/betteroffalone/gen/move_data.milo_xbox";
    std::string songPath = root + "/songs/betteroffalone/gen/betteroffalone.milo_xbox";
    std::string movesPath = root + "/songs/betteroffalone/gen/moves.milo_xbox";
    if (!FileExists(path))
        GTEST_SKIP() << "betteroffalone move_data.milo_xbox not found";

    ObjectDir *dir = TryLoadStandalone(path);
    ASSERT_NE(dir, nullptr) << "betteroffalone move_data.milo_xbox failed to load";

    MoveGraph *source = dir->Find<MoveGraph>("move_graph", true);
    ASSERT_NE(source, nullptr) << "move_data missing move_graph";

    LayoutResolutionStats sourceStats = AnalyzeMoveGraphLayout(source);
    int sourceVariants = CountMoveGraphVariants(source);
    PrintMoveGraphLayoutSample(source, "source", 8);
    printf(
        "  source move_graph: parents=%zu variants=%d layout total=%d found=%d missing=%d rest=%d dance=%d\n",
        source->MoveParents().size(),
        sourceVariants,
        sourceStats.total,
        sourceStats.found,
        sourceStats.missing,
        sourceStats.rest,
        sourceStats.dance
    );

    MoveGraph *copied = Hmx::Object::New<MoveGraph>();
    copied->Copy(source, Hmx::Object::kCopyDeep);

    LayoutResolutionStats copiedStats = AnalyzeMoveGraphLayout(copied);
    int copiedVariants = CountMoveGraphVariants(copied);
    PrintMoveGraphLayoutSample(copied, "copied", 8);
    printf(
        "  copied move_graph: parents=%zu variants=%d layout total=%d found=%d missing=%d rest=%d dance=%d\n",
        copied->MoveParents().size(),
        copiedVariants,
        copiedStats.total,
        copiedStats.found,
        copiedStats.missing,
        copiedStats.rest,
        copiedStats.dance
    );

    EXPECT_GT(sourceVariants, 0);
    EXPECT_GT(sourceStats.dance, 0)
        << "standalone move_graph should resolve at least some dance variants";
    EXPECT_EQ(copiedVariants, sourceVariants)
        << "MoveGraph deep copy should preserve variant count";
    EXPECT_EQ(copiedStats.found, sourceStats.found)
        << "MoveGraph deep copy should preserve layout resolution";
    EXPECT_EQ(copiedStats.dance, sourceStats.dance)
        << "MoveGraph deep copy should preserve dance-variant availability";

    if (FileExists(songPath)) {
        ObjectDir *songDir = TryLoadStandalone(songPath);
        ASSERT_NE(songDir, nullptr) << "betteroffalone.milo_xbox failed to load";

        ObjectDir *songMoves = songDir->Find<ObjectDir>("moves", true);
        ObjectDir *embeddedMoveData = songDir->Find<ObjectDir>("move_data", true);
        printf(
            "  song world lookup: moves=%p move_data=%p\n",
            (void *)songMoves,
            (void *)embeddedMoveData
        );
        if (songMoves) {
            printf("  song moves subdirs:");
            for (int i = 0; i < (int)songMoves->SubDirs().size(); i++) {
                ObjectDir *sub = songMoves->SubDirs()[i];
                printf(" [%d]='%s'", i, sub ? sub->Name() : "(null)");
            }
            printf("\n");
        }
    }

    if (FileExists(movesPath)) {
        ObjectDir *movesDir = TryLoadStandalone(movesPath);
        ASSERT_NE(movesDir, nullptr) << "moves.milo_xbox failed to load";

        ObjectDir *embeddedMoveData = movesDir->Find<ObjectDir>("move_data", true);
        ASSERT_NE(embeddedMoveData, nullptr)
            << "moves.milo_xbox missing embedded move_data directory";
        printf("  standalone moves subdirs:");
        for (int i = 0; i < (int)movesDir->SubDirs().size(); i++) {
            ObjectDir *sub = movesDir->SubDirs()[i];
            printf(" [%d]='%s'", i, sub ? sub->Name() : "(null)");
        }
        printf("\n");

        MoveGraph *embeddedGraph = embeddedMoveData->Find<MoveGraph>("move_graph", true);
        ASSERT_NE(embeddedGraph, nullptr)
            << "embedded move_data missing move_graph";

        LayoutResolutionStats embeddedStats = AnalyzeMoveGraphLayout(embeddedGraph);
        int embeddedVariants = CountMoveGraphVariants(embeddedGraph);
        PrintMoveGraphLayoutSample(embeddedGraph, "embedded", 8);
        printf(
            "  embedded move_graph: parents=%zu variants=%d layout total=%d found=%d missing=%d rest=%d dance=%d\n",
            embeddedGraph->MoveParents().size(),
            embeddedVariants,
            embeddedStats.total,
            embeddedStats.found,
            embeddedStats.missing,
            embeddedStats.rest,
            embeddedStats.dance
        );

        EXPECT_EQ(embeddedVariants, sourceVariants)
            << "song milo embedded move_graph should match standalone move_data";
        EXPECT_EQ(embeddedStats.found, sourceStats.found)
            << "song milo embedded move_graph should preserve layout resolution";
        EXPECT_EQ(embeddedStats.dance, sourceStats.dance)
            << "song milo embedded move_graph should preserve dance-variant availability";
    }
}

TEST_F(AssetLoadingTest, BetterOffAloneAuthoredSongAnimsContainDanceMoves) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set";

    const char *paths[] = {
        "/songs/betteroffalone/gen/easy.milo_xbox",
        "/songs/betteroffalone/gen/medium.milo_xbox",
        "/songs/betteroffalone/gen/expert.milo_xbox",
        nullptr
    };
    const char *labels[] = { "easy", "medium", "expert" };

    for (int i = 0; paths[i]; i++) {
        std::string path = root + paths[i];
        if (!FileExists(path))
            GTEST_SKIP() << "betteroffalone authored anim asset missing: " << path;

        ObjectDir *dir = TryLoadStandalone(path);
        ASSERT_NE(dir, nullptr) << labels[i] << ".milo_xbox failed to load";

        RndPropAnim *songAnim = dir->Find<RndPropAnim>("song.anim", true);
        ASSERT_NE(songAnim, nullptr) << labels[i] << ".milo_xbox missing song.anim";

        MoveTrackStats stats = AnalyzeMoveTrack(dir, songAnim);
        printf(
            "  %s song.anim move keys: total=%d null=%d rest=%d nonRest=%d\n",
            labels[i],
            stats.total,
            stats.nulls,
            stats.rests,
            stats.nonRests
        );

        EXPECT_EQ(stats.total, 92)
            << labels[i] << " song.anim should preserve the authored 92-key move timeline";
        EXPECT_GT(stats.nonRests, 0)
            << labels[i] << " song.anim should contain real dance moves";
        EXPECT_GT(stats.rests, 0)
            << labels[i] << " song.anim should retain intro/outro rest markers";
    }
}

// Character loading — main character has complex bone hierarchy + animations
TEST_F(AssetLoadingTest, LoadMainCharacter) {
    ObjectDir *dir = LoadMainCharacterFromLibrary();
    if (!dir)
        GTEST_SKIP() << "char/main not found";
    ASSERT_NE(dir, nullptr) << "Failed to load main character";

    int count = 0;
    for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it)
        count++;
    printf("  main.milo_xbox: '%s' class='%s' objects=%d\n",
           dir->Name(), dir->ClassName().Str(), count);
    EXPECT_GT(count, 0);
}

TEST_F(AssetLoadingTest, MainCharacterFileMergerConfiguresOutfitAndVisemeByDefault) {
    ObjectDir *dir = LoadMainCharacterFromLibrary();
    if (!dir)
        GTEST_SKIP() << "char/main not found";
    ASSERT_NE(dir, nullptr) << "Failed to load main character";

    HamCharacter *character = FindMainCharacter(dir);
    ASSERT_NE(character, nullptr) << "No HamCharacter found in main.milo_xbox";

    FileMerger *fm = FindCharacterFileMerger(character);
    ASSERT_NE(fm, nullptr) << "main.milo_xbox missing char.fm";

    character->SetOutfit("mo01");
    character->SetOutfitDir("char/main/dancer");
    character->StartLoad(false);

    FileMerger::Merger *outfitMerger = nullptr;
    FileMerger::Merger *visemeMerger = nullptr;
    ObjVector<FileMerger::Merger> &mergers = fm->Mergers();
    for (int i = 0; i < mergers.size(); i++) {
        if (mergers[i].mName == "outfit")
            outfitMerger = &mergers[i];
        else if (mergers[i].mName == "viseme")
            visemeMerger = &mergers[i];
    }

    ASSERT_NE(outfitMerger, nullptr);
    ASSERT_NE(visemeMerger, nullptr);
    EXPECT_FALSE(outfitMerger->mSelected.empty());
    EXPECT_FALSE(visemeMerger->mSelected.empty());
    EXPECT_NE(character->Find<ObjectDir>("viseme", false), nullptr);
}

TEST_F(AssetLoadingTest, BackupOutfitBonePointersMatchServoDirectory) {
    ObjectDir *dir = LoadMainCharacterFromLibrary();
    if (!dir)
        GTEST_SKIP() << "char/main not found";
    ASSERT_NE(dir, nullptr) << "Failed to load main character";

    HamCharacter *character = FindMainCharacter(dir);
    ASSERT_NE(character, nullptr) << "No HamCharacter found in main.milo_xbox";

    FileMerger *fm = FindCharacterFileMerger(character);
    ASSERT_NE(fm, nullptr) << "main.milo_xbox missing char.fm";

    // HamCharacter::PostLoad starts async outfit loading via the organizer.
    // In the game, TheLoadMgr is polled every frame to drain these. In tests,
    // we force-release the FileMerger from the organizer so sync StartLoad works.
    fm->ForceReleaseOrganizer();

    // North-star regression: backup outfits should collapse mesh bone pointers
    // onto the same animated transforms that bone.servo resolves in its own dir.
    character->SetOutfit("lush01_bd01");
    character->SetOutfitDir("char/main/backup");
    character->StartLoad(false);

    CharServoBone *servo = character->Find<CharServoBone>("bone.servo", true);
    ASSERT_NE(servo, nullptr) << "main character missing bone.servo after outfit merge";
    ASSERT_NE(servo->Dir(), nullptr) << "bone.servo should resolve against a character dir";

    int skinnedMeshes = CountSkinnedMeshes(character);
    ASSERT_GT(skinnedMeshes, 0) << "Expected skinned meshes after backup outfit merge";

    int checkedBones = 0;
    int unresolvedBones = 0;
    int mismatches = 0;
    int logged = 0;

    for (ObjDirItr<RndMesh> it(character, true); it != nullptr; ++it) {
        RndMesh *mesh = it;
        if (mesh->NumBones() <= 0)
            continue;

        for (int i = 0; i < mesh->NumBones(); i++) {
            RndTransformable *meshBone = mesh->BoneTransAt(i);
            if (!meshBone)
                continue;

            RndTransformable *servoBone =
                CharUtlFindBoneTrans(meshBone->Name(), servo->Dir());
            if (!servoBone) {
                unresolvedBones++;
                if (logged < 8) {
                    printf(
                        "  UNRESOLVED mesh='%s' bone[%d]='%s' meshDir='%s' servoDir='%s'\n",
                        mesh->Name(),
                        i,
                        meshBone->Name(),
                        meshBone->Dir() ? meshBone->Dir()->Name() : "(null)",
                        servo->Dir() ? servo->Dir()->Name() : "(null)"
                    );
                    logged++;
                }
                continue;
            }

            checkedBones++;
            if (servoBone != meshBone) {
                mismatches++;
                if (logged < 8) {
                    printf(
                        "  MISMATCH mesh='%s' bone[%d]='%s' meshPtr=%p servoPtr=%p "
                        "meshDir='%s' servoDir='%s'\n",
                        mesh->Name(),
                        i,
                        meshBone->Name(),
                        (void *)meshBone,
                        (void *)servoBone,
                        meshBone->Dir() ? meshBone->Dir()->Name() : "(null)",
                        servoBone->Dir() ? servoBone->Dir()->Name() : "(null)"
                    );
                    logged++;
                }
            }
        }
    }

    printf(
        "  backup outfit pointer audit: skinnedMeshes=%d checkedBones=%d unresolved=%d "
        "mismatches=%d\n",
        skinnedMeshes,
        checkedBones,
        unresolvedBones,
        mismatches
    );

    ASSERT_GT(checkedBones, 20)
        << "Need enough mesh bones resolved to make the parity check meaningful";
    EXPECT_EQ(unresolvedBones, 0)
        << "Every skinned mesh bone should resolve from the servo directory";
    EXPECT_EQ(mismatches, 0)
        << "Skinned mesh bones should point at the same RndTransformables as bone.servo";
}

TEST_F(AssetLoadingTest, BackupOutfitPreservesArmPollableInventory) {
    ObjectDir *dir = LoadMainCharacterFromLibrary();
    if (!dir)
        GTEST_SKIP() << "char/main not found";
    ASSERT_NE(dir, nullptr) << "Failed to load main character";

    HamCharacter *character = FindMainCharacter(dir);
    ASSERT_NE(character, nullptr) << "No HamCharacter found in main.milo_xbox";

    FileMerger *fm = FindCharacterFileMerger(character);
    ASSERT_NE(fm, nullptr) << "main.milo_xbox missing char.fm";

    ArmPollableInventory before = CollectArmPollableInventory(character);

    fm->ForceReleaseOrganizer();
    character->SetOutfit("lush01_bd01");
    character->SetOutfitDir("char/main/backup");
    character->StartLoad(false);

    ArmPollableInventory after = CollectArmPollableInventory(character);

    ASSERT_FALSE(before.byClass.empty())
        << "Expected at least one authored arm pollable in main.milo_xbox";

    for (std::map<std::string, std::vector<std::string> >::const_iterator it =
             before.byClass.begin();
         it != before.byClass.end();
         ++it) {
        size_t afterCount = 0;
        std::map<std::string, std::vector<std::string> >::const_iterator found =
            after.byClass.find(it->first);
        if (found != after.byClass.end())
            afterCount = found->second.size();

        EXPECT_GE(afterCount, it->second.size())
            << "Backup outfit merge should not drop preexisting arm pollables for class "
            << it->first;
    }
}

TEST_F(AssetLoadingTest, BoneServoCarriesAndAppliesForeArmRotZChannels) {
    ObjectDir *charDir = LoadMainCharacterFromLibrary();
    if (!charDir)
        GTEST_SKIP() << "char/main not found";
    ASSERT_NE(charDir, nullptr) << "Failed to load main character";

    ObjectDir *clipDir = LoadCrowdClipLibrary();
    if (!clipDir)
        GTEST_SKIP() << "char/crowd/anim not found";
    ASSERT_NE(clipDir, nullptr) << "Failed to load crowd clip library";

    HamCharacter *character = FindMainCharacter(charDir);
    ASSERT_NE(character, nullptr) << "No HamCharacter found in main.milo_xbox";

    CharServoBone *servo = character->Find<CharServoBone>("bone.servo", true);
    ASSERT_NE(servo, nullptr) << "main character missing bone.servo";
    ASSERT_NE(servo->Dir(), nullptr) << "bone.servo should resolve against a character dir";

    CharClip *clip = FindClipByName(clipDir, "crouching_great_01");
    ASSERT_NE(clip, nullptr) << "Failed to find crouching_great_01";

    bool hasLForeArm = false;
    bool hasRForeArm = false;
    std::vector<CharBones::Bone> bones = servo->GetBones();
    for (size_t i = 0; i < bones.size(); i++) {
        if (bones[i].name == Symbol("bone_L-foreArm.rotz"))
            hasLForeArm = true;
        if (bones[i].name == Symbol("bone_R-foreArm.rotz"))
            hasRForeArm = true;
    }

    ASSERT_TRUE(hasLForeArm) << "bone.servo dropped bone_L-foreArm.rotz";
    ASSERT_TRUE(hasRForeArm) << "bone.servo dropped bone_R-foreArm.rotz";

    float beat = clip->StartBeat() + clip->LengthBeats() * 0.5f;
    void *lChan = clip->GetChannel(Symbol("bone_L-foreArm.rotz"));
    void *rChan = clip->GetChannel(Symbol("bone_R-foreArm.rotz"));
    ASSERT_NE(lChan, nullptr);
    ASSERT_NE(rChan, nullptr);

    float evalL = 0.0f;
    float evalR = 0.0f;
    clip->EvaluateChannel(&evalL, lChan, beat);
    clip->EvaluateChannel(&evalR, rChan, beat);

    servo->AcquirePose();
    clip->ScaleDown(*servo, 0.0f);
    clip->ScaleAdd(*servo, 1.0f, beat, 0.0f);

    float *servoL = (float *)servo->FindPtr(Symbol("bone_L-foreArm.rotz"));
    float *servoR = (float *)servo->FindPtr(Symbol("bone_R-foreArm.rotz"));
    ASSERT_NE(servoL, nullptr);
    ASSERT_NE(servoR, nullptr);

    EXPECT_NEAR(*servoL, evalL, 1e-4f);
    EXPECT_NEAR(*servoR, evalR, 1e-4f);
    EXPECT_GT(std::fabs(*servoL), 0.05f);
    EXPECT_GT(std::fabs(*servoR), 0.05f);

    servo->PoseMeshes();

    RndTransformable *lForeArmMesh =
        CharUtlFindBoneTrans("bone_L-foreArm.mesh", servo->Dir());
    RndTransformable *rForeArmMesh =
        CharUtlFindBoneTrans("bone_R-foreArm.mesh", servo->Dir());
    ASSERT_NE(lForeArmMesh, nullptr);
    ASSERT_NE(rForeArmMesh, nullptr);

    float localL = GetZAngle(lForeArmMesh->LocalXfm().m);
    float localR = GetZAngle(rForeArmMesh->LocalXfm().m);

    printf(
        "  servo forearm audit: evalL=%0.4f evalR=%0.4f servoL=%0.4f servoR=%0.4f "
        "localL=%0.4f localR=%0.4f\n",
        evalL,
        evalR,
        *servoL,
        *servoR,
        localL,
        localR
    );

    EXPECT_NEAR(localL, evalL, 1e-3f);
    EXPECT_NEAR(localR, evalR, 1e-3f);
}

TEST_F(AssetLoadingTest, SkinnedMeshesCarryNontrivialForeTwistWeights) {
    ObjectDir *charDir = LoadMainCharacterFromLibrary();
    if (!charDir)
        GTEST_SKIP() << "char/main not found";
    ASSERT_NE(charDir, nullptr) << "Failed to load main character";

    HamCharacter *character = FindMainCharacter(charDir);
    ASSERT_NE(character, nullptr) << "No HamCharacter found in main.milo_xbox";

    FileMerger *fm = FindCharacterFileMerger(character);
    ASSERT_NE(fm, nullptr) << "main.milo_xbox missing char.fm";
    fm->ForceReleaseOrganizer();
    character->SetOutfit("lush01_bd01");
    character->SetOutfitDir("char/main/backup");
    character->StartLoad(false);

    int checkedMeshes = 0;
    int meshesUsingTwist = 0;

    for (ObjDirItr<RndMesh> it(character, true); it != nullptr; ++it) {
        RndMesh *mesh = it;
        if (!mesh->IsSkinned() || mesh->NumBones() <= 0)
            continue;

        int lUpper = -1, lFore = -1, lHand = -1, lTwist1 = -1, lTwist2 = -1;
        int rUpper = -1, rFore = -1, rHand = -1, rTwist1 = -1, rTwist2 = -1;
        for (int b = 0; b < mesh->NumBones(); b++) {
            RndTransformable *bone = mesh->BoneTransAt(b);
            const char *name = bone ? bone->Name() : "";
            if (strcmp(name, "bone_L-upperArm.mesh") == 0) lUpper = b;
            else if (strcmp(name, "bone_L-foreArm.mesh") == 0) lFore = b;
            else if (strcmp(name, "bone_L-hand.mesh") == 0) lHand = b;
            else if (strcmp(name, "bone_L-foreTwist1.mesh") == 0) lTwist1 = b;
            else if (strcmp(name, "bone_L-foreTwist2.mesh") == 0) lTwist2 = b;
            else if (strcmp(name, "bone_R-upperArm.mesh") == 0) rUpper = b;
            else if (strcmp(name, "bone_R-foreArm.mesh") == 0) rFore = b;
            else if (strcmp(name, "bone_R-hand.mesh") == 0) rHand = b;
            else if (strcmp(name, "bone_R-foreTwist1.mesh") == 0) rTwist1 = b;
            else if (strcmp(name, "bone_R-foreTwist2.mesh") == 0) rTwist2 = b;
        }

        const bool hasLeftChain =
            lUpper >= 0 && lFore >= 0 && lHand >= 0 && lTwist1 >= 0 && lTwist2 >= 0;
        const bool hasRightChain =
            rUpper >= 0 && rFore >= 0 && rHand >= 0 && rTwist1 >= 0 && rTwist2 >= 0;
        if (!hasLeftChain && !hasRightChain)
            continue;

        checkedMeshes++;

        int numVerts = mesh->NumVerts();
        int numCompressedVerts = mesh->NumCompressedVerts();
        int vertCount = numCompressedVerts > 0 ? numCompressedVerts : numVerts;
        ASSERT_GT(vertCount, 0) << "skinned mesh has no vertex data: " << mesh->Name();

        std::vector<GpuVertexSkinned> verts(vertCount);
        int unpacked = 0;
        if (numCompressedVerts > 0 && mesh->CompressedVerts()) {
            unpacked = VertexFormats::UnpackCompressedSkinnedVertices(
                mesh->CompressedVerts(), numCompressedVerts, verts.data(), vertCount
            );
        } else {
            unpacked = VertexFormats::UnpackSkinnedVertices(*mesh, verts.data(), vertCount);
        }
        ASSERT_GT(unpacked, 0) << "failed to unpack skinned vertices for " << mesh->Name();

        std::vector<float> totalWeight(mesh->NumBones(), 0.0f);
        std::vector<int> nonzeroVerts(mesh->NumBones(), 0);
        for (int v = 0; v < unpacked; v++) {
            const GpuVertexSkinned &gv = verts[v];
            for (int j = 0; j < 4; j++) {
                int boneIdx = gv.boneIndices[j];
                float w = gv.boneWeights[j];
                if (boneIdx < 0 || boneIdx >= mesh->NumBones() || w <= 0.0f)
                    continue;
                totalWeight[boneIdx] += w;
                nonzeroVerts[boneIdx]++;
            }
        }

        auto logSide = [&](const char *side,
                           int upper,
                           int fore,
                           int hand,
                           int twist1,
                           int twist2) {
            if (upper < 0)
                return;
            printf(
                "  mesh '%s' %s verts(raw=%d compressed=%d) "
                "weights: upper(sum=%.1f verts=%d) fore(sum=%.1f verts=%d) "
                "hand(sum=%.1f verts=%d) twist1(sum=%.1f verts=%d) twist2(sum=%.1f verts=%d)\n",
                mesh->Name(),
                side,
                numVerts,
                numCompressedVerts,
                totalWeight[upper], nonzeroVerts[upper],
                totalWeight[fore], nonzeroVerts[fore],
                totalWeight[hand], nonzeroVerts[hand],
                totalWeight[twist1], nonzeroVerts[twist1],
                totalWeight[twist2], nonzeroVerts[twist2]
            );
        };

        if (hasLeftChain) {
            logSide("left", lUpper, lFore, lHand, lTwist1, lTwist2);
            if (totalWeight[lTwist1] > 1.0f || totalWeight[lTwist2] > 1.0f)
                meshesUsingTwist++;
        }
        if (hasRightChain) {
            logSide("right", rUpper, rFore, rHand, rTwist1, rTwist2);
            if (totalWeight[rTwist1] > 1.0f || totalWeight[rTwist2] > 1.0f)
                meshesUsingTwist++;
        }
    }

    EXPECT_GT(checkedMeshes, 0)
        << "Expected at least one skinned mesh with a forearm + foreTwist palette";
    EXPECT_GT(meshesUsingTwist, 0)
        << "At least one skinned forearm mesh should actually weight vertices to foreTwist bones";
}

// ============================================================================
// Inspect forearm vertex bone assignments from real compressed outfit mesh
// ============================================================================

TEST_F(AssetLoadingTest, InspectForearmVertexBoneAssignments) {
    ObjectDir *charDir = LoadMainCharacterFromLibrary();
    if (!charDir)
        GTEST_SKIP() << "char/main not found";

    HamCharacter *character = FindMainCharacter(charDir);
    ASSERT_NE(character, nullptr);

    FileMerger *fm = FindCharacterFileMerger(character);
    ASSERT_NE(fm, nullptr);
    fm->ForceReleaseOrganizer();
    character->SetOutfit("lush01_bd01");
    character->SetOutfitDir("char/main/backup");
    character->StartLoad(false);

    // Find an outfit mesh with compressed verts and forearm bones
    for (ObjDirItr<RndMesh> it(character, true); it != nullptr; ++it) {
        RndMesh *mesh = it;
        if (!mesh->IsSkinned() || mesh->NumBones() <= 0 ||
            mesh->NumCompressedVerts() <= 0)
            continue;

        // Build bone name map for this mesh's palette
        int numBones = mesh->NumBones();
        std::vector<std::string> boneNames(numBones);
        int forearmBoneIdx = -1;
        int twist1Idx = -1, twist2Idx = -1;
        for (int b = 0; b < numBones; b++) {
            RndTransformable *bone = mesh->BoneTransAt(b);
            boneNames[b] = bone ? bone->Name() : "(null)";
            if (boneNames[b] == "bone_L-foreArm.mesh") forearmBoneIdx = b;
            if (boneNames[b] == "bone_L-foreTwist1.mesh") twist1Idx = b;
            if (boneNames[b] == "bone_L-foreTwist2.mesh") twist2Idx = b;
        }

        if (forearmBoneIdx < 0 && twist1Idx < 0)
            continue;

        printf("\n=== FOREARM VERTEX INSPECTION: '%s' ===\n", mesh->Name());
        printf("  Bone palette (%d bones):\n", numBones);
        for (int b = 0; b < numBones; b++) {
            RndTransformable *bone = mesh->BoneTransAt(b);
            const Transform& off = mesh->BoneOffsetAt(b);
            printf("    [%2d] '%s' offset.v=(%.2f,%.2f,%.2f) offset.m.x.x=%.3f\n",
                   b, boneNames[b].c_str(),
                   off.v.x, off.v.y, off.v.z, off.m.x.x);
        }

        // Unpack compressed vertices
        int numVerts = mesh->NumCompressedVerts();
        std::vector<GpuVertexSkinned> verts(numVerts);
        int unpacked = VertexFormats::UnpackCompressedSkinnedVertices(
            mesh->CompressedVerts(), numVerts, verts.data(), numVerts);

        // Find vertices weighted to forearm-area bones and dump details
        printf("  Forearm-area vertices (first 10 with significant forearm/twist weight):\n");
        int found = 0;
        for (int v = 0; v < unpacked && found < 10; v++) {
            const GpuVertexSkinned &gv = verts[v];
            // Check if any bone influence is a forearm-area bone
            bool hasForearm = false;
            for (int j = 0; j < 4; j++) {
                int bi = gv.boneIndices[j];
                float w = gv.boneWeights[j];
                if (w > 0.01f && bi < numBones &&
                    (bi == forearmBoneIdx || bi == twist1Idx || bi == twist2Idx)) {
                    hasForearm = true;
                    break;
                }
            }
            if (!hasForearm) continue;
            found++;

            float wSum = gv.boneWeights[0] + gv.boneWeights[1] +
                         gv.boneWeights[2] + gv.boneWeights[3];
            printf("    v[%d] pos=(%.2f,%.2f,%.2f) wSum=%.3f\n",
                   v, gv.pos[0], gv.pos[1], gv.pos[2], wSum);
            for (int j = 0; j < 4; j++) {
                int bi = gv.boneIndices[j];
                float w = gv.boneWeights[j];
                if (w > 0.001f) {
                    const char *bn = (bi >= 0 && bi < numBones)
                        ? boneNames[bi].c_str() : "OUT_OF_RANGE";
                    printf("      influence[%d]: bone[%d]='%s' weight=%.4f\n",
                           j, bi, bn, w);
                }
            }
        }
        printf("  Total forearm-area vertices found: %d / %d unpacked\n", found, unpacked);

        // Only inspect first matching mesh
        break;
    }
}

// ============================================================================
// CPU-side skinning of real forearm vertex — check if result makes sense
// ============================================================================

TEST_F(AssetLoadingTest, CpuSkinForearmVertexFromCompressedMesh) {
    ObjectDir *charDir = LoadMainCharacterFromLibrary();
    if (!charDir)
        GTEST_SKIP() << "char/main not found";

    HamCharacter *character = FindMainCharacter(charDir);
    ASSERT_NE(character, nullptr);

    // Merge backup outfit FIRST
    FileMerger *fm = FindCharacterFileMerger(character);
    ASSERT_NE(fm, nullptr);
    fm->ForceReleaseOrganizer();
    character->SetOutfit("lush01_bd01");
    character->SetOutfitDir("char/main/backup");
    character->StartLoad(false);

    // Load clips and pose AFTER merge
    ObjectDir *clipDir = LoadCrowdClipLibrary();
    if (!clipDir)
        GTEST_SKIP() << "clips not found";

    CharClip *clip = FindClipByName(clipDir, "crouching_great_01");
    ASSERT_NE(clip, nullptr);

    // Apply the clip at a specific beat (direct pose) + solve twists
    clip->PoseMeshes(character, 37.9f);
    CharTwistSolver::SolveAll(character);

    // Find compressed outfit mesh with forearm bones
    for (ObjDirItr<RndMesh> it(character, true); it != nullptr; ++it) {
        RndMesh *mesh = it;
        if (!mesh->IsSkinned() || mesh->NumBones() <= 0 ||
            mesh->NumCompressedVerts() <= 0)
            continue;

        int twist2Idx = -1, handIdx = -1;
        for (int b = 0; b < mesh->NumBones(); b++) {
            RndTransformable *bone = mesh->BoneTransAt(b);
            if (!bone) continue;
            if (strcmp(bone->Name(), "bone_L-foreTwist2.mesh") == 0) twist2Idx = b;
            if (strcmp(bone->Name(), "bone_L-hand.mesh") == 0) handIdx = b;
        }
        if (twist2Idx < 0) continue;

        printf("\n=== CPU SKINNING TEST: '%s' ===\n", mesh->Name());

        // Compute skin matrices (same as FillBoneUniforms)
        int numBones = mesh->NumBones();
        std::vector<std::array<float, 16>> skinMats(numBones);
        for (int b = 0; b < numBones; b++) {
            RndTransformable *bone = mesh->BoneTransAt(b);
            if (bone) {
                Transform skinMatrix;
                Multiply(mesh->BoneOffsetAt(b), bone->WorldXfm(), skinMatrix);
                TransformToMat4(skinMatrix, skinMats[b].data());
            } else {
                memset(skinMats[b].data(), 0, 64);
                skinMats[b][0] = skinMats[b][5] = skinMats[b][10] = skinMats[b][15] = 1.0f;
            }
        }

        // Print the skin matrices for forearm bones
        auto printMat = [](const char *name, int idx, const float m[16]) {
            printf("  skinMat[%d] '%s':\n", idx, name);
            printf("    row0: [%8.4f %8.4f %8.4f %8.4f]\n", m[0], m[1], m[2], m[3]);
            printf("    row1: [%8.4f %8.4f %8.4f %8.4f]\n", m[4], m[5], m[6], m[7]);
            printf("    row2: [%8.4f %8.4f %8.4f %8.4f]\n", m[8], m[9], m[10], m[11]);
            printf("    row3: [%8.4f %8.4f %8.4f %8.4f]\n", m[12], m[13], m[14], m[15]);
        };
        if (twist2Idx >= 0) {
            RndTransformable *t2 = mesh->BoneTransAt(twist2Idx);
            printMat(t2 ? t2->Name() : "?", twist2Idx, skinMats[twist2Idx].data());
        }
        if (handIdx >= 0) {
            RndTransformable *t = mesh->BoneTransAt(handIdx);
            printMat(t ? t->Name() : "?", handIdx, skinMats[handIdx].data());
        }

        // Unpack compressed verts and skin a forearm vertex on CPU
        int numVerts = mesh->NumCompressedVerts();
        std::vector<GpuVertexSkinned> verts(numVerts);
        int unpacked = VertexFormats::UnpackCompressedSkinnedVertices(
            mesh->CompressedVerts(), numVerts, verts.data(), numVerts);

        printf("  CPU skinning first 5 forearm vertices:\n");
        int found = 0;
        for (int v = 0; v < unpacked && found < 5; v++) {
            const GpuVertexSkinned &gv = verts[v];
            bool hasTwist = false;
            for (int j = 0; j < 4; j++) {
                if (gv.boneWeights[j] > 0.01f && gv.boneIndices[j] == twist2Idx)
                    hasTwist = true;
            }
            if (!hasTwist) continue;
            found++;

            // Normalize weights
            float wSum = gv.boneWeights[0] + gv.boneWeights[1] +
                         gv.boneWeights[2] + gv.boneWeights[3];
            float nw[4] = {0};
            if (wSum > 0) {
                for (int j = 0; j < 4; j++) nw[j] = gv.boneWeights[j] / wSum;
            }

            // CPU skin: blendedPos = sum(weight * (boneMatrix * pos))
            float blendedPos[3] = {0, 0, 0};
            for (int j = 0; j < 4; j++) {
                if (nw[j] <= 0) continue;
                int bi = gv.boneIndices[j];
                if (bi >= numBones) continue;
                const float *m = skinMats[bi].data();
                // M * v (column-vector, matching WGSL):
                // result = col0*px + col1*py + col2*pz + col3*1
                // But TransformToMat4 stores row-major, WGSL reads as column-major
                // So M_wgsl * v = v * M_original (row-vector)
                float px = gv.pos[0], py = gv.pos[1], pz = gv.pos[2];
                float rx = px*m[0] + py*m[4] + pz*m[8]  + m[12];
                float ry = px*m[1] + py*m[5] + pz*m[9]  + m[13];
                float rz = px*m[2] + py*m[6] + pz*m[10] + m[14];
                blendedPos[0] += nw[j] * rx;
                blendedPos[1] += nw[j] * ry;
                blendedPos[2] += nw[j] * rz;
            }

            printf("    v[%d] bindPos=(%.2f,%.2f,%.2f) → skinned=(%.2f,%.2f,%.2f)\n",
                   v, gv.pos[0], gv.pos[1], gv.pos[2],
                   blendedPos[0], blendedPos[1], blendedPos[2]);
        }

        // Also print actual bone world positions for reference
        printf("  Bone world positions:\n");
        for (int b : {twist2Idx, handIdx}) {
            if (b < 0) continue;
            RndTransformable *bone = mesh->BoneTransAt(b);
            if (!bone) continue;
            const Vector3 &wp = bone->WorldXfm().v;
            printf("    [%d] '%s' worldPos=(%.2f,%.2f,%.2f)\n",
                   b, bone->Name(), wp.x, wp.y, wp.z);
        }

        break;  // only first matching mesh
    }
}

// ============================================================================
// Verify object iteration doesn't loop infinitely or crash
// ============================================================================

TEST_F(AssetLoadingTest, ObjectIterationSafety) {
    ObjectDir *dir = TryLoadArchive("char/shared/main_resource.milo");
    if (!dir) {
        GTEST_SKIP() << "No archive assets available";
    }

    // Iterate with a safety limit
    const int kMaxObjects = 100000;
    int count = 0;
    for (ObjDirItr<Hmx::Object> it(dir, true); it != nullptr; ++it) {
        ASSERT_LT(count, kMaxObjects) << "Object iteration exceeded safety limit";
        EXPECT_NE(it->Name(), nullptr);
        EXPECT_NE(it->ClassName().Str(), nullptr);
        count++;
    }

    printf("ObjectIterationSafety: %d objects iterated without issue\n", count);
    EXPECT_GT(count, 0);
}

// ============================================================================
// Repeated load/delete cycle — check for memory leaks and dangling refs
// ============================================================================

TEST_F(AssetLoadingTest, RepeatedLoadCycle) {
    const char *testPath = "char/shared/main_resource.milo";
    ObjectDir *probe = TryLoadArchive(testPath);
    if (!probe) {
        GTEST_SKIP() << "No archive assets available";
    }
    delete probe;

    // Load and delete 5 times
    for (int cycle = 0; cycle < 5; cycle++) {
        ObjectDir *dir = TryLoadArchive(testPath);
        ASSERT_NE(dir, nullptr) << "Load failed on cycle " << cycle;

        int count = 0;
        for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it)
            count++;

        printf("  Cycle %d: loaded %d objects\n", cycle, count);
        EXPECT_GT(count, 0);
        delete dir;
    }
}

// ============================================================================
// Bulk loading — try every .milo_xbox file in the library
// ============================================================================
// Walks the MILO_LIB directory tree and attempts to load every .milo_xbox.
// Reports per-category pass/fail counts. Failures are non-fatal (EXPECT, not
// ASSERT) so one bad file doesn't abort the entire sweep.
//
// Filter by category with env var:
//   MILO_BULK_CATEGORY=ui    (ui, world, char, sfx, flow, songs, all)
//   MILO_BULK_LIMIT=50       (max files per category, 0=unlimited)
//
// Run:
//   cd native/build && ctest -R BulkLoad --output-on-failure

static void CollectMiloFiles(const std::string &dir, std::vector<std::string> &out) {
    DIR *dp = opendir(dir.c_str());
    if (!dp) return;
    struct dirent *ent;
    while ((ent = readdir(dp)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = dir + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            CollectMiloFiles(full, out);
        } else if (S_ISREG(st.st_mode)) {
            std::string name(ent->d_name);
            if (name.size() > 10 && name.substr(name.size() - 10) == ".milo_xbox") {
                out.push_back(full);
            }
        }
    }
    closedir(dp);
}

static std::string CategoryFromPath(const std::string &path, const std::string &root) {
    std::string rel = path.substr(root.size() + 1);
    size_t slash = rel.find('/');
    return (slash != std::string::npos) ? rel.substr(0, slash) : "other";
}

TEST_F(AssetLoadingTest, BulkLoadAllFiles) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set";

    // Make MILO_FAIL non-fatal so one bad file doesn't abort the sweep
    setenv("MILO_FATAL_FAILS", "0", 1);

    const char *catFilter = getenv("MILO_BULK_CATEGORY");
    std::string category = catFilter ? catFilter : "all";

    const char *limitStr = getenv("MILO_BULK_LIMIT");
    const char *bulkAll = getenv("MILO_BULK_ALL");
    int limit = limitStr ? atoi(limitStr) : (bulkAll && std::string(bulkAll) == "1") ? 0 : 20;

    // Collect all .milo_xbox files
    std::vector<std::string> allFiles;
    CollectMiloFiles(root, allFiles);
    std::sort(allFiles.begin(), allFiles.end());

    ASSERT_GT((int)allFiles.size(), 0) << "No .milo_xbox files found in " << root;
    printf("Found %d .milo_xbox files in library\n", (int)allFiles.size());

    // Filter by category
    std::vector<std::string> files;
    for (auto &f : allFiles) {
        if (category != "all") {
            std::string cat = CategoryFromPath(f, root);
            if (cat != category) continue;
        }
        files.push_back(f);
        if (limit > 0 && (int)files.size() >= limit) break;
    }

    printf("Testing %d files (category=%s, limit=%d)\n",
           (int)files.size(), category.c_str(), limit);

    int passed = 0, failed = 0, skipped = 0;
    std::vector<std::string> failures;

    for (auto &path : files) {
        std::string rel = path.substr(root.size() + 1);
        ObjectDir *dir = TryLoadStandalone(path);
        if (dir) {
            passed++;
            // Don't delete dir — ObjDirPtr destructor cascade is O(n^2) for
            // world files with many shared subdirs (HasDirPtrs walks the ref
            // ring per subdir). One venue file takes 30+ seconds to destroy.
            // Loading is what we're testing, not destruction.
        } else {
            failures.push_back(rel);
            failed++;
        }
    }

    printf("\n=== Bulk Load Results ===\n");
    printf("Passed: %d  Failed: %d  Skipped: %d  Total: %d\n",
           passed, failed, skipped, (int)files.size());

    if (!failures.empty()) {
        printf("\nFailed files (%d):\n", (int)failures.size());
        for (auto &f : failures)
            printf("  FAIL: %s\n", f.c_str());
    }

    // We expect at least 90% pass rate
    if (files.size() > 10) {
        float passRate = (float)passed / (float)files.size();
        EXPECT_GE(passRate, 0.9f)
            << "Pass rate " << (passRate * 100) << "% is below 90% threshold";
    }
}

// Per-category tests — can run in parallel with ctest -j$(nproc)
// Default subset size for large categories (sfx, songs).
// Set MILO_BULK_ALL=1 to load every file instead of a subset.
static const int kDefaultSubsetSize = 5;

static void RunCategoryBulkLoad(const char *category) {
    std::string root = GetMiloLibRoot();
    if (root.empty()) {
        GTEST_SKIP() << "MILO_LIB not set";
        return;
    }
    setenv("MILO_FATAL_FAILS", "0", 1);

    std::vector<std::string> allFiles;
    CollectMiloFiles(root, allFiles);
    std::sort(allFiles.begin(), allFiles.end());

    std::vector<std::string> files;
    for (auto &f : allFiles) {
        if (CategoryFromPath(f, root) == category)
            files.push_back(f);
    }
    if (files.empty()) {
        GTEST_SKIP() << "No files for category " << category;
        return;
    }

    // For large categories, only test a spread-out subset by default.
    // MILO_BULK_ALL=1 runs the full set.
    const char *bulkAll = std::getenv("MILO_BULK_ALL");
    bool runAll = bulkAll && std::string(bulkAll) == "1";
    std::vector<std::string> subset;
    if (!runAll && (int)files.size() > kDefaultSubsetSize) {
        // Pick evenly spaced files for coverage across the category
        for (int i = 0; i < kDefaultSubsetSize; i++) {
            int idx = i * (int)files.size() / kDefaultSubsetSize;
            subset.push_back(files[idx]);
        }
        printf("Category '%s': %d files (testing subset of %d, set MILO_BULK_ALL=1 for all)\n",
               category, (int)files.size(), (int)subset.size());
    } else {
        subset = files;
        printf("Category '%s': %d files\n", category, (int)subset.size());
    }

    int passed = 0, failed = 0;
    std::vector<std::string> failures;

    for (auto &path : subset) {
        ObjectDir *dir = TryLoadStandalone(path);
        if (dir) {
            passed++;
        } else {
            failures.push_back(path.substr(root.size() + 1));
            failed++;
        }
    }

    printf("  Passed: %d  Failed: %d\n", passed, failed);
    for (auto &f : failures)
        printf("  FAIL: %s\n", f.c_str());

    EXPECT_EQ(failed, 0) << failed << " files failed to load";
}

TEST_F(AssetLoadingTest, BulkLoad_Flow)  { RunCategoryBulkLoad("flow"); }
TEST_F(AssetLoadingTest, BulkLoad_Char)  { RunCategoryBulkLoad("char"); }
TEST_F(AssetLoadingTest, BulkLoad_World) { RunCategoryBulkLoad("world"); }
TEST_F(AssetLoadingTest, BulkLoad_UI)    { RunCategoryBulkLoad("ui"); }
TEST_F(AssetLoadingTest, BulkLoad_SFX)   { RunCategoryBulkLoad("sfx"); }
TEST_F(AssetLoadingTest, BulkLoad_Songs) { RunCategoryBulkLoad("songs"); }

// ============================================================================
// Subdir loading validation — verify inlined subdirs are populated
// ============================================================================

#include "rndobj/PropAnim.h"
#include "rndobj/Dir.h"
#include "flow/Flow.h"

TEST_F(AssetLoadingTest, ChooseModeSubdirLoading) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set";
    std::string path = root + "/ui/choose_mode/gen/choose_mode.milo_xbox";
    if (!FileExists(path))
        GTEST_SKIP() << "choose_mode.milo_xbox not found";

    FilePath fp(path.c_str());
    ObjectDir *dir = DirLoader::LoadObjects(fp, nullptr, nullptr);
    ASSERT_NE(dir, nullptr);

    printf("Dir: '%s' class='%s'\n", dir->Name(), dir->ClassName().Str());

    printf("SubDirs (%d):\n", (int)dir->SubDirs().size());
    for (int i = 0; i < (int)dir->SubDirs().size(); i++) {
        ObjectDir *sub = dir->SubDirs()[i];
        if (sub) {
            int count = 0;
            for (ObjDirItr<Hmx::Object> it(sub, false); it != nullptr; ++it)
                count++;
            printf("  [%d] '%s' class='%s' objects=%d subdirs=%d\n",
                   i, sub->Name(), sub->ClassName().Str(), count,
                   (int)sub->SubDirs().size());
        } else {
            printf("  [%d] nullptr\n", i);
        }
    }

    int flatCount = 0;
    for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it)
        flatCount++;
    int recCount = 0;
    for (ObjDirItr<Hmx::Object> it(dir, true); it != nullptr; ++it)
        recCount++;
    printf("Objects: flat=%d recursive=%d\n", flatCount, recCount);

    int paCount = 0;
    for (ObjDirItr<RndPropAnim> it(dir, true); it != nullptr; ++it) {
        printf("  PropAnim '%s' (dir='%s') end=%.1f\n",
               it->Name(), it->Dir() ? it->Dir()->Name() : "?", it->EndFrame());
        paCount++;
    }
    printf("PropAnims: %d\n", paCount);

    int flowCount = 0;
    for (ObjDirItr<Flow> it(dir, true); it != nullptr; ++it) {
        printf("  Flow '%s' (dir='%s')\n",
               it->Name(), it->Dir() ? it->Dir()->Name() : "?");
        flowCount++;
    }
    printf("Flows: %d\n", flowCount);

    // Check for RndDir objects (nested dirs that aren't formal subdirs)
    printf("\nRndDir objects in main dir:\n");
    int rndDirCount = 0;
    for (ObjDirItr<RndDir> it(dir, false); it != nullptr; ++it) {
        printf("  RndDir '%s' (dir='%s')\n", it->Name(), it->Dir() ? it->Dir()->Name() : "?");
        // Count objects inside this nested RndDir
        int innerCount = 0;
        for (ObjDirItr<Hmx::Object> inner((ObjectDir*)&*it, false); inner != nullptr; ++inner)
            innerCount++;
        int innerPA = 0;
        for (ObjDirItr<RndPropAnim> inner((ObjectDir*)&*it, false); inner != nullptr; ++inner) {
            printf("    PropAnim '%s' end=%.1f\n", inner->Name(), inner->EndFrame());
            innerPA++;
        }
        printf("    Objects=%d PropAnims=%d subdirs=%d\n", innerCount, innerPA, (int)it->SubDirs().size());
        rndDirCount++;
    }
    printf("RndDir count: %d\n\n", rndDirCount);

    // game_mode_icon RndDir should have icon_enter PropAnims
    // Note: ObjDirItr only traverses SubDirs(), not nested RndDir objects.
    // The flat iteration of the main dir finds game_mode_icon as an object;
    // its PropAnims must be queried by iterating inside that RndDir specifically.
    bool hasIconEnter = false;
    for (ObjDirItr<RndDir> rdit(dir, false); rdit != nullptr; ++rdit) {
        if (std::strcmp(rdit->Name(), "game_mode_icon") == 0) {
            for (ObjDirItr<RndPropAnim> pa((ObjectDir*)&*rdit, false); pa != nullptr; ++pa) {
                if (std::strstr(pa->Name(), "icon_enter") != nullptr)
                    hasIconEnter = true;
            }
        }
    }
    EXPECT_TRUE(hasIconEnter) << "game_mode_icon RndDir missing icon_enter PropAnims";
    EXPECT_GE(rndDirCount, 2) << "Expected at least game_mode_icon + self as RndDir objects";
}
