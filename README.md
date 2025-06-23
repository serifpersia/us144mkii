

Current feat: 
- Vendor Specific Config & Initialization,
- Working Playback with glitches due to missing feedback clock resync(maybe)

To run need pulseaudio and libusb, to compile you need dev packages like gcc to compile the src c code.

debian:
sudo apt update
sudo apt install build-essential pulseaudio pulseaudio-utils libusb-1.0-0-dev
gcc -o tascam_streamer tascam_fifo_streamer.c -lusb-1.0 -Wall -Wextra -pedantic -std=c11
chmod +x run_tascam_streamer.sh
./run_tascam_streamer.sh

fedora:
sudo dnf install @development-tools pulseaudio pulseaudio-tools libusb1-devel
gcc -o tascam_streamer tascam_fifo_streamer.c -lusb-1.0 -Wall -Wextra -pedantic -std=c11
chmod +x run_tascam_streamer.sh
./run_tascam_streamer.sh

arch:
sudo pacman -Syu
sudo pacman -S base-devel pulseaudio pulseaudio-libs libusb
gcc -o tascam_streamer tascam_fifo_streamer.c -lusb-1.0 -Wall -Wextra -pedantic -std=c11
chmod +x run_tascam_streamer.sh
./run_tascam_streamer.sh

void:
sudo xbps-install -S
sudo xbps-install base-devel pulseaudio libusb
gcc -o tascam_streamer tascam_fifo_streamer.c -lusb-1.0 -Wall -Wextra -pedantic -std=c11
chmod +x run_tascam_streamer.sh
./run_tascam_streamer.sh
