# ALSA Driver for TASCAM US-144MKII

An unofficial ALSA kernel module for the TASCAM US-144MKII & US-144 USB audio interface.

For TASCAM US-122MKII check us122mkii branch.

## Project Status

**Upstream Status**  
A version of this driver has been merged into the Linux kernel. However, the in-kernel driver
is **older and lacks fixes and improvements present in this repository**.

This repository contains the **newest and most complete version** of the driver, with improved
functionality and fixes not yet available in the upstream kernel.

### Implemented Features
- **Audio Playback**
- **Audio Capture (Recording)**
- **MIDI IN / OUT**

### Known Limitations
- Non-MKII US-144 devices need more testing

## Installation and Usage

This is an out-of-tree kernel module, meaning you must compile it against the headers for your specific kernel version.

Old version of this driver has been merged and is available on rolling release distros like Arch(6.18.x).
For Arch users, a community-maintained DKMS package is available in AUR if user intends to install improved driver.
Install it via:
```bash
paru -S us144mkii-dkms-git
```
or if you are using yay: 
```bash
yay -S us144mkii-dkms-git
```
### Step 1: Blacklist the Stock `snd-usb-us122l` Driver
The standard kernel includes a driver that will conflict with our custom module. You must prevent it from loading.

Follow the steps to blacklist it if `lsmod | grep snd_usb_us122l` returns results.

1.  **Create a blacklist file.** This tells the system *not* to load the `snd-usb-us122l` module.
    ```bash
    echo "blacklist snd_usb_us122l" | sudo tee /etc/modprobe.d/blacklist-us122l.conf
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

> **Note on a More Aggressive Method:** If the method above does not work, some systems (like Arch) may load the conflicting module before the blacklist is processed. A more forceful method is to use a `udev` rule to de-authorize the device for the kernel entirely, preventing any driver from binding to it automatically.
>
> Create the file `/etc/udev/rules.d/99-tascam-blacklist.rules` and add the following line. This targets the Tascam US-122L/144MKII series product ID (`8007`).
> ```
> ATTR{idVendor}=="0644", ATTR{idProduct}=="8007", ATTR{authorized}="0"
> ```
> After saving, run `sudo udevadm control --reload` and reboot. Note that with this rule in place, you will likely need to load the `us144mkii` driver manually with `sudo insmod snd-usb-us144mkii.ko` each time. The `modprobe` method is preferred for automatic loading.


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
```

2.  Compile the module:
    ```bash
    make
    ```

3.  Load the compiled module into the kernel:
    ```bash
    sudo insmod snd-usb-us144mkii.ko
    ```

4.  Connect your TASCAM US-144MKII. Verify that the driver loaded and the audio card is recognized by the system:
    ```bash
    # Check if the kernel module is loaded
    lsmod | grep snd_usb_us144mkii

    # Check if ALSA sees the new sound card
    aplay -l
    ```
    The first command should show `snd_usb_us144mkii`. The second command should list your "TASCAM US-144MKII" as an available playback device. You should now be able to select it in your audio settings and play sound.

### Step 4: Install for Automatic Loading on Boot
To make the driver load automatically every time you start your computer, follow these steps after you have successfully compiled it in Step 3.

You can use build_and_install script to do automate this process just `sudo chmod +x build_and_install.sh` before you run it with `./build_and_install.sh` or just do it
the manual way.

1.  **Copy the compiled module to the kernel's extra modules directory.** This makes it available to system tools.
    ```bash
    sudo mkdir -p /lib/modules/$(uname -r)/extra/us144mkii
    sudo cp snd-usb-us144mkii.ko /lib/modules/$(uname -r)/extra/us144mkii
    ```

2.  **Update module dependencies.** This command rebuilds the map of modules so the kernel knows about our new driver.
    ```bash
    sudo depmod -a
    ```

Now, after a reboot, the `us144mkii` driver should load automatically.

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
