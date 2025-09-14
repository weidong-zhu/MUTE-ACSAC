#!/bin/bash

# Build dependencies for this project

sudo apt install fakeroot kernel-package libelf-dev build-essential libncurses-dev flex bison libssl-dev libfdt-dev libncursesw5-dev pkg-config libgtk-3-dev libspice-server-dev libssh-dev libaio-dev

# Generating configure information.

../configure  --enable-kvm --enable-vnc --enable-curses --enable-spice --enable-gtk --target-list=x86_64-softmmu --disable-werror --extra-cflags="-w" --extra-ldflags="-Wl,--no-as-needed,-lgmp" 

# Installation

make clean

make -j8 


