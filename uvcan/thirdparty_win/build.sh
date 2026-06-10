#!/usr/bin/env bash
#
# Builds the third-party libraries needed by the Windows (mingw-w64) UI build
# of uvcan: GLFW (windowing/GL context/input) and FreeType (font rendering),
# as static libs, plus stages GLEW's amalgamated source (compiled straight into
# uvcan). The headers + .a files are staged into include/ and lib/, which the
# makefile's win target points at (-Ithirdparty_win/include -Lthirdparty_win/lib).
#
# Requires: x86_64-w64-mingw32-gcc, cmake, curl. Run from this directory:
#   ./build.sh
#
set -euo pipefail
cd "$(dirname "$0")"

GLFW_VER=3.4
GLEW_VER=2.2.0
FT_VER=2.13.3

mkdir -p src include lib
cd src

# --- download official sources (idempotent) ---
[ -f glfw.zip ]        || curl -fsSL -o glfw.zip        "https://github.com/glfw/glfw/releases/download/${GLFW_VER}/glfw-${GLFW_VER}.zip"
[ -f glew.tgz ]        || curl -fsSL -o glew.tgz        "https://github.com/nigels-com/glew/releases/download/glew-${GLEW_VER}/glew-${GLEW_VER}.tgz"
[ -f freetype.tar.gz ] || curl -fsSL -o freetype.tar.gz "https://download.savannah.gnu.org/releases/freetype/freetype-${FT_VER}.tar.gz"

unzip -q -o glfw.zip
tar xzf glew.tgz
tar xzf freetype.tar.gz

TOOLCHAIN="$(pwd)/../mingw-toolchain.cmake"

# --- GLFW (static) ---
cmake -S glfw-${GLFW_VER} -B glfw-${GLFW_VER}/build -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
	-DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF \
	-DGLFW_BUILD_WAYLAND=OFF -DGLFW_BUILD_X11=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build glfw-${GLFW_VER}/build -j"$(nproc)"

# --- FreeType (static, minimal: no zlib/png/harfbuzz/brotli/bzip2) ---
cmake -S freetype-${FT_VER} -B freetype-${FT_VER}/build -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
	-DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
	-DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
cmake --build freetype-${FT_VER}/build -j"$(nproc)"

# --- stage headers, libs and GLEW source ---
cd ..
rm -rf include/GLFW include/GL include/freetype include/ft2build.h
cp -r src/glfw-${GLFW_VER}/include/GLFW       include/
cp -r src/glew-${GLEW_VER}/include/GL         include/
cp    src/freetype-${FT_VER}/include/ft2build.h include/
cp -r src/freetype-${FT_VER}/include/freetype include/
cp    src/glfw-${GLFW_VER}/build/src/libglfw3.a    lib/
cp    src/freetype-${FT_VER}/build/libfreetype.a   lib/
cp    src/glew-${GLEW_VER}/src/glew.c              glew.c

echo "Done. Staged: $(ls lib/) and include/{GLFW,GL,freetype,ft2build.h} + glew.c"
