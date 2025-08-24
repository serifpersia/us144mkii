// MIT License
// Copyright (c) 2025 serifpersia

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <float.h>
#include <errno.h>
#include <jack/jack.h>
#include <stdatomic.h>

#define ISO_PLAYBACK_PACKETS_PER_TRANSFER 8
#define NUM_PLAYBACK_TRANSFERS          8
#define NUM_FEEDBACK_TRANSFERS          8
#define NUM_CAPTURE_TRANSFERS           8
#define CAPTURE_PACKET_SIZE             131072

#define TASCAM_VID 0x0644
#define TASCAM_PID 0x8020
#define EP_AUDIO_OUT         0x02
#define EP_PLAYBACK_FEEDBACK 0x81
#define EP_CAPTURE_DATA      0x86

#define RT_H2D_CLASS_EP   0x22
#define RT_D2H_VENDOR_DEV 0xc0
#define RT_H2D_VENDOR_DEV 0x40

#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100
#define VENDOR_REQ_REGISTER_WRITE 65
#define VENDOR_REQ_MODE_CONTROL   73

#define BYTES_PER_SAMPLE 3
#define DEVICE_CHANNELS 4
#define DEVICE_FRAME_SIZE (DEVICE_CHANNELS * BYTES_PER_SAMPLE)
#define FEEDBACK_PACKET_SIZE 3
#define MAX_FEEDBACK_PACKETS_PER_URB 5
#define USB_TIMEOUT 1000
#define S24_MAX_VALUE 8388607.0

#define FEEDBACK_ACCUMULATOR_SIZE 128
#define WARMUP_THRESHOLD (ISO_PLAYBACK_PACKETS_PER_TRANSFER * 2)

struct latency_profile_config {
    const char *name;
    int feedback_packets_per_urb;
};

struct sample_rate_config {
    int rate;
    const unsigned char rate_data[3];
    uint16_t rate_vendor_wValue;
    unsigned int feedback_base_value;
    unsigned int feedback_max_value;
    const struct latency_profile_config profiles[5];
};

static const struct sample_rate_config g_rate_configs[] = {
    { 44100, {0x44, 0xac, 0x00}, 0x1000, 42, 46, { {"Lowest",1}, {"Low",1}, {"Normal",2}, {"High",5}, {"Highest",5} } },
    { 48000, {0x80, 0xbb, 0x00}, 0x1002, 46, 50, { {"Lowest",1}, {"Low",1}, {"Normal",2}, {"High",5}, {"Highest",5} } },
    { 88200, {0x88, 0x58, 0x01}, 0x1008, 86, 90, { {"Lowest",1}, {"Low",1}, {"Normal",2}, {"High",5}, {"Highest",5} } },
    { 96000, {0x00, 0x77, 0x01}, 0x100a, 94, 98, { {"Lowest",1}, {"Low",1}, {"Normal",2}, {"High",5}, {"Highest",5} } }
};
#define NUM_SUPPORTED_RATES (sizeof(g_rate_configs) / sizeof(g_rate_configs[0]))
#define NUM_PROFILES 5

static volatile bool is_running = true;
static bool g_debug_mode = false;
jack_client_t *jack_client = NULL;
jack_port_t *jack_playback_ports[DEVICE_CHANNELS];
jack_port_t *jack_capture_ports[DEVICE_CHANNELS];

struct stream_state {
    pthread_mutex_t lock;
    const struct sample_rate_config *rate_cfg;
    const struct latency_profile_config *profile_cfg;

    unsigned char *jack_buffer;
    atomic_uint jack_buffer_read_pos_frames;
    atomic_uint jack_buffer_write_pos_frames;
    unsigned int ring_buffer_frames;

    unsigned int feedback_accumulator_pattern[FEEDBACK_ACCUMULATOR_SIZE];
    unsigned int feedback_pattern_out_idx;
    unsigned int feedback_pattern_in_idx;
    atomic_bool feedback_synced;
    atomic_bool feedback_warmed_up;

    atomic_ulong underrun_count;
    atomic_ulong overrun_count;
    atomic_ulong sync_loss_count;
    atomic_uint current_buffer_fill;
};

void print_usage(const char *prog_name);
int perform_initialization_sequence(libusb_device_handle *handle, const struct sample_rate_config *rate_config);
static void LIBUSB_CALL iso_playback_callback(struct libusb_transfer *transfer);
static void LIBUSB_CALL feedback_callback(struct libusb_transfer *transfer);
static void LIBUSB_CALL capture_callback(struct libusb_transfer *transfer);
static void generate_feedback_pattern(unsigned int base_frames, int frame_adjustment, unsigned int* output_pattern);
int jack_process_callback(jack_nframes_t nframes, void *arg);

void sigint_handler(int signum) {
    if (is_running) {
        printf("\nCtrl+C detected, stopping...\n");
        is_running = false;
    }
}

int main(int argc, char *argv[]) {
    int sample_rate = 0;
    int profile_index = -2; // Use -2 as an uninitialized sentinel

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            sample_rate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            profile_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            g_debug_mode = true;
        }
    }

    if (sample_rate == 0 || profile_index == -2) {
        print_usage(argv[0]);
        return 1;
    }

    const struct sample_rate_config *rate_config = NULL;
    for (unsigned int i = 0; i < NUM_SUPPORTED_RATES; i++) {
        if (g_rate_configs[i].rate == sample_rate) {
            rate_config = &g_rate_configs[i];
            break;
        }
    }

    if (!rate_config) { fprintf(stderr, "Error: Sample rate %d is not supported.\n", sample_rate); return 1; }

    libusb_device_handle *handle = NULL;
    struct libusb_transfer *playback_transfers[NUM_PLAYBACK_TRANSFERS] = {0};
    struct libusb_transfer *feedback_transfers[NUM_FEEDBACK_TRANSFERS] = {0};
    struct libusb_transfer *capture_transfers[NUM_CAPTURE_TRANSFERS] = {0};
    struct stream_state state = {0};
    bool kernel_driver_was_active[2] = {false, false};
    int r = 0;

    printf("--- TASCAM US-144MKII JACK User-Space Driver ---\n");

    pthread_mutex_init(&state.lock, NULL);
    state.rate_cfg = rate_config;

    atomic_init(&state.underrun_count, 0);
    atomic_init(&state.overrun_count, 0);
    atomic_init(&state.sync_loss_count, 0);
    atomic_init(&state.current_buffer_fill, 0);
    atomic_init(&state.feedback_synced, false);
    atomic_init(&state.feedback_warmed_up, false);

    signal(SIGINT, sigint_handler);
    if (libusb_init(NULL) < 0) { r = 1; goto cleanup; }

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

    if (perform_initialization_sequence(handle, rate_config) != 0) { r = 1; goto cleanup; }

    jack_status_t status;
    jack_client = jack_client_open("tascam_us144mkii", JackNullOption, &status);
    if (jack_client == NULL) { fprintf(stderr, "jack_client_open() failed, status = 0x%2.0x\n", status); r = 1; goto cleanup; }

    unsigned int jack_buffer_size = jack_get_buffer_size(jack_client);
    printf("Detected JACK configuration: Buffer = %u frames\n", jack_buffer_size);

    if (profile_index == -1) {
        printf("Automatic profile selection enabled...\n");
        switch (sample_rate) {
            case 44100:
                if      (jack_buffer_size <= 49)  profile_index = 0;
                else if (jack_buffer_size <= 64)  profile_index = 1;
                else if (jack_buffer_size <= 128) profile_index = 2;
                else if (jack_buffer_size <= 256) profile_index = 3;
                else                              profile_index = 4;
                break;
            case 48000:
                if      (jack_buffer_size <= 48)  profile_index = 0;
                else if (jack_buffer_size <= 64)  profile_index = 1;
                else if (jack_buffer_size <= 128) profile_index = 2;
                else if (jack_buffer_size <= 256) profile_index = 3;
                else                              profile_index = 4;
                break;
            case 88200:
                if      (jack_buffer_size <= 98)   profile_index = 0;
                else if (jack_buffer_size <= 128)  profile_index = 1;
                else if (jack_buffer_size <= 256)  profile_index = 2;
                else if (jack_buffer_size <= 512)  profile_index = 3;
                else                               profile_index = 4;
                break;
            case 96000:
                if      (jack_buffer_size <= 96)   profile_index = 0;
                else if (jack_buffer_size <= 128)  profile_index = 1;
                else if (jack_buffer_size <= 256)  profile_index = 2;
                else if (jack_buffer_size <= 512)  profile_index = 3;
                else                               profile_index = 4;
                break;
            default:
                printf("Warning: Unknown sample rate for auto-selection, defaulting to 'Lowest'.\n");
                profile_index = 0;
        }
        printf("Matched JACK buffer %u to Profile %d (%s)\n", jack_buffer_size, profile_index, rate_config->profiles[profile_index].name);
    }

    if (profile_index >= NUM_PROFILES) { fprintf(stderr, "Error: Invalid profile index %d.\n", profile_index); return 1; }
    const struct latency_profile_config *profile_config = &rate_config->profiles[profile_index];
    state.profile_cfg = profile_config;

    state.ring_buffer_frames = (jack_buffer_size * 2) + 1;
    printf("Calculated optimal ring buffer size: %u frames\n", state.ring_buffer_frames);

    state.jack_buffer = malloc(state.ring_buffer_frames * DEVICE_FRAME_SIZE);
    if (!state.jack_buffer) { fprintf(stderr, "Failed to allocate intermediate buffer\n"); r = 1; goto cleanup; }
    memset(state.jack_buffer, 0, state.ring_buffer_frames * DEVICE_FRAME_SIZE);

    jack_set_process_callback(jack_client, jack_process_callback, &state);
    for (int i = 0; i < DEVICE_CHANNELS; i++) {
        char port_name[32];
        sprintf(port_name, "playback_%d", i + 1);
        jack_playback_ports[i] = jack_port_register(jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        sprintf(port_name, "capture_%d", i + 1);
        jack_capture_ports[i] = jack_port_register(jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }
    if (jack_activate(jack_client)) { fprintf(stderr, "Cannot activate JACK client\n"); r = 1; goto cleanup; }
    printf("JACK client activated. Connect your applications.\n");

    printf("Priming audio buffer before starting USB streams...\n");
    const unsigned int prime_target_frames = state.ring_buffer_frames / 2;
    while (is_running) {
        unsigned int read_pos = atomic_load(&state.jack_buffer_read_pos_frames);
        unsigned int write_pos = atomic_load(&state.jack_buffer_write_pos_frames);
        unsigned int avail = (write_pos - read_pos + state.ring_buffer_frames) % state.ring_buffer_frames;
        float fill_percent = 100.0f * avail / state.ring_buffer_frames;
        fprintf(stdout, "\rPriming buffer... %u / %u frames (%5.1f%%)", avail, prime_target_frames, fill_percent);
        fflush(stdout);
        if (avail >= prime_target_frames) {
            printf("\nBuffer primed. Starting USB streams.\n\n");
            break;
        }
        usleep(10000);
    }

    if (!is_running) goto cleanup;

    const int max_frames_per_packet = (rate_config->rate / 8000) + 2;
    const int playback_packet_max_size = max_frames_per_packet * DEVICE_FRAME_SIZE;
    const int playback_transfer_size = playback_packet_max_size * ISO_PLAYBACK_PACKETS_PER_TRANSFER;
    const int feedback_transfer_size = FEEDBACK_PACKET_SIZE * MAX_FEEDBACK_PACKETS_PER_URB;
    const int capture_transfer_size = CAPTURE_PACKET_SIZE;

    for (int i = 0; i < NUM_PLAYBACK_TRANSFERS; i++) {
        playback_transfers[i] = libusb_alloc_transfer(ISO_PLAYBACK_PACKETS_PER_TRANSFER);
        unsigned char *buf = malloc(playback_transfer_size);
        int nominal_packet_size = (rate_config->rate / 8000) * DEVICE_FRAME_SIZE;
        int nominal_transfer_size = nominal_packet_size * ISO_PLAYBACK_PACKETS_PER_TRANSFER;
        memset(buf, 0, playback_transfer_size);
        libusb_fill_iso_transfer(playback_transfers[i], handle, EP_AUDIO_OUT, buf, nominal_transfer_size, ISO_PLAYBACK_PACKETS_PER_TRANSFER, iso_playback_callback, &state, USB_TIMEOUT);
        libusb_set_iso_packet_lengths(playback_transfers[i], nominal_packet_size);
        if (libusb_submit_transfer(playback_transfers[i]) < 0) { r = 1; goto cleanup; }
    }
    for (int i = 0; i < NUM_FEEDBACK_TRANSFERS; i++) {
        feedback_transfers[i] = libusb_alloc_transfer(profile_config->feedback_packets_per_urb);
        unsigned char *buf = malloc(feedback_transfer_size);
        libusb_fill_iso_transfer(feedback_transfers[i], handle, EP_PLAYBACK_FEEDBACK, buf, feedback_transfer_size, profile_config->feedback_packets_per_urb, feedback_callback, &state, USB_TIMEOUT);
        libusb_set_iso_packet_lengths(feedback_transfers[i], FEEDBACK_PACKET_SIZE);
        if (libusb_submit_transfer(feedback_transfers[i]) < 0) { r = 1; goto cleanup; }
    }
    for (int i = 0; i < NUM_CAPTURE_TRANSFERS; i++) {
        capture_transfers[i] = libusb_alloc_transfer(0);
        unsigned char *buf = malloc(capture_transfer_size);
        libusb_fill_bulk_transfer(capture_transfers[i], handle, EP_CAPTURE_DATA, buf, capture_transfer_size, capture_callback, &state, USB_TIMEOUT);
        if (libusb_submit_transfer(capture_transfers[i]) < 0) { r = 1; goto cleanup; }
    }

    while (is_running) {
        struct timeval tv = {0, 100000};
        libusb_handle_events_timeout_completed(NULL, &tv, NULL);

        if (g_debug_mode) {
            unsigned long underruns = atomic_load(&state.underrun_count);
            unsigned long overruns = atomic_load(&state.overrun_count);
            unsigned long sync_losses = atomic_load(&state.sync_loss_count);
            unsigned int buffer_fill = atomic_load(&state.current_buffer_fill);
            float fill_percent = 100.0f * buffer_fill / state.ring_buffer_frames;
            const char* sync_status = atomic_load(&state.feedback_synced) ? "SYNCED" : "NO SYNC";
            fprintf(stdout, "\rBuffer: %4u/%u frames (%5.1f%%) | Underruns: %-5lu | Overruns: %-5lu | Sync: %-7s (Losses: %lu)",
                    buffer_fill, state.ring_buffer_frames, fill_percent, underruns, overruns, sync_status, sync_losses);
            fflush(stdout);
        }
    }

    cleanup:
    printf("\n");
    is_running = false;
    if (jack_client) jack_client_close(jack_client);
    for (int i = 0; i < NUM_PLAYBACK_TRANSFERS; i++) if (playback_transfers[i]) libusb_cancel_transfer(playback_transfers[i]);
    for (int i = 0; i < NUM_FEEDBACK_TRANSFERS; i++) if (feedback_transfers[i]) libusb_cancel_transfer(feedback_transfers[i]);
    for (int i = 0; i < NUM_CAPTURE_TRANSFERS; i++) if (capture_transfers[i]) libusb_cancel_transfer(capture_transfers[i]);
    struct timeval tv = {0, 200000};
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
    if (handle) {
        libusb_release_interface(handle, 1);
        libusb_release_interface(handle, 0);
        for(int i = 0; i < 2; i++) if (kernel_driver_was_active[i]) libusb_attach_kernel_driver(handle, i);
        libusb_close(handle);
    }
    for (int i = 0; i < NUM_PLAYBACK_TRANSFERS; i++) if (playback_transfers[i]) { if (playback_transfers[i]->buffer) free(playback_transfers[i]->buffer); libusb_free_transfer(playback_transfers[i]); }
    for (int i = 0; i < NUM_FEEDBACK_TRANSFERS; i++) if (feedback_transfers[i]) { if (feedback_transfers[i]->buffer) free(feedback_transfers[i]->buffer); libusb_free_transfer(feedback_transfers[i]); }
    for (int i = 0; i < NUM_CAPTURE_TRANSFERS; i++) if (capture_transfers[i]) { if (capture_transfers[i]->buffer) free(capture_transfers[i]->buffer); libusb_free_transfer(capture_transfers[i]); }
    pthread_mutex_destroy(&state.lock);
    if (state.jack_buffer) free(state.jack_buffer);
    if (r != 1) libusb_exit(NULL);
    printf("Cleanup complete.\n");
    return r;
}

int jack_process_callback(jack_nframes_t nframes, void *arg) {
    struct stream_state *state = (struct stream_state *)arg;
    jack_default_audio_sample_t *in[DEVICE_CHANNELS];
    for (int i = 0; i < DEVICE_CHANNELS; i++) {
        in[i] = (jack_default_audio_sample_t *)jack_port_get_buffer(jack_playback_ports[i], nframes);
    }
    unsigned int read_pos = atomic_load(&state->jack_buffer_read_pos_frames);
    unsigned int write_pos = atomic_load(&state->jack_buffer_write_pos_frames);
    unsigned int space = (read_pos - write_pos - 1 + state->ring_buffer_frames) % state->ring_buffer_frames;
    if (space < nframes) {
        atomic_fetch_add(&state->overrun_count, 1);
        return 0;
    }
    for (jack_nframes_t f = 0; f < nframes; f++) {
        unsigned int current_write_pos = (write_pos + f) % state->ring_buffer_frames;
        unsigned char *frame_ptr = state->jack_buffer + (current_write_pos * DEVICE_FRAME_SIZE);
        memset(frame_ptr, 0, DEVICE_FRAME_SIZE);
        for (int c = 0; c < 2; c++) {
            float sample_float = in[c][f];
            if (sample_float > 1.0f) sample_float = 1.0f;
            else if (sample_float < -1.0f) sample_float = -1.0f;
            int32_t sample_int = (int32_t)(sample_float * S24_MAX_VALUE);
            frame_ptr[c * 3 + 0] = (sample_int >> 0) & 0xFF;
            frame_ptr[c * 3 + 1] = (sample_int >> 8) & 0xFF;
            frame_ptr[c * 3 + 2] = (sample_int >> 16) & 0xFF;
        }
    }
    atomic_store(&state->jack_buffer_write_pos_frames, (write_pos + nframes) % state->ring_buffer_frames);
    return 0;
}

static void LIBUSB_CALL iso_playback_callback(struct libusb_transfer *transfer) {
    if (!is_running) return;
    struct stream_state *state = transfer->user_data;
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
            fprintf(stderr, "\nPlayback callback error: %s\n", libusb_error_name(transfer->status));
            is_running = false;
        }
        return;
    }
    int nominal_frames = state->rate_cfg->rate / 8000;
    unsigned char *buf_ptr = transfer->buffer;
    size_t total_bytes_in_urb = 0;
    for (int i = 0; i < transfer->num_iso_packets; i++) {
        unsigned int frames_for_packet;
        if (!atomic_load(&state->feedback_warmed_up)) {
            frames_for_packet = nominal_frames;
        } else {
            frames_for_packet = state->feedback_accumulator_pattern[state->feedback_pattern_out_idx];
            state->feedback_pattern_out_idx = (state->feedback_pattern_out_idx + 1) % FEEDBACK_ACCUMULATOR_SIZE;
        }
        size_t bytes_for_packet = frames_for_packet * DEVICE_FRAME_SIZE;
        unsigned char* packet_buf_ptr = buf_ptr + total_bytes_in_urb;
        unsigned int read_pos = atomic_load(&state->jack_buffer_read_pos_frames);
        unsigned int write_pos = atomic_load(&state->jack_buffer_write_pos_frames);
        unsigned int avail = (write_pos - read_pos + state->ring_buffer_frames) % state->ring_buffer_frames;
        atomic_store(&state->current_buffer_fill, avail);
        if (avail < frames_for_packet) {
            memset(packet_buf_ptr, 0, bytes_for_packet);
            if (atomic_load(&state->feedback_warmed_up)) {
                atomic_fetch_add(&state->underrun_count, 1);
            }
        } else {
            for (unsigned int f = 0; f < frames_for_packet; f++) {
                unsigned int current_read_pos = (read_pos + f) % state->ring_buffer_frames;
                unsigned char* src_frame_ptr = state->jack_buffer + (current_read_pos * DEVICE_FRAME_SIZE);
                unsigned char* dest_frame_ptr = packet_buf_ptr + (f * DEVICE_FRAME_SIZE);
                memcpy(dest_frame_ptr, src_frame_ptr, DEVICE_FRAME_SIZE);
            }
            atomic_store(&state->jack_buffer_read_pos_frames, (read_pos + frames_for_packet) % state->ring_buffer_frames);
        }
        transfer->iso_packet_desc[i].length = bytes_for_packet;
        total_bytes_in_urb += bytes_for_packet;
    }
    transfer->length = total_bytes_in_urb;
    if (is_running && libusb_submit_transfer(transfer) < 0) {
        fprintf(stderr, "\nError resubmitting playback transfer\n");
        is_running = false;
    }
}

static void LIBUSB_CALL capture_callback(struct libusb_transfer *transfer) {
    if (!is_running) return;
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "\nCapture transfer error: %s\n", libusb_error_name(transfer->status));
        if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) is_running = false;
    }
    if (is_running && libusb_submit_transfer(transfer) < 0) {
        fprintf(stderr, "\nFailed to re-submit capture transfer\n");
        is_running = false;
    }
}

int perform_initialization_sequence(libusb_device_handle *handle, const struct sample_rate_config *rate_config) {
    unsigned char buf[64]; int r;
    printf("\n--- STARTING DEVICE CONFIGURATION ---\n");
    #define CHECK(desc, call) r = (call); if (r < 0) { fprintf(stderr, "  [FAIL] %s: %s\n", desc, libusb_error_name(r)); return -1; } else { printf("  [OK] %s\n", desc); }
    CHECK("Set Configuration 1", libusb_set_configuration(handle, 1));
    for (int i=0; i<=1; i++) { CHECK("Claim Interface", libusb_claim_interface(handle, i)); CHECK("Set Alt Setting", libusb_set_interface_alt_setting(handle, i, 1)); }
    CHECK("Status Check", libusb_control_transfer(handle, RT_D2H_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0000, 0x0000, buf, 1, USB_TIMEOUT));
    CHECK("Set Initial Mode", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0010, 0x0000, NULL, 0, USB_TIMEOUT));
    printf("--- Set Sample Rate to %d Hz ---\n", rate_config->rate);
    CHECK("Set Rate on Feedback EP (0x81)", libusb_control_transfer(handle, RT_H2D_CLASS_EP, UAC_SET_CUR, UAC_SAMPLING_FREQ_CONTROL, EP_PLAYBACK_FEEDBACK, (unsigned char*)rate_config->rate_data, 3, USB_TIMEOUT));
    CHECK("Set Rate on Playback EP (0x02)", libusb_control_transfer(handle, RT_H2D_CLASS_EP, UAC_SET_CUR, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_OUT, (unsigned char*)rate_config->rate_data, 3, USB_TIMEOUT));
    CHECK("Reg Write 1 (0x0d04)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0d04, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 2 (0x0e00)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0e00, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 3 (0x0f00)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0f00, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 4 (Rate-Dep)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, rate_config->rate_vendor_wValue, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 5 (0x110b)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x110b, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Enable Streaming", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0030, 0x0000, NULL, 0, USB_TIMEOUT));
    printf("--- CONFIGURATION COMPLETE ---\n\n"); return 0;
}

static void generate_feedback_pattern(unsigned int base_frames, int frame_adjustment, unsigned int* output_pattern) {
    int num_steps = 8;
    int num_adj_packets = abs(frame_adjustment);
    int adj = (frame_adjustment > 0) ? 1 : -1;
    int accumulator = 0;
    for (int i = 0; i < num_steps; i++) {
        accumulator += num_adj_packets;
        if (accumulator >= num_steps) {
            output_pattern[i] = base_frames + adj;
            accumulator -= num_steps;
        } else {
            output_pattern[i] = base_frames;
        }
    }
}

static void LIBUSB_CALL feedback_callback(struct libusb_transfer *transfer) {
    if (!is_running) return;
    struct stream_state *state = transfer->user_data;
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
            if (atomic_load(&state->feedback_synced)) {
                atomic_fetch_add(&state->sync_loss_count, 1);
            }
            atomic_store(&state->feedback_synced, false);
            atomic_store(&state->feedback_warmed_up, false);
        }
        goto resubmit;
    }
    bool sync_lost_this_urb = false;
    for (int p = 0; p < transfer->num_iso_packets; p++) {
        struct libusb_iso_packet_descriptor *pack = &transfer->iso_packet_desc[p];
        if (pack->status != 0 || pack->actual_length < 1) { sync_lost_this_urb = true; continue; }
        uint8_t feedback_value = transfer->buffer[p * FEEDBACK_PACKET_SIZE];
        if (feedback_value >= state->rate_cfg->feedback_base_value && feedback_value <= state->rate_cfg->feedback_max_value) {
            unsigned int generated_pattern[8];
            unsigned int base_frames = state->rate_cfg->rate / 8000;
            int frame_adjustment = feedback_value - (8 * base_frames);
            generate_feedback_pattern(base_frames, frame_adjustment, generated_pattern);
            pthread_mutex_lock(&state->lock);
            for (int i = 0; i < 8; i++) {
                unsigned int in_idx = (state->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;
                state->feedback_accumulator_pattern[in_idx] = generated_pattern[i];
            }
            state->feedback_pattern_in_idx = (state->feedback_pattern_in_idx + 8) % FEEDBACK_ACCUMULATOR_SIZE;
            pthread_mutex_unlock(&state->lock);
        } else { sync_lost_this_urb = true; }
    }
    if (sync_lost_this_urb) {
        if (atomic_load(&state->feedback_synced)) {
            atomic_fetch_add(&state->sync_loss_count, 1);
        }
        atomic_store(&state->feedback_synced, false);
        atomic_store(&state->feedback_warmed_up, false);
    } else {
        atomic_store(&state->feedback_synced, true);
        size_t fill_level = (state->feedback_pattern_in_idx - state->feedback_pattern_out_idx + FEEDBACK_ACCUMULATOR_SIZE) % FEEDBACK_ACCUMULATOR_SIZE;
        if (!atomic_load(&state->feedback_warmed_up) && fill_level >= WARMUP_THRESHOLD) {
            atomic_store(&state->feedback_warmed_up, true);
        }
    }
    resubmit: if (is_running) libusb_submit_transfer(transfer);
}

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -r <rate> -p <profile> [-d | --debug]\n", prog_name);
    fprintf(stderr, "  -r <rate>    : 44100, 48000, 88200, 96000\n");
    fprintf(stderr, "  -p <profile> : -1 for Automatic, or 0-4 for manual (Lowest..Highest)\n");
    fprintf(stderr, "  -d, --debug  : Enable live monitoring of buffer health.\n");
}
