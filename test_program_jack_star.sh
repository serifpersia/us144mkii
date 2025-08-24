#!/bin/bash

# Script to compile and run the Tascam user-space JACK driver.
# It interactively asks for the desired sample rate and latency profile.

# --- Configuration ---
SOURCE_FILE="tascam_test_program_jack.c"
PROG_NAME="test_program_jack"

# --- Check for root ---
if [ "$EUID" -eq 0 ]; then
  echo "Please do not run this script with sudo or as the root user."
  echo "It must be run as a regular user who is part of the 'audio' group."
  exit 1
fi

# --- Cleanup function ---
cleanup() {
    echo -e "\nScript interrupted. Cleaning up..."
    pkill -f "$PROG_NAME" 2>/dev/null
    echo "Driver program stopped."
    exit 0
}

trap cleanup SIGINT

# --- Main Script ---
echo "--- Tascam Driver Setup ---"

# 1. Ask for Sample Rate
echo "Please select a sample rate:"
echo "  1) 44100 Hz"
echo "  2) 48000 Hz"
echo "  3) 88200 Hz"
echo "  4) 96000 Hz"
read -p "Enter choice [1-4, default: 2]: " rate_choice
rate_choice=${rate_choice:-2} # Default to 48000 Hz

case $rate_choice in
    1) RATE=44100 ;;
    2) RATE=48000 ;;
    3) RATE=88200 ;;
    4) RATE=96000 ;;
    *) echo "Invalid choice. Exiting."; exit 1 ;;
esac

# 2. Ask for Latency Profile
echo -e "\nPlease select a latency profile:"
echo "  1) Automatic (Recommended)"
echo "  2) Lowest"
echo "  3) Low"
echo "  4) Normal"
echo "  5) High"
echo "  6) Highest"
read -p "Enter choice [1-6, default: 1]: " profile_choice
profile_choice=${profile_choice:-1} # Default to Automatic

case $profile_choice in
    1) PROFILE=-1 ;; # Use -1 to signal "Automatic" mode
    2) PROFILE=0 ;;
    3) PROFILE=1 ;;
    4) PROFILE=2 ;;
    5) PROFILE=3 ;;
    6) PROFILE=4 ;;
    *) echo "Invalid choice. Exiting."; exit 1 ;;
esac

# 3. Ask for Debug Mode
read -p "Enable live monitoring (debug mode)? [y/N]: " debug_choice
DEBUG_FLAG=""
if [[ "$debug_choice" == "y" || "$debug_choice" == "Y" ]]; then
    DEBUG_FLAG="-d"
fi

# 4. Instruct user to start JACK
echo "------------------------------------------------------------"
echo "Configuration selected:"
echo "  - Sample Rate: $RATE Hz"
echo ""
echo "ACTION REQUIRED:"
echo "Please start the JACK server now (e.g., using QJackCtl)."
echo "Ensure its Sample Rate is set to $RATE Hz."
echo "------------------------------------------------------------"
read -p "Press Enter when JACK is running..."

# 5. Verify JACK is running
echo "Checking for a running JACK server..."
if ! jack_lsp >/dev/null 2>&1; then
    echo "ERROR: JACK server was not detected. Please start it and try again."
    exit 1
fi
echo "JACK server detected."

# 6. Compile the driver program
echo "Compiling the driver program..."
rm -f "$PROG_NAME"
gcc -o "$PROG_NAME" "$SOURCE_FILE" -Wall -lusb-1.0 -lpthread -lm -ljack
if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi
echo "Compilation successful."

# 7. Run the driver
echo ""
echo "--- Starting Tascam User-Space Driver ---"
echo "Connect your JACK clients now (e.g., Ardour, QJackCtl Graph/Patchbay)."
echo "Press Ctrl+C in this terminal to stop the driver."
echo ""

"./$PROG_NAME" -r "$RATE" -p "$PROFILE" $DEBUG_FLAG

echo "Driver stopped."
