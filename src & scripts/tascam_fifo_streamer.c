
# MIT License

#Copyright (c) 2025 serifpersia

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libusb-1.0/libusb.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>

// --- Device and Endpoint Configuration ---
#define TASCAM_VID 0x0644
#define TASCAM_PID 0x8020
#define EP_AUDIO_OUT         0x02
#define EP_PLAYBACK_FEEDBACK 0x81

// --- USB Request Types ---
#define RT_H2D_CLASS_EP   0x22
#define RT_D2H_CLASS_EP   0xa2
#define RT_H2D_VENDOR_DEV 0x40
#define RT_D2H_VENDOR_DEV 0xc0

// --- UAC / Vendor Requests ---
#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100
#define VENDOR_REQ_REGISTER_WRITE 65
#define VENDOR_REQ_MODE_CONTROL   73

// --- Streaming Configuration ---
#define BYTES_PER_SAMPLE 3
#define DEVICE_CHANNELS 4
#define DEVICE_FRAME_SIZE (DEVICE_CHANNELS * BYTES_PER_SAMPLE) // 12 bytes

#define PIPE_CHANNELS 2
#define PIPE_FRAME_SIZE (PIPE_CHANNELS * BYTES_PER_SAMPLE) // 6 bytes

#define ISO_PACKETS_PER_TRANSFER 8
#define NUM_ISO_TRANSFERS 8
#define USB_TIMEOUT 1000

// --- Global State ---
static volatile bool is_running = true;
struct stream_state {
    int playback_fifo_fd;
};

// --- Function Prototypes ---
void print_usage(const char *prog_name);
int perform_initialization_sequence(libusb_device_handle *handle, int rate);
static void LIBUSB_CALL iso_playback_callback(struct libusb_transfer *transfer);
static void LIBUSB_CALL feedback_callback(struct libusb_transfer *transfer);

// --- Signal Handler ---
void sigint_handler(int signum) {
    if (is_running) {
        printf("\nCtrl+C detected, stopping...\n");
        is_running = false;
    }
}

// --- Main Program ---
int main(int argc, char *argv[]) {
    libusb_device_handle *handle = NULL;
    struct libusb_transfer *playback_transfers[NUM_ISO_TRANSFERS] = {0};
    struct libusb_transfer *feedback_transfers[NUM_ISO_TRANSFERS] = {0};
    struct stream_state state = {.playback_fifo_fd = -1};
    bool kernel_driver_was_active[2] = {false, false};
    int r = 0;

    int sample_rate = 0;
    const char *playback_pipe_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) { sample_rate = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--playback-pipe") == 0 && i + 1 < argc) { playback_pipe_path = argv[++i]; }
        else { print_usage(argv[0]); return 1; }
    }

    if (sample_rate == 0 || !playback_pipe_path) {
        fprintf(stderr, "Error: Missing required arguments.\n");
        print_usage(argv[0]); return 1;
    }
    if (sample_rate != 44100 && sample_rate != 48000 && sample_rate != 88200 && sample_rate != 96000) {
        fprintf(stderr, "Error: Sample rate %d is not supported.\n", sample_rate); return 1;
    }

    const int frames_per_packet_playback = (sample_rate > 48000) ? 12 : 6;
    const int playback_packet_size = frames_per_packet_playback * DEVICE_FRAME_SIZE;
    const int playback_transfer_size = playback_packet_size * ISO_PACKETS_PER_TRANSFER;
    const int feedback_packet_size = 3;
    const int feedback_transfer_size = feedback_packet_size * ISO_PACKETS_PER_TRANSFER;

    printf("Initializing TASCAM US-144 MKII for %d Hz...\n", sample_rate);
    printf("  Playback Pipe: %s (using Iso EP 0x%02x)\n", playback_pipe_path, EP_AUDIO_OUT);

    signal(SIGINT, sigint_handler);
    if (libusb_init(NULL) < 0) return 1;
    handle = libusb_open_device_with_vid_pid(NULL, TASCAM_VID, TASCAM_PID);
    if (!handle) { fprintf(stderr, "Device not found\n"); r = 1; goto cleanup; }

    for (int i = 0; i < 2; i++) {
        if (libusb_kernel_driver_active(handle, i)) {
            kernel_driver_was_active[i] = true;
            if ((r = libusb_detach_kernel_driver(handle, i)) != 0) {
                fprintf(stderr, "Could not detach kernel driver for interface %d: %s\n", i, libusb_error_name(r));
                r = 1; goto cleanup;
            }
        }
    }

    if (perform_initialization_sequence(handle, sample_rate) != 0) { fprintf(stderr, "Device configuration failed.\n"); r = 1; goto cleanup; }

    state.playback_fifo_fd = open(playback_pipe_path, O_RDONLY | O_NONBLOCK);
    if (state.playback_fifo_fd < 0) { perror("Opening playback FIFO failed"); r = 1; goto cleanup; }
    char drain_buf[1024]; while (read(state.playback_fifo_fd, drain_buf, sizeof(drain_buf)) > 0);

    printf("Starting playback stream (EP 0x%02x) and feedback stream (EP 0x%02x)...\n", EP_AUDIO_OUT, EP_PLAYBACK_FEEDBACK);
    for (int i = 0; i < NUM_ISO_TRANSFERS; i++) {
        playback_transfers[i] = libusb_alloc_transfer(ISO_PACKETS_PER_TRANSFER);
        unsigned char *buf = malloc(playback_transfer_size);
        memset(buf, 0, playback_transfer_size);
        libusb_fill_iso_transfer(playback_transfers[i], handle, EP_AUDIO_OUT, buf, playback_transfer_size, ISO_PACKETS_PER_TRANSFER, iso_playback_callback, &state, USB_TIMEOUT);
        libusb_set_iso_packet_lengths(playback_transfers[i], playback_packet_size);
        libusb_submit_transfer(playback_transfers[i]);

        feedback_transfers[i] = libusb_alloc_transfer(ISO_PACKETS_PER_TRANSFER);
        buf = malloc(feedback_transfer_size);
        libusb_fill_iso_transfer(feedback_transfers[i], handle, EP_PLAYBACK_FEEDBACK, buf, feedback_transfer_size, ISO_PACKETS_PER_TRANSFER, feedback_callback, NULL, USB_TIMEOUT);
        libusb_set_iso_packet_lengths(feedback_transfers[i], feedback_packet_size);
        libusb_submit_transfer(feedback_transfers[i]);
    }

    printf("\n--- Playback active. Press Ctrl+C to stop. ---\n");
    while (is_running) {
        libusb_handle_events_timeout_completed(NULL, &(struct timeval){0, 100000}, NULL);
    }

cleanup:
    is_running = false;
    printf("\nCleaning up...\n");
    for (int i = 0; i < NUM_ISO_TRANSFERS; i++) {
        if (playback_transfers[i]) libusb_cancel_transfer(playback_transfers[i]);
        if (feedback_transfers[i]) libusb_cancel_transfer(feedback_transfers[i]);
    }
    if (handle) {
        struct timeval tv = {0, 100000};
        libusb_handle_events_timeout_completed(NULL, &tv, NULL);
    }
    if (handle) {
        libusb_release_interface(handle, 1); libusb_release_interface(handle, 0);
        for(int i = 0; i < 2; i++) if (kernel_driver_was_active[i]) libusb_attach_kernel_driver(handle, i);
        libusb_close(handle);
        handle = NULL;
    }
    for (int i = 0; i < NUM_ISO_TRANSFERS; i++) {
        if (playback_transfers[i]) { if (playback_transfers[i]->buffer) free(playback_transfers[i]->buffer); libusb_free_transfer(playback_transfers[i]); }
        if (feedback_transfers[i]) { if (feedback_transfers[i]->buffer) free(feedback_transfers[i]->buffer); libusb_free_transfer(feedback_transfers[i]); }
    }
    if (state.playback_fifo_fd >= 0) close(state.playback_fifo_fd);
    libusb_exit(NULL);
    printf("Cleanup complete.\n");
    return r;
}

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -r <rate> --playback-pipe <path>\n", prog_name);
    fprintf(stderr, "  -r <rate>             : Set sample rate. Supported: 44100, 48000, 88200, 96000.\n");
    fprintf(stderr, "  --playback-pipe <path>: Path to the named pipe for audio playback.\n");
}

static void LIBUSB_CALL feedback_callback(struct libusb_transfer *transfer) {
    if (!is_running) return;
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (is_running) libusb_submit_transfer(transfer);
    }
}

static void LIBUSB_CALL iso_playback_callback(struct libusb_transfer *transfer) {
    if (!is_running) return;
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "Playback callback error (EP 0x%02x): %s\n", transfer->endpoint, libusb_error_name(transfer->status));
        is_running = false;
        return;
    }

    struct stream_state *state = transfer->user_data;
    int total_frames = transfer->length / DEVICE_FRAME_SIZE;
    int bytes_to_read_from_fifo = total_frames * PIPE_FRAME_SIZE;
    unsigned char source_buf[bytes_to_read_from_fifo];

    ssize_t n = read(state->playback_fifo_fd, source_buf, bytes_to_read_from_fifo);

    unsigned char *device_buf = transfer->buffer;
    if (n > 0) {
        int frames_read = n / PIPE_FRAME_SIZE;
        unsigned char *src = source_buf;
        for (int i = 0; i < frames_read; i++) {
            memcpy(device_buf, src, PIPE_FRAME_SIZE);
            memset(device_buf + PIPE_FRAME_SIZE, 0, PIPE_FRAME_SIZE);
            device_buf += DEVICE_FRAME_SIZE;
            src += PIPE_FRAME_SIZE;
        }
        if (n < bytes_to_read_from_fifo) {
            memset(device_buf, 0, transfer->length - (frames_read * DEVICE_FRAME_SIZE));
        }
    } else {
        memset(transfer->buffer, 0, transfer->length);
    }

    if (is_running && libusb_submit_transfer(transfer) < 0) {
        fprintf(stderr, "Error resubmitting playback transfer\n");
        is_running = false;
    }
}

int perform_initialization_sequence(libusb_device_handle *handle, int rate) {
    unsigned char buf[64]; int r;
    unsigned char rate_data[3];
    uint16_t rate_vendor_wValue;

    switch(rate) {
        case 44100: memcpy(rate_data, (unsigned char[]){0x44, 0xac, 0x00}, 3); rate_vendor_wValue = 0x1000; break;
        case 48000: memcpy(rate_data, (unsigned char[]){0x80, 0xbb, 0x00}, 3); rate_vendor_wValue = 0x1002; break;
        case 88200: memcpy(rate_data, (unsigned char[]){0x88, 0x58, 0x01}, 3); rate_vendor_wValue = 0x1008; break;
        case 96000: memcpy(rate_data, (unsigned char[]){0x00, 0x77, 0x01}, 3); rate_vendor_wValue = 0x100a; break;
        default: fprintf(stderr, "Invalid sample rate for initialization.\n"); return -1;
    }

    printf("\n--- STARTING DEVICE CONFIGURATION (per Spec v5.0) ---\n");

    #define CHECK(desc, call) \
        r = (call); \
        if (r < 0) { fprintf(stderr, "  [FAIL] %s: %s\n", desc, libusb_error_name(r)); return -1; } \
        else { printf("  [OK] %s (returned %d)\n", desc, r); }

    printf("  [INFO] Step 1: Set Interfaces (already done in main, re-verifying)\n");
    r = libusb_set_configuration(handle, 1);
    if (r < 0 && r != LIBUSB_ERROR_BUSY) { fprintf(stderr, "  [FAIL] Set Configuration 1: %s\n", libusb_error_name(r)); return -1; }
    for (int i=0; i<=1; i++) {
        r = libusb_claim_interface(handle, i);
        if (r < 0) { fprintf(stderr, "  [FAIL] Claim Interface %d: %s\n", i, libusb_error_name(r)); return -1; }
        r = libusb_set_interface_alt_setting(handle, i, 1);
        if (r < 0) { fprintf(stderr, "  [FAIL] Set Alt Setting on Intf %d: %s\n", i, libusb_error_name(r)); return -1; }
    }
    printf("  [OK] Step 1: Interfaces set and claimed.\n");

    printf("\n-- Step 2: Initial Handshake --\n");
    CHECK("Status Check", libusb_control_transfer(handle, RT_D2H_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0000, 0x0000, buf, 1, USB_TIMEOUT));

    printf("\n-- Step 3: Set Initial Mode --\n");
    CHECK("Set Initial Mode", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0010, 0x0000, NULL, 0, USB_TIMEOUT));

    printf("\n-- Step 4: Set Sample Rate to %d Hz --\n", rate);
    // We still set the capture rate, as the device expects it as part of the sequence.
    CHECK("Set Rate on Capture EP (0x86)", libusb_control_transfer(handle, RT_H2D_CLASS_EP, UAC_SET_CUR, UAC_SAMPLING_FREQ_CONTROL, 0x86, rate_data, 3, USB_TIMEOUT));
    CHECK("Set Rate on Playback EP (0x02)", libusb_control_transfer(handle, RT_H2D_CLASS_EP, UAC_SET_CUR, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_OUT, rate_data, 3, USB_TIMEOUT));

    printf("\n-- Step 5: Configure Internal Registers --\n");
    CHECK("Reg Write 1 (0x0d04)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0d04, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 2 (0x0e00)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0e00, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 3 (0x0f00)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0f00, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 4 (Rate-Dep)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, rate_vendor_wValue, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 5 (0x110b)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x110b, 0x0101, NULL, 0, USB_TIMEOUT));

    printf("\n-- Step 6: Enable Streaming --\n");
    CHECK("Enable Streaming", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0030, 0x0000, NULL, 0, USB_TIMEOUT));

    printf("\n--- CONFIGURATION COMPLETE ---\n\n");
    return 0;
}
