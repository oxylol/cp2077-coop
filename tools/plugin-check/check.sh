#!/usr/bin/env bash
# Compile-checks the Windows game plugin (src/plugin) on Linux against the real RED4ext.SDK and RedLib headers,
# using zig's clang with a mingw target. Catches C++ and API mistakes without Windows or MSVC; nothing is linked.
# MSVC-only layout asserts are switched off (mingw lays some types out differently); everything else is a real
# compile.
#
#   tools/plugin-check/check.sh
#
# Needs: python3 -m pip install ziglang; the SDKs (vendor/ after tools/dev/bootstrap.ps1, or set RED4EXT_SDK and
# REDLIB); GameNetworkingSockets headers (found in ~/.xmake/packages after a Linux `xmake`, or set GNS_INCLUDE).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
sdk="${RED4EXT_SDK:-$repo/vendor/RED4ext.SDK}"
redlib="${REDLIB:-$repo/vendor/RedLib}"
gns="${GNS_INCLUDE:-}"
if [ -z "$gns" ]; then
    header="$(find "$HOME/.xmake/packages" -name steamnetworkingsockets.h -path '*GameNetworkingSockets*' 2>/dev/null | head -1 || true)"
    [ -n "$header" ] && gns="$(dirname "$header")"
fi
for dir in "$sdk/include" "$redlib/include" "$gns"; do
    if [ -z "$dir" ] || [ ! -d "$dir" ]; then
        echo "missing headers: '$dir' (see the comment at the top of this script)" >&2
        exit 2
    fi
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Windows file names are case-insensitive; the SDK and RedLib include a few headers with a different case.
mkdir -p "$work/shim/RED4ext/rtti"
echo '#include <windows.h>' > "$work/shim/Windows.h"
for name in IType.hpp IType-inl.hpp ERTTIType.hpp; do
    echo "#include \"$sdk/include/RED4ext/RTTI/$name\"" > "$work/shim/RED4ext/rtti/$name"
done

rc=0
for f in "$repo"/src/plugin/*.cpp; do
    wrapper="$work/w_$(basename "$f")"
    printf '#include "%s"\n#define RED4EXT_ASSERT_SIZE(cls, size) static_assert(true)\n#define RED4EXT_ASSERT_OFFSET(cls, mbr, off)\n#include "%s"\n' \
        "$here/prelude_win.h" "$f" > "$wrapper"
    out="$(python3 -m ziglang c++ -target x86_64-windows-gnu -std=c++20 -c -o /dev/null -w \
        -DRED4EXT_HEADER_ONLY -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DSTEAMNETWORKINGSOCKETS_STATIC_LINK \
        -I"$work/shim" -I"$repo/src" -I"$sdk/include" -I"$redlib/include" -I"$redlib/vendor" -I"$gns" \
        "$wrapper" 2>&1 | grep -E "error|fatal" | grep -v "In file included" || true)"
    if [ -n "$out" ]; then
        echo "== $(basename "$f")"
        echo "$out" | head -15
        rc=1
    else
        echo "ok $(basename "$f")"
    fi
done
exit $rc
