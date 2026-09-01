#!/bin/sh
#
# Run the host test suite: unit tests for the port's pure functions, then
# every filesystem scenario on a fresh image, each one validated with fsck.
#
# A scenario is only considered to pass if BOTH its own checks pass AND the
# resulting image is clean according to exfatfsck.  A scenario that "works"
# but leaves a corrupt volume is a failure.
#
# Usage:  ./run-tests.sh [scenario ...]      (default: all)
#

set -u

B=build
IMG=$B/test.img
IMGSIZE=64          # MB
CLUSTERS=8          # sectors per cluster -> 4 KB clusters

pass=0
fail=0

if [ ! -x "$B/fstest" ]; then
	echo "build first: make"
	exit 2
fi

# ---- unit tests -----------------------------------------------------
echo
if "$B/unittest"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
	echo "UNIT TESTS FAILED"
fi

# ---- filesystem scenarios -------------------------------------------
if [ $# -gt 0 ]; then
	SCENARIOS="$*"
else
	SCENARIOS="small spanning grow shrink dirs rename delete names fragment fill"
fi

echo
echo "== filesystem scenarios =="
for s in $SCENARIOS; do
	printf '%-10s ' "$s"

	rm -f "$IMG"
	dd if=/dev/zero of="$IMG" bs=1048576 count=$IMGSIZE 2>/dev/null
	if ! "$B/mkexfatfs" -s $CLUSTERS "$IMG" >/dev/null 2>&1; then
		echo "MKFS FAILED"
		fail=$((fail + 1))
		continue
	fi

	out=$("$B/fstest" "$IMG" "$s" 2>&1)
	rc=$?

	fsckout=$("$B/exfatfsck" "$IMG" 2>&1)
	# exfatfsck exits non-zero on errors; also guard against a changed
	# message by requiring the explicit "No errors found".
	fsckrc=$?
	case "$fsckout" in
		*"No errors found"*) fsckclean=1 ;;
		*)                   fsckclean=0 ;;
	esac

	if [ $rc -eq 0 ] && [ $fsckrc -eq 0 ] && [ $fsckclean -eq 1 ]; then
		echo "ok"
		pass=$((pass + 1))
	else
		echo "FAIL"
		[ -n "$out" ] && echo "$out" | sed 's/^/    /'
		if [ $fsckclean -ne 1 ] || [ $fsckrc -ne 0 ]; then
			echo "    --- fsck said ---"
			echo "$fsckout" | sed 's/^/    /'
		fi
		fail=$((fail + 1))
	fi
done

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
