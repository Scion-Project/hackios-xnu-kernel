#!/bin/bash
# Get Apple's XNU sources (the release that Darwin userland 16.5 was built from) and put the HakiOS
# work on top of them: hakios.patch changes Apple's files, files/ holds the new ones.
# usage: xnu/apply-series.sh [DIR]              (default: $XNU_SRC, else xnu-src/xnu-8796.121.2 next to this directory)
# env:   XNU_URL   where to fetch from (default: Apple's repository on GitHub; an existing clone
#                  that has the tag will do: XNU_URL=file:///path/to/xnu)
# The result is a git tree on the branch "hakios" with the work uncommitted, which is how
# xnu/export.sh expects to find it. A directory that already exists is left alone.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DIR=${1:-${XNU_SRC:-$HERE/../xnu-src/xnu-8796.121.2}}
URL=${XNU_URL:-https://github.com/apple-oss-distributions/xnu.git}
TAG=$(python3 "$HERE/../tools/os-profile.py" get xnu_tag)
BASE=$(python3 "$HERE/../tools/os-profile.py" get xnu_commit)

[ -e "$DIR" ] && { echo "apply.sh: $DIR already exists; not touching it" >&2; exit 1; }
SERIES=$HERE/../patches/xnu-8796
[ -s "$SERIES/series" ] || { echo "apply-series.sh: no $SERIES/series" >&2; exit 1; }
[ -f "$HERE/../drivers/map" ] || { echo "apply.sh: no drivers/map next to xnu/ (the drivers are in drivers/)" >&2; exit 1; }
mkdir -p "$(dirname "$DIR")"
DIR=$(cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")
git init -q "$DIR"
trap 'echo "apply-series.sh: failed; removing the incomplete $DIR" >&2; rm -rf "$DIR"' ERR
cd "$DIR"
git remote add origin "$URL"
git fetch -q --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG"      # that one release, without the history
git checkout -q -b hakios "$TAG"
[ "$(git rev-parse HEAD)" = "$BASE" ] || { echo "apply.sh: $TAG is $(git rev-parse HEAD), expected $BASE" >&2; false; }
echo 'BUILD/' >> .git/info/exclude
grep -v '^#' "$SERIES/series" | while read -r p; do
	git apply --whitespace=nowarn "$SERIES/$p" || { echo "apply-series.sh: $p does not apply" >&2; exit 1; }
done
# HFS Plus (the data volume): Apple's release, fetched like XNU, and this port's changes over it
"$HERE/hfs/hfs.sh" put "$DIR"
# the drivers are kept by subsystem (drivers/<subsystem>/), not at their place in Apple's tree: drivers/map says where
# each one goes, one line per file: "<subsystem> <path in the tree>"
NDRIVERS=0
while read -r sub tree _; do
	case "$sub" in ''|'#'*) continue ;; esac
	[ -f "$HERE/../drivers/$sub/$(basename "$tree")" ] || { echo "apply.sh: drivers/$sub/$(basename "$tree") is in drivers/map but missing" >&2; false; }
	mkdir -p "$(dirname "$tree")"
	cp -p "$HERE/../drivers/$sub/$(basename "$tree")" "$tree"
	NDRIVERS=$((NDRIVERS + 1))
done < "$HERE/../drivers/map"
# the drivers of the SoC and device repositories checked out (socs/*/drivers/, devices/*/drivers/)
python3 "$HERE/../tools/hakios-config.py" external-files | while IFS="$(printf '\t')" read -r src tree; do
	mkdir -p "$(dirname "$tree")"
	cp -p "$src" "$tree"
done
# what the numbered patches change in the drivers today, until it is folded into them
if [ -s "$SERIES/9000-driver-fixups.patch" ]; then git apply --whitespace=nowarn "$SERIES/9000-driver-fixups.patch"; fi
# which of them are built is the configuration's (drivers/*/conf, CONFIG_HAKIOS_*): put into the tree as XNU reads it.
# What that writes is made again for every build, so it is kept out of the work that xnu/export.sh saves.
printf '%s\n' config/MASTER.hakios bsd/dev/hakios.drivers.h >> .git/info/exclude
python3 "$HERE/../tools/hakios-config.py" kernel --tree "$DIR"
echo "apply-series.sh: $DIR: $TAG + the series + $NDRIVERS drivers"
