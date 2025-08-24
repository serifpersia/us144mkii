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
#include <fcntl.h>
#include <errno.h>

// --- Latency Tuning ---
#define ISO_PLAYBACK_PACKETS_PER_TRANSFER 40
#define NUM_PLAYBACK_TRANSFERS 8
#define NUM_FEEDBACK_TRANSFERS 8
#define NUM_CAPTURE_TRANSFERS 4
#define CAPTURE_PACKET_SIZE 131072

// --- Device and Endpoint Configuration ---
#define TASCAM_VID 0x0644
#define TASCAM_PID 0x8020
#define EP_AUDIO_OUT         0x02
#define EP_PLAYBACK_FEEDBACK 0x81
#define EP_CAPTURE_DATA      0x86

// --- USB Request Types ---
#define RT_H2D_CLASS_EP   0x22
#define RT_D2H_VENDOR_DEV 0xc0
#define RT_H2D_VENDOR_DEV 0x40

// --- UAC / Vendor Requests ---
#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100
#define VENDOR_REQ_REGISTER_WRITE 65
#define VENDOR_REQ_MODE_CONTROL   73

// --- Streaming Configuration ---
#define BYTES_PER_SAMPLE 3
#define DEVICE_CHANNELS 4
#define SINE_CHANNELS 2
#define DEVICE_FRAME_SIZE (DEVICE_CHANNELS * BYTES_PER_SAMPLE)
#define SINE_FRAME_SIZE   (SINE_CHANNELS * BYTES_PER_SAMPLE)
#define FEEDBACK_PACKET_SIZE 3
#define MAX_FEEDBACK_PACKETS_PER_URB 5
#define USB_TIMEOUT 1000

// --- Sine Wave Configuration ---
#define SINE_FREQUENCY 440.0
#define SINE_AMPLITUDE 8388600.0

// --- Feedback Synchronization Engine ---
#define FEEDBACK_ACCUMULATOR_SIZE 128
#define WARMUP_THRESHOLD (ISO_PLAYBACK_PACKETS_PER_TRANSFER * 2)

// --- Data Structures for Rate/Profile Configuration ---
struct latency_profile_config {
    const char *name;
    int feedback_packets_per_urb;
    int asio_buffer_size_frames;
    double expected_feedback_ms;
};

struct sample_rate_config {
    int rate;
    const unsigned char rate_data[3];
    uint16_t rate_vendor_wValue;
    unsigned int feedback_base_value;
    unsigned int feedback_max_value;
    const struct latency_profile_config profiles[5];
};

// --- Global Configuration Table ---
static const struct sample_rate_config g_rate_configs[] = {
    { 44100, {0x44, 0xac, 0x00}, 0x1000, 42, 46, { {"Lowest",1,49,2.0}, {"Low",1,64,2.0}, {"Normal",2,128,2.0}, {"High",5,256,5.0}, {"Highest",5,512,5.0} } },
    { 48000, {0x80, 0xbb, 0x00}, 0x1002, 46, 50, { {"Lowest",1,48,1.0}, {"Low",1,64,2.0}, {"Normal",2,128,2.0}, {"High",5,256,5.0}, {"Highest",5,512,5.0} } },
    { 88200, {0x88, 0x58, 0x01}, 0x1008, 86, 90, { {"Lowest",1,98,1.0}, {"Low",1,128,2.0}, {"Normal",2,256,2.0}, {"High",5,512,5.0}, {"Highest",5,1024,5.0} } },
    { 96000, {0x00, 0x77, 0x01}, 0x100a, 94, 98, { {"Lowest",1,96,1.0}, {"Low",1,128,2.0}, {"Normal",2,256,2.0}, {"High",5,512,5.0}, {"Highest",5,1024,5.0} } }
};
#define NUM_SUPPORTED_RATES (sizeof(g_rate_configs) / sizeof(g_rate_configs[0]))
#define NUM_PROFILES 5

// --- Global State ---
static volatile bool is_running = true;

struct stream_state {
    pthread_mutex_t lock;
    const struct sample_rate_config *rate_cfg;
    const struct latency_profile_config *profile_cfg;
    double sine_phase;
    bool playing_sine;
    unsigned int feedback_accumulator_pattern[FEEDBACK_ACCUMULATOR_SIZE];
    unsigned int feedback_pattern_out_idx;
    unsigned int feedback_pattern_in_idx;
    bool feedback_synced;
    bool feedback_warmed_up;
    int last_feedback_value;
    struct timeval last_feedback_completion_time;
    double last_feedback_interval_ms;
    double min_feedback_interval_ms;
    double max_feedback_interval_ms;
    double avg_feedback_interval_sum;
    unsigned long feedback_interval_count;
    unsigned long underrun_count;
    unsigned long overrun_count;
};

struct logging_thread_args {
    struct stream_state *state;
    bool minimal_log;
    int log_interval_ms;
};

// --- Function Prototypes ---
void print_usage(const char *prog_name);
void print_latency_report(void);
int perform_initialization_sequence(libusb_device_handle *handle, const struct sample_rate_config *rate_config);
static void LIBUSB_CALL iso_playback_callback(struct libusb_transfer *transfer);
static void LIBUSB_CALL feedback_callback(struct libusb_transfer *transfer);
static void LIBUSB_CALL capture_callback(struct libusb_transfer *transfer);
void *logging_thread_func(void *arg);
double timeval_diff_ms(struct timeval *start, struct timeval *end);
static void generate_feedback_pattern(unsigned int base_frames, int frame_adjustment, unsigned int* output_pattern);


void sigint_handler(int signum) {
    if (is_running) {
        printf("\n\n\n\n\nCtrl+C detected, stopping...\n");
        is_running = false;
    }
}

void print_latency_report(void) {
    const double ESTIMATED_OS_DRIVER_LATENCY_MS = 2.0;
    const double ESTIMATED_DEVICE_HW_LATENCY_MS = 4.0;
    double packet_duration_ms = 0.125;
    double transfer_latency_ms = ISO_PLAYBACK_PACKETS_PER_TRANSFER * packet_duration_ms;
    double buffer_latency_ms = NUM_PLAYBACK_TRANSFERS * transfer_latency_ms;
    double total_estimated_latency = buffer_latency_ms + ESTIMATED_OS_DRIVER_LATENCY_MS + ESTIMATED_DEVICE_HW_LATENCY_MS;

    printf("\n--- Audio Latency Report ---\n");
    printf("Configurable Latency (Software Buffer):\n");
    printf("  USB Transfer Size ......: %.3f ms (%d packets)\n", transfer_latency_ms, ISO_PLAYBACK_PACKETS_PER_TRANSFER);
    printf("  Software Buffer Size ...: %.3f ms (%d transfers)\n", buffer_latency_ms, NUM_PLAYBACK_TRANSFERS);
    printf("--------------------------------------------------\n");
    printf("Fixed Latency (Estimates):\n");
    printf("  OS & Driver Overhead ...: %.1f ms\n", ESTIMATED_OS_DRIVER_LATENCY_MS);
    printf("  Device Hardware ........: %.1f ms (Internal Buffer + D/A)\n", ESTIMATED_DEVICE_HW_LATENCY_MS);
    printf("--------------------------------------------------\n");
    printf(" >> Total Estimated Output Latency: %.2f ms <<\n\n", total_estimated_latency);
}


int main(int argc, char *argv[]) {
    int sample_rate = 0;
    int profile_index = -1;
    bool minimal_log = false;
    int log_interval_ms = 100;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) sample_rate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) profile_index = atoi(argv[++i]);
        else if (strcmp(argv[i], "--minimal-log") == 0) minimal_log = true;
        else if (strcmp(argv[i], "--log-interval") == 0 && i + 1 < argc) log_interval_ms = atoi(argv[++i]);
    }

    if (sample_rate == 0 || profile_index < 0) {
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

    if (!rate_config) {
        fprintf(stderr, "Error: Sample rate %d is not supported.\n", sample_rate);
        return 1;
    }
    if (profile_index >= NUM_PROFILES) {
        fprintf(stderr, "Error: Invalid profile index %d.\n", profile_index);
        return 1;
    }
    const struct latency_profile_config *profile_config = &rate_config->profiles[profile_index];

    libusb_device_handle *handle = NULL;
    struct libusb_transfer *playback_transfers[NUM_PLAYBACK_TRANSFERS] = {0};
    struct libusb_transfer *feedback_transfers[NUM_FEEDBACK_TRANSFERS] = {0};
    struct libusb_transfer *capture_transfers[NUM_CAPTURE_TRANSFERS] = {0};
    struct stream_state state = { .sine_phase = 0.0, .playing_sine = false };
    struct logging_thread_args log_args = { &state, minimal_log, log_interval_ms };
    pthread_t logging_thread = 0;
    bool kernel_driver_was_active[2] = {false, false};
    int r = 0;

    const int max_frames_per_packet = (rate_config->rate / 8000) + 2;
    const int playback_packet_max_size = max_frames_per_packet * DEVICE_FRAME_SIZE;
    const int playback_transfer_size = playback_packet_max_size * ISO_PLAYBACK_PACKETS_PER_TRANSFER;
    const int feedback_transfer_size = FEEDBACK_PACKET_SIZE * MAX_FEEDBACK_PACKETS_PER_URB;
    const int capture_transfer_size = CAPTURE_PACKET_SIZE;

    printf("--- TASCAM US-144MKII Sine Wave Generator ---\n");
    printf("Profile: %d, Rate: %d Hz, Latency: %s (%d-sample buffer)\n",
           profile_index, rate_config->rate, profile_config->name, profile_config->asio_buffer_size_frames);
    printf("Config:  Feedback URB contains %d packet(s), expected interval %.1f ms.\n",
           profile_config->feedback_packets_per_urb, profile_config->expected_feedback_ms);
    printf("Signal:  Generating %.1f Hz sine wave.\n", SINE_FREQUENCY);
    print_latency_report();

    pthread_mutex_init(&state.lock, NULL);
    state.rate_cfg = rate_config;
    state.profile_cfg = profile_config;
    state.min_feedback_interval_ms = DBL_MAX;

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

    if (perform_initialization_sequence(handle, rate_config) != 0) {
        fprintf(stderr, "Device configuration failed.\n"); r = 1; goto cleanup;
    }

    printf("Starting playback stream... (waiting for buffer warm-up)\n");
    for (int i = 0; i < NUM_PLAYBACK_TRANSFERS; i++) {
        playback_transfers[i] = libusb_alloc_transfer(ISO_PLAYBACK_PACKETS_PER_TRANSFER);
        if (!playback_transfers[i]) { fprintf(stderr, "Failed to allocate playback transfer %d\n", i); r = 1; goto cleanup; }
        unsigned char *buf = malloc(playback_transfer_size);
        if (!buf) { fprintf(stderr, "Failed to allocate playback buffer %d\n", i); r = 1; goto cleanup; }

        int nominal_packet_size = (rate_config->rate / 8000) * DEVICE_FRAME_SIZE;
        int nominal_transfer_size = nominal_packet_size * ISO_PLAYBACK_PACKETS_PER_TRANSFER;
        memset(buf, 0, playback_transfer_size);
        libusb_fill_iso_transfer(playback_transfers[i], handle, EP_AUDIO_OUT, buf, nominal_transfer_size, ISO_PLAYBACK_PACKETS_PER_TRANSFER, iso_playback_callback, &state, USB_TIMEOUT);
        libusb_set_iso_packet_lengths(playback_transfers[i], nominal_packet_size);

        int submit_res = libusb_submit_transfer(playback_transfers[i]);
        if (submit_res < 0) { fprintf(stderr, "Failed to submit playback transfer %d: %s\n", i, libusb_error_name(submit_res)); r = 1; goto cleanup; }
    }

    printf("Starting feedback stream...\n");
    for (int i = 0; i < NUM_FEEDBACK_TRANSFERS; i++) {
        feedback_transfers[i] = libusb_alloc_transfer(profile_config->feedback_packets_per_urb);
        if (!feedback_transfers[i]) { fprintf(stderr, "Failed to allocate feedback transfer %d\n", i); r = 1; goto cleanup; }
        unsigned char *buf = malloc(feedback_transfer_size);
        if (!buf) { fprintf(stderr, "Failed to allocate feedback buffer %d\n", i); r = 1; goto cleanup; }
        libusb_fill_iso_transfer(feedback_transfers[i], handle, EP_PLAYBACK_FEEDBACK, buf, feedback_transfer_size, profile_config->feedback_packets_per_urb, feedback_callback, &state, USB_TIMEOUT);
        libusb_set_iso_packet_lengths(feedback_transfers[i], FEEDBACK_PACKET_SIZE);
        int submit_res = libusb_submit_transfer(feedback_transfers[i]);
        if (submit_res < 0) { fprintf(stderr, "Failed to submit feedback transfer %d: %s\n", i, libusb_error_name(submit_res)); r = 1; goto cleanup; }
    }

    printf("Starting capture stream... (bulk transfers on EP 0x%02X)\n", EP_CAPTURE_DATA);
    for (int i = 0; i < NUM_CAPTURE_TRANSFERS; i++) {
        capture_transfers[i] = libusb_alloc_transfer(0);
        if (!capture_transfers[i]) { fprintf(stderr, "Failed to allocate capture transfer %d\n", i); r = 1; goto cleanup; }
        unsigned char *buf = malloc(capture_transfer_size);
        if (!buf) { fprintf(stderr, "Failed to allocate capture buffer for transfer %d\n", i); r = 1; goto cleanup; }
        libusb_fill_bulk_transfer(capture_transfers[i], handle, EP_CAPTURE_DATA, buf, capture_transfer_size, capture_callback, &state, USB_TIMEOUT);
        int submit_res = libusb_submit_transfer(capture_transfers[i]);
        if (submit_res < 0) { fprintf(stderr, "Failed to submit capture transfer %d: %s\n", i, libusb_error_name(submit_res)); r = 1; goto cleanup; }
    }
    printf("Initial capture transfers submitted.\n");

    if (pthread_create(&logging_thread, NULL, logging_thread_func, &log_args) != 0) {
        fprintf(stderr, "Failed to create logging thread.\n");
        is_running = false;
    }

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    printf("\n--- Playback active. Press 'p' to toggle sine wave, Ctrl+C to stop. ---\n");
    if (!minimal_log) printf("\n\n\n\n\n");

    while (is_running) {
        libusb_handle_events_timeout_completed(NULL, &(struct timeval){0, 50000}, NULL);
        char c = 0;
        if (read(STDIN_FILENO, &c, 1) > 0 && c == 'p') {
            pthread_mutex_lock(&state.lock);
            state.playing_sine = !state.playing_sine;
            if (!minimal_log) printf("\033[6A\033[K\r>>> Playback mode changed to: %s <<<\n", state.playing_sine ? "SINE WAVE" : "SILENCE");
            else printf("\nPlayback mode: %s\n", state.playing_sine ? "SINE WAVE" : "SILENCE");
            fflush(stdout);
            pthread_mutex_unlock(&state.lock);
        }
    }

    cleanup:
    is_running = false;
    if (logging_thread) pthread_join(logging_thread, NULL);

    for (int i = 0; i < NUM_PLAYBACK_TRANSFERS; i++) if (playback_transfers[i]) libusb_cancel_transfer(playback_transfers[i]);
    for (int i = 0; i < NUM_FEEDBACK_TRANSFERS; i++) if (feedback_transfers[i]) libusb_cancel_transfer(feedback_transfers[i]);
    for (int i = 0; i < NUM_CAPTURE_TRANSFERS; i++) if (capture_transfers[i]) libusb_cancel_transfer(capture_transfers[i]);

    struct timeval tv = {0, 100000};
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
    if (r != 1) libusb_exit(NULL);
    printf("Cleanup complete.\n");
    return r;
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

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -r <rate> -p <profile> [options]\n", prog_name);
    fprintf(stderr, "Required:\n");
    fprintf(stderr, "  -r <rate>         : 44100, 48000, 88200, 96000\n");
    fprintf(stderr, "  -p <profile>      : 0-4 (Lowest, Low, Normal, High, Highest)\n");
    fprintf(stderr, "Optional:\n");
    fprintf(stderr, "  --minimal-log     : Switch to a simple, single-line status summary.\n");
    fprintf(stderr, "  --log-interval <ms>: Set summary update frequency (default: 100ms).\n");
}

double timeval_diff_ms(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 + (end->tv_usec - start->tv_usec) / 1000.0;
}

void *logging_thread_func(void *arg) {
    struct logging_thread_args *args = (struct logging_thread_args *)arg;
    struct stream_state *state = args->state;
    const int bar_width = 20;

    while (is_running) {
        usleep(args->log_interval_ms * 1000);
        pthread_mutex_lock(&state->lock);

        const char *health = (state->underrun_count > 0 || state->overrun_count > 0) ? "\033[1;31mUNSTABLE\033[0m" : "\033[1;32mSTABLE\033[0m";
        const char *sync_status_str = state->feedback_warmed_up ? "\033[1;32mACQUIRED\033[0m" : (state->feedback_synced ? "\033[1;33mWARM-UP\033[0m" : "\033[1;31mLOST/OFF\033[0m");
        double avg_interval = (state->feedback_interval_count > 0) ? state->avg_feedback_interval_sum / state->feedback_interval_count : 0.0;

        if (args->minimal_log) {
            printf("Health: %s, Sync: %s, Avg Interval: %.2fms, Underruns: %lu, Overruns: %lu \r",
                   (state->underrun_count > 0 || state->overrun_count > 0) ? "UNSTABLE" : "STABLE",
                   state->feedback_warmed_up ? "ACQUIRED" : "WARMING",
                   avg_interval, state->underrun_count, state->overrun_count);
        } else {
            size_t fill = (state->feedback_pattern_in_idx - state->feedback_pattern_out_idx + FEEDBACK_ACCUMULATOR_SIZE) % FEEDBACK_ACCUMULATOR_SIZE;
            int filled_chars = (int)((double)fill / FEEDBACK_ACCUMULATOR_SIZE * bar_width);
            printf("\033[5A\033[K\n\033[K\n\033[K\n\033[K\n\033[K\n\033[5A");
            printf("--- TASCAM US-144MKII Stream Health ---\n");
            printf(" Health: %-18s Sync: %-18s Feedback: %-3d\n", health, sync_status_str, state->last_feedback_value);
            printf(" Buffer: [");
            for(int i=0; i<bar_width; ++i) putchar(i < filled_chars ? '#' : '-');
            printf("] %3zu/%d\n", fill, FEEDBACK_ACCUMULATOR_SIZE);
            printf(" Interval (ms) -> Now: %4.2f  Min: %4.2f  Avg: %4.2f  Max: %4.2f\n",
                   state->last_feedback_interval_ms,
                   state->min_feedback_interval_ms == DBL_MAX ? 0.0 : state->min_feedback_interval_ms,
                   avg_interval, state->max_feedback_interval_ms);
            printf(" Errors        -> Underruns: %-5lu Overruns: %lu\n", state->underrun_count, state->overrun_count);
        }
        fflush(stdout);
        pthread_mutex_unlock(&state->lock);
    }
    return NULL;
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
    struct timeval now;
    gettimeofday(&now, NULL);

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
            pthread_mutex_lock(&state->lock);
            if (state->feedback_synced) printf("\nSync Lost (URB Error: %s)!\n", libusb_error_name(transfer->status));
            state->feedback_synced = false;
            state->feedback_warmed_up = false;
            pthread_mutex_unlock(&state->lock);
        }
        goto resubmit;
    }

    pthread_mutex_lock(&state->lock);
    if (state->last_feedback_completion_time.tv_sec > 0) {
        state->last_feedback_interval_ms = timeval_diff_ms(&state->last_feedback_completion_time, &now);
        if (state->feedback_warmed_up) {
            if (state->last_feedback_interval_ms < state->min_feedback_interval_ms) state->min_feedback_interval_ms = state->last_feedback_interval_ms;
            if (state->last_feedback_interval_ms > state->max_feedback_interval_ms) state->max_feedback_interval_ms = state->last_feedback_interval_ms;
            state->avg_feedback_interval_sum += state->last_feedback_interval_ms;
            state->feedback_interval_count++;
        }
    }
    state->last_feedback_completion_time = now;

    bool was_synced = state->feedback_synced;
    bool sync_lost_this_urb = false;

    for (int p = 0; p < transfer->num_iso_packets; p++) {
        struct libusb_iso_packet_descriptor *pack = &transfer->iso_packet_desc[p];
        if (pack->status != 0 || pack->actual_length < 1) {
            sync_lost_this_urb = true;
            continue;
        }
        size_t packet_offset = p * FEEDBACK_PACKET_SIZE;
        uint8_t feedback_value = transfer->buffer[packet_offset];
        state->last_feedback_value = feedback_value;

        if (feedback_value >= state->rate_cfg->feedback_base_value && feedback_value <= state->rate_cfg->feedback_max_value) {
            unsigned int generated_pattern[8];
            unsigned int base_frames = state->rate_cfg->rate / 8000;
            unsigned int target_total_frames = feedback_value;
            int frame_adjustment = target_total_frames - (8 * base_frames);

            generate_feedback_pattern(base_frames, frame_adjustment, generated_pattern);

            size_t fill_level = (state->feedback_pattern_in_idx - state->feedback_pattern_out_idx + FEEDBACK_ACCUMULATOR_SIZE) % FEEDBACK_ACCUMULATOR_SIZE;
            if (fill_level > (FEEDBACK_ACCUMULATOR_SIZE - 16)) state->overrun_count++;

            for (int i = 0; i < 8; i++) {
                unsigned int in_idx = (state->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;
                state->feedback_accumulator_pattern[in_idx] = generated_pattern[i];
            }
            state->feedback_pattern_in_idx = (state->feedback_pattern_in_idx + 8) % FEEDBACK_ACCUMULATOR_SIZE;
        } else {
            sync_lost_this_urb = true;
        }
    }

    if (sync_lost_this_urb) {
        if (was_synced) printf("\nSync Lost (Bad Packet)!\n");
        state->feedback_synced = false;
        state->feedback_warmed_up = false;
    } else {
        if (!was_synced) printf("\nSync Acquired!\n");
        state->feedback_synced = true;
        size_t fill_level = (state->feedback_pattern_in_idx - state->feedback_pattern_out_idx + FEEDBACK_ACCUMULATOR_SIZE) % FEEDBACK_ACCUMULATOR_SIZE;
        if (!state->feedback_warmed_up && fill_level >= WARMUP_THRESHOLD) {
            state->feedback_warmed_up = true;
            state->min_feedback_interval_ms = DBL_MAX;
            state->max_feedback_interval_ms = 0.0;
            state->avg_feedback_interval_sum = 0.0;
            state->feedback_interval_count = 0;
            printf("\nBuffer warmed up. Measuring steady-state performance.\n");
        }
    }
    pthread_mutex_unlock(&state->lock);

    resubmit:
    if (is_running) libusb_submit_transfer(transfer);
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

    pthread_mutex_lock(&state->lock);
    int nominal_frames = state->rate_cfg->rate / 8000;
    unsigned char *buf_ptr = transfer->buffer;
    size_t total_bytes_in_urb = 0;
    const double phase_increment = (2.0 * M_PI * SINE_FREQUENCY) / state->rate_cfg->rate;

    for (int i = 0; i < transfer->num_iso_packets; i++) {
        unsigned int frames_for_packet;
        if (!state->feedback_warmed_up || state->feedback_pattern_out_idx == state->feedback_pattern_in_idx) {
            if (state->feedback_warmed_up) state->underrun_count++;
            frames_for_packet = nominal_frames;
        } else {
            frames_for_packet = state->feedback_accumulator_pattern[state->feedback_pattern_out_idx];
            state->feedback_pattern_out_idx = (state->feedback_pattern_out_idx + 1) % FEEDBACK_ACCUMULATOR_SIZE;
        }
        size_t bytes_for_packet = frames_for_packet * DEVICE_FRAME_SIZE;
        unsigned char* packet_buf_ptr = buf_ptr + total_bytes_in_urb;

        if (state->playing_sine && state->feedback_warmed_up) {
            for (unsigned int f = 0; f < frames_for_packet; f++) {
                double value = sin(state->sine_phase);
                int32_t sample_int = (int32_t)(value * SINE_AMPLITUDE);
                unsigned char* frame_ptr = packet_buf_ptr + (f * DEVICE_FRAME_SIZE);
                frame_ptr[0] = (sample_int >> 0) & 0xFF; frame_ptr[1] = (sample_int >> 8) & 0xFF; frame_ptr[2] = (sample_int >> 16) & 0xFF;
                frame_ptr[3] = (sample_int >> 0) & 0xFF; frame_ptr[4] = (sample_int >> 8) & 0xFF; frame_ptr[5] = (sample_int >> 16) & 0xFF;
                memset(frame_ptr + 6, 0, DEVICE_FRAME_SIZE - 6);
                state->sine_phase += phase_increment;
            }
            if (state->sine_phase >= 2.0 * M_PI) state->sine_phase -= 2.0 * M_PI;
        } else {
            memset(packet_buf_ptr, 0, bytes_for_packet);
        }

        transfer->iso_packet_desc[i].length = bytes_for_packet;
        total_bytes_in_urb += bytes_for_packet;
    }
    pthread_mutex_unlock(&state->lock);

    transfer->length = total_bytes_in_urb;

    if (is_running && libusb_submit_transfer(transfer) < 0) {
        fprintf(stderr, "\nError resubmitting playback transfer\n");
        is_running = false;
    }
}

int perform_initialization_sequence(libusb_device_handle *handle, const struct sample_rate_config *rate_config) {
    unsigned char buf[64]; int r;
    printf("\n--- STARTING DEVICE CONFIGURATION (per Spec v5.0) ---\n");
    #define CHECK(desc, call) r = (call); if (r < 0) { fprintf(stderr, "  [FAIL] %s: %s\n", desc, libusb_error_name(r)); return -1; } else { printf("  [OK] %s (returned %d)\n", desc, r); }
    printf("  [INFO] Step 1: Set Interfaces\n");
    r = libusb_set_configuration(handle, 1); if (r < 0 && r != LIBUSB_ERROR_BUSY) { fprintf(stderr, "  [FAIL] Set Configuration 1: %s\n", libusb_error_name(r)); return -1; }
    for (int i=0; i<=1; i++) { r = libusb_claim_interface(handle, i); if (r < 0) { fprintf(stderr, "  [FAIL] Claim Interface %d: %s\n", i, libusb_error_name(r)); return -1; } r = libusb_set_interface_alt_setting(handle, i, 1); if (r < 0) { fprintf(stderr, "  [FAIL] Set Alt Setting on Intf %d: %s\n", i, libusb_error_name(r)); return -1; } }
    printf("  [OK] Step 1: Interfaces set and claimed.\n");
    printf("\n-- Step 2: Initial Handshake --\n"); CHECK("Status Check", libusb_control_transfer(handle, RT_D2H_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0000, 0x0000, buf, 1, USB_TIMEOUT));
    printf("\n-- Step 3: Set Initial Mode --\n"); CHECK("Set Initial Mode", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0010, 0x0000, NULL, 0, USB_TIMEOUT));
    printf("\n-- Step 4: Set Sample Rate to %d Hz --\n", rate_config->rate);
    CHECK("Set Rate on Feedback EP (0x81)", libusb_control_transfer(handle, RT_H2D_CLASS_EP, UAC_SET_CUR, UAC_SAMPLING_FREQ_CONTROL, EP_PLAYBACK_FEEDBACK, (unsigned char*)rate_config->rate_data, 3, USB_TIMEOUT));
    CHECK("Set Rate on Playback EP (0x02)", libusb_control_transfer(handle, RT_H2D_CLASS_EP, UAC_SET_CUR, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_OUT, (unsigned char*)rate_config->rate_data, 3, USB_TIMEOUT));
    printf("\n-- Step 5: Configure Internal Registers --\n"); CHECK("Reg Write 1 (0x0d04)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0d04, 0x0101, NULL, 0, USB_TIMEOUT)); CHECK("Reg Write 2 (0x0e00)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0e00, 0x0101, NULL, 0, USB_TIMEOUT)); CHECK("Reg Write 3 (0x0f00)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x0f00, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 4 (Rate-Dep)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, rate_config->rate_vendor_wValue, 0x0101, NULL, 0, USB_TIMEOUT));
    CHECK("Reg Write 5 (0x110b)", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_REGISTER_WRITE, 0x110b, 0x0101, NULL, 0, USB_TIMEOUT));
    printf("\n-- Step 6: Enable Streaming --\n"); CHECK("Enable Streaming", libusb_control_transfer(handle, RT_H2D_VENDOR_DEV, VENDOR_REQ_MODE_CONTROL, 0x0030, 0x0000, NULL, 0, USB_TIMEOUT));
    printf("\n--- CONFIGURATION COMPLETE ---\n\n"); return 0;
}
