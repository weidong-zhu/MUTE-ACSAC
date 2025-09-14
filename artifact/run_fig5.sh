#!/bin/bash
#
# ACSAC Artifact Evaluation Script for Figure 5
#
# This script automates the MSR workload evaluation to measure Write Amplification Factor (WAF).
# It performs a full VM reboot for each workload and captures the WAF from the host machine.
#

# Stop on any error (except where explicitly handled)
set -e

# --- Configuration ---
VM_USER="weidong"
VM_PASS="1"
VM_SSH_PORT="8080"
VM_HOST="localhost"
FIO_FILL_SCRIPT="/home/weidong/OptaneBench/fio/fill.fio"
RESULTS_DIR="results"
CSV_FILE="waf.csv"

# Associative array to hold all WAF results
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
echo "--- Starting Figure 5 Evaluation ---"
mkdir -p "$RESULTS_DIR"
cd "$RESULTS_DIR"

# Define all individual test cases and workloads
TEST_CASES=("SSD-FDE" "PEARL-P" "PEARL-H" "MDEFTL-P" "MDEFTL-H" "MUTE-PO" "MUTE-PH" "MUTE-H")
#TEST_CASES=("MUTE-H")

WORKLOADS=("hm" "prxy" "rsrch" "wdev")

# Loop through each test case
for test_case in "${TEST_CASES[@]}"; do
    # Loop through each workload for the current test case
    for workload in "${WORKLOADS[@]}"; do

        echo -e "\n\n======================================================="
        echo "--- Testing Case: $test_case | Workload: $workload ---"
        echo "======================================================="

        # Determine the configuration directory
        config_dir=""
        if [[ "$test_case" == "SSD-FDE" ]]; then config_dir="baseline"
        elif [[ "$test_case" == PEARL-* ]]; then config_dir="pearl"
        elif [[ "$test_case" == MDEFTL-* ]]; then config_dir="mdeftl"
        elif [[ "$test_case" == MUTE-* ]]; then config_dir="mute"
        fi

        # --- VM Setup for the current run ---
        cd ../$config_dir/build

        echo "Compiling $config_dir..."
        ./install.sh

        echo "Starting VM..."
        tmux new-session -d -s "vm_session" "./run-blackbox-fde.sh"

        echo "Waiting for VM to boot..."
        while ! sshpass -p "$VM_PASS" ssh -p $VM_SSH_PORT -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ${VM_USER}@${VM_HOST} "exit" >/dev/null 2>&1; do
            sleep 5
        done
        echo "VM is up!"

        # --- Run Test Logic in VM ---

        # Define workload paths
        warmup_workload_path="/home/weidong/trace_running/workloads_in_use/${workload}_0.csv.comb.atc"
        normal_workload_path="/home/weidong/trace_running/workloads_in_use/normal_${workload}_20000.txt"

        if [[ "$test_case" == "MUTE-H" ]]; then
            # Special workflow for MUTE-H
            echo "Running special MUTE-H workflow..."
            
            # 1. Fill the drive
            echo "Preparing MUTE-H by filling the drive..."
            run_in_vm "fio ${FIO_FILL_SCRIPT}"

            # 2. Run the warmup trace BEFORE enabling hidden mode
            echo "Running MUTE-H warmup trace: $workload"
            run_in_vm "./stale_test_msr.sh ${warmup_workload_path}"

            # 3. Enable hidden mode
            run_in_vm "/home/${VM_USER}/Toy-program/aio_test/hidden_enabler /dev/nvme0n1 Hidden hiddenpassword"
            
            # 4. Run the normal trace for the actual measurement
            echo "Running MUTE-H measurement trace: $workload"
            run_in_vm "./stale_test_msr.sh ${normal_workload_path}"

        else
            # Standard workflow for all other test cases
            
            # If it's any other hidden mode, fill the drive and enable the mode
            if [[ "$test_case" == *"-H" || "$test_case" == *"-PH" ]]; then
                echo "Preparing for hidden mode test by filling the drive..."
                run_in_vm "fio ${FIO_FILL_SCRIPT}"
                
                case "$test_case" in
                    "PEARL-H") run_in_vm "/home/${VM_USER}/Toy-program/aio_test/hidden_enabler /dev/nvme0n1 Hidden hiddenpassword";;
                    "MDEFTL-H") run_in_vm "/home/${VM_USER}/Toy-program/aio_test/hidden_enabler /dev/nvme0n1 Hidden hiddenpassword";;
                    "MUTE-PH") run_in_vm "/home/${VM_USER}/Toy-program/aio_test/hidden_enabler /dev/nvme0n1 Public+hidden hiddenpassword";;
                esac
            fi
            
            # Run the normal MSR trace for measurement
            echo "Running MSR workload in VM: $workload"
            run_in_vm "./stale_test_msr.sh ${normal_workload_path}"
        fi

        # Shutdown VM to generate WAF result on host
        echo "Shutting down VM to generate WAF result..."
        run_in_vm "poweroff" || true
        while tmux has-session -t "vm_session" 2>/dev/null; do
            sleep 5
        done
        echo "VM has shut down."

        # --- Capture Result on Host ---
        echo "Capturing WAF result from host..."
        waf_value=$(cat waf.result)
        results[${test_case}_${workload}]=$waf_value
        echo "Captured WAF for $test_case/$workload: $waf_value"
        #rm waf.result

        # Return to the main results directory for the next loop
        cd ../..
        cd "$RESULTS_DIR"
    done
done

# --- Final Report Generation ---
echo -e "\n\n--- All test runs complete. Generating final CSV file: $CSV_FILE ---"
{
    echo "trace,SSD-FDE,PEARL-P,PEARL-H,MDEFTL-P,MDEFTL-H,MUTE-PO,MUTE-PH,MUTE-H"
    echo "hm,${results[SSD-FDE_hm]},${results[PEARL-P_hm]},${results[PEARL-H_hm]},${results[MDEFTL-P_hm]},${results[MDEFTL-H_hm]},${results[MUTE-PO_hm]},${results[MUTE-PH_hm]},${results[MUTE-H_hm]}"
    echo "prxy,${results[SSD-FDE_prxy]},${results[PEARL-P_prxy]},${results[PEARL-H_prxy]},${results[MDEFTL-P_prxy]},${results[MDEFTL-H_prxy]},${results[MUTE-PO_prxy]},${results[MUTE-PH_prxy]},${results[MUTE-H_prxy]}"
    echo "rsrch,${results[SSD-FDE_rsrch]},${results[PEARL-P_rsrch]},${results[PEARL-H_rsrch]},${results[MDEFTL-P_rsrch]},${results[MDEFTL-H_rsrch]},${results[MUTE-PO_rsrch]},${results[MUTE-PH_rsrch]},${results[MUTE-H_rsrch]}"
    echo "wdev,${results[SSD-FDE_wdev]},${results[PEARL-P_wdev]},${results[PEARL-H_wdev]},${results[MDEFTL-P_wdev]},${results[MDEFTL-H_wdev]},${results[MUTE-PO_wdev]},${results[MUTE-PH_wdev]},${results[MUTE-H_wdev]}"
} > "$CSV_FILE"

cd ..
echo -e "\n--- Evaluation for Figure 5 Complete! ---"
echo "Results are in $RESULTS_DIR/$CSV_FILE"
