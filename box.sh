#!/bin/bash
export PYTHONPATH=$PYTHONPATH:$(pwd)

if [ "$1" == "serve" ]; then
    echo "[Box] Starting API Server on http://localhost:8000 ..."
    python3 box/server.py
elif [ "$1" == "index" ]; then
    echo "[Box] Indexing Codebase ..."
    python3 -c "from box.intelligence import BoxIntelligence; BoxIntelligence().index_root('.')"
else
    echo "Box Autonomous Engine"
    echo "Usage:"
    echo "  ./box.sh serve   - Start the background API server"
    echo "  ./box.sh index   - Re-index the codebase"
    echo "  ./box.sh build   - Run the original training pipeline"
fi
