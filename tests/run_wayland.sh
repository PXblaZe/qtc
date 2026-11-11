#!/bin/sh
# run_wayland.sh <program> [args]: runs a test program on Qt's Wayland platform, against a
# headless weston started for it alone. Exit status: the program's, or 77 (skipped) when weston
# cannot start here.
weston=${WESTON:-weston}
dir=$(mktemp -d)
chmod 700 "$dir"
export XDG_RUNTIME_DIR="$dir"
"$weston" --backend=headless-backend.so --socket=qtc-test > "$dir/weston.log" 2>&1 &
pid=$!
i=0
while [ ! -S "$dir/qtc-test" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
if [ ! -S "$dir/qtc-test" ]; then
    echo "SKIP: weston did not start"; cat "$dir/weston.log"
    kill $pid 2>/dev/null; rm -rf "$dir"; exit 77
fi
WAYLAND_DISPLAY=qtc-test QT_QPA_PLATFORM=wayland "$@"
status=$?
kill $pid 2>/dev/null
wait $pid 2>/dev/null
rm -rf "$dir"
exit $status
