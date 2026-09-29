Opus 1.5.2 (https://opus-codec.org), BSD licence -- see COPYING.

Source: https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
SHA-256: 65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1

Lib/opus.lib is a static x64 Release build with the static CRT (/MT), to
match DoxCoSync. Built with the CMake shipped in Visual Studio 18:

  cmake -S opus-1.5.2 -B build -A x64 -DOPUS_STATIC_RUNTIME=ON
        -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DBUILD_TESTING=OFF
  cmake --build build --config Release

Include/ holds the public headers from opus-1.5.2/include.
