#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

usage() {
    cat <<'EOF'
Usage: build-scripts/build-native-tiles.sh [options] [make-target-or-var ...]

Fetch and compose the standard tilesets used by nightly builds, then build a
binary tuned for this machine with the newest compiler installed locally:
  make -j"$JOBS" CLANG="$CLANG" RELEASE=1 TILES=1 SDL3=0 SOUND=1 LOCALIZE=0 ...

Unlike build-clang22-nightly-tiles.sh this does not pin a compiler version: it
picks the highest clang++ found on the system (falling back to g++) and adds
-march=native/-mtune=native plus ThinLTO, so the result only runs on CPUs
compatible with this one.

Options:
  --force-tilesets  Recompose tilesets even when the source stamp matches.
  --powersave       Use half the detected CPU threads for make.
  --skip-fetch      Use cached tileset repositories without fetching updates.
  --skip-tilesets   Do not fetch or compose tilesets; only run the build.
  --skip-build      Fetch and compose tilesets, but do not run make.
  --no-native       Build a portable binary (no -march=native).
  --no-lto          Disable link-time optimization.
  -h, --help        Show this help.

Environment:
  CLANG             Compiler executable. Default: newest clang++ found, else g++
  ARCH_FLAGS        Architecture tuning flags. Default: -march=native -mtune=native
  JOBS              Make parallelism. Default: nproc
  COMPOSE_JOBS      Tileset compose parallelism. Default: min(nproc, 2)
  CACHE_DIR         Asset/cache directory. Default: .cache/nightly-assets
  VENV_DIR          Python venv for pyvips. Default: $CACHE_DIR/venv

Examples:
  build-scripts/build-native-tiles.sh
  build-scripts/build-native-tiles.sh --powersave
  build-scripts/build-native-tiles.sh bindist
  ARCH_FLAGS='-march=alderlake' build-scripts/build-native-tiles.sh
EOF
}

nproc_or_one() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 1
    fi
}

half_nproc_or_one() {
    local jobs
    jobs="$(nproc_or_one)"
    if (( jobs > 1 )); then
        echo $(( jobs / 2 ))
    else
        echo 1
    fi
}

default_compose_jobs() {
    local jobs
    jobs="$(nproc_or_one)"
    if (( jobs > 2 )); then
        echo 2
    else
        echo "$jobs"
    fi
}

# Highest-versioned clang++ on this machine, else plain clang++, else g++.
detect_compiler() {
    local ver candidate best=""
    local best_ver=-1

    for ver in {30..15}; do
        candidate="clang++-$ver"
        if command -v "$candidate" >/dev/null 2>&1; then
            echo "$candidate"
            return
        fi
    done

    if command -v clang++ >/dev/null 2>&1; then
        echo "clang++"
        return
    fi

    # No clang at all: fall back to the newest g++.
    for ver in {20..9}; do
        candidate="g++-$ver"
        if command -v "$candidate" >/dev/null 2>&1 && (( ver > best_ver )); then
            best="$candidate"
            best_ver="$ver"
        fi
    done

    if [[ -n "$best" ]]; then
        echo "$best"
    else
        echo "g++"
    fi
}

JOBS_WAS_SET=0
COMPOSE_JOBS_WAS_SET=0
[[ -n "${JOBS+x}" ]] && JOBS_WAS_SET=1
[[ -n "${COMPOSE_JOBS+x}" ]] && COMPOSE_JOBS_WAS_SET=1

CLANG="${CLANG:-$(detect_compiler)}"
JOBS="${JOBS:-$(nproc_or_one)}"
COMPOSE_JOBS="${COMPOSE_JOBS:-$(default_compose_jobs)}"
CACHE_DIR="${CACHE_DIR:-$ROOT/.cache/nightly-assets}"
VENV_DIR="${VENV_DIR:-$CACHE_DIR/venv}"
ARCH_FLAGS="${ARCH_FLAGS:--march=native -mtune=native}"

FORCE_TILESETS=0
FETCH_REPOS=1
COMPOSE_TILESETS=1
POWERSAVE=0
RUN_BUILD=1
USE_NATIVE=1
USE_LTO=1
MAKE_ARGS=()

while (( $# > 0 )); do
    case "$1" in
        --force-tilesets)
            FORCE_TILESETS=1
            shift
            ;;
        --powersave)
            POWERSAVE=1
            shift
            ;;
        --skip-fetch)
            FETCH_REPOS=0
            shift
            ;;
        --skip-tilesets)
            COMPOSE_TILESETS=0
            FETCH_REPOS=0
            shift
            ;;
        --skip-build)
            RUN_BUILD=0
            shift
            ;;
        --no-native)
            USE_NATIVE=0
            shift
            ;;
        --no-lto)
            USE_LTO=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            MAKE_ARGS+=("$@")
            break
            ;;
        *)
            MAKE_ARGS+=("$1")
            shift
            ;;
    esac
done

if (( POWERSAVE == 1 )); then
    if (( JOBS_WAS_SET == 0 )); then
        JOBS="$(half_nproc_or_one)"
    fi

    if (( COMPOSE_JOBS_WAS_SET == 0 )); then
        COMPOSE_JOBS=1
    fi
fi

require_command() {
    local cmd="$1"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Missing required command: $cmd" >&2
        exit 1
    fi
}

ensure_base_tools() {
    require_command git
    require_command make
    require_command python3
    require_command sha256sum
    require_command "$CLANG"
}

# Probe a compiler/linker flag with a trivial link; used to keep the tuning
# flags optional rather than a hard requirement.
compiler_supports() {
    local flag="$1"
    local tmp
    tmp="$(mktemp -d)"
    if printf 'int main(){return 0;}\n' > "$tmp/probe.cpp" &&
        "$CLANG" $flag -o "$tmp/probe" "$tmp/probe.cpp" >/dev/null 2>&1; then
        rm -rf "$tmp"
        return 0
    fi
    rm -rf "$tmp"
    return 1
}

# Build up the extra flags the Makefile appends last (OTHERS), so they win over
# the defaults it picks for RELEASE builds.
configure_tuning() {
    local flag
    EXTRA_CXXFLAGS=""
    EXTRA_LDFLAGS=""
    LINKER_NAME="default"

    if (( USE_NATIVE == 1 )); then
        for flag in $ARCH_FLAGS; do
            if compiler_supports "$flag"; then
                EXTRA_CXXFLAGS+=" $flag"
            else
                echo "Skipping unsupported architecture flag: $flag" >&2
            fi
        done
        # Free the compiler from PLT indirection for our own symbols.
        if compiler_supports -fno-semantic-interposition; then
            EXTRA_CXXFLAGS+=" -fno-semantic-interposition"
        fi
    fi

    if (( USE_LTO == 1 )); then
        # The Makefile adds plain -flto for clang; ThinLTO gets most of the win
        # at a fraction of the link time and memory, and OTHERS is appended
        # after LTOFLAGS so this overrides it.
        if [[ "$CLANG" == *clang* ]] && compiler_supports -flto=thin; then
            EXTRA_CXXFLAGS+=" -flto=thin"
            EXTRA_LDFLAGS+=" -flto=thin"
        fi
    fi

    # Prefer lld, then mold, then gold: LTO links are linker-bound.
    if compiler_supports -fuse-ld=lld; then
        LINKER_NAME="lld"
    elif compiler_supports -fuse-ld=mold; then
        LINKER_NAME="mold"
    elif compiler_supports -fuse-ld=gold; then
        LINKER_NAME="gold"
    fi

    # Link-only: the compile lines are built with -Werror, and an unused
    # -fuse-ld= there is an error.
    if [[ "$LINKER_NAME" != "default" ]]; then
        EXTRA_LDFLAGS+=" -fuse-ld=$LINKER_NAME"
    fi
}

ensure_python() {
    PYTHON="${PYTHON:-python3}"
    if "$PYTHON" -c 'import pyvips' >/dev/null 2>&1; then
        return
    fi

    mkdir -p "$CACHE_DIR"
    if [[ ! -x "$VENV_DIR/bin/python" ]]; then
        echo "Creating Python venv for tileset composition: $VENV_DIR"
        if ! python3 -m venv "$VENV_DIR"; then
            cat >&2 <<'EOF'
Unable to create the Python venv. Install python3-venv, then rerun this script.

Debian/Ubuntu:
  sudo apt-get install -y python3-venv python3-pip libvips-dev
EOF
            exit 1
        fi
    fi

    PYTHON="$VENV_DIR/bin/python"
    if ! "$PYTHON" -m pip --version >/dev/null 2>&1; then
        echo "Bootstrapping pip in $VENV_DIR"
        if ! "$PYTHON" -m ensurepip --upgrade; then
            cat >&2 <<'EOF'
Unable to bootstrap pip in the Python venv.

Debian/Ubuntu:
  sudo apt-get install -y python3-venv python3-pip libvips-dev

If those are already installed, remove the cached venv and rerun:
  rm -rf .cache/nightly-assets/venv
EOF
            exit 1
        fi
    fi

    if ! "$PYTHON" -c 'import pyvips' >/dev/null 2>&1; then
        echo "Installing pyvips into $VENV_DIR"
        "$PYTHON" -m pip install --upgrade pip pyvips
    fi

    if ! "$PYTHON" -c 'import pyvips' >/dev/null 2>&1; then
        cat >&2 <<'EOF'
Unable to import pyvips. Install libvips, then rerun this script.

Debian/Ubuntu:
  sudo apt-get install -y python3-venv python3-pip libvips-dev
EOF
        exit 1
    fi
}

update_sparse_repo() {
    local name="$1"
    local url="$2"
    local dest="$3"
    local sparse_path="$4"

    if [[ ! -d "$dest/.git" ]]; then
        if [[ -e "$dest" ]]; then
            echo "$dest exists but is not a git repository" >&2
            exit 1
        fi

        echo "Cloning $name into $dest"
        if [[ -n "$sparse_path" ]]; then
            git clone --depth=1 --filter=blob:none --sparse "$url" "$dest"
            git -C "$dest" sparse-checkout set "$sparse_path"
        else
            git clone --depth=1 "$url" "$dest"
        fi
        return
    fi

    if (( FETCH_REPOS == 0 )); then
        echo "Using cached $name at $(git -C "$dest" rev-parse --short HEAD)"
        return
    fi

    echo "Updating $name"
    git -C "$dest" fetch --depth=1 origin master
    git -C "$dest" checkout --force FETCH_HEAD
    if [[ -n "$sparse_path" ]]; then
        git -C "$dest" sparse-checkout set "$sparse_path"
    fi
}

has_composed_output() {
    local dest="$1"
    local png
    local json

    png="$(find "$dest" -maxdepth 1 -type f -name '*.png' -print -quit 2>/dev/null || true)"
    json="$(find "$dest" -maxdepth 1 -type f -name '*.json' ! -name 'layering.json' -print -quit 2>/dev/null || true)"

    [[ -s "$dest/tileset.txt" && -n "$png" && -n "$json" ]]
}

compose_signature() {
    local name="$1"
    local repo_dir="$2"
    local rel_path="$3"
    local args_text="$4"
    local revision

    revision="$(git -C "$repo_dir" rev-parse HEAD)"
    {
        printf 'name=%s\n' "$name"
        printf 'repo=%s\n' "$repo_dir"
        printf 'path=%s\n' "$rel_path"
        printf 'revision=%s\n' "$revision"
        printf 'args=%s\n' "$args_text"
        sha256sum tools/gfx_tools/compose.py tools/format/json_formatter.cgi
    } | sha256sum | awk '{ print $1 }'
}

compose_one() {
    local record="$1"
    local name repo_key rel_path args_text repo_dir source dest stamp signature old_signature
    local -a compose_args=()

    IFS='|' read -r name repo_key rel_path args_text <<<"$record"
    case "$repo_key" in
        cdda)
            repo_dir="$CDDA_TILESETS_DIR"
            ;;
        cuteclysm)
            repo_dir="$CUTECLYSM_DIR"
            ;;
        *)
            echo "Unknown tileset repo key: $repo_key" >&2
            exit 1
            ;;
    esac

    source="$repo_dir/$rel_path"
    dest="$ROOT/gfx/$name"
    stamp="$dest/.nightly-compose.stamp"
    signature="$(compose_signature "$name" "$repo_dir" "$rel_path" "$args_text")"

    if [[ -f "$stamp" ]]; then
        old_signature="$(<"$stamp")"
    else
        old_signature=""
    fi

    if (( FORCE_TILESETS == 0 )) && [[ "$old_signature" == "$signature" ]] && has_composed_output "$dest"; then
        echo "Tileset $name is up to date"
        return
    fi

    read -r -a compose_args <<<"$args_text"
    echo "Composing tileset $name"
    "$PYTHON" tools/gfx_tools/compose.py "${compose_args[@]}" \
        --feedback CONCISE --format-json --loglevel INFO \
        "$source" "$dest"

    cp -f "$source/tileset.txt" "$dest/"
    [[ -f "$source/fallback.png" ]] && cp -f "$source/fallback.png" "$dest/"
    [[ -f "$source/layering.json" ]] && cp -f "$source/layering.json" "$dest/"
    printf '%s\n' "$signature" > "$stamp"
}

compose_tilesets() {
    local record
    local active_jobs

    CDDA_TILESETS_DIR="$CACHE_DIR/CDDA-Tilesets"
    CUTECLYSM_DIR="$CACHE_DIR/CDDA-tileset"
    export CDDA_TILESETS_DIR CUTECLYSM_DIR PYTHON ROOT FORCE_TILESETS

    update_sparse_repo "CDDA-Tilesets" "https://github.com/I-am-Erk/CDDA-Tilesets" "$CDDA_TILESETS_DIR" "gfx"
    update_sparse_repo "Cuteclysm" "https://github.com/pixel-32/CDDA-tileset" "$CUTECLYSM_DIR" ""

    make CLANG="$CLANG" tools/format/json_formatter.cgi

    echo "Tileset compose parallelism: $COMPOSE_JOBS"
    local -a tilesets=(
        "Altica|cdda|gfx/Altica|--use-all --obsolete-fillers"
        "ASCII_Overmap|cdda|gfx/ASCII_Overmap|--use-all"
        "BrownLikeBears|cdda|gfx/BrownLikeBears|--use-all --obsolete-fillers"
        "ChibiUltica|cdda|gfx/Chibi_Ultica|--use-all"
        "HollowMoon|cdda|gfx/HollowMoon|--use-all --obsolete-fillers"
        "Larwick_Overmap|cdda|gfx/Larwick_Overmap|--use-all"
        "MShockXotto+|cdda|gfx/MShockXotto+|--use-all"
        "NeoDays|cdda|gfx/NeoDays|--use-all"
        "Retrodays|cdda|gfx/Retrodays|--use-all"
        "GiantDays|cdda|gfx/GiantDays|--use-all"
        "SmashButton_iso|cdda|gfx/HitButton_iso|--use-all"
        "SurveyorsMap|cdda|gfx/SurveyorsMap|--use-all"
        "UltimateCataclysm|cdda|gfx/UltimateCataclysm|--use-all --obsolete-fillers"
        "Ultica_iso|cdda|gfx/Ultica_iso|--use-all"
        "PenAndPaper|cdda|gfx/PenAndPaper|--use-all"
        "Cuteclysm|cuteclysm|.|--use-all"
    )

    for record in "${tilesets[@]}"; do
        if (( COMPOSE_JOBS > 1 )); then
            compose_one "$record" &
            while true; do
                active_jobs="$(jobs -pr | wc -l)"
                if (( active_jobs < COMPOSE_JOBS )); then
                    break
                fi
                wait -n
            done
        else
            compose_one "$record"
        fi
    done

    wait
}

ensure_base_tools
configure_tuning

echo "Compiler: $CLANG ($("$CLANG" --version | head -n 1))"
echo "Tuning flags:${EXTRA_CXXFLAGS:- none}"
echo "Linker: $LINKER_NAME"

if (( COMPOSE_TILESETS == 1 )); then
    ensure_python
    compose_tilesets
fi

if (( RUN_BUILD == 1 )); then
    echo "Build parallelism: $JOBS"
    # OTHERS/LDFLAGS are passed through the environment, not on the command
    # line, so the Makefile's own `+=` additions survive.
    OTHERS="${OTHERS:-}$EXTRA_CXXFLAGS" \
    LDFLAGS="${LDFLAGS:-}$EXTRA_LDFLAGS" \
    make -j "$JOBS" \
        CLANG="$CLANG" \
        RELEASE=1 \
        TILES=1 \
        SOUND=1 \
        LOCALIZE=0 \
        ASTYLE=0 \
        LINTJSON=0 \
        TESTS=0 \
        LTO=$(( USE_LTO )) \
        GOLD=0 \
        "${MAKE_ARGS[@]}" \
        SDL3=0
fi
