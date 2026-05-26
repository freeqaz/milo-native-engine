#pragma once

#include "gesture/BaseSkeleton.h"
#include "gesture/Skeleton.h"
#include <string>
#include <thread>
#include <mutex>
// <atomic> not usable with clang + GCC 15 headers; use volatile bool instead
// #include <atomic>

// Maps COCO 17 keypoints from YOLO pose to DC3's 20 SkeletonJoints,
// receives data from pose_server.py over a Unix socket.
class NativeSkeletonProvider {
public:
    static const int kCOCOKeypoints = 17;
    static const int kMaxPersons = 6;

    struct PersonData {
        int trackId = -1;
        Vector3 joints[kNumJoints];           // DC3 20-joint positions (meters)
        JointConfidence confidence[kNumJoints];
        bool valid = false;
    };

    NativeSkeletonProvider();
    ~NativeSkeletonProvider();

    bool Start(const std::string &socketPath = "/tmp/dc3_pose.sock",
               const std::string &modelPath = "yolo11n-pose.pt",
               int cameraIndex = 0);
    void Stop();
    bool IsRunning() const { return mRunning; }

    // Call each frame to read latest data
    void Poll();

    // Access tracked persons (thread-safe snapshot from last Poll)
    int NumPersons() const { return mNumPersons; }
    const PersonData &GetPerson(int idx) const { return mPersons[idx]; }

    // Find person by BOTSORT track ID, returns -1 if not found
    int FindByTrackId(int trackId) const;

    // Fill a Skeleton object from person data (by index or direct PersonData)
    void FillSkeleton(Skeleton &skel, int personIdx) const;
    void FillSkeleton(Skeleton &skel, const PersonData &person) const;

    // Fill a skeleton with a neutral standing pose (hands at sides).
    // Used as fallback when no pose server is connected.
    static void FillDummySkeleton(Skeleton &skel);

private:
    void ReaderThread();
    void MapCOCOToDC3(const float cocoKpts[][3], PersonData &out);
    Vector3 NormalizedToMeters(float nx, float ny) const;

    std::string mSocketPath;
    int mSocketFd = -1;
    pid_t mServerPid = -1;

    std::thread mReaderThread;
    volatile bool mRunning = false;

    // Double-buffered: reader writes to mBack, Poll() swaps to mFront
    std::mutex mSwapMutex;
    PersonData mFront[kMaxPersons];
    PersonData mBack[kMaxPersons];
    int mNumPersonsFront = 0;
    int mNumPersonsBack = 0;
    int mNumPersons = 0;
    PersonData mPersons[kMaxPersons]; // Snapshot for game thread

    // Coordinate mapping: approximate camera FOV to meter space
    // Default assumes ~2m viewing distance, 1.2m horizontal FOV
    float mViewWidth = 2.4f;   // meters across camera view
    float mViewHeight = 1.8f;  // meters vertical
    float mViewDepth = 3.0f;   // meters from camera to subject (fixed Z)
};

extern NativeSkeletonProvider *TheSkeletonProvider;
