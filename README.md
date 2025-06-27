

Current feat: 
- Vendor Specific Config & Initialization,
- Working Playback(not fully tested but works without issues at 48khz)

- migrated working playback(with some glitches still) to kernel ALSA driver
    - To compile and run it follow these steps:
        - Blacklist us122l driver in order to be able to use this custom driver
        - Get your linux headers via your package manager
        - cd to driver directory & run sudo insmod us144mk2.ko
        - Under sound setting you should see TASCAM US144MKII device
        - the device should produce audio playback with glitches
    - The release alsa custom driver might not work on newer kernel versions currently its been written on 
    debian 12 6.1 kernel. You might need to compile kernel with `make` and sudo insmod it.

To run need pulseaudio and libusb, to compile you need dev packages like gcc to compile the src c code.

To make the compiled or release kernel module run on boot execute these commands
`echo "us144mkii" | sudo tee /etc/modules-load.d/us144mkii.conf`
`sudo mkdir -p /lib/modules/$(uname -r)/extra/us144mkii/`
`sudo cp us144mkii.ko /lib/modules/$(uname -r)/extra/us144mkii/`
`sudo depmod -a`
`sudo systemctl restart systemd-modules-load.service`


debian:
sudo apt update
sudo apt install build-essential pulseaudio pulseaudio-utils libusb-1.0-0-dev
gcc -o tascam_streamer tascam_fifo_streamer.c -lusb-1.0 -Wall
chmod +x run_tascam_streamer.sh
./run_tascam_streamer.sh

fedora:
sudo dnf install @development-tools pulseaudio pulseaudio-tools libusb1-devel
gcc -o tascam_streamer tascam_fifo_streamer.c -lusb-1.0 -Wall
chmod +x run_tascam_streamer.sh
./run_tascam_streamer.sh

arch:
sudo pacman -Syu
sudo pacman -S base-devel pulseaudio pulseaudio-libs libusb
gcc -o tascam_streamer tascam_fifo_streamer.c -lusb-1.0 -Wall
chmod +x run_tascam_streamer.sh
./run_tascam_streamer.sh

void:
sudo xbps-install -S
sudo xbps-install base-devel pulseaudio libusb
gcc -o tascam_streamer tascam_fifo_streamer.c -lusb-1.0 -Wall
chmod +x run_tascam_streamer.sh
./run_tascam_streamer.sh
