cmake_minimum_required(VERSION 3.28)

# Must be set before project(): the deployment target is consumed while CMake probes the
# compiler, so setting it afterwards leaves the probe (and every try_compile) on a different
# floor than the real build.
#
# Deliberately NOT wrapped in if(APPLE) - APPLE is only defined *by* project(), so a guard
# here would silently never run, leaving the variable empty and the binary on the SDK
# default. CMake ignores this variable on non-Apple platforms, so unconditional is both
# correct and the common idiom.
#
# Without it the binary inherits the build machine's SDK default - macOS 26 on the machine
# this was ported on - and then refuses to launch on anything older, which nobody notices
# until someone else tries to run it. 12.0 is Qt 6.8's own floor (its official binaries
# report minos 12.0), so going lower would only link against a Qt that can't run there
# anyway, and every Apple Silicon Mac has been offered macOS 12+.
#
# Has to agree with VCPKG_OSX_DEPLOYMENT_TARGET in triplets/arm64-osx-pixet.cmake. If the two
# drift, every dependency link emits "object file was built for newer macOS version than
# being linked" and the effective floor becomes whichever is higher.
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version supported")

# CMAKE_OSX_ARCHITECTURES is deliberately left unset: CMake defaults it to the host
# architecture, which is the intended arm64-only build, and it stays overridable. The Qt
# install is already a universal x86_64+arm64 binary, so adding Intel later means configuring
# with -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" and building the vcpkg dependencies for
# x64-osx too - not restructuring anything here.

# VERSION here is the single source of truth for the version string: src/core/version.cpp
# reads it via the PIXET_VERSION compile definition (see src/core/CMakeLists.txt), and the
# .app bundle's CFBundleShortVersionString comes from the same place. Previously it was
# hardcoded in version.cpp alone, which the bundle metadata had no way to reach.
project(pixet VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/index)
add_subdirectory(src/app)
add_subdirectory(tests)
