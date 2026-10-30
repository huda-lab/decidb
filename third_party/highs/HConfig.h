#ifndef HCONFIG_H_
#define HCONFIG_H_

#define FAST_BUILD
#define CUPDLP_CPU
#define CMAKE_BUILD_TYPE "Release"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define HIGHS_HAVE_MM_PAUSE
#endif

#if defined(__GNUC__) || defined(__clang__)
#define HIGHS_HAVE_BUILTIN_CLZ
#endif

#if defined(_MSC_VER)
#define HIGHS_HAVE_BITSCAN_REVERSE
#endif

#define HIGHS_GITHASH "04024d701"
#define HIGHS_VERSION_MAJOR 1
#define HIGHS_VERSION_MINOR 15
#define HIGHS_VERSION_PATCH 1
#define HIGHS_EXTRAS_VERSION "1.15.1"

#endif
