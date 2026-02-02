#!/bin/bash
# Fitter seed sweep - tries multiple seeds and reports timing for each
# Usage: ./seed_sweep.sh [start_seed] [end_seed]
# Default: seeds 2 through 6

START=${1:-2}
END=${2:-6}
PROJECT=ap_core
QSF="${PROJECT}.qsf"
RESULTS_FILE="seed_sweep_results.txt"

echo "Seed sweep: $START to $END" | tee "$RESULTS_FILE"
echo "========================================" | tee -a "$RESULTS_FILE"

for seed in $(seq $START $END); do
    echo ""
    echo ">>> Building with SEED=$seed ..." | tee -a "$RESULTS_FILE"

    # Update seed in QSF
    sed -i "s/^set_global_assignment -name SEED .*/set_global_assignment -name SEED $seed/" "$QSF"

    # Run full compile
    quartus_sh --flow compile "$PROJECT" > "output_files/seed_${seed}_build.log" 2>&1
    status=$?

    if [ $status -ne 0 ]; then
        echo "  SEED $seed: BUILD FAILED (exit code $status)" | tee -a "$RESULTS_FILE"
        continue
    fi

    # Extract warm and cold slack
    warm=$(grep "Slow 1100mV 85C Model Setup 'ic|mp_ram" output_files/${PROJECT}.sta.summary | awk '{print $NF}')
    cold=$(grep "Slow 1100mV 0C Model Setup 'ic|mp_ram" output_files/${PROJECT}.sta.summary | awk '{print $NF}')

    echo "  SEED $seed: warm=$warm  cold=$cold" | tee -a "$RESULTS_FILE"

    # Save the full STA summary for this seed
    cp output_files/${PROJECT}.sta.summary "output_files/sta_seed_${seed}.summary"

    # If both pass (positive slack), save the SOF
    warm_pass=$(echo "$warm >= 0" | bc -l 2>/dev/null)
    cold_pass=$(echo "$cold >= 0" | bc -l 2>/dev/null)
    if [ "$warm_pass" = "1" ] && [ "$cold_pass" = "1" ]; then
        echo "  >>> SEED $seed PASSES both corners! Saving SOF." | tee -a "$RESULTS_FILE"
        cp output_files/${PROJECT}.sof "output_files/${PROJECT}_seed${seed}.sof"
    fi
done

echo ""
echo "========================================" | tee -a "$RESULTS_FILE"
echo "Sweep complete. Results in $RESULTS_FILE" | tee -a "$RESULTS_FILE"
