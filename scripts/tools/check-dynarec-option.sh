#!/usr/bin/env bash
# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#
# Proves the OPT_DYNAREC block in CMakeLists.txt: configures the tree in
# every state the option has, on the host CPU and on a simulated ppc64le,
# and checks the flags that reach dosbox_config.h. With --full it also
# builds and runs the test suite where a build is expected to succeed,
# and confirms the forced recompiler fails where the warning says it will.
#
# Usage: scripts/tools/check-dynarec-option.sh [--full] [--toolchain FILE]
#   --toolchain FILE   vcpkg toolchain file (default: $VCPKG_ROOT/scripts/
#                      buildsystems/vcpkg.cmake when VCPKG_ROOT is set,
#                      system libraries otherwise)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WORK_DIR="$PROJECT_DIR/.workspace/dynarec-check"
FAKE_TOOLCHAIN="$WORK_DIR/fake-ppc64le-toolchain.cmake"

log_info()  { printf '\033[0;34m[INFO]\033[0m %s\n' "$1"; }
log_pass()  { printf '\033[0;32m[PASS]\033[0m %s\n' "$1"; }
log_fail()  { printf '\033[0;31m[FAIL]\033[0m %s\n' "$1" >&2; }
die()       { printf '\033[0;31m[ERROR]\033[0m %s\n' "$1" >&2; exit 1; }
usage_error() { printf '\033[0;31m[ERROR]\033[0m %s\n' "$1" >&2; exit 2; }

FULL=0
TOOLCHAIN=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --full) FULL=1; shift ;;
    --toolchain) [[ $# -ge 2 ]] || usage_error "--toolchain needs a file"; TOOLCHAIN="$2"; shift 2 ;;
    -h|--help) sed -n '5,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) usage_error "unknown argument: $1" ;;
  esac
done
[[ "$(uname -s)" == Linux ]] || die "this script runs on Linux hosts, not $(uname -s)"
case "$(uname -m)" in
  x86_64) HOST_X86=1; HOST_ARM=0; HOST_DYN="C_DYNAMIC_X86"; HOST_TRIPLET=x64-linux ;;
  aarch64) HOST_X86=0; HOST_ARM=1; HOST_DYN="C_DYNREC"; HOST_TRIPLET=arm64-linux ;;
  *) die "this script runs on x86_64 or aarch64 hosts, not $(uname -m)" ;;
esac
if [[ -z "$TOOLCHAIN" && -n "${VCPKG_ROOT:-}" ]]; then
  TOOLCHAIN="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
fi
if [[ -n "$TOOLCHAIN" ]]; then
  [[ -f "$TOOLCHAIN" ]] || die "toolchain file not found: $TOOLCHAIN"
  TOOLCHAIN="$(realpath -- "$TOOLCHAIN")"
fi
command -v cmake >/dev/null 2>&1 || die "cmake not found"
command -v ninja >/dev/null 2>&1 || die "ninja not found"

mkdir -p "$WORK_DIR"
printf 'set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR ppc64le)\n' > "$FAKE_TOOLCHAIN"


failures=0
checks=0

configure() {
  local name=$1; shift
  local build="$WORK_DIR/$name"
  local -a args=(-S "$PROJECT_DIR" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DIS_PRESET_USED=TRUE)
  if [[ -n "$TOOLCHAIN" ]]; then
    args+=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DVCPKG_TARGET_TRIPLET="$HOST_TRIPLET")
  fi
  rm -rf "$build" "$WORK_DIR/$name".{configure,build,ctest}.log
  set +e
  cmake "${args[@]}" "$@" > "$WORK_DIR/$name.configure.log" 2>&1
  CONFIGURE_EXIT=$?
  set -e
}

# fake_cpu_args: the cmake arguments that make the configure see ppc64le
fake_cpu_args() {
  if [[ -n "$TOOLCHAIN" ]]; then
    printf -- '-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=%s' "$FAKE_TOOLCHAIN"
  else
    printf -- '-DCMAKE_TOOLCHAIN_FILE=%s' "$FAKE_TOOLCHAIN"
  fi
}

check() {
  local ok=$1 what=$2
  checks=$((checks + 1))
  if [[ "$ok" == 1 ]]; then log_pass "$what"; else log_fail "$what"; failures=$((failures + 1)); fi
}

expect_configured() {
  local name=$1
  check "$([[ $CONFIGURE_EXIT -eq 0 ]] && echo 1 || echo 0)" "$name: configure succeeds"
}

expect_configure_fails() {
  local name=$1 pattern=$2
  local ok=0
  if [[ $CONFIGURE_EXIT -ne 0 ]] && grep -q -- "$pattern" "$WORK_DIR/$name.configure.log"; then ok=1; fi
  check "$ok" "$name: configure fails with '$pattern'"
}

expect_define() {
  local name=$1 macro=$2 value=$3
  local header="$WORK_DIR/$name/include/dosbox_config.h"
  local ok=0
  if [[ -f "$header" ]] && grep -q -E "^#define $macro $value\$" "$header"; then ok=1; fi
  check "$ok" "$name: $macro is $value"
}

expect_flags() {
  local name=$1 x86=$2 arm=$3 dyn_x86=$4 dynrec=$5
  expect_define "$name" C_TARGET_CPU_X86 "$x86"
  expect_define "$name" C_TARGET_CPU_ARM "$arm"
  expect_define "$name" C_DYNAMIC_X86 "$dyn_x86"
  expect_define "$name" C_DYNREC "$dynrec"
  expect_define "$name" C_FPU_X86 "$x86"
  expect_define "$name" C_UNALIGNED_MEMORY "$x86"
}

expect_log() {
  local name=$1 pattern=$2
  check "$(grep -q -- "$pattern" "$WORK_DIR/$name.configure.log" && echo 1 || echo 0)" "$name: configure output has '$pattern'"
}

build_and_test() {
  local name=$1
  local build="$WORK_DIR/$name"
  local ok=0
  if cmake --build "$build" -- -j"$(nproc)" > "$WORK_DIR/$name.build.log" 2>&1 \
     && (cd "$build" && ctest > "$WORK_DIR/$name.ctest.log" 2>&1); then ok=1; fi
  check "$ok" "$name: builds and passes the test suite ($(grep -E -o '[0-9]+ tests failed out of [0-9]+' "$WORK_DIR/$name.ctest.log" 2>/dev/null || echo 'no ctest summary'))"
}

expect_build_fails() {
  local name=$1 pattern=$2
  local ok=0
  if ! cmake --build "$WORK_DIR/$name" -- -j"$(nproc)" > "$WORK_DIR/$name.build.log" 2>&1 \
     && grep -q -- "$pattern" "$WORK_DIR/$name.build.log"; then ok=1; fi
  check "$ok" "$name: build fails with '$pattern'"
}

host_dyn_flags() { # x86 arm dyn_x86 dynrec for a dynamic-core host build
  if [[ "$HOST_DYN" == C_DYNAMIC_X86 ]]; then echo "$HOST_X86 $HOST_ARM 1 0"; else echo "$HOST_X86 $HOST_ARM 0 1"; fi
}

log_info "work dir $WORK_DIR, toolchain ${TOOLCHAIN:-system libraries}, host $(uname -m)"

read -r host_x86 host_arm host_dyn_x86 host_dynrec <<< "$(host_dyn_flags)"
configure default;      expect_configured default; expect_flags default "$host_x86" "$host_arm" "$host_dyn_x86" "$host_dynrec"; expect_log default "dynamic core enabled\|DYNREC recompiler enabled"
configure no -DOPT_DYNAREC=NO;    expect_configured no;    expect_flags no    "$HOST_X86" "$HOST_ARM" 0 0; expect_log no "interpreter core only"
configure lower -DOPT_DYNAREC=no; expect_configured lower; expect_flags lower "$HOST_X86" "$HOST_ARM" 0 0
configure off -DOPT_DYNAREC=OFF;  expect_configured off;   expect_flags off   "$HOST_X86" "$HOST_ARM" 0 0
configure bogus -DOPT_DYNAREC=maybe; expect_configure_fails bogus "OPT_DYNAREC must be KNOWN_CPU, YES or NO"
configure forceoff -DOPT_DYNAREC=NO -DOPT_FORCE_DYNREC=ON; expect_configured forceoff; expect_flags forceoff "$HOST_X86" "$HOST_ARM" 0 0; expect_log forceoff "OPT_FORCE_DYNREC has no effect"
configure yes -DOPT_DYNAREC=YES; expect_configured yes; expect_flags yes "$host_x86" "$host_arm" "$host_dyn_x86" "$host_dynrec"; expect_log yes " enabled"
configure on -DOPT_DYNAREC=ON;   expect_configured on;  expect_flags on  "$host_x86" "$host_arm" "$host_dyn_x86" "$host_dynrec"
configure force -DOPT_FORCE_DYNREC=ON; expect_configured force; expect_flags force "$HOST_X86" "$HOST_ARM" 0 1; expect_log force "DYNREC recompiler enabled"
configure osxarm -DCMAKE_OSX_ARCHITECTURES=arm64; expect_configured osxarm; expect_flags osxarm 0 1 0 1
configure osxuni "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"; expect_configure_fails osxuni "One architecture per configure"
configure ppc "$(fake_cpu_args)"; expect_configured ppc; expect_flags ppc 0 0 0 0; expect_log ppc "is not on the list of CPUs"; expect_log ppc "interpreter core only"
configure ppcyes "$(fake_cpu_args)" -DOPT_DYNAREC=YES; expect_configured ppcyes; expect_flags ppcyes 0 0 0 1; expect_log ppcyes "You asked for it"

if [[ $FULL -eq 1 ]]; then
  build_and_test default
  build_and_test no
  build_and_test ppc
  expect_build_fails ppcyes 'Unsupported target CPU'
fi

log_info "$checks checks, $failures failed"
[[ $failures -eq 0 ]]
