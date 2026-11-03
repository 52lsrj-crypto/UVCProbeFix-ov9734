#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd -P)"
export UVCP_CONFIGURATION=Debug
exec "$ROOT/Build.command"
