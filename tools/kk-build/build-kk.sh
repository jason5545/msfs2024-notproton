#!/bin/sh
# build-kk.sh — builds KosmicKrisp for NotProton the way fxgl/steamac host/kosmickrisp/build.sh (v1.9.7) does,
# outside the steamac tree, so the launcher 1.9.7 dylib (sha256 98caa61c…) can be rebuilt and extended.
#
#   tools/kk-build/build-kk.sh prepare [our patches…]   fresh source: Mesa MESA_COMMIT + steamac v1.9.7
#                                                       patches 0001-0077 + the given patches, in order
#   tools/kk-build/build-kk.sh build                    (1) mesa_clc/vtn_bindgen2, (2) the driver, util test,
#                                                       stage into $KK/stage-<name> (name = $KK_NAME or "dev")
#   tools/kk-build/build-kk.sh driver                   rebuild only the driver (incremental) and stage it
#
# Layout (persistent, never /tmp):
#   $KK            ~/GitHub/msfs-src/kk
#   $KK/src        Mesa tree (git clone --shared of ~/GitHub/mesa, detached at MESA_COMMIT, patches applied)
#   $KK/steamac-197  git archive of fxgl/steamac v1.9.7 host/kosmickrisp + host/moltenvk/probe
#   $KK/work       venv, build-clc, clc, build
#
# Same meson options as steamac's build.sh: clc tools with Homebrew LLVM (shared) + SPIRV-LLVM-Translator;
# the driver release, -Db_ndebug=true, -Dllvm=disabled -Dmesa-clc=system, --prefer-static,
# MACOSX_DEPLOYMENT_TARGET=26.0, install_name @rpath/libvulkan_kosmickrisp.dylib, ad-hoc signed.
# Skipped from steamac's script: host/moltenvk/repro (needs the steamac MoltenVK build); the probe is
# compiled against Homebrew's vulkan headers instead of MoltenVK's.
set -eu
MESA_COMMIT=ce576c29ed90822ea15564e1e60cb48e455881bb
KK=${KK:-$HOME/GitHub/msfs-src/kk}
src=$KK/src
work=$KK/work
clc=$work/clc
steamac=$KK/steamac-197
NAME=${KK_NAME:-dev}
JOBS=${JOBS:-8}
python=$(command -v python3.14 || command -v python3.13 || command -v python3.12)

prepare() {
	[ -d "$src/.git" ] || git clone -q --shared --no-checkout "$HOME/GitHub/mesa" "$src"
	git -C "$src" checkout -q -f --detach "$MESA_COMMIT"
	git -C "$src" clean -q -fdx
	for p in "$steamac"/kosmickrisp/patches/*.patch "$@"; do
		echo ">> applying $(basename "$p")"
		git -C "$src" apply --whitespace=nowarn "$p"
	done
	echo "steamac patch revision $(cat "$steamac"/kosmickrisp/patches/*.patch | shasum -a 256 | cut -c1-8)"
}

venv() {
	if [ ! -x "$work/venv/bin/python3" ]; then
		mkdir -p "$work"
		"$python" -m venv "$work/venv"
		"$work/venv/bin/pip" -q install "mako==1.3.10" "pyyaml==6.0.3" "packaging==25.0"
	fi
}

build_clc() {
	rm -rf "$work/build-clc" "$clc"
	PATH="$work/venv/bin:$PATH:$(brew --prefix llvm)/bin" meson setup "$work/build-clc" "$src" \
		--prefix="$clc" --buildtype=release -Db_ndebug=true \
		-Dplatforms= -Dvulkan-drivers= -Dgallium-drivers= -Dopengl=false -Dglx=disabled \
		-Degl=disabled -Dgbm=disabled -Dzstd=disabled \
		-Dllvm=enabled -Dshared-llvm=enabled \
		-Dmesa-clc=enabled -Dinstall-mesa-clc=true \
		-Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true > "$work/clc-setup.log"
	PATH="$work/venv/bin:$PATH:$(brew --prefix llvm)/bin" ninja -j "$JOBS" -C "$work/build-clc" install > "$work/clc-build.log"
}

setup_driver() {
	rm -rf "$work/build"
	PATH="$clc/bin:$work/venv/bin:$PATH" MACOSX_DEPLOYMENT_TARGET=26.0 meson setup "$work/build" "$src" \
		--buildtype=release -Db_ndebug=true \
		-Dplatforms=macos -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= -Dopengl=false \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dzstd=disabled -Dexpat=disabled \
		-Dllvm=disabled -Dspirv-tools=disabled -Dmesa-clc=system -Dprecomp-compiler=enabled \
		--prefer-static > "$work/driver-setup.log"
}

build_driver() {
	PATH="$clc/bin:$work/venv/bin:$PATH" MACOSX_DEPLOYMENT_TARGET=26.0 ninja -j "$JOBS" -C "$work/build" > "$work/driver-build.log" ||
		{ grep -E 'error|FAILED' "$work/driver-build.log" | head -40; exit 1; }
}

util_test() {
	uqf=$work/util_queue_finish
	eval xcrun clang $("$work/venv/bin/python3" "$steamac/kosmickrisp/test/util-cflags.py" "$work/build") \
		"$steamac/kosmickrisp/test/util_queue_finish.c" "$work/build/src/util/libmesa_util.a" \
		"$work/build/src/c11/impl/libmesa_util_c11.a" -lz -lpthread -framework CoreFoundation -o "$uqf"
	"$uqf"
}

stage() {
	stage=$KK/stage-$NAME
	rm -rf "$stage"; mkdir -p "$stage"
	staged=$stage/libvulkan_kosmickrisp.dylib
	cp "$work/build/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib" "$staged"
	install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib "$staged"
	if otool -L "$staged" | sed 1,2d | grep -qv -e '^[[:space:]]*/usr/lib/' -e '^[[:space:]]*/System/'; then
		echo "libvulkan_kosmickrisp links non-system libraries:" >&2; otool -L "$staged" >&2; exit 1
	fi
	nm -gU "$staged" | grep -q ' _vk_icdGetInstanceProcAddr$'
	codesign --force -s - "$staged"
	printf '{\n    "file_format_version": "1.0.1",\n    "ICD": {\n        "library_path": "./libvulkan_kosmickrisp.dylib",\n        "api_version": "1.3.0"\n    }\n}\n' > "$stage/kosmickrisp_icd.json"
	loader=$(brew --prefix vulkan-loader)/lib
	xcrun clang -std=c11 -Wall -Werror -O1 -I"$(brew --prefix vulkan-headers)/include" \
		"$steamac/moltenvk/probe/probe.c" -L"$loader" -lvulkan -Wl,-rpath,"$loader" -o "$work/probe"
	VK_DRIVER_FILES=$stage/kosmickrisp_icd.json "$work/probe" > "$stage/probe.txt" 2>&1 || { cat "$stage/probe.txt"; exit 1; }
	{
		echo "KosmicKrisp for NotProton (msfs2024-steamac tools/kk-build/build-kk.sh)"
		echo "mesa:     $MESA_COMMIT + fxgl/steamac v1.9.7 host/kosmickrisp/patches (rev $(cat "$steamac"/kosmickrisp/patches/*.patch | shasum -a 256 | cut -c1-8))"
		echo "extra:    $(git -C "$src" status --porcelain | wc -l | tr -d ' ') changed paths vs MESA_COMMIT"
		echo "driver:   $(sed -n 's/^driver: *//p' "$stage/probe.txt")"
		echo "Xcode:    $(xcodebuild -version | tr '\n' ' ')"
		echo "built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "sha256:   $(shasum -a 256 "$staged" | cut -d' ' -f1)"
	} > "$stage/KOSMICKRISP.txt"
	cat "$stage/KOSMICKRISP.txt"
}

cmd=${1:-build}; [ $# -gt 0 ] && shift
case $cmd in
	prepare) prepare "$@" ;;
	build) venv; build_clc; setup_driver; build_driver; util_test; stage ;;
	driver) [ -f "$work/build/build.ninja" ] || setup_driver; build_driver; stage ;;
	*) echo "usage: $0 prepare [patches…] | build | driver" >&2; exit 2 ;;
esac
