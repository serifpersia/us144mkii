# ALSA Driver for TASCAM US-122MKII

An unofficial ALSA kernel module for the TASCAM US-122MKII USB audio interface.

## 📢 Project Status


### ✅ Implemented Features
*   **Audio Playback:**
*   **Audio Capture (Recording):** 

### 📝 To-Do & Known Limitations
*   *MIDI IN/OUT(not planned for now)
*   *bug fixes that US122MKII users might find

## Installation and Usage

This is an out-of-tree kernel module, meaning you must compile it against the headers for your specific kernel version.


### Step 1: Blacklist the Stock `snd-usb-us122l & snd-usb-audio` Drivers

The standard kernel includes a driver that will conflict with our custom module. You must prevent it from loading.

Follow the steps to blacklist it if `lsmod | grep snd_usb_us122l` returns results.

1.  **Create a blacklist file.** This tells the system *not* to load the `snd-usb-us122l * snd-usb-audio` module.
    ```bash
    echo "blacklist snd_usb_us122l" | sudo tee /etc/modprobe.d/blacklist-us122l.conf
    ```
    
    ```bash
    echo "blacklist snd_usb_audio" | sudo tee /etc/modprobe.d/blacklist-snd_usb_audio.conf
    ```
    
2.  **Rebuild your initramfs.** This is a critical step that ensures the blacklist is applied at the very start of the boot process, before the stock driver has a chance to load. Run the command corresponding to your distribution:
    *   **Debian / Ubuntu / Pop!_OS / Mint:**
        ```bash
        sudo update-initramfs -u
        ```
    *   **Fedora / RHEL / CentOS Stream:**
        ```bash
        sudo dracut --force
        ```
    *   **Arch Linux / Manjaro:**
        ```bash
        sudo mkinitcpio -P
        ```
    *   **openSUSE:**
        ```bash
        sudo mkinitrd
        ```
3.  **Reboot your computer.**
    Reboot the system and check with `lsmod | grep snd_usb_us122l` again if there is no output the blacklisting is complete.

    
### Step 2: Install Prerequisites (Kernel Headers & Build Tools)
You need the necessary tools to compile kernel modules and the headers for your currently running kernel. Open a terminal and run the command for your Linux distribution:

*You can attempt build without installing linux headers package first, if you are unable to build then you would need to!

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


### Step 3: Compile and Load the Driver
This process will build the module from source and load it for your current session. This is the best way to test it.

1.  Clone this repository and navigate into the source directory.

```bash
git clone https://github.com/serifpersia/us144mkii.git
cd us144mkii/
git checkout us122mkii
```

2.  Compile the module:
    ```bash
    make
    ```

3.  Load the compiled module into the kernel:
    ```bash
    sudo insmod snd-usb-us122mkii.ko
    ```

4.  Connect your TASCAM US-122MKII. Verify that the driver loaded and the audio card is recognized by the system:
    ```bash
    # Check if the kernel module is loaded
    lsmod | grep snd_usb_us122mkii

    # Check if ALSA sees the new sound card
    aplay -l
    ```
    The first command should show `snd_usb_us122mkii`. The second command should list your "TASCAM US-122MKII" as an available playback device. You should now be able to select it in your audio settings and play sound.

### Step 4: Install for Automatic Loading on Boot
To make the driver load automatically every time you start your computer, follow these steps after you have successfully compiled it in Step 3.

You can use build_and_install script to do automate this process just `sudo chmod +x build_and_install.sh` before you run it with `./build_and_install.sh` or just do it
the manual way.

1.  **Copy the compiled module to the kernel's extra modules directory.** This makes it available to system tools.
    ```bash
    sudo mkdir -p /lib/modules/$(uname -r)/extra/us122mkii
    sudo cp snd-usb-us122mkii.ko /lib/modules/$(uname -r)/extra/us122mkii
    ```

2.  **Update module dependencies.** This command rebuilds the map of modules so the kernel knows about our new driver.
    ```bash
    sudo depmod -a
    ```

Now, after a reboot, the `us122mkii` driver should load automatically.

## Reporting Issues & Feedback

If you test this driver, please share your feedback to help improve it. Include:

- Linux distro and version  
- Kernel version (`uname -r`)  
- Exact TASCAM model  
- How you installed and loaded the driver  
- Any errors or problems (logs help)  
- Which features worked (playback, capture, MIDI)  
- Your setup details (e.g., DAW, ALSA/JACK version, buffer/periods used)

All feedback is welcome—whether it’s a bug, a success, or a suggestion!

Please report your findings via the GitHub Issues page.

## License

This project is licensed under the **GPL-2.0** see the [LICENSE](LICENSE) file for details.
