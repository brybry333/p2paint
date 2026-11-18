#!/usr/bin/env bash
# build.sh: p2paint.dll for 64-bit Windows CS:S, cross-compiled on Linux into win/build/out/ (clang-cl + xwin's MSVC
# CRT/SDK: the MSVC ABI like the game, so the SDK headers give the Windows interface layouts). Same sources as the
# Linux build (p2paint_platform_win.cpp instead of _linux), plus the SDK's tier1 / mathlib compiled here (the SDK
# ships no Windows build of them). Loaded by anything that LoadLibrary's it: the bhoptools fork, tools/wininject.exe.
# Needs clang, lld, mingw-w64-gcc (wininject), an xwin splat in ~/.local/share/xwin (or XWIN=) and the SDK in
# third_party/. win/sdk-compat = two SDK headers patched for clang-cl. RTTI stays on (-GR): the engine
# dynamic_casts our ConVars (without it, typing p2paint_version or a server's query of it crashed the game).
# Run it from a path without spaces (a symlink to this folder is fine): the compiler flags break on them.
set -euo pipefail
C=$(cd "$(dirname "$0")/.." && pwd -L)
W=$C/win
X=${XWIN:-$HOME/.local/share/xwin}
B=$W/build
OUT=$B/out
mkdir -p "$OUT" "$B/obj/tier1" "$B/obj/mathlib" "$B/obj/p2paint" "$B/gen"
SDK=$C/third_party/source-sdk-2013/src
MSVC="--target=x86_64-pc-windows-msvc -nologo -imsvc $X/crt/include -imsvc $X/sdk/include/ucrt -imsvc $X/sdk/include/um -imsvc $X/sdk/include/shared"
LIBS="-libpath:$X/crt/lib/x86_64 -libpath:$X/sdk/lib/um/x86_64 -libpath:$X/sdk/lib/ucrt/x86_64 -libpath:$SDK/lib/public/x64"
CF="$MSVC /clang:-ffile-prefix-map=$C=. -O2 -MT -EHs-c- -D_HAS_EXCEPTIONS=0 -GR -Wno-everything -D_WIN32 -DWIN32 -D_WINDOWS \
    -D_WIN64 -DCOMPILER_MSVC -DCOMPILER_MSVC64 -DPLATFORM_64BITS -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DNDEBUG -DSOURCESDK \
    -I$W/sdk-compat -I$SDK/public -I$SDK/public/tier0 -I$SDK/public/tier1 -I$SDK/common \
    -FI$W/sdk-compat/tier1/bitbuf.h -FI$W/sdk-compat/mathlib/ssemath.h"

# the same lists as the Linux Makefile, Windows files where the SDK has them; no bitbuf / snappy / NetAdr / uniqueid
# (network messages, compression, addresses, UUIDs: unused, and they need clang-cl fixes or more system libraries)
TIER1="byteswap characterset checksum_crc checksum_md5 checksum_sha1 commandbuffer convar datamanager
       diff exprevaluator generichash ilocalize interface KeyValues keyvaluesjson kvpacker lzmaDecoder lzss mempool memstack
       splitstring processor_detect rangecheckedvar reliabletimer stringpool strtools strtools_unicode tier1
       tokenreader sparsematrix utlbuffer utlbufferutil utlstring utlsymbol utlbinaryblock"
MATH="color_conversion halton lightdesc mathlib_base powsse sparse_convolution_noise sseconst sse 3dnow ssenoise anorms
      bumpvects IceKey imagequant polyhedron quantize randsse spherical simdvectormatrix vector vmatrix almostequal"

python3 "$C/tools/embed.py" "$C/game" "$B/gen/p2paint_files.h"

compile() {	# <obj dir> <extra flags> <sources...>: changed ones only, 8 at a time
    local dir=$1 extra=$2; shift 2
    local f n todo=()
    for f in "$@"; do
        n=$dir/$(basename "${f%.*}").obj
        [[ $n -nt $f ]] || todo+=("$f")
    done
    (( ${#todo[@]} )) || return 0
    printf '%s\n' "${todo[@]}" | xargs -P 8 -I{} sh -c 'clang-cl '"$CF $extra"' -c "{}" -Fo"'"$dir"'/$(basename "{}" | sed "s/\.[^.]*$//").obj" || exit 255'
}

echo "== tier1, mathlib (SDK)"
compile "$B/obj/tier1" "-DTIER1_STATIC_LIB" $(for f in $TIER1; do echo "$SDK/tier1/$f.cpp"; done)
compile "$B/obj/mathlib" "-DMATHLIB_LIB -I$SDK/public/mathlib" $(for f in $MATH; do [[ $f == vector ]] || echo "$SDK/mathlib/$f.cpp"; done)
# vector.cpp defines vec3_origin non-const, mathlib.h (pulled in by the forced ssemath.h) declares it const
[[ $B/obj/mathlib/vector.obj -nt $SDK/mathlib/vector.cpp ]] || \
    clang-cl ${CF//-FI*/} -DMATHLIB_LIB -I$SDK/public/mathlib -c "$SDK/mathlib/vector.cpp" -Fo"$B/obj/mathlib/vector.obj"
[[ $B/obj/tier1/LzmaDec.obj -nt $SDK/utils/lzma/C/LzmaDec.c ]] || \
    clang-cl $MSVC -O2 -MT -Wno-everything -TC -c "$SDK/utils/lzma/C/LzmaDec.c" -Fo"$B/obj/tier1/LzmaDec.obj"

echo "== p2paint"
rm -f "$B"/obj/p2paint/*.obj	# (ours change often and share headers: always rebuilt)
compile "$B/obj/p2paint" "-I$C/src -I$B/gen" $(ls "$C"/src/*.cpp | grep -v p2paint_platform_linux)

echo "== p2paint.dll"
lld-link -nologo -dll -out:"$OUT/p2paint.dll" $LIBS "$B"/obj/p2paint/*.obj "$B"/obj/tier1/*.obj "$B"/obj/mathlib/*.obj \
    tier0.lib vstdlib.lib tier2.lib libcmt.lib libvcruntime.lib libucrt.lib kernel32.lib user32.lib advapi32.lib

echo "== wininject.exe (tests)"
x86_64-w64-mingw32-gcc -O2 -Wall -municode -o "$OUT/wininject.exe" "$C/tools/wininject.c" -static
ls -la "$OUT"
