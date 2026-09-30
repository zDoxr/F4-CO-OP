#pragma once
#include "TestStubs.h"
// Only the declaration CoSyncContentSync.cpp makes; the offline build never
// reads it (SnapshotLoadedPlugins uses the test lists).
struct DataHandler {};
template <class T> struct RelocPtr { T operator*() const { return T(); } };
