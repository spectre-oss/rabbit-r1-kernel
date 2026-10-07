#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
printf '%s\n' 'Partial source release: Rabbit panel and WLAN are excluded.' 'The full-device build recipe is unavailable in this snapshot.' 'See Documentation/rabbit-r1/partial-release.md. Do not flash.' >&2
exit 2
