#!/bin/bash
#
# ACSAC Artifact Evaluation Script for Figure 3 (v3)
#
# This script automates the FIO benchmark evaluation.
# FIX: Each mode (e.g., PEARL-P, PEARL-H) is run in a separate, clean VM instance.
# FIX: Corrected the absolute path for the 'hidden_enabler' tool inside the VM.
#

# Stop on any error (except where explicitly handled)
set -e

# --- Configuration ---
VM_USER="weidong"
VM_PASS="1"
VM_SSH_PORT="8080"
VM_HOST="localhost"
FIO_DIR="/home/weidong/OptaneBench/fio"
RESULTS_DIR="results"
CSV_FILE="bw.csv"

# Associative array to hold all bandwidth results
declare -A results

# --- Helper Functions ---
# (These functions are now correct and remain unchanged)

# Function to run a command inside the running VM
run_in_vm() {
    local cmd="$1"
    echo "VM <<< $cmd"
    sshpass -p "$VM_PASS" ssh -t -p $VM_SSH_PORT -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ${VM_USER}@${VM_HOST} "echo '$VM_PASS' | sudo -S bash -c \"$cmd\""
}

# Function to extract bandwidth from an FIO output file
parse_bw() {
    local file="$1"
    grep 'bw=' "$file" | awk -F'(bw=|MiB/s)' '{print $2}' | head -n 1
}

# --- Main Logic ---

# 1. Setup
echo "--- Starting Figure 3 Evaluation ---"
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
        run_in_vm "fio ${FIO_DIR}/fill.fio"
    fi

    # Enable the specific hidden mode if necessary
    # Note the corrected absolute path to hidden_enabler
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

    # Run the standard FIO benchmarks
    echo "Running FIO benchmarks for $test_case..."
    run_in_vm "fio ${FIO_DIR}/fio-sw-4k.fio > /home/${VM_USER}/fio_result.sw"
    run_in_vm "fio ${FIO_DIR}/fio-rw-4k.fio > /home/${VM_USER}/fio_result.rw"
    run_in_vm "fio ${FIO_DIR}/fio-sr-4k.fio > /home/${VM_USER}/fio_result.sr"
    run_in_vm "fio ${FIO_DIR}/fio-rr-4k.fio > /home/${VM_USER}/fio_result.rr"

    # Copy results and parse
    sshpass -p "$VM_PASS" scp -P $VM_SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ${VM_USER}@${VM_HOST}:/home/${VM_USER}/fio_result.* .
    results[${test_case}_SW]=$(parse_bw fio_result.sw); results[${test_case}_SR]=$(parse_bw fio_result.sr)
    results[${test_case}_RW]=$(parse_bw fio_result.rw); results[${test_case}_RR]=$(parse_bw fio_result.rr)
    rm fio_result.*

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
    echo "SW,${results[SSD-FDE_SW]},${results[PEARL-P_SW]},${results[PEARL-H_SW]},${results[MDEFTL-P_SW]},${results[MDEFTL-H_SW]},${results[MUTE-PO_SW]},${results[MUTE-PH_SW]},${results[MUTE-H_SW]}"
    echo "SR,${results[SSD-FDE_SR]},${results[PEARL-P_SR]},${results[PEARL-H_SR]},${results[MDEFTL-P_SR]},${results[MDEFTL-H_SR]},${results[MUTE-PO_SR]},${results[MUTE-PH_SR]},${results[MUTE-H_SR]}"
    echo "RW,${results[SSD-FDE_RW]},${results[PEARL-P_RW]},${results[PEARL-H_RW]},${results[MDEFTL-P_RW]},${results[MDEFTL-H_RW]},${results[MUTE-PO_RW]},${results[MUTE-PH_RW]},${results[MUTE-H_RW]}"
    echo "RR,${results[SSD-FDE_RR]},${results[PEARL-P_RR]},${results[PEARL-H_RR]},${results[MDEFTL-P_RR]},${results[MDEFTL-H_RR]},${results[MUTE-PO_RR]},${results[MUTE-PH_RR]},${results[MUTE-H_RR]}"
} > "$CSV_FILE"

cd ..
echo -e "\n--- Evaluation for Figure 3 Complete! ---"
echo "Results are in $RESULTS_DIR/$CSV_FILE"
