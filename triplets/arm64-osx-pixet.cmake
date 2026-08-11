# Overlay triplet: vcpkg's builtin arm64-osx, plus a pinned macOS deployment target.
#
# Every line except the last is identical to vcpkg/triplets/arm64-osx.cmake. The addition is
# what makes the build distributable rather than machine-local: without
# VCPKG_OSX_DEPLOYMENT_TARGET each port compiles against the SDK's default minimum (macOS 26
# on the machine this was ported on), and linking those objects into an app that targets 12.0
# both warns on every single dependency and leaves the binary's real floor as whichever value
# is higher. The symptom is invisible locally and total elsewhere: the .app simply refuses to
# launch on any macOS older than the machine that built it.
#
# Must stay in step with CMAKE_OSX_DEPLOYMENT_TARGET in the top-level CMakeLists.txt.
#
# Selected via VCPKG_TARGET_TRIPLET + VCPKG_OVERLAY_TRIPLETS in CMakePresets.json's mac-base.
# Note that changing the deployment target changes every port's ABI hash, so the first
# configure after touching this rebuilds the whole dependency set from source (~30 min) and
# gets no hits from an existing binary cache.

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 12.0)
