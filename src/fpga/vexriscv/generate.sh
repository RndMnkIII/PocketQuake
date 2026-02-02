#!/bin/bash
# Generate VexRiscv with custom cache sizes for PocketQuake
# Usage: ./generate.sh
#
# Prerequisites: java, sbt

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VEXRISCV_DIR="$SCRIPT_DIR/VexRiscv"

# Clone VexRiscv if not already present
if [ ! -d "$VEXRISCV_DIR" ]; then
    echo "Cloning VexRiscv..."
    git clone https://github.com/SpinalHDL/VexRiscv.git "$VEXRISCV_DIR"
fi

# Copy generation script into VexRiscv project
cp "$SCRIPT_DIR/GenPocketQuake.scala" "$VEXRISCV_DIR/src/main/scala/vexriscv/demo/"

# Generate
echo "Generating VexRiscv (32KB I$ + 128KB D$)..."
cd "$VEXRISCV_DIR"
sbt "runMain vexriscv.demo.GenPocketQuake"

# Copy output
if [ -f "$VEXRISCV_DIR/VexRiscv.v" ]; then
    # Back up old version
    if [ -f "$SCRIPT_DIR/VexRiscv_Full.v" ]; then
        cp "$SCRIPT_DIR/VexRiscv_Full.v" "$SCRIPT_DIR/VexRiscv_Full.v.bak"
        echo "Backed up old VexRiscv_Full.v -> VexRiscv_Full.v.bak"
    fi
    cp "$VEXRISCV_DIR/VexRiscv.v" "$SCRIPT_DIR/VexRiscv_Full.v"
    echo "Copied generated VexRiscv.v -> VexRiscv_Full.v"
    echo ""
    echo "Cacheability for 0x3X (PSRAM+SRAM) is built into the generation"
    echo "(StaticMemoryTranslatorPlugin ioRange excludes 0x0, 0x1, 0x3)"
else
    echo "ERROR: VexRiscv.v not found after generation"
    exit 1
fi
