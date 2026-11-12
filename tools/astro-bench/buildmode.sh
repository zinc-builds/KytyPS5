#!/bin/bash
# Build modes shared by refbuild.sh, build.sh and pgo-train.sh (sourced, not run).
#
# KYTY_BUILD_MODE:
#   plain   (default) the normal Release build.
#   lto     ThinLTO.
#   pgo     ThinLTO + PGO with KYTY_PGO_PROFILE: the fast build. Both sides of an A/B use the same
#           mode and the same profile file, so the profile is never implied: pin it once, e.g.
#             export KYTY_PGO_PROFILE=$(readlink -e ~/.cache/kyty-pgo/current.profdata)
#   pgogen  instrumented build that writes profiles (pgo-train.sh builds it).
# KYTY_PGO_DIR: profile store (default ~/.cache/kyty-pgo; pgo-train.sh writes <sha>.profdata and
#   the current.profdata link there).
#
# kyty_build_mode <source dir> sets, for the caller:
#   mode        the mode name
#   mode_tag    cache-key suffix: "" (plain), -lto, -pgogen, -pgo-<first 12 hex of the profile's sha256>
#   mode_cmake  array of cmake arguments for that source tree
#   mode_desc   one line for the BUILD_MODE file next to an install
# Sources older than the CMake options (no config_lto_pgo in src/utils.cmake) get the same flags
# through CMAKE_*_FLAGS, so a reference build of an older commit still matches.

KYTY_PGO_DIR=${KYTY_PGO_DIR:-$HOME/.cache/kyty-pgo}

kyty_build_mode() {
	local src=$1 prof="" hash="" lto=OFF pgo=OFF
	mode=${KYTY_BUILD_MODE:-plain}
	case "$mode" in
	plain) mode_tag="" ;;
	lto) mode_tag=-lto lto=thin ;;
	pgogen) mode_tag=-pgogen pgo=generate ;;
	pgo)
		[ -n "$KYTY_PGO_PROFILE" ] || {
			echo "KYTY_BUILD_MODE=pgo needs KYTY_PGO_PROFILE (e.g. \$(readlink -e $KYTY_PGO_DIR/current.profdata))" >&2
			return 1
		}
		prof=$(readlink -e "$KYTY_PGO_PROFILE") && [ -f "$prof" ] || {
			echo "KYTY_PGO_PROFILE=$KYTY_PGO_PROFILE: no such file" >&2
			return 1
		}
		hash=$(sha256sum "$prof" | cut -c1-12)
		mode_tag=-pgo-$hash lto=thin pgo=use
		;;
	*)
		echo "KYTY_BUILD_MODE must be plain, lto, pgo or pgogen, not '$mode'" >&2
		return 1
		;;
	esac
	mode_desc="mode=$mode${prof:+ profile=$prof sha256=$hash}"
	if grep -q 'config_lto_pgo' "$src/src/utils.cmake" 2>/dev/null; then
		mode_cmake=(-DKYTY_LTO=$lto -DKYTY_PGO=$pgo "-DKYTY_PGO_PROFILE=$prof")
		return 0
	fi
	# Older source: the same flags by hand (utils.cmake appends its own flags to these).
	local cflags="" ldflags=""
	if [ $lto = thin ]; then
		cflags+=" -flto=thin"
		ldflags+=" -fuse-ld=lld -flto=thin -Wl,--thinlto-cache-dir=$HOME/.cache/kyty-thinlto"
		ldflags+=" -Wl,--thinlto-cache-policy=cache_size_bytes=8g:prune_after=168h -Wl,--thinlto-jobs=8"
	fi
	[ $pgo = generate ] && cflags+=" -fprofile-generate" ldflags+=" -fprofile-generate"
	[ $pgo = use ] && cflags+=" -fprofile-use=$prof -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin"
	mode_cmake=("-DCMAKE_C_FLAGS=$cflags" "-DCMAKE_CXX_FLAGS=$cflags" "-DCMAKE_EXE_LINKER_FLAGS=$ldflags")
}

# kyty_seed_install <install dir>: copy the save and the warm shader cache in, as setup-install.sh
# does, unless the install already has them (KYTY_SETUP_FROM: source install dir).
kyty_seed_install() {
	local from=${KYTY_SETUP_FROM:-$HOME/AI/emu/KytyPS5/_Build/linux/install} d
	for d in _SaveData _PipelineCache; do
		[ -d "$1/$d" ] || { [ -d "$from/$d" ] && cp -a "$from/$d" "$1/"; }
	done
	return 0
}
