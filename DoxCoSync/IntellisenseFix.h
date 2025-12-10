#pragma once

//
// IntelliSense-only fix for F4SE STATIC_ASSERT false positives
//
#ifdef __INTELLISENSE__
#ifdef STATIC_ASSERT
#undef STATIC_ASSERT
#endif
#define STATIC_ASSERT(x)
#endif
