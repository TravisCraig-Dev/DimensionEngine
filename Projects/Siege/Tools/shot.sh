#!/bin/bash
# shot.sh <scene name> <out.png> [frames]: renders a Siege scene with the debug Player (scene by file name).
cd "$(dirname "$0")/../../.." || exit 1
scene="$(pwd -W)/Projects/Siege/Assets/Scenes/$1.scene"
timeout 400 out/build/${BUILD:-debug}/bin/DimensionPlayer.exe "$(pwd -W)/Projects/Siege" --scene "$scene" --frames "${3:-60}" --screenshot "$2" $EXTRA > "$2.log" 2>&1
echo "exit $?"
grep -iE "\[error\]|\[warn\]" "$2.log" | sort | uniq -c | head -15
