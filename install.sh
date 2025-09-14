#!/bin/bash

# This script automates the setup for the artifact.
# It verifies the OS, installs dependencies, and prepares the disk image.

# Exit immediately if a command exits with a non-zero status.
set -e

## OS Verification
echo "Verifying operating system..."
if ! grep -q "ID=ubuntu" /etc/os-release; then
    echo "Error: This script is intended for Ubuntu-based distributions." >&2
    exit 1
fi
echo "OS check passed. Found Ubuntu."
echo "--------------------------------------------------"

## Dependency Installation
echo "Installing necessary dependencies..."
# Update package lists
sudo apt-get update -y

# Install all required packages in a single command
sudo apt-get install -y \
    sshpass \
    fakeroot \
    kernel-package \
    libelf-dev \
    build-essential \
    libncurses-dev \
    flex \
    bison \
    libssl-dev \
    libfdt-dev \
    libncursesw5-dev \
    pkg-config \
    libgtk-3-dev \
    libspice-server-dev \
    libssh-dev \
    libaio-dev

echo "Dependencies installed successfully."
echo "--------------------------------------------------"

## Image Setup
echo "Setting up the disk image..."

# Define paths for clarity
IMAGE_DIR="$HOME/images"
SOURCE_ARTIFACT="./artifact/mute_ae.img.tar.gz"
DEST_ARTIFACT="$IMAGE_DIR/mute_ae.img.tar.gz"


# 1. Create the target directory
echo "-> Creating directory: $IMAGE_DIR"
mkdir -p "$IMAGE_DIR"

# 2. Download the compressed image file
echo "-> Copying compressed image..."
wget -O $IMAGE_DIR/mute_ae.img.tar.gz "https://fiudit-my.sharepoint.com/:u:/g/personal/weizhu_fiu_edu/EfTfdcf1DKtNkxIaQ8pKCnsBXT61amwReZvCFpgeuv7jng?e=iDCgDt&download=1"

# 3. Decompress the image in the target directory
echo "-> Decompressing image..."
tar -xzvf "$DEST_ARTIFACT" -C "$IMAGE_DIR/"

echo "Image 'mute_ae.img' is now available in $IMAGE_DIR"

## Install libGMP

# 1. Download the GMP package
wget https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz

# 2. Decompress the package
tar xf gmp-6.3.0.tar.xz

cd gmp-6.3.0

# 3. Configuration setting
./configure

# 4. Compile the GMP source code
make -j8

# 5. Install the GMP
sudo make install

echo "--------------------------------------------------"

echo "Setup finished successfully!"
