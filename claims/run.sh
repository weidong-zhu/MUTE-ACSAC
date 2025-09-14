#!/bin/bash

# This script runs the artifact experiments and generates the final PDF report.
# It should be executed from within the 'claims/' directory.

# Exit immediately if any command fails, ensuring the script stops on error.
set -e

echo "Starting artifact evaluation..."

# --- 1. Run Experiments ---
# Navigate from 'claims/' into the 'artifact/' directory.
echo "Changing directory to ../artifact/ to run experiments..."
cd ../artifact/

echo "Running experiment scripts..."
./run_fig3.sh
./run_fig4.sh
./run_fig5.sh
python3 fig6.py # Using python3 is recommended
echo "All experiments completed."

# --- 2. Copy Results ---
# Define the destination directory for the CSV files.
DEST_DIR="../claims/expected/results_csv"

echo "Copying result files to $DEST_DIR..."

# Create the destination directory if it doesn't exist.
mkdir -p "$DEST_DIR"

# Copy all CSV files from the results folder.
cp results/*.csv "$DEST_DIR/"
echo "Results successfully copied."

# --- 3. Generate PDF Report ---
# Navigate from 'artifact/' into the 'claims/expected/' directory.
echo "Changing directory to ../claims/expected/ to build PDF..."
cd ../claims/expected/

echo "Running 'make' to generate the report..."
make
echo "PDF report generated."

