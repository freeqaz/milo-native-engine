#include "Skeleton_Native.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#ifndef __EMSCRIPTEN__
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#ifdef __linux__
#include <linux/limits.h>
#else
#include <limits.h>
#endif
#endif

NativeSkeletonProvider *TheSkeletonProvider = nullptr;

#ifdef __EMSCRIPTEN__
// Stubs — no Kinect skeleton tracking on web
NativeSkeletonProvider::NativeSkeletonProvider() { memset(mFront, 0, sizeof(mFront)); memset(mBack, 0, sizeof(mBack)); memset(mPersons, 0, sizeof(mPersons)); }
NativeSkeletonProvider::~NativeSkeletonProvider() {}
bool NativeSkeletonProvider::Start(const std::string&, const std::string&, int) { return false; }
void NativeSkeletonProvider::Stop() {}
void NativeSkeletonProvider::Poll() {}
int NativeSkeletonProvider::FindByTrackId(int) const { return -1; }
Vector3 NativeSkeletonProvider::NormalizedToMeters(float, float) const { return Vector3(0,0,0); }
void NativeSkeletonProvider::MapCOCOToDC3(const float[][3], PersonData&) {}
void NativeSkeletonProvider::FillSkeleton(Skeleton&, int) const {}
#else

// COCO keypoint indices
enum COCOKeypoint {
    COCO_NOSE = 0,
    COCO_LEFT_EYE = 1,
    COCO_RIGHT_EYE = 2,
    COCO_LEFT_EAR = 3,
    COCO_RIGHT_EAR = 4,
    COCO_LEFT_SHOULDER = 5,
    COCO_RIGHT_SHOULDER = 6,
    COCO_LEFT_ELBOW = 7,
    COCO_RIGHT_ELBOW = 8,
    COCO_LEFT_WRIST = 9,
    COCO_RIGHT_WRIST = 10,
    COCO_LEFT_HIP = 11,
    COCO_RIGHT_HIP = 12,
    COCO_LEFT_KNEE = 13,
    COCO_RIGHT_KNEE = 14,
    COCO_LEFT_ANKLE = 15,
    COCO_RIGHT_ANKLE = 16,
};

NativeSkeletonProvider::NativeSkeletonProvider() {
    memset(mFront, 0, sizeof(mFront));
    memset(mBack, 0, sizeof(mBack));
    memset(mPersons, 0, sizeof(mPersons));
}

NativeSkeletonProvider::~NativeSkeletonProvider() {
    Stop();
}

bool NativeSkeletonProvider::Start(
    const std::string &socketPath, const std::string &modelPath, int cameraIndex
) {
    if (mRunning) return true;

    mSocketPath = socketPath;

    // Launch pose_server.py as child process
    // Resolve script path relative to the executable location
    std::string scriptPath;
    {
        char exePath[PATH_MAX] = {};
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0) {
            exePath[len] = '\0';
            // Walk up from executable (native/build/milo-viewer) to project root
            std::string dir(exePath);
            // Strip executable name
            size_t slash = dir.rfind('/');
            if (slash != std::string::npos) dir = dir.substr(0, slash);
            // Strip "build" directory
            slash = dir.rfind('/');
            if (slash != std::string::npos) dir = dir.substr(0, slash);
            scriptPath = dir + "/scripts/pose_server.py";
        } else {
            scriptPath = "native/scripts/pose_server.py";
        }
    }

    mServerPid = fork();
    if (mServerPid == 0) {
        // Child process
        execlp("python3", "python3",
               scriptPath.c_str(),
               "--socket", socketPath.c_str(),
               "--model", modelPath.c_str(),
               "--camera", std::to_string(cameraIndex).c_str(),
               nullptr);
        // If exec fails
        perror("Failed to launch pose_server.py");
        _exit(1);
    } else if (mServerPid < 0) {
        perror("fork failed");
        return false;
    }

    printf("Launched pose_server.py (pid %d)\n", mServerPid);

    // Wait for socket to appear, then connect
    for (int attempt = 0; attempt < 50; attempt++) {
        usleep(100000); // 100ms

        mSocketFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (mSocketFd < 0) continue;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(mSocketFd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            printf("Connected to pose server\n");
            mRunning = true;
            mReaderThread = std::thread(&NativeSkeletonProvider::ReaderThread, this);
            return true;
        }

        close(mSocketFd);
        mSocketFd = -1;
    }

    fprintf(stderr, "Failed to connect to pose server after 5s\n");
    Stop();
    return false;
}

void NativeSkeletonProvider::Stop() {
    mRunning = false;

    if (mReaderThread.joinable()) {
        mReaderThread.join();
    }

    if (mSocketFd >= 0) {
        close(mSocketFd);
        mSocketFd = -1;
    }

    if (mServerPid > 0) {
        kill(mServerPid, SIGTERM);
        int status;
        waitpid(mServerPid, &status, WNOHANG);
        mServerPid = -1;
    }
}

static bool readExact(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n <= 0) return false;
        p += n;
        remaining -= n;
    }
    return true;
}

void NativeSkeletonProvider::ReaderThread() {
    while (mRunning) {
        // Poll for data with timeout
        struct pollfd pfd;
        pfd.fd = mSocketFd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 100); // 100ms timeout
        if (ret <= 0) continue;

        // Read packet length prefix
        uint32_t packetLen;
        if (!readExact(mSocketFd, &packetLen, 4)) {
            fprintf(stderr, "Pose server disconnected\n");
            mRunning = false;
            break;
        }

        // Read packet
        std::vector<uint8_t> packet(packetLen);
        if (!readExact(mSocketFd, packet.data(), packetLen)) {
            fprintf(stderr, "Pose server read error\n");
            mRunning = false;
            break;
        }

        // Parse header: frame_id (u32), num_persons (u32), timestamp (f64)
        if (packetLen < 16) continue;
        const uint8_t *p = packet.data();

        uint32_t frameId, numPersons;
        double timestamp;
        memcpy(&frameId, p, 4); p += 4;
        memcpy(&numPersons, p, 4); p += 4;
        memcpy(&timestamp, p, 8); p += 8;

        if (numPersons > (uint32_t)kMaxPersons)
            numPersons = kMaxPersons;

        // Parse per-person data
        PersonData newBack[kMaxPersons];
        memset(newBack, 0, sizeof(newBack));

        for (uint32_t i = 0; i < numPersons; i++) {
            int32_t trackId;
            memcpy(&trackId, p, 4); p += 4;

            float cocoKpts[17][3]; // x, y, conf
            memcpy(cocoKpts, p, 17 * 3 * sizeof(float));
            p += 17 * 3 * sizeof(float);

            newBack[i].trackId = trackId;
            MapCOCOToDC3(cocoKpts, newBack[i]);
            newBack[i].valid = true;
        }

        // Swap to back buffer
        {
            std::lock_guard<std::mutex> lock(mSwapMutex);
            memcpy(mBack, newBack, sizeof(mBack));
            mNumPersonsBack = numPersons;
        }
    }
}

void NativeSkeletonProvider::Poll() {
    std::lock_guard<std::mutex> lock(mSwapMutex);
    memcpy(mPersons, mBack, sizeof(mPersons));
    mNumPersons = mNumPersonsBack;
}

int NativeSkeletonProvider::FindByTrackId(int trackId) const {
    for (int i = 0; i < mNumPersons; i++) {
        if (mPersons[i].valid && mPersons[i].trackId == trackId)
            return i;
    }
    return -1;
}

Vector3 NativeSkeletonProvider::NormalizedToMeters(float nx, float ny) const {
    // Map normalized [0,1] camera coords to approximate meter-space
    // Origin at hip center, X right, Y up, Z toward camera
    float x = (nx - 0.5f) * mViewWidth;
    float y = (0.5f - ny) * mViewHeight; // flip Y (image Y is down)
    float z = mViewDepth;
    return Vector3(x, y, z);
}

void NativeSkeletonProvider::MapCOCOToDC3(const float cocoKpts[][3], PersonData &out) {
    // Helper: convert a single COCO keypoint
    auto kpt = [&](int idx) -> Vector3 {
        return NormalizedToMeters(cocoKpts[idx][0], cocoKpts[idx][1]);
    };
    auto kptConf = [&](int idx) -> JointConfidence {
        float c = cocoKpts[idx][2];
        if (c < 0.3f) return kConfidenceNotTracked;
        if (c < 0.6f) return kConfidenceInferred;
        return kConfidenceTracked;
    };
    auto midpoint = [](const Vector3 &a, const Vector3 &b) -> Vector3 {
        return Vector3((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f);
    };
    auto minConf = [](JointConfidence a, JointConfidence b) -> JointConfidence {
        return (a < b) ? a : b;
    };

    // Direct mappings
    out.joints[kJointHead] = kpt(COCO_NOSE);
    out.confidence[kJointHead] = kptConf(COCO_NOSE);

    out.joints[kJointShoulderLeft] = kpt(COCO_LEFT_SHOULDER);
    out.confidence[kJointShoulderLeft] = kptConf(COCO_LEFT_SHOULDER);

    out.joints[kJointElbowLeft] = kpt(COCO_LEFT_ELBOW);
    out.confidence[kJointElbowLeft] = kptConf(COCO_LEFT_ELBOW);

    out.joints[kJointWristLeft] = kpt(COCO_LEFT_WRIST);
    out.confidence[kJointWristLeft] = kptConf(COCO_LEFT_WRIST);

    out.joints[kJointHandLeft] = kpt(COCO_LEFT_WRIST); // no hand keypoint
    out.confidence[kJointHandLeft] = kptConf(COCO_LEFT_WRIST);

    out.joints[kJointShoulderRight] = kpt(COCO_RIGHT_SHOULDER);
    out.confidence[kJointShoulderRight] = kptConf(COCO_RIGHT_SHOULDER);

    out.joints[kJointElbowRight] = kpt(COCO_RIGHT_ELBOW);
    out.confidence[kJointElbowRight] = kptConf(COCO_RIGHT_ELBOW);

    out.joints[kJointWristRight] = kpt(COCO_RIGHT_WRIST);
    out.confidence[kJointWristRight] = kptConf(COCO_RIGHT_WRIST);

    out.joints[kJointHandRight] = kpt(COCO_RIGHT_WRIST);
    out.confidence[kJointHandRight] = kptConf(COCO_RIGHT_WRIST);

    out.joints[kJointHipLeft] = kpt(COCO_LEFT_HIP);
    out.confidence[kJointHipLeft] = kptConf(COCO_LEFT_HIP);

    out.joints[kJointKneeLeft] = kpt(COCO_LEFT_KNEE);
    out.confidence[kJointKneeLeft] = kptConf(COCO_LEFT_KNEE);

    out.joints[kJointAnkleLeft] = kpt(COCO_LEFT_ANKLE);
    out.confidence[kJointAnkleLeft] = kptConf(COCO_LEFT_ANKLE);

    out.joints[kJointHipRight] = kpt(COCO_RIGHT_HIP);
    out.confidence[kJointHipRight] = kptConf(COCO_RIGHT_HIP);

    out.joints[kJointKneeRight] = kpt(COCO_RIGHT_KNEE);
    out.confidence[kJointKneeRight] = kptConf(COCO_RIGHT_KNEE);

    out.joints[kJointAnkleRight] = kpt(COCO_RIGHT_ANKLE);
    out.confidence[kJointAnkleRight] = kptConf(COCO_RIGHT_ANKLE);

    out.joints[kJointFootLeft] = kpt(COCO_LEFT_ANKLE); // approximate
    out.confidence[kJointFootLeft] = kptConf(COCO_LEFT_ANKLE);

    out.joints[kJointFootRight] = kpt(COCO_RIGHT_ANKLE);
    out.confidence[kJointFootRight] = kptConf(COCO_RIGHT_ANKLE);

    // Synthesized joints
    Vector3 hipCenter = midpoint(kpt(COCO_LEFT_HIP), kpt(COCO_RIGHT_HIP));
    Vector3 shoulderCenter = midpoint(kpt(COCO_LEFT_SHOULDER), kpt(COCO_RIGHT_SHOULDER));

    out.joints[kJointHipCenter] = hipCenter;
    out.confidence[kJointHipCenter] = minConf(kptConf(COCO_LEFT_HIP), kptConf(COCO_RIGHT_HIP));

    out.joints[kJointShoulderCenter] = shoulderCenter;
    out.confidence[kJointShoulderCenter] = minConf(kptConf(COCO_LEFT_SHOULDER), kptConf(COCO_RIGHT_SHOULDER));

    out.joints[kJointSpine] = midpoint(hipCenter, shoulderCenter);
    out.confidence[kJointSpine] = minConf(out.confidence[kJointHipCenter], out.confidence[kJointShoulderCenter]);
}

void NativeSkeletonProvider::FillSkeleton(Skeleton &skel, int personIdx) const {
    if (personIdx < 0 || personIdx >= mNumPersons || !mPersons[personIdx].valid)
        return;
    FillSkeleton(skel, mPersons[personIdx]);
}

void NativeSkeletonProvider::FillSkeleton(Skeleton &skel, const PersonData &person) const {
    // Access protected members directly via friend declaration (LP64-safe)
    for (int j = 0; j < kNumJoints; j++) {
        skel.mTrackedJoints[j].mJointPos[kCoordCamera] = person.joints[j];
        skel.mTrackedJoints[j].mSmoothedPos = person.joints[j];
        skel.mTrackedJoints[j].mJointConf = person.confidence[j];
    }

    skel.mTracking = kSkeletonTracked;
    skel.mTrackingID = person.trackId;
}

#endif // !__EMSCRIPTEN__

void NativeSkeletonProvider::FillDummySkeleton(Skeleton &skel) {
    // Neutral standing pose — hands at sides, below hip height.
    // Passes quality filter (20 confident joints, not sitting/sideways)
    // but gesture filters see disengaged player (hands below hips).
    static const struct { SkeletonJoint joint; float x, y, z; } kPose[] = {
        { kJointHipCenter,       0.00f, 0.90f, 2.0f },
        { kJointSpine,           0.00f, 1.10f, 2.0f },
        { kJointShoulderCenter,  0.00f, 1.40f, 2.0f },
        { kJointHead,            0.00f, 1.60f, 2.0f },
        { kJointShoulderLeft,   -0.20f, 1.40f, 2.0f },
        { kJointElbowLeft,      -0.25f, 1.15f, 2.0f },
        { kJointWristLeft,      -0.22f, 0.90f, 2.0f },
        { kJointHandLeft,       -0.22f, 0.85f, 2.0f },
        { kJointShoulderRight,   0.20f, 1.40f, 2.0f },
        { kJointElbowRight,      0.25f, 1.15f, 2.0f },
        { kJointWristRight,      0.22f, 0.90f, 2.0f },
        { kJointHandRight,       0.22f, 0.85f, 2.0f },
        { kJointHipLeft,        -0.12f, 0.85f, 2.0f },
        { kJointKneeLeft,       -0.12f, 0.45f, 2.0f },
        { kJointAnkleLeft,      -0.12f, 0.05f, 2.0f },
        { kJointHipRight,        0.12f, 0.85f, 2.0f },
        { kJointKneeRight,       0.12f, 0.45f, 2.0f },
        { kJointAnkleRight,      0.12f, 0.05f, 2.0f },
        { kJointFootLeft,       -0.12f, 0.00f, 2.0f },
        { kJointFootRight,       0.12f, 0.00f, 2.0f },
    };

    for (const auto &j : kPose) {
        Vector3 pos(j.x, j.y, j.z);
        skel.mTrackedJoints[j.joint].mJointPos[kCoordCamera] = pos;
        skel.mTrackedJoints[j.joint].mSmoothedPos = pos;
        skel.mTrackedJoints[j.joint].mJointConf = kConfidenceTracked;
    }

    skel.mTracking = kSkeletonTracked;
    skel.mTrackingID = 1;
}
