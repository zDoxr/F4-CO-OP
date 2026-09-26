#pragma once
#include <cstdint>

struct BShkbHkxDB;
struct hkbBehaviorGraph;

struct BShkbAnimationGraph
{
    virtual ~BShkbAnimationGraph() {};

    uint8_t pad8[0x370 - 0x8];

    BShkbHkxDB* hkxDB;
    hkbBehaviorGraph* behaviorGraph;
};
