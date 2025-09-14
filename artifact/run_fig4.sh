#!/bin/bash
#
# ACSAC Artifact Evaluation Script for Figure 4
#
# This script automates the MSR workload evaluation for Baseline, PEARL, MDEFTL, and MUTE.
# It measures average latency and generates latency.csv.
#

# Stop on any error (except where explicitly handled)
set -e

# --- Configuration ---
VM_USER="weidong"
VM_PASS="1"
VM_SSH_PORT="8080"
VM_HOST="localhost"
FIO_FILL_SCRIPT="/home/weidong/OptaneBench/fio/fill.fio"
MSR_RUNNER_SCRIPT="/home/weidong/performance_test_all.sh"
MSR_RESULTS_PATH="/home/weidong/trace_running/workloads_in_use"
RESULTS_DIR="results"
CSV_FILE="latency.csv"

# Associative array to hold all latency results
declare -A results

# --- Helper Functions ---

# Function to run a command inside the running VM
run_in_vm() {
    local cmd="$1"
    echo "VM <<< $cmd"
    sshpass -p "$VM_PASS" ssh -t -p $VM_SSH_PORT -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ${VM_USER}@${VM_HOST} "echo '$VM_PASS' | sudo -S bash -c \"$cmd\""
}

# --- Main Logic ---

# 1. Setup
echo "--- Starting Figure 4 Evaluation ---"
mkdir -p "$RESULTS_DIR"
cd "$RESULTS_DIR"

# Define all individual test cases
TEST_CASES=("SSD-FDE" "PEARL-P" "PEARL-H" "MDEFTL-P" "MDEFTL-H" "MUTE-PO" "MUTE-PH" "MUTE-H")

for test_case in "${TEST_CASES[@]}"; do
    echo -e "\n\n============================================="
    echo "--- Starting Test Case: $test_case ---"
    echo "============================================="

    # Determine the configuration directory for the current test case
    config_dir=""
    if [[ "$test_case" == "SSD-FDE" ]]; then config_dir="baseline"
    elif [[ "$test_case" == PEARL-* ]]; then config_dir="pearl"
    elif [[ "$test_case" == MDEFTL-* ]]; then config_dir="mdeftl"
    elif [[ "$test_case" == MUTE-* ]]; then config_dir="mute"
    fi

    # --- VM Setup for the current test case ---
    cd ../$config_dir/build

    echo "Compiling $config_dir for $test_case..."
    ./install.sh

    echo "Starting VM for $test_case..."
    tmux new-session -d -s "vm_session" "./run-blackbox-fde.sh"

    echo "Waiting for VM to boot..."
    while ! sshpass -p "$VM_PASS" ssh -p $VM_SSH_PORT -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ${VM_USER}@${VM_HOST} "exit" >/dev/null 2>&1; do
        sleep 5
    done
    echo "VM is up!"

    # --- Run Test Logic for the current test case ---

    # If it's a hidden mode, fill the drive first
    if [[ "$test_case" == *"-H" || "$test_case" == *"-PH" ]]; then
        echo "Preparing for hidden mode test by filling the drive..."
        run_in_vm "fio ${FIO_FILL_SCRIPT}"
    fi

    # Enable the specific hidden mode if necessary
    case "$test_case" in
        "PEARL-H")
            run_in_vm "/home/${VM_USER}/Toy-program/aio_test/hidden_enabler /dev/nvme0n1 Hidden hiddenpassword"
            ;;
        "MDEFTL-H")
            run_in_vm "/home/${VM_USER}/Toy-program/aio_test/hidden_enabler /dev/nvme0n1 Hidden hiddenpassword"
            ;;
        "MUTE-PH")
            run_in_vm "/home/${VM_USER}/Toy-program/aio_test/hidden_enabler /dev/nvme0n1 Public+hidden hiddenpassword"
            ;;
        "MUTE-H")
            run_in_vm "/home/${VM_USER}/Toy-program/aio_test/hidden_enabler /dev/nvme0n1 Hidden hiddenpassword"
            ;;
    esac

    # Run the MSR benchmark script
    echo "Running MSR workloads for $test_case..."
    run_in_vm "$MSR_RUNNER_SCRIPT"

    # Copy latency results back from the VM
    echo "Copying latency files from VM..."
    sshpass -p "$VM_PASS" scp -P $VM_SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "${VM_USER}@${VM_HOST}:${MSR_RESULTS_PATH}/*.latency" .
    
    # Parse latency files and store results
    results[${test_case}_hm]=$(cat normal_hm_20000.txt.latency)
    results[${test_case}_prxy]=$(cat normal_prxy_20000.txt.latency)
    results[${test_case}_rsrch]=$(cat normal_rsrch_20000.txt.latency)
    results[${test_case}_wdev]=$(cat normal_wdev_20000.txt.latency)
    rm *.latency

    # --- VM Shutdown for the current test case ---
    echo "Shutting down VM for $test_case..."
    run_in_vm "poweroff" || true
    while tmux has-session -t "vm_session" 2>/dev/null; do
        sleep 5
    done
    echo "VM has shut down."

    # Return to the main results directory for the next loop
    cd ../..
    cd "$RESULTS_DIR"
done

# --- Final Report Generation ---
echo -e "\n\n--- All test cases complete. Generating final CSV file: $CSV_FILE ---"
{
    echo "trace,SSD-FDE,PEARL-P,PEARL-H,MDEFTL-P,MDEFTL-H,MUTE-PO,MUTE-PH,MUTE-H"
    echo "hm,${results[SSD-FDE_hm]},${results[PEARL-P_hm]},${results[PEARL-H_hm]},${results[MDEFTL-P_hm]},${results[MDEFTL-H_hm]},${results[MUTE-PO_hm]},${results[MUTE-PH_hm]},${results[MUTE-H_hm]}"
    echo "prxy,${results[SSD-FDE_prxy]},${results[PEARL-P_prxy]},${results[PEARL-H_prxy]},${results[MDEFTL-P_prxy]},${results[MDEFTL-H_prxy]},${results[MUTE-PO_prxy]},${results[MUTE-PH_prxy]},${results[MUTE-H_prxy]}"
    echo "rsrch,${results[SSD-FDE_rsrch]},${results[PEARL-P_rsrch]},${results[PEARL-H_rsrch]},${results[MDEFTL-P_rsrch]},${results[MDEFTL-H_rsrch]},${results[MUTE-PO_rsrch]},${results[MUTE-PH_rsrch]},${results[MUTE-H_rsrch]}"
    echo "wdev,${results[SSD-FDE_wdev]},${results[PEARL-P_wdev]},${results[PEARL-H_wdev]},${results[MDEFTL-P_wdev]},${results[MDEFTL-H_wdev]},${results[MUTE-PO_wdev]},${results[MUTE-PH_wdev]},${results[MUTE-H_wdev]}"
} > "$CSV_FILE"

cd ..
echo -e "\n--- Evaluation for Figure 4 Complete! ---"
echo "Results are in $RESULTS_DIR/$CSV_FILE"
