# Enabling Plausible Deniability in Flash-based Storage through Data Permutation

## Introduction

Plausible deniability (PD) allows at-risk users to deny the existence of their sensitive data stored on storage devices. This is critical to protect the privacy and the personal safety of users, as adversaries might force users to decrypt their devices, risking the disclosure of sensitive data that could endanger their lives and liberty. 

In this work, we show how current PD systems built on flash memory fail to obscure distinguishable data layouts created when hidden data is written. This deficiency makes them vulnerable to coercive adversaries who can capture single or multiple data snapshots of storage devices for scrutiny. To defend against this threat, we propose MUTE, a perMUTationbased PD systEm designed for flash memory. Building upon widely-adopted full disk encryption (FDE) mechanisms that provide device-level data encryption, MUTE modifies the distribution of initialization vectors (IVs) for encryption blocks within FDE, translating the hidden data into a permutation derived from the IV. Unlike other PD solutions, MUTE allows for storing hidden data without requiring the reduction of storage capacity. Moreover, it preserves the plausible deniability of the hidden data in a provably secure manner by maintaining the original logic of data operations on the flash memory without changing the data layout. We implement MUTE in the flash translation layer (FTL) of flash-based SSDs using FEMU, a widely-used emulator supporting flash memory research. Our evaluation with various micro-benchmarks and real-world workloads demonstrates that MUTE provides practical write and read throughputs of 23.4 MB/s and 15.7 MB/s and a capacity of 25.3 GB for hidden data in a 512 GB SSD, comparable with existing PD systems. MUTE achieves strong PD guarantees for flash-based devices against coercive adversaries, outperforming current PD systems.

```
@inproceedings{MUTE:ACSAC2025,
  author = {Weidong Zhu and Wenxuan Bao and Vincent Bindschaedler and Sara Rampazzi and Kevin R. B. Butler},
  title = {Enabling Plausible Deniability in Flash-based Storage through Data Permutation},
  booktitle = {Proceedings of 41st Annual Computer Security Applications Conference (ACSAC)},
  month = {December},
  year = {2025},
}
```

## Experimental Setting

To run our artifact, users should have an Ubuntu computer with sufficient memory and storage space. Please follow the implementation section and Section 8.1 for the detail of our settings.

Since our system was built on FEMU, which is a QEMU-based SSD emulator, users needs to run our emulator on an VM image. Therefore, we recommend to run the artifact on private infrastructure that can give root privilege for the testing. We have uploaded our image to OnceDrive, and reviewers can use the command `./install.sh` to install necessary dependencies and the VM image. Note that the VM image will take 50 GB storage space.

Moreover, running our artifact needs the `sudo` privilege, as QEMU needs to use KVM for virtualization. Please ensure that the user has the root privilege in the host machine.

## Running Our Artifact

We fully automated the testing process to allow reviewers to reproduce our experimental results in our evaluation sections, including Figure 3 - 6.

To run our artifact, reviewers should follow the following commands:

```bash
### Enter the claim folder

cd claims/

### Run the testing script. All evaluation result will be saved in claims/expected/results_csv

sudo ./run.sh

### If you have Latex compiler in the host machine, you would see a PDF file in claims/expected, and the file name is permupd.pdf
### If you don't have a Latex compiler in the Ubuntu machine, you could scp the folder claims/expected to another machine that have Latex compiler to compile the source for the result.

```
