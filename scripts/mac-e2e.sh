#!/bin/bash

./scripts/configure.sh mac-release
./scripts/build.sh mac-release
ctest --test-dir build/mac-release
./scripts/deploy-mac.sh
