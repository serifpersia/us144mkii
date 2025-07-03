# ALSA Driver for TASCAM US-144MKII

An unofficial ALSA kernel module for the TASCAM US-144MKII USB audio interface.

## ❗ Current Status: Work in Progress

This driver is under active development. While playback is functional, several key features are not yet implemented.

### ✅ Implemented Features
*   **Vendor-Specific Device Initialization:** Correctly sets up the device for audio streaming.
*   **Audio Playback:**
    *   2-channel, 24-bit (S24_3LE) format.
    *   Supported sample rates: 44.1, 48, 88.2, and 96 kHz.

### 📝 To-Do & Known Limitations
*   **Audio Capture (Recording):** Not yet implemented.
*   **MIDI IN/OUT:** Not yet implemented.

## Installation and Usage

This is an out-of-tree kernel module, meaning you must compile it against the headers for your specific kernel version.

### Step 1: Install Prerequisites (Kernel Headers & Build Tools)
You need the necessary tools to compile kernel modules and the headers for your currently running kernel. Open a terminal and run the command for your Linux distribution:

*   **Debian / Ubuntu / Pop!_OS / Mint:**
    ```bash
    sudo apt update
    sudo apt install build-essential linux-headers-$(uname -r)
    ```

*   **Fedora / CentOS Stream / RHEL:**
    ```bash
    sudo dnf install kernel-devel kernel-headers make gcc
    ```

*   **Arch Linux / Manjaro:**
    ```bash
    sudo pacman -S base-devel linux-headers
    ```

*   **openSUSE:**
    ```bash
    sudo zypper install -t pattern devel_basis
    sudo zypper install kernel-devel
    ```

### Step 2: Blacklist the Stock `snd-usb-us122l` Driver
The standard kernel includes a driver that will claim the US-144MKII. This driver will conflict with our custom module. You must prevent it from loading.

1.  Create a blacklist configuration file using the following command. This tells the system *not* to load the `snd-usb-us122l` module at boot.
    ```bash
    echo "blacklist snd_usb_us122l" | sudo tee /etc/modprobe.d/blacklist-us144mkii.conf
    ```
2.  **Reboot your computer** for this change to take full effect.
3.  After rebooting, verify the stock driver is not loaded by running `lsmod | grep snd_usb_us122l`. This command should produce no output.

### Step 3: Compile and Load the Driver
This process will build the module from source and load it for your current session. This is the best way to test it.

1.  Clone this repository and navigate into the source directory.

2.  Compile the module:
    ```bash
    make
    ```

3.  Load the compiled module into the kernel:
    ```bash
    sudo insmod us144mkii.ko
    ```

4.  Connect your TASCAM US-144MKII. Verify that the driver loaded and the audio card is recognized by the system:
    ```bash
    # Check if the kernel module is loaded
    lsmod | grep us144mkii

    # Check if ALSA sees the new sound card
    aplay -l
    ```
    The first command should show `us144mkii`. The second command should list your "TASCAM US-144MKII" as an available playback device. You should now be able to select it in your audio settings and play sound.

### Step 4: Install for Automatic Loading on Boot
To make the driver load automatically every time you start your computer, follow these steps after you have successfully compiled it in Step 3.

1.  **Tell the system to load the module on boot.**
    ```bash
    echo "us144mkii" | sudo tee /etc/modules-load.d/us144mkii.conf
    ```

2.  **Copy the compiled module to the kernel's extra modules directory.** This makes it available to system tools.
    ```bash
    sudo cp us144mkii.ko /lib/modules/$(uname -r)/extra/
    ```

3.  **Update module dependencies.** This command rebuilds the map of modules so the kernel knows about our new driver.
    ```bash
    sudo depmod -a
    ```

Now, after a reboot, the `us144mkii` driver should load automatically.



## License

This project is licensed under the **GPL-2.0** see the [LICENSE](LICENSE) file for details.
