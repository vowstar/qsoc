#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#
# Annotate a ctest log: a test that failed and then passed on retry, and,
# with a limit given, a test that ran longer than that many seconds.
# Usage: ctest-report.sh <ctest.log> [slow-seconds]
set -eu
log="$1"
slow="${2:-0}"
[ -f "$log" ] || exit 0
tr -d '\r' < "$log" | awk -v slow="$slow" '
    match($0, /Test +#[0-9]+: [^ ]+ /) {
        split(substr($0, RSTART, RLENGTH), f, " ")
        name = f[3]
        if ($0 ~ / Passed +[0-9.]+ sec$/) {
            passed[name] = 1
            if (slow > 0 && $(NF - 1) + 0 > slow)
                printf "::warning title=slow test::%s took %d s, over %d s: give it a CI shard label (ci-sim-1, ci-sim-2 or ci-formal)\n", name, $(NF - 1), slow
        } else {
            failed[name] = 1
        }
    }
    END {
        for (name in failed)
            if (name in passed)
                printf "::warning title=retried::%s failed once and passed on retry\n", name
    }'
