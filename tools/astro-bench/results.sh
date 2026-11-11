#!/bin/bash
# results.sh: the shared bench result store (~/.cache/kyty-builds/results.jsonl, one line per
# garden.sh run) and A/B verdicts. See results.py for the commands, e.g.:
#   results.sh ref <sha> [--window 200-500]   stored reference numbers of a commit (median, spread)
#   results.sh table "<label-glob>"            median and spread of stored runs by label
exec python3 -I "$(dirname "$0")/results.py" "$@"
