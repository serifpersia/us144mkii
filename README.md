

Current feat: 
- Vendor Specific Config & Initialization,
- Working Playback(not fully tested but works without issues at 48khz)

- To compile and run it follow these steps:
  - Blacklist us122l driver in order to be able to use this custom driver
    - Get your linux headers via your package manager
    - cd to driver directory & run sudo insmod us144mkii.ko
    - the device should produce audio playback
        
    - The release alsa custom driver might not work on newer kernel versions currently its been written on 
    debian 12 6.1 kernel. You might need to compile kernel with `make` and sudo insmod it. Usual build essentials gcc make packages needed to be installed to compile the ALSA kernel module

To make the compiled or release kernel module run on boot execute these commands
    `echo "us144mkii" | sudo tee /etc/modules-load.d/us144mkii.conf`
    `sudo mkdir -p /lib/modules/$(uname -r)/extra/us144mkii/`
    `sudo cp us144mkii.ko /lib/modules/$(uname -r)/extra/us144mkii/`
    `sudo depmod -a`
    `sudo systemctl restart systemd-modules-load.service`

