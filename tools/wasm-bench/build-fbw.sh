#!/bin/sh
# Builds the FBW A380X systems benchmark: fbw/lib.rs (with flybywiresim/aircraft's a380_systems and
# systems crates) -> wasm32-unknown-unknown -> wasm2c -> build/fbw/fbwbench.exe (x64 PE, -march=x86-64-v2)
# and build/fbw/fbwbench-mac (arm64 macOS, the same C).
#
#   FBW=<flybywiresim/aircraft checkout> build-fbw.sh
#
# FBW defaults to $MSFS_SRC/ref/fbw-aircraft (a sparse checkout of Cargo.*, rust-toolchain.toml,
# fbw-common/src/wasm/systems and fbw-a380x/src/wasm/systems is enough). Rust: RUSTUP_HOME/CARGO_HOME
# default to $MSFS_SRC/rust (rustup with 1.96.0, the repo's toolchain, and wasm32-unknown-unknown).
# The crate is generated in $MSFS_SRC/fbw-bench, so this repo carries no path of the machine.
set -eu
T=$(cd "$(dirname "$0")" && pwd)
MSFS_SRC=${MSFS_SRC:-$HOME/GitHub/msfs-src}
FBW=$(cd "${FBW:-$MSFS_SRC/ref/fbw-aircraft}" && pwd)
export RUSTUP_HOME=${RUSTUP_HOME:-$MSFS_SRC/rust/rustup} CARGO_HOME=${CARGO_HOME:-$MSFS_SRC/rust/cargo}
CARGO="$CARGO_HOME/bin/cargo"
LLVM=/opt/homebrew/opt/llvm/bin
WABT=$(brew --prefix wabt)
RT=$WABT/share/wabt/wasm2c
[ -f "$RT/wasm-rt-impl.c" ] || RT=$(dirname "$(find "$WABT/" -name wasm-rt-impl.c | head -1)")
MINGW=${MINGW:-$HOME/GitHub/squark-app/scratch/downloads/llvm-mingw-20261006-ucrt-macos-universal}

C=$MSFS_SRC/fbw-bench
mkdir -p "$C/src"
cp "$T/fbw/lib.rs" "$C/src/lib.rs"
cp "$FBW/Cargo.lock" "$C/Cargo.lock"
cat > "$C/Cargo.toml" <<EOF
[package]
name = "fbw"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[dependencies]
a380_systems = { path = "$FBW/fbw-a380x/src/wasm/systems/a380_systems" }
systems = { path = "$FBW/fbw-common/src/wasm/systems/systems" }
getrandom = "0.3"
uom = { version = "0.37.0" }

[profile.release]
lto = true
opt-level = 3
panic = "abort"

[workspace]
EOF
# The aircraft's workspace dependencies (uom.workspace = true, ...) resolve against its own Cargo.toml.
sed -n '/^\[workspace.dependencies\]/,/^\[/p' "$FBW/Cargo.toml" | sed '$d' >> "$C/Cargo.toml"
(cd "$C" && RUSTFLAGS='--cfg getrandom_backend="custom" -C target-feature=+bulk-memory' \
  "$CARGO" +1.96.0 build --release --target wasm32-unknown-unknown)

O=$T/build/fbw
mkdir -p "$O"
cp "$C/target/wasm32-unknown-unknown/release/fbw.wasm" "$O/fbw.wasm"
wasm2c --module-name=fbw "$O/fbw.wasm" -o "$O/fbw.c"
SRC="$T/fbw/host-fbw.c $O/fbw.c $RT/wasm-rt-impl.c $RT/wasm-rt-mem-impl.c"
INC="-I$O -I$WABT/include -I$RT"
"$MINGW/bin/x86_64-w64-mingw32-clang" -O2 -march=x86-64-v2 $INC $SRC -o "$O/fbwbench.exe" -static &
"$LLVM/clang" -O2 -mcpu=apple-m1 $INC $SRC -o "$O/fbwbench-mac" &
wait
ls -l "$O/fbw.wasm" "$O/fbwbench.exe"
"$LLVM/llvm-size" -A "$O/fbwbench.exe" | awk '$1==".text"{print "x64_text_bytes=" $2}'
