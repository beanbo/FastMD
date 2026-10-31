#!/usr/bin/env bash
# Cross-builds the window-free half of FastMD - the parser, the HTML reader, edit mode's core - with mingw-w64 on
# Linux, and runs it under Wine. No window, no DirectWrite: only what the edit tests, the fuzz harness and the
# benchmarks link. The real build is MSVC (app/build.ps1); this is for CI on Linux and for working without Windows.
#
#   app/tests/mingw/build.sh                 # build the edit tests, both fuzz harnesses and the benchmarks
#   app/tests/mingw/build.sh test            # … and run the edit tests
#   app/tests/mingw/build.sh bench [args]    # … and run the benchmarks (args go to fastmd-bench)
#   app/tests/mingw/build.sh fuzz [args]     # … and run the fuzz harness (args go to fastmd-fuzz)
#   app/tests/mingw/build.sh regress         # … and replay the fuzz regression inputs (tests/fuzz/regress)
#   app/tests/mingw/build.sh target --seconds 600   # … and mutate file bytes through fastmd-fuzz-target
#
# Needs: g++-mingw-w64-x86-64-posix and wine64 (Debian / Ubuntu: apt install mingw-w64 wine64).
# Output: app/build/mingw/ (or $MINGW_OUT). CXXFLAGS_EXTRA / LDFLAGS_EXTRA add flags (e.g. -pg for gprof).
set -euo pipefail
app="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${MINGW_OUT:-$app/build/mingw}"
mkdir -p "$out/obj" "$out/generated"
cxx=x86_64-w64-mingw32-g++-posix
cc=x86_64-w64-mingw32-gcc-posix
command -v "$cxx" >/dev/null || { echo "build.sh: $cxx not found (apt install mingw-w64)" >&2; exit 1; }

ver=$(sed -n 's/^project(FastMD VERSION \([0-9.]*\).*/\1/p' "$app/CMakeLists.txt")
IFS=. read -r vmaj vmin vpat <<<"$ver"
sed -e "s/@PROJECT_VERSION_MAJOR@/$vmaj/;s/@PROJECT_VERSION_MINOR@/$vmin/;s/@PROJECT_VERSION_PATCH@/$vpat/" \
    -e "s/@PROJECT_VERSION@/$ver/g" "$app/src/version.h.in" >"$out/generated/version.h.new"
cmp -s "$out/generated/version.h.new" "$out/generated/version.h" || mv "$out/generated/version.h.new" "$out/generated/version.h"

defs=(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DMD4C_USE_UTF16 -DFASTMD_NO_APP)
# -D_GLIBCXX_ASSERTIONS: bounds checks in the standard containers - the nearest thing to a sanitizer mingw has
flags=(-std=c++20 -O2 -g -municode -D_GLIBCXX_ASSERTIONS -Wall -Wno-unused-function -Wno-sign-compare
       -Wno-unknown-pragmas -Wno-missing-braces -Wno-unused-variable -Wno-parentheses -Wno-int-in-bool-context ${CXXFLAGS_EXTRA:-})
inc=(-I"$out/generated" -I"$app/src")

# compile one source if it is newer than its object (or the object is missing)
objs=()
compile() {  # compile <source> [extra flags…]
    local src="$1"; shift
    local obj="$out/obj/$(basename "${src%.*}").o"
    if [[ ! -f "$obj" || "$src" -nt "$obj" || -n "$(find "$app/src" "$app/tests" -name '*.h' -newer "$obj" -print -quit)" ]]; then
        echo "  cc $(realpath --relative-to="$app" "$src")"
        if [[ "$src" == *.c ]]; then
            "$cc" -O2 -g "${defs[@]}" -c "$src" -o "$obj"
        else
            "$cxx" "${flags[@]}" "${defs[@]}" "${inc[@]}" "$@" -c "$src" -o "$obj"
        fi
    fi
    objs+=("$obj")
}

core=(src/parse.cpp src/html.cpp src/highlight.cpp src/util.cpp src/theme.cpp src/editcore.cpp src/editops.cpp
      src/editfile.cpp)
for s in "${core[@]}"; do compile "$app/$s"; done
compile "$app/third_party/md4c/md4c.c"
coreobjs=("${objs[@]}")
libs=(-static -luser32 -ladvapi32 -lshlwapi -ldwmapi)

link() {  # link <exe> <entry: main|wmain> <main source> [extra flags…]
    local exe="$1" entry="$2" main="$3"; shift 3
    objs=()
    compile "$app/$main" "$@"
    local lf=("${libs[@]}")
    [[ "$entry" == wmain ]] && lf+=(-municode)
    "$cxx" ${LDFLAGS_EXTRA:-} -o "$out/$exe" "${objs[@]}" "${coreobjs[@]}" "${lf[@]}"
}
# the edit tests find their cases through FASTMD_APP_DIR: a Windows path Wine can open (Z: is the Linux root)
link fastmd-edit-tests.exe wmain tests/edit/edit_tests.cpp "-DFASTMD_APP_DIR=\"Z:${app}\""
link fastmd-fuzz.exe wmain tests/fuzz/fuzz.cpp
link fastmd-bench.exe main tests/bench/bench.cpp
link fastmd-fuzz-target.exe wmain tests/fuzz/fuzz_target.cpp -DFASTMD_FUZZ_STANDALONE
echo "built $out/fastmd-edit-tests.exe, fastmd-fuzz.exe, fastmd-fuzz-target.exe, fastmd-bench.exe"

wine=$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)
export WINEDEBUG=-all
mode="${1:-}"
[[ $# -gt 0 ]] && shift
case "$mode" in
    "") ;;
    test) "$wine" "$out/fastmd-edit-tests.exe" "$@" ;;
    bench) cd "$app/tests" && "$wine" "$out/fastmd-bench.exe" --corpus ../../bench/corpus "$@" ;;
    fuzz) mkdir -p "$app/tests/out/fuzz" && cd "$app/tests" && \
          "$wine" "$out/fastmd-fuzz.exe" --corpus 'Z:'"$app"'/../bench/corpus' "$@" ;;
    regress) mkdir -p "$app/tests/out/fuzz" && cd "$app/tests" && "$wine" "$out/fastmd-fuzz-target.exe" 'Z:'"$app"'/tests/fuzz/regress' fuzz/seed-broken-html.md "$@" ;;
    target) mkdir -p "$app/tests/out/fuzz" && cd "$app/tests" && \
            "$wine" "$out/fastmd-fuzz-target.exe" "$@" 'Z:'"$app"'/../bench/corpus' 'Z:'"$app"'/tests' ;;
    *) echo "build.sh: unknown mode $mode (test, bench, fuzz, regress, target)" >&2; exit 2 ;;
esac
