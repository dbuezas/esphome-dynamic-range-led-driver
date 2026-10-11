#!/usr/bin/env bash
# Build and run the host tests. Needs only a C++17 compiler.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Wextra -I stubs -I ../components -o /tmp/test_dynamic_range \
    test_dynamic_range.cpp ../components/dynamic_range/dynamic_range.cpp
/tmp/test_dynamic_range
c++ -std=c++17 -Wall -Wextra -I stubs -I ../components -o /tmp/test_bp5758d_dither \
    test_bp5758d_dither.cpp ../components/bp5758d_dither/bp5758d_dither.cpp \
    ../components/dynamic_range/dynamic_range.cpp
/tmp/test_bp5758d_dither
