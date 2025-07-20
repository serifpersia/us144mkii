# ALSA Driver for TASCAM US-144MKII

An unofficial ALSA kernel module for the TASCAM US-144MKII USB audio interface.

## ❗ Current Status: Work in Progress

This driver is under active development.

### ✅ Implemented Features
*   **Audio Playback:**
*   **Audio Capture (Recording):** 
*   **MIDI IN/OUT:**

### 📝 To-Do & Known Limitations
*   Find Bugs, if possible improve performance/stablity

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
The standard kernel includes a driver that will conflict with our custom module. You must prevent it from loading.

1.  **Create a blacklist file.** This tells the system *not* to load the `snd-usb-us122l` module.
    ```bash
    echo "blacklist snd_usb_us122l" | sudo tee /etc/modprobe.d/blacklist-us144mkii.conf
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

4.  After rebooting, verify the stock driver is not loaded by running `lsmod | grep snd_usb_us122l`. This command should produce no output.

> **Note on a More Aggressive Method:** If the method above does not work, some systems (like Arch) may load the conflicting module before the blacklist is processed. A more forceful method is to use a `udev` rule to de-authorize the device for the kernel entirely, preventing any driver from binding to it automatically.
>
> Create the file `/etc/udev/rules.d/99-tascam-blacklist.rules` and add the following line. This targets the Tascam US-122L/144MKII series product ID (`8007`).
> ```
> ATTR{idVendor}=="0644", ATTR{idProduct}=="8007", ATTR{authorized}="0"
> ```
> After saving, run `sudo udevadm control --reload` and reboot. Note that with this rule in place, you will likely need to load the `us144mkii` driver manually with `sudo insmod us144mkii.ko` each time. The `modprobe` method is preferred for automatic loading.


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

### Tascam Control Panel

<img width="552" height="469" alt="Screenshot_20250720_231914" src="https://github.com/user-attachments/assets/960f58dc-a072-492e-9cf8-189d2801af29" />


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

## License

This project is licensed under the **GPL-2.0** see the [LICENSE](LICENSE) file for details.
