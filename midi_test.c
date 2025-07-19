#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#define NOTE_INTERVAL_MS 100
#define NUM_CHORD_NOTES 3

#define TASCAM_VID 0x0644
#define TASCAM_PID 0x8020
#define EP_MIDI_OUT          0x04
#define EP_MIDI_IN           0x83
#define EP_AUDIO_OUT         0x02
#define EP_CAPTURE_DATA      0x86

#define RT_H2D_CLASS_EP   0x22
#define RT_D2H_VENDOR_DEV 0xc0
#define RT_H2D_VENDOR_DEV 0x40

#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100
#define VENDOR_REQ_REGISTER_WRITE 65
#define VENDOR_REQ_MODE_CONTROL   73
#define USB_TIMEOUT 1000

#define NUM_AUDIO_TRANSFERS 8
#define ISO_AUDIO_PACKETS_PER_TRANSFER 8
#define BYTES_PER_SAMPLE 3
#define DEVICE_CHANNELS 4
#define DEVICE_FRAME_SIZE (DEVICE_CHANNELS * BYTES_PER_SAMPLE)
#define NUM_MIDI_IN_TRANSFERS 4
#define MIDI_IN_BUF_SIZE 64

static volatile bool is_running = true;
long long total_bytes_sent = 0;

int perform_device_init(libusb_device_handle *handle);
static void LIBUSB_CALL iso_audio_callback(struct libusb_transfer *transfer);
static void LIBUSB_CALL midi_in_callback(struct libusb_transfer *transfer);
void log_raw_midi_in(uint8_t *buf, int len);

void sigint_handler(int signum) {
    if (is_running) {
        printf("\nCtrl+C detected, shutting down...\n");
        is_running = false;
    }
}

void send_tascam_midi_message(libusb_device_handle *handle, uint8_t *midi_msg, long long *total_bytes_sent) {
    const size_t transfer_size = 9;
    uint8_t packet1[transfer_size];
    uint8_t packet2[transfer_size];
    int r, actual_length;

    // Packet 1: Header and first MIDI byte
    memset(packet1, 0xfd, transfer_size);
    packet1[0] = (0 << 4) | (midi_msg[0] >> 4); // Cable 0, CIN
    packet1[1] = midi_msg[0];
    packet1[8] = 0x00;

    // Packet 2: Second and third MIDI bytes
    memset(packet2, 0xfd, transfer_size);
    packet2[0] = midi_msg[1];
    packet2[1] = midi_msg[2];
    packet2[8] = 0x00;

    r = libusb_bulk_transfer(handle, EP_MIDI_OUT, packet1, transfer_size, &actual_length, USB_TIMEOUT);
    if (r != 0) {
        fprintf(stderr, "MIDI transfer error on packet 1: %s\n", libusb_error_name(r));
        is_running = false;
        return;
    }
    *total_bytes_sent += actual_length;

    r = libusb_bulk_transfer(handle, EP_MIDI_OUT, packet2, transfer_size, &actual_length, USB_TIMEOUT);
    if (r != 0) {
        fprintf(stderr, "MIDI transfer error on packet 2: %s\n", libusb_error_name(r));
        is_running = false;
        return;
    }
    *total_bytes_sent += actual_length;
}

int main(int argc, char *argv[]) {
    libusb_device_handle *handle = NULL;
    struct libusb_transfer *audio_transfers[NUM_AUDIO_TRANSFERS] = {0};
    struct libusb_transfer *midi_in_transfers[NUM_MIDI_IN_TRANSFERS] = {0};
    bool kernel_driver_was_active[2] = {false, false};
    int r = 0;

    printf("--- TASCAM US-144MKII MIDI Loopback Test (Two-Packet) ---\n");
    printf("Please connect a MIDI cable from MIDI OUT to MIDI IN.\n");
    printf("Sending a %d-note chord every %d ms. Press Ctrl+C to stop.\n", NUM_CHORD_NOTES, NOTE_INTERVAL_MS);

    srand(time(NULL));
    signal(SIGINT, sigint_handler);
    if (libusb_init(NULL) < 0) { r = 1; goto cleanup; }
    handle = libusb_open_device_with_vid_pid(NULL, TASCAM_VID, TASCAM_PID);
    if (!handle) { fprintf(stderr, "Device not found\n"); r = 1; goto cleanup; }

    for (int i = 0; i < 2; i++) {
        if (libusb_kernel_driver_active(handle, i)) {
            kernel_driver_was_active[i] = true;
            if ((r = libusb_detach_kernel_driver(handle, i)) != 0) {
                fprintf(stderr, "Could not detach driver for iface %d: %s\n", i, libusb_error_name(r));
                r = 1; goto cleanup;
            }
        }
    }
    if (perform_device_init(handle) != 0) { r = 1; goto cleanup; }

    const int nominal_frames_per_packet = 44100 / 8000;
    const int audio_packet_size = nominal_frames_per_packet * DEVICE_FRAME_SIZE;
    const int audio_transfer_size = audio_packet_size * ISO_AUDIO_PACKETS_PER_TRANSFER;

    printf("Starting silent audio stream...\n");
    for (int i = 0; i < NUM_AUDIO_TRANSFERS; i++) {
        audio_transfers[i] = libusb_alloc_transfer(ISO_AUDIO_PACKETS_PER_TRANSFER);
        unsigned char *buf = calloc(1, audio_transfer_size);
        if (!buf) { fprintf(stderr, "Audio buffer alloc failed\n"); r=1; goto cleanup; }
        libusb_fill_iso_transfer(audio_transfers[i], handle, EP_AUDIO_OUT, buf, audio_transfer_size, ISO_AUDIO_PACKETS_PER_TRANSFER, iso_audio_callback, NULL, USB_TIMEOUT);
        libusb_set_iso_packet_lengths(audio_transfers[i], audio_packet_size);
        if (libusb_submit_transfer(audio_transfers[i]) < 0) {
             fprintf(stderr, "Failed to submit initial audio transfer\n"); r=1; goto cleanup;
        }
    }

    printf("Starting MIDI IN listener...\n");
    for (int i = 0; i < NUM_MIDI_IN_TRANSFERS; i++) {
        midi_in_transfers[i] = libusb_alloc_transfer(0);
        unsigned char* buf = malloc(MIDI_IN_BUF_SIZE);
        if (!buf) { fprintf(stderr, "MIDI IN buffer alloc failed\n"); r=1; goto cleanup; }
        libusb_fill_bulk_transfer(midi_in_transfers[i], handle, EP_MIDI_IN, buf, MIDI_IN_BUF_SIZE, midi_in_callback, NULL, 0);
        if (libusb_submit_transfer(midi_in_transfers[i]) < 0) {
            fprintf(stderr, "Failed to submit initial MIDI IN transfer\n"); r=1; goto cleanup;
        }
    }

    printf("\n--- Starting MIDI loop...---\n");

    enum { STATE_SEND_ON, STATE_SEND_OFF } midi_send_state = STATE_SEND_ON;
    struct timespec last_action_time;
    clock_gettime(CLOCK_MONOTONIC, &last_action_time);

    while (is_running) {
        struct timeval tv = {0, 1000};
        libusb_handle_events_timeout(NULL, &tv);

        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        double elapsed_ms = (current_time.tv_sec - last_action_time.tv_sec) * 1000.0;
        elapsed_ms += (current_time.tv_nsec - last_action_time.tv_nsec) / 1000000.0;

        if (elapsed_ms < NOTE_INTERVAL_MS) {
            continue;
        }

        if (midi_send_state == STATE_SEND_ON) {
            printf("--- SENDING NOTE ON ---\n");
            uint8_t note = 60;
            uint8_t velocity = (rand() % 123) + 5;
            uint8_t midi_msg[] = {0x90, note, velocity};
            send_tascam_midi_message(handle, midi_msg, &total_bytes_sent);
        } else { // STATE_SEND_OFF
            printf("--- SENDING NOTE OFF ---\n\n");
            uint8_t note = 60;
            uint8_t midi_msg[] = {0x80, note, 0};
            send_tascam_midi_message(handle, midi_msg, &total_bytes_sent);
        }

        midi_send_state = (midi_send_state == STATE_SEND_ON) ? STATE_SEND_OFF : STATE_SEND_ON;
        clock_gettime(CLOCK_MONOTONIC, &last_action_time);
    }

cleanup:
    for(int i=0; i < NUM_AUDIO_TRANSFERS; i++) if (audio_transfers[i]) libusb_cancel_transfer(audio_transfers[i]);
    for(int i=0; i < NUM_MIDI_IN_TRANSFERS; i++) if (midi_in_transfers[i]) libusb_cancel_transfer(midi_in_transfers[i]);
    struct timeval final_tv = {0, 200000};
    libusb_handle_events_timeout_completed(NULL, &final_tv, NULL);
    if (handle) {
        libusb_release_interface(handle, 1);
        libusb_release_interface(handle, 0);
        for(int i = 0; i < 2; i++) if (kernel_driver_was_active[i]) libusb_attach_kernel_driver(handle, i);
        libusb_close(handle);
    }
    for (int i=0; i<NUM_AUDIO_TRANSFERS; i++) if(audio_transfers[i]) { if (audio_transfers[i]->buffer) free(audio_transfers[i]->buffer); libusb_free_transfer(audio_transfers[i]); }
    for (int i=0; i<NUM_MIDI_IN_TRANSFERS; i++) if(midi_in_transfers[i]) { if (midi_in_transfers[i]->buffer) free(midi_in_transfers[i]->buffer); libusb_free_transfer(midi_in_transfers[i]); }

    libusb_exit(NULL);

    printf("\n\n------ FINAL REPORT ------\n");
    printf("Total Raw MIDI Bytes Sent:     %lld\n", total_bytes_sent);
    printf("--------------------------\n");

    printf("Cleanup complete.\n");
    return r;
}

void log_raw_midi_in(uint8_t *buf, int len) {
    printf("RECV RAW USB DATA (%d bytes):", len);
    for(int i=0; i<len; i++) printf(" %02x", buf[i]);
    printf("\n");
}

static void LIBUSB_CALL midi_in_callback(struct libusb_transfer *transfer) {
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->actual_length > 0) {
            log_raw_midi_in(transfer->buffer, transfer->actual_length);
        }
        if (is_running) {
            libusb_submit_transfer(transfer);
        }
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "MIDI IN callback error: %s\n", libusb_error_name(transfer->status));
        is_running = false;
    }
}

static void LIBUSB_CALL iso_audio_callback(struct libusb_transfer *transfer) {
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) { if (is_running) libusb_submit_transfer(transfer);
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "Audio callback error: %s\n", libusb_error_name(transfer->status));
        is_running = false;
    }
}

int perform_device_init(libusb_device_handle *handle) {
    const unsigned char rate_data_44100[] = {0x44, 0xac, 0x00};
    uint16_t rate_vendor_wValue = 0x1000;
    unsigned char buf[1]; int r;
    char log_msg[128];

    printf("\n--- STARTING DEVICE INITIALIZATION (Verified Sequence) ---\n");
    #define CHECK(desc, call) r = (call); if (r < 0) { fprintf(stderr, "  [FAIL] %s: %s\n", desc, libusb_error_name(r)); return -1; } else { printf("  [OK] %s\n", desc); }
    printf("  [INFO] Step 1: Set Interfaces\n");
    r = libusb_set_configuration(handle, 1);
    if (r < 0 && r != LIBUSB_ERROR_BUSY) { fprintf(stderr, "  [FAIL] Set Configuration 1: %s\n", libusb_error_name(r)); return -1; }
    for (int i = 0; i <= 1; i++) {
        snprintf(log_msg, sizeof(log_msg), "Claim Interface %d", i); CHECK(log_msg, libusb_claim_interface(handle, i));
        snprintf(log_msg, sizeof(log_msg), "Set Alt Setting on Intf %d", i); CHECK(log_msg, libusb_set_interface_alt_setting(handle, i, 1));
    }
    printf("\n-- Step 2: Handshake --\n");
    CHECK("Vendor Handshake Read", libusb_control_transfer(handle, RT_D2H_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0000, 0x0000, buf, 1, USB_TIMEOUT));
    printf("\n-- Step 3: Set Initial Mode --\n");
    CHECK("Vendor Set Mode to 0x0010", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0010, 0x0000, NULL, 0, USB_TIMEOUT));
    printf("\n-- Step 4: Set Sample Rate (Prerequisite for MIDI) --\n");
    CHECK("UAC Set Rate on Capture EP", libusb_control_transfer(handle, RT_H2D_CLASS_EP, UAC_SET_CUR, UAC_SAMPLING_FREQ_CONTROL, EP_CAPTURE_DATA, (unsigned char*)rate_data_44100, 3, USB_TIMEOUT));
    CHECK("UAC Set Rate on Playback EP", libusb_control_transfer(handle, RT_H2D_CLASS_EP, UAC_SET_CUR, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_OUT, (unsigned char*)rate_data_44100, 3, USB_TIMEOUT));
    printf("\n-- Step 5: Configure Internal Registers --\n");
    CHECK("Vendor Register Write (0x0d04)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0d04, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Vendor Register Write (0x0e00)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0e00, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Vendor Register Write (0x0f00)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0f00, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Vendor Register Write (Rate)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, rate_vendor_wValue, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Vendor Register Write (0x110b)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x110b, 0x0101, NULL, 0, USB_TIMEOUT));
    printf("\n-- Step 6: Enable Streaming --\n");
    CHECK("Vendor Set Mode to 0x0030 (Enable Streaming)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0030, 0x0000, NULL, 0, USB_TIMEOUT));

    printf("\n--- INITIALIZATION COMPLETE ---\n");
    return 0;
}
