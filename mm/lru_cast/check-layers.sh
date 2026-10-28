#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# CAST: check that each component includes only what it may.
#
#	core/	nothing of CAST's but compat/
#	policy/	core/core.h
#	walk/	core/core.h
#	tune/	core/core.h, policy/policy.h
#	.	every component's public header, nothing inside a component
#
# compat/ alone may reach the host's mm-private headers (../../).
#
# Inside its own directory a component includes what it likes. And the
# main cache's operating point, read inline everywhere, is written by
# policy/params.c alone: lru_cast_params is named there and in policy.h only.
# Run from anywhere; prints each offence and exits 1 if there is one.

cd "$(dirname "$0")" || exit 2
public="core/core.h policy/policy.h walk/walk.h tune/tune.h control.h version.h"
bad=0

allowed() {	# $1 the including file, $2 the included path
	case "$2" in
	compat/*|version.h) return 0 ;;
	esac
	case "$1" in
	compat/*)	return 1 ;;
	core/*)		deps="" ;;
	policy/*)	deps="core/core.h" ;;
	walk/*)		deps="core/core.h" ;;
	tune/*)		deps="core/core.h policy/policy.h" ;;
	*)		deps="$public" ;;
	esac
	dir=${1%/*}
	[ "$dir" != "$1" ] && [ "${2%/*}" = "$dir" ] && return 0
	for d in $deps; do
		[ "$2" = "$d" ] && return 0
	done
	return 1
}

for f in $(find . -name '*.[chS]' | sed 's|^\./||' | sort); do
	for inc in $(sed -n 's/^#include "\([^"]*\)".*/\1/p' "$f"); do
		case "$f" in compat/*) case "$inc" in ../../*) continue ;; esac ;; esac
		if ! allowed "$f" "$inc"; then
			echo "$f: #include \"$inc\""
			bad=1
		fi
	done
done
for f in $(grep -rl --include='*.[chS]' -w lru_cast_params . | sed 's|^\./||' | sort); do
	case "$f" in
	policy/params.c|policy/policy.h) ;;
	*)	grep -n -w lru_cast_params "$f" | sed "s|^|$f:|"; bad=1 ;;
	esac
done
exit $bad
