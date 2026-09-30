#!/usr/bin/env bash
# Start the jelly from a launcher. Only one jelly at a time: a second click does nothing while one is running.
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
lock="${XDG_RUNTIME_DIR:-/tmp}/jev-jelly.lock"
cd "$root" && exec flock -n "$lock" "$root/jelly"
