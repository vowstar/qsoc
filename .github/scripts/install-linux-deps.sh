#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

set -euo pipefail

if (( $# == 0 )); then
    printf 'Expected dependency package arguments\n' >&2
    exit 2
fi

found=0
for mirrors in /etc/apt/apt-mirrors.txt /etc/apt/apt-mirrors-security.txt; do
    if [[ -f "$mirrors" ]]; then
        printf '%s\n' 'https://mirrors.edge.kernel.org/ubuntu/' | sudo tee "$mirrors" >/dev/null
        found=1
    fi
done
if (( found == 0 )); then
    printf 'The runner Ubuntu mirror list is unavailable\n' >&2
    exit 1
fi

sudo apt-get update --error-on=any
sudo apt-get install -y "$@"
