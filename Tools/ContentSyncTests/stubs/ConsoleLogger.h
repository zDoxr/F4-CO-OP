#pragma once
#include "TestStubs.h"
#define LOG_INFO(fmt, ...)  COSYNC_TEST_LOG("[INFO] ", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  COSYNC_TEST_LOG("[WARN] ", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) COSYNC_TEST_LOG("[ERROR]", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) COSYNC_TEST_LOG("[DEBUG]", fmt, ##__VA_ARGS__)
