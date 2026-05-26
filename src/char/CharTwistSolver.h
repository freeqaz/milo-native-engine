#pragma once

class ObjectDir;
class CharPollable;

namespace CharTwistSolver {
    void SolveAll(ObjectDir* dir);
    bool IsTwistPollable(const CharPollable* p);
    bool IsDriverPollable(const CharPollable* p);
}
