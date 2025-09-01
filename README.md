# ALSA Driver for TASCAM US-144MKII

An unofficial ALSA kernel module for the TASCAM US-144MKII USB audio interface.

## 📢 Project Status

✅ **Upstreamed** — This driver has been merged into the [`sound/for-next`](https://git.kernel.org/pub/scm/linux/kernel/git/tiwai/sound.git/log/?h=for-next) branch for inclusion in an upcoming Linux kernel release.

📦 The repo is archived and no further updates will be pushed.

### ✅ Implemented Features
*   **Audio Playback:**
*   **Audio Capture (Recording):** 
*   **MIDI IN/OUT:**

### 📝 To-Do & Known Limitations
*   Find Bugs, if possible improve performance/stablity
*   *MIDI IN/OUT works only in active audio streaming(DAW ALSA/JACK or browser audio)
*   Non MKII US-144 needs testing to see if the driver will work with it.

## Installation and Usage

This is an out-of-tree kernel module, meaning you must compile it against the headers for your specific kernel version.

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
    lsmod | grep us144mkii

    # Check if ALSA sees the new sound card
    aplay -l
    ```
    The first command should show `us144mkii`. The second command should list your "TASCAM US-144MKII" as an available playback device. You should now be able to select it in your audio settings and play sound.

### Step 4: Install for Automatic Loading on Boot
To make the driver load automatically every time you start your computer, follow these steps after you have successfully compiled it in Step 3.

You can use build_install script to do automate this process just `sudo chmod +x build_install.sh` before you run it with `./build_install.sh` or just do it
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

### Tascam Control Panel

<img width="543" height="480" alt="image" src="https://github.com/user-attachments/assets/43981c8d-c59e-4d43-b1c8-33e512085219" />


A control panel app built with Qt6 and ALSA.

Get it from releases or build it.

## Prerequisites

Before building the application, ensure you have the following installed on your system:

*   **CMake** (version 3.16 or higher)
*   **C++ Compiler** (supporting C++17, e.g., GCC/G++)
*   **Qt6 Development Libraries** (specifically the `Widgets` module)
*   **ALSA Development Libraries**
*   **Make** (or Ninja)

### Installation of Prerequisites by Distribution

#### Debian/Ubuntu

sudo apt update
sudo apt install cmake build-essential qt6-base-dev qt6-base-dev-tools libasound2-dev

#### Fedora/RHEL/CentOS

sudo dnf install cmake "Development Tools" qt6-qtbase-devel alsa-lib-devel

#### Arch Linux

sudo pacman -Syu

sudo pacman -S cmake base-devel qt6-base alsa-lib

#### openSUSE

sudo zypper install cmake gcc-c++ libqt6-qtbase-devel alsa-devel

## Building the Application

Follow these steps to build the `TascamControlPanel` application from source:

1.  **Clone the repository** (if you haven't already):

    ```git clone https://github.com/serifpersia/us144mkii.git```

    ```cd tascam_controls/```

2.  **Create a build directory** and navigate into it:

    ```mkdir build```
    ```cd build```

4.  **Configure the project** with CMake:

    ```cmake ..```

    This step will check for all necessary dependencies and generate the build files.

5.  **Build the application**:

    ```make -j$(nproc)```

    This command compiles the source code. The -j$(nproc) option uses all available CPU cores to speed up the compilation process.

## Running the Application

After a successful build, the executable will be located in the `build` directory.

```./TascamControlPanel```

## Cleaning the Build

To remove all compiled files and intermediate artifacts, simply delete the `build` directory:

cd ..
rm -rf build


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
