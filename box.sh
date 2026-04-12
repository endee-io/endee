#!/usr/bin/env bash

# Box Autonomous AI Engine - UNIX Launcher
# Compatible with Linux, macOS, and BSD

# Get the directory of the script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
export PYTHONPATH="$PYTHONPATH:$SCRIPT_DIR"

# Check if help was requested or no arguments
if [ -z "$1" ]; then
    echo "Box Autonomous AI Engine"
    echo ""
    echo "Usage:"
    echo "  ./box.sh index      - Scan and index the codebase"
    echo "  ./box.sh search     - Semantic/Hybrid lookup"
    echo "  ./box.sh serve      - Start the background API server"
    echo "  ./box.sh backup     - Manage snapshots"
    echo "  ./box.sh status     - System health check"
    echo "  ./box.sh build      - Run the dataset building pipeline"
    exit 0
fi

# Pass all arguments to the box.cli module
python3 -m box.cli "$@"
exit $?
