#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

set -euo pipefail

if (( $# == 0 )); then
    printf 'Expected dependency package arguments\n' >&2
    exit 2
fi

sudo apt-get -o Acquire::Retries=3 update --error-on=any
sudo apt-get -o Acquire::Retries=3 install -y "$@"
