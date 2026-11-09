#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p work
/usr/bin/time -f 'CONFIGURE elapsed=%e s' cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release 2>&1 | tee work/configure.log
/usr/bin/time -f 'BUILD elapsed=%e s' cmake --build work/build --clean-first -j2 2>&1 | tee work/build.log
/usr/bin/time -f 'PUBLIC wall=%e s' python3 tests/verify.py --binary work/build/mvdb_local --repo . --group public 2>&1 | tee work/public.log
/usr/bin/time -f 'EXTRA wall=%e s' python3 tests/verify.py --binary work/build/mvdb_local --repo . --group extra 2>&1 | tee work/extra.log
ctest --test-dir work/build --output-on-failure -R 'storage_regression|core_regression|simd_regression|index_regression'
