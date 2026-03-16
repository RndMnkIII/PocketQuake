#!/bin/bash
# Two-phase sweep:
#   Phase 1: Run quartus_map 5 times, keep the best synthesis result
#   Phase 2: Seed sweep (fit+sta only) on the best map
set -e

FPGA_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$FPGA_DIR"

QUARTUS_DIR=/home/alberto/altera_lite/25.1std/quartus/bin
export PATH="$QUARTUS_DIR:$PATH"

PROJECT=ap_core
QSF="${PROJECT}.qsf"
RESULTS_DIR="$FPGA_DIR/sweep_results"
mkdir -p "$RESULTS_DIR"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS="$RESULTS_DIR/sweep_${TIMESTAMP}.csv"

MAP_SEED=13  # Seed used during map phase (affects fitter, but map may vary with threads)
NUM_MAPS=1
NUM_SEEDS=5

# ============================================================
# Phase 1: Run quartus_map N times, keep best
# ============================================================
echo "=========================================="
echo "Phase 1: Run quartus_map ${NUM_MAPS} times"
echo "=========================================="
echo ""

# Set seed for consistent fitter behavior during map evaluation
sed -i "s/^set_global_assignment -name SEED .*/set_global_assignment -name SEED $MAP_SEED/" "$QSF"

best_map=""
best_map_aluts="999999"

for i in $(seq 1 $NUM_MAPS); do
    echo -n "  Map $i/$NUM_MAPS: "

    # Clean previous map outputs to force full re-synthesis
    rm -f output_files/${PROJECT}.map.rpt output_files/${PROJECT}.map.summary 2>/dev/null
    rm -f db/${PROJECT}.map.* db/${PROJECT}.cmp.* 2>/dev/null
    rm -f db/${PROJECT}.pre_map.* 2>/dev/null

    quartus_map "$PROJECT" > "output_files/map_run${i}.log" 2>&1

    # Extract combinational ALUTs as quality metric (from map.rpt, not summary)
    aluts=$(grep "Combinational ALUT usage for logic" output_files/${PROJECT}.map.rpt 2>/dev/null | grep -oP ';\s*\K\d+' | head -1)
    regs=$(grep "Total registers" output_files/${PROJECT}.map.summary 2>/dev/null | grep -oP '\d+' | head -1)
    total_aluts=$aluts

    echo "ALUTs=$aluts regs=$regs total=$total_aluts"

    # Save map database
    cp -r db "$RESULTS_DIR/db_map${i}" 2>/dev/null || true
    cp output_files/${PROJECT}.map.summary "$RESULTS_DIR/map${i}.summary" 2>/dev/null || true

    # Track best (fewer ALUTs = better)
    is_better=$(python3 -c "print(1 if int('${total_aluts:-999999}') < int('${best_map_aluts}') else 0)" 2>/dev/null)
    if [ "$is_better" = "1" ]; then
        best_map=$i
        best_map_aluts=$total_aluts
        echo "    >>> New best map!"
    fi
done

echo ""
echo "Best map: run $best_map (total ALUTs: $best_map_aluts)"
echo ""

# Restore best map database
if [ -d "$RESULTS_DIR/db_map${best_map}" ]; then
    echo "Restoring best map database (run $best_map)..."
    rm -rf db
    cp -r "$RESULTS_DIR/db_map${best_map}" db
fi

# Quick validation: run fit+sta with MAP_SEED to confirm (non-fatal)
echo ""
echo "Validating best map with seed $MAP_SEED..."
quartus_fit --read_settings_files=on --write_settings_files=off "$PROJECT" -c "$PROJECT" > "output_files/map_validate_fit.log" 2>&1 || true
quartus_sta "$PROJECT" -c "$PROJECT" > "output_files/map_validate_sta.log" 2>&1 || true

val_slack=$(grep -A15 "Slow 1100mV 85C Model Setup Summary" output_files/${PROJECT}.sta.rpt | grep "mp_ram.*general\[0\]" | awk -F';' '{for(i=1;i<=NF;i++) if($i ~ /^ *-?[0-9]+\.[0-9]+ *$/) {gsub(/ /,"",$i); print $i; exit}}')
val_alm=$(grep "Logic utilization" output_files/${PROJECT}.fit.summary | grep -oP '\d+(?= /)' | head -1)
val_m10k=$(grep "Total RAM Blocks" output_files/${PROJECT}.fit.summary | grep -oP '\d+(?= /)' | head -1)

echo "  Validation: ALM=$val_alm M10K=$val_m10k slack=$val_slack"

# ============================================================
# Phase 2: Seed sweep on best map
# ============================================================
echo ""
echo "=========================================="
echo "Phase 2: ${NUM_SEEDS}-seed sweep (fit+sta)"
echo "=========================================="
echo ""

echo "seed,slack_85c,alm_used,alm_pct,m10k" > "$RESULTS"

best_seed=""
best_slack="-999"

for seed in $(seq 1 $NUM_SEEDS); do
    echo -n "  Seed $seed/$NUM_SEEDS: "
    sed -i "s/^set_global_assignment -name SEED .*/set_global_assignment -name SEED $seed/" "$QSF"

    if ! quartus_fit --read_settings_files=on --write_settings_files=off "$PROJECT" -c "$PROJECT" > "output_files/sweep_s${seed}_fit.log" 2>&1; then
        echo "FIT_FAIL"
        echo "${seed},FIT_FAIL,,,," >> "$RESULTS"
        continue
    fi
    quartus_sta "$PROJECT" -c "$PROJECT" > "output_files/sweep_s${seed}_sta.log" 2>&1 || true

    cp "output_files/${PROJECT}.sta.summary" "$RESULTS_DIR/sta_s${seed}.summary" 2>/dev/null
    cp "output_files/${PROJECT}.fit.summary" "$RESULTS_DIR/fit_s${seed}.summary" 2>/dev/null

    slack=$(grep -A15 "Slow 1100mV 85C Model Setup Summary" output_files/${PROJECT}.sta.rpt | grep "mp_ram.*general\[0\]" | awk -F';' '{for(i=1;i<=NF;i++) if($i ~ /^ *-?[0-9]+\.[0-9]+ *$/) {gsub(/ /,"",$i); print $i; exit}}')
    alm_used=$(grep "Logic utilization" output_files/${PROJECT}.fit.summary | grep -oP '\d+(?= /)' | head -1)
    alm_pct=$(grep "Logic utilization" output_files/${PROJECT}.fit.summary | grep -oP '\d+(?= %)' | head -1)
    m10k=$(grep "Total RAM Blocks" output_files/${PROJECT}.fit.summary | grep -oP '\d+(?= /)' | head -1)

    printf "slack=%-8s ALM=%s (%s%%) M10K=%s\n" "$slack" "$alm_used" "$alm_pct" "$m10k"
    echo "${seed},${slack},${alm_used},${alm_pct},${m10k}" >> "$RESULTS"

    is_better=$(python3 -c "print(1 if float('${slack:-"-999"}') > float('${best_slack}') else 0)" 2>/dev/null)
    if [ "$is_better" = "1" ]; then
        best_seed=$seed
        best_slack=$slack
    fi
done

# Restore best seed
sed -i "s/^set_global_assignment -name SEED .*/set_global_assignment -name SEED $best_seed/" "$QSF"

echo ""
echo "=========================================="
echo "RESULTS (sorted by slack):"
echo "=========================================="
echo ""
head -1 "$RESULTS"
tail -n +2 "$RESULTS" | sort -t, -k2 -rn
echo ""
echo "Best seed: $best_seed (slack: $best_slack)"
echo "QSF set to seed $best_seed"
echo "Results: $RESULTS"
