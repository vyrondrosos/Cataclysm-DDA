#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

usage() {
    cat <<'EOF'
Usage: build-scripts/build-clang22-nightly-tiles.sh [options] [make-target-or-var ...]

Fetch and compose the standard tilesets used by nightly builds, then build:
  make -j"$JOBS" CLANG="$CLANG" RELEASE=1 TILES=1 SDL3=0 SOUND=1 LOCALIZE=0 ...

Options:
  --force-tilesets  Recompose tilesets even when the source stamp matches.
  --powersave       Use half the detected CPU threads for make.
  --skip-fetch      Use cached tileset repositories without fetching updates.
  --skip-tilesets   Do not fetch or compose tilesets; only run the build.
  --skip-build      Fetch and compose tilesets, but do not run make.
  -h, --help        Show this help.

Environment:
  CLANG             Compiler executable. Default: clang++-22
  JOBS              Make parallelism. Default: nproc
  COMPOSE_JOBS      Tileset compose parallelism. Default: min(nproc, 2)
  CACHE_DIR         Asset/cache directory. Default: .cache/nightly-assets
  VENV_DIR          Python venv for pyvips. Default: $CACHE_DIR/venv

Examples:
  build-scripts/build-clang22-nightly-tiles.sh
  build-scripts/build-clang22-nightly-tiles.sh --powersave
  build-scripts/build-clang22-nightly-tiles.sh bindist
  COMPOSE_JOBS=1 build-scripts/build-clang22-nightly-tiles.sh LTO=1 bindist
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

JOBS_WAS_SET=0
COMPOSE_JOBS_WAS_SET=0
[[ -n "${JOBS+x}" ]] && JOBS_WAS_SET=1
[[ -n "${COMPOSE_JOBS+x}" ]] && COMPOSE_JOBS_WAS_SET=1

CLANG="${CLANG:-clang++-22}"
JOBS="${JOBS:-$(nproc_or_one)}"
COMPOSE_JOBS="${COMPOSE_JOBS:-$(default_compose_jobs)}"
CACHE_DIR="${CACHE_DIR:-$ROOT/.cache/nightly-assets}"
VENV_DIR="${VENV_DIR:-$CACHE_DIR/venv}"

FORCE_TILESETS=0
FETCH_REPOS=1
COMPOSE_TILESETS=1
POWERSAVE=0
RUN_BUILD=1
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

if (( COMPOSE_TILESETS == 1 )); then
    ensure_python
    compose_tilesets
fi

if (( RUN_BUILD == 1 )); then
    echo "Build parallelism: $JOBS"
    make -j "$JOBS" \
        CLANG="$CLANG" \
        RELEASE=1 \
        TILES=1 \
        SOUND=1 \
        LOCALIZE=0 \
        ASTYLE=0 \
        LINTJSON=0 \
        TESTS=0 \
        "${MAKE_ARGS[@]}" \
        SDL3=0
fi
