/**
 * Edge Gunshot Detector with LAROD Integration - v1.2.104
 * Developed by Claude Coding
 * Real-time gunshot detection with email notifications
 * © 2025 Claude Coding. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <poll.h>
#include <glib.h>
#include <gio/gio.h>

// Parameter handling for manifest-defined parameters

// Email notification includes
#include <curl/curl.h>

// PipeWire includes (from official example)
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

// LAROD includes
#include <larod.h>

// FFT and mel spectrogram includes
#include <complex.h>
#include <fftw3.h>

// Audio processing constants (from v1.1.78 working model)
#define SAMPLE_RATE 48000
#define TARGET_SAMPLE_RATE 22050
#define N_FFT 1024
#define HOP_LENGTH 512
#define N_MELS 28
#define N_FRAMES 160
#define EXPECTED_INPUT_SIZE (N_MELS * N_FRAMES)

// Audio buffer (sized for gunshot detection)
#define AUDIO_BUFFER_SIZE 180800  // Enough for 160 frames at 48kHz
#define INFERENCE_THRESHOLD 88000  // ~2 seconds at 48kHz

// Mel filter bank parameters
#define N_FFT_BINS (N_FFT / 2 + 1)  // 513 bins
#define MEL_FMIN 0.0f
#define MEL_FMAX (TARGET_SAMPLE_RATE / 2.0f)  // Nyquist frequency
#define MEL_NORM_SLANEY 1  // Use Slaney normalization (librosa default)

// Configuration
#define CONFIG_PATH "/usr/local/packages/gunshot_detector/conf/gunshot_detector.conf"
static float confidence_threshold = 0.45f;  // Default 45%

// Parameter configuration via manifest.json

// Email notification configuration
static bool email_enabled = false;
static char smtp_server[256] = "smtp.gmail.com";
static int smtp_port = 587;
static char smtp_username[256] = "";
static char smtp_password[256] = "";
static char recipient_email[256] = "";
static time_t last_email_time = 0;
static const int EMAIL_RATE_LIMIT_SECONDS = 120;  // 2 minutes between emails

// Global mel filter bank matrix (pre-computed)
static float mel_filter_bank[N_MELS][N_FFT_BINS];
static bool mel_filters_initialized = false;

// FFT workspace
static fftwf_complex *fft_in = NULL;
static fftwf_complex *fft_out = NULL;
static fftwf_plan fft_plan = NULL;
static float *hann_window = NULL;
static bool fft_initialized = false;

// LAROD variables
static larodConnection *conn = NULL;
static const larodDevice *dev = NULL;
static larodModel *model = NULL;
static larodJobRequest *infReq = NULL;
static larodTensor **inputTensors = NULL;
static larodTensor **outputTensors = NULL;
static size_t numInputs = 0;
static size_t numOutputs = 0;

// Tensor file descriptors and memory mapping
static int inputTensorFd = -1;
static int outputTensorFd = -1;
static void *inputTensorAddr = NULL;
static void *outputTensorAddr = NULL;
static size_t inputTensorSize = EXPECTED_INPUT_SIZE * sizeof(int8_t);
static size_t outputTensorSize = 2 * sizeof(int8_t);

// Audio processing state
static float audio_buffer[AUDIO_BUFFER_SIZE];
static uint32_t samples_accumulated = 0;
static uint32_t debug_counter = 0;

// Global running flag and ML state
static volatile bool running = true;
static volatile bool ml_ready = false;
static uint32_t inference_count = 0;
static uint32_t detection_count = 0;

// Stream data (based on official audiocapture.c structure)
struct stream_data {
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    char name[64];
    float peak[SPA_AUDIO_MAX_CHANNELS];
    bool is_target_stream;
};

// PipeWire globals (from official example)
struct pw_main_loop *loop;
struct pw_context *context;
struct pw_core *core;
struct pw_registry *registry;
struct spa_hook registry_listener;

/**
 * Check for parameter files in various locations
 */
static void debug_parameter_locations(void) {
    const char *potential_paths[] = {
        "/usr/local/packages/gunshot_detector/config.json",
        "/usr/local/packages/gunshot_detector/param.json", 
        "/usr/local/packages/gunshot_detector/parameters.conf",
        "/etc/gunshot_detector/config.json",
        "/var/lib/gunshot_detector/config.json",
        "/tmp/gunshot_detector_params.json",
        "/usr/local/packages/gunshot_detector/",
        NULL
    };
    
    syslog(LOG_INFO, "[DEBUG] Checking parameter file locations...");
    
    for (int i = 0; potential_paths[i] != NULL; i++) {
        struct stat st;
        if (stat(potential_paths[i], &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                syslog(LOG_INFO, "[DEBUG] Directory exists: %s", potential_paths[i]);
            } else {
                syslog(LOG_INFO, "[DEBUG] File exists: %s (size: %ld bytes)", potential_paths[i], st.st_size);
            }
        } else {
            syslog(LOG_DEBUG, "[DEBUG] Not found: %s (%s)", potential_paths[i], strerror(errno));
        }
    }
}

/**
 * Load configuration from Axis parameter config file
 */
static void load_config(void) {
    syslog(LOG_INFO, "[CONFIG] Loading configuration from %s", CONFIG_PATH);
    
    // Read from Axis parameter config file
    FILE *config_file = fopen(CONFIG_PATH, "r");
    if (!config_file) {
        syslog(LOG_WARNING, "[CONFIG] File %s not found: %s", CONFIG_PATH, strerror(errno));
        syslog(LOG_INFO, "[CONFIG] Using defaults - threshold: %.0f%%, email: %s", 
               confidence_threshold * 100.0f, email_enabled ? "enabled" : "disabled");
        return;
    }
    
    char line[256];
    syslog(LOG_INFO, "[CONFIG] Reading Axis parameter file...");
    
    while (fgets(line, sizeof(line), config_file)) {
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        syslog(LOG_INFO, "[CONFIG] Line: %s", line);
        
        // Parse threshold parameter (format: threshold="35")
        if (strstr(line, "threshold=")) {
            int threshold_int = 0;
            syslog(LOG_INFO, "[CONFIG] Found threshold line, attempting to parse...");
            syslog(LOG_INFO, "[CONFIG] Trying to parse line: '%s' with format: 'threshold=\"%%d\"'", line);
            int parsed_count = sscanf(line, "threshold=\"%d\"", &threshold_int);
            syslog(LOG_INFO, "[CONFIG] sscanf returned %d, threshold_int=%d", parsed_count, threshold_int);
            if (parsed_count == 1) {
                float threshold = threshold_int / 100.0f;  // Convert percentage to decimal
                syslog(LOG_INFO, "[CONFIG] Successfully parsed threshold: %d%% (%.3f)", threshold_int, threshold);
                if (threshold >= 0.30f && threshold <= 0.70f) {
                    float old_threshold = confidence_threshold;
                    confidence_threshold = threshold;
                    syslog(LOG_INFO, "[CONFIG] ✅ Updated threshold: %.0f%% -> %.0f%%", 
                           old_threshold * 100.0f, confidence_threshold * 100.0f);
                } else {
                    syslog(LOG_WARNING, "[CONFIG] ❌ Threshold %d%% out of range (30-70%%), keeping %.0f%%", 
                           threshold_int, confidence_threshold * 100.0f);
                }
            } else {
                syslog(LOG_WARNING, "[CONFIG] ❌ Failed to parse threshold from line: %s", line);
            }
        }
        
        // Parse email_enabled parameter (format: email_enabled="yes")
        if (strstr(line, "email_enabled=")) {
            char enabled_str[16];
            if (sscanf(line, "email_enabled=\"%15[^\"]\"", enabled_str) == 1) {
                email_enabled = (strcmp(enabled_str, "yes") == 0);
                syslog(LOG_INFO, "[CONFIG] Email notifications: %s", email_enabled ? "enabled" : "disabled");
            }
        }
        
        // Parse smtp_server parameter
        if (strstr(line, "smtp_server=")) {
            if (sscanf(line, "smtp_server=\"%255[^\"]\"", smtp_server) == 1) {
                syslog(LOG_INFO, "[CONFIG] SMTP server: %s", smtp_server);
            }
        }
        
        // Parse smtp_port parameter
        if (strstr(line, "smtp_port=")) {
            if (sscanf(line, "smtp_port=\"%d\"", &smtp_port) == 1) {
                syslog(LOG_INFO, "[CONFIG] SMTP port: %d", smtp_port);
            }
        }
        
        // Parse smtp_username parameter
        if (strstr(line, "smtp_username=")) {
            if (sscanf(line, "smtp_username=\"%255[^\"]\"", smtp_username) == 1) {
                syslog(LOG_INFO, "[CONFIG] SMTP username: %s", smtp_username);
            }
        }
        
        // Parse smtp_password parameter (don't log for security)
        if (strstr(line, "smtp_password=")) {
            if (sscanf(line, "smtp_password=\"%255[^\"]\"", smtp_password) == 1) {
                syslog(LOG_INFO, "[CONFIG] SMTP password: %s", strlen(smtp_password) > 0 ? "[configured]" : "[empty]");
            }
        }
        
        // Parse recipient_email parameter
        if (strstr(line, "recipient_email=")) {
            if (sscanf(line, "recipient_email=\"%255[^\"]\"", recipient_email) == 1) {
                syslog(LOG_INFO, "[CONFIG] Recipient email: %s", recipient_email);
            }
        }
    }
    
    fclose(config_file);
}

/**
 * DBus signal handler for parameter changes
 */
static void on_parameter_changed(GDBusConnection *connection,
                                const gchar *sender_name,
                                const gchar *object_path,
                                const gchar *interface_name,
                                const gchar *signal_name,
                                GVariant *parameters,
                                gpointer user_data) {
    gchar *param_name = NULL;
    gchar *param_value = NULL;
    
    // Parse the signal parameters
    g_variant_get(parameters, "(ss)", &param_name, &param_value);
    
    syslog(LOG_INFO, "[DBUS] Parameter changed: %s = %s", param_name, param_value);
    
    // Check if it's a threshold change
    if (param_name && strstr(param_name, "threshold")) {
        int threshold_int = atoi(param_value);
        if (threshold_int >= 30 && threshold_int <= 70) {
            float old_threshold = confidence_threshold;
            confidence_threshold = threshold_int / 100.0f;
            syslog(LOG_INFO, "[DBUS] ✅ Real-time threshold update: %.0f%% -> %.0f%%", 
                   old_threshold * 100.0f, confidence_threshold * 100.0f);
        }
    }
    
    // Check for other parameter changes
    if (param_name && strstr(param_name, "email_enabled")) {
        email_enabled = (strcmp(param_value, "yes") == 0);
        syslog(LOG_INFO, "[DBUS] Email notifications: %s", email_enabled ? "enabled" : "disabled");
    }
    
    g_free(param_name);
    g_free(param_value);
}

// Config reload timer
static time_t last_config_check = 0;

/**
 * Periodic config file check (every 5 seconds)
 */
static void check_config_changes(void) {
    time_t now = time(NULL);
    if (now - last_config_check < 5) {
        return;  // Check every 5 seconds
    }
    last_config_check = now;
    
    // Check if config file was modified
    struct stat st;
    if (stat(CONFIG_PATH, &st) == 0) {
        static time_t last_mtime = 0;
        if (st.st_mtime != last_mtime) {
            last_mtime = st.st_mtime;
            syslog(LOG_INFO, "[CONFIG] Configuration file changed, reloading...");
            load_config();
        }
    }
}

/**
 * Setup simple config file monitoring (safe approach)
 */
static void setup_config_monitoring(void) {
    syslog(LOG_INFO, "[CONFIG] Setting up file-based parameter monitoring");
    last_config_check = time(NULL);
}

/**
 * Email payload structure for libcurl
 */
struct email_upload_status {
    char *data;
    size_t length;
    size_t position;
};

/**
 * Callback for libcurl to read email data
 */
static size_t email_payload_source(void *ptr, size_t size, size_t nmemb, void *userp) {
    struct email_upload_status *upload_ctx = (struct email_upload_status *)userp;
    const char *data = upload_ctx->data;
    size_t room = size * nmemb;
    
    if (upload_ctx->position >= upload_ctx->length) {
        return 0;  // No more data
    }
    
    size_t remaining = upload_ctx->length - upload_ctx->position;
    size_t len = (remaining < room) ? remaining : room;
    
    memcpy(ptr, data + upload_ctx->position, len);
    upload_ctx->position += len;
    
    return len;
}

/**
 * Send email notification for gunshot detection
 */
static bool send_email_notification(float confidence, float rms) {
    if (!email_enabled || strlen(smtp_username) == 0 || strlen(recipient_email) == 0) {
        return false;
    }
    
    // Rate limiting - only send one email every 2 minutes
    time_t current_time = time(NULL);
    if (current_time - last_email_time < EMAIL_RATE_LIMIT_SECONDS) {
        syslog(LOG_DEBUG, "[EMAIL] Rate limited - last email sent %ld seconds ago", 
               current_time - last_email_time);
        return false;
    }
    
    CURL *curl;
    CURLcode res = CURLE_OK;
    struct curl_slist *recipients = NULL;
    struct email_upload_status upload_ctx;
    
    // Build email content
    char email_body[2048];
    char timestamp[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    snprintf(email_body, sizeof(email_body),
        "To: %s\r\n"
        "From: %s\r\n"
        "Subject: 🔫 Gunshot Detected - Security Alert\r\n"
        "\r\n"
        "GUNSHOT DETECTION ALERT\r\n"
        "========================\r\n"
        "\r\n"
        "Time: %s\r\n"
        "Confidence: %.1f%%\r\n"
        "Audio RMS: %.3f\r\n"
        "Camera: Axis Gunshot Detector\r\n"
        "\r\n"
        "This is an automated security notification.\r\n"
        "Please investigate immediately.\r\n"
        "\r\n"
        "-- Axis Gunshot Detection System\r\n",
        recipient_email, smtp_username, timestamp, confidence, rms);
    
    upload_ctx.data = email_body;
    upload_ctx.length = strlen(email_body);
    upload_ctx.position = 0;
    
    curl = curl_easy_init();
    if (!curl) {
        syslog(LOG_ERR, "[EMAIL] Failed to initialize curl");
        return false;
    }
    
    // Build SMTP URL - use smtp:// for port 587 (STARTTLS) or smtps:// for port 465 (SSL)
    char smtp_url[512];
    if (smtp_port == 465) {
        snprintf(smtp_url, sizeof(smtp_url), "smtps://%s:%d", smtp_server, smtp_port);
    } else {
        snprintf(smtp_url, sizeof(smtp_url), "smtp://%s:%d", smtp_server, smtp_port);
    }
    
    syslog(LOG_INFO, "[EMAIL] Connecting to %s", smtp_url);
    
    // Configure SMTP settings
    curl_easy_setopt(curl, CURLOPT_URL, smtp_url);
    
    // SSL/TLS configuration based on port
    if (smtp_port == 465) {
        // Port 465: Use SSL from the start
        curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    } else {
        // Port 587: Use STARTTLS
        curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_TRY);
    }
    
    // Additional SSL settings for Gmail compatibility
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERNAME, smtp_username);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, smtp_password);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, smtp_username);
    
    recipients = curl_slist_append(recipients, recipient_email);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, email_payload_source);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload_ctx);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    
    // Set timeout and enable verbose logging for debugging
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    
    syslog(LOG_INFO, "[EMAIL] Attempting to send email to %s via %s", recipient_email, smtp_url);
    syslog(LOG_INFO, "[EMAIL] Username: %s, SSL Mode: %s", 
           smtp_username, (smtp_port == 465) ? "SSL" : "STARTTLS");
    
    // Send the email
    res = curl_easy_perform(curl);
    
    bool success = (res == CURLE_OK);
    if (success) {
        last_email_time = current_time;
        syslog(LOG_INFO, "[EMAIL] ✅ Gunshot alert sent to %s (%.1f%% confidence)", 
               recipient_email, confidence);
    } else {
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        syslog(LOG_ERR, "[EMAIL] ❌ Failed to send email: %s (Response code: %ld)", 
               curl_easy_strerror(res), response_code);
        syslog(LOG_ERR, "[EMAIL] Debug: URL=%s, Port=%d, Username=%s", 
               smtp_url, smtp_port, smtp_username);
    }
    
    // Cleanup
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    
    return success;
}


/**
 * Create and map temporary file for tensor data
 */
static bool create_and_map_tmp_file(const char *pattern, size_t size, void **addr, int *fd) {
    char tmp_file[256];
    
    snprintf(tmp_file, sizeof(tmp_file), "%s", pattern);
    *fd = mkstemp(tmp_file);
    if (*fd < 0) {
        syslog(LOG_ERR, "Failed to create temporary file: %s", strerror(errno));
        return false;
    }
    
    if (ftruncate(*fd, size) != 0) {
        syslog(LOG_ERR, "Failed to set file size: %s", strerror(errno));
        close(*fd);
        *fd = -1;
        return false;
    }
    
    *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (*addr == MAP_FAILED) {
        syslog(LOG_ERR, "Failed to mmap file: %s", strerror(errno));
        close(*fd);
        *fd = -1;
        return false;
    }
    
    unlink(tmp_file);
    syslog(LOG_DEBUG, "Created temporary tensor file, size: %zu bytes", size);
    return true;
}

/**
 * Convert frequency to mel scale
 */
static float hz_to_mel(float hz) {
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

/**
 * Convert mel scale to frequency
 */
static float mel_to_hz(float mel) {
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

/**
 * Initialize FFT workspace and Hann window
 */
static bool init_fft_workspace(void) {
    if (fft_initialized) {
        return true;
    }
    
    fft_in = fftwf_alloc_complex(N_FFT);
    fft_out = fftwf_alloc_complex(N_FFT);
    hann_window = malloc(N_FFT * sizeof(float));
    
    if (!fft_in || !fft_out || !hann_window) {
        syslog(LOG_ERR, "[FFT] Failed to allocate FFT workspace");
        return false;
    }
    
    fft_plan = fftwf_plan_dft_1d(N_FFT, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!fft_plan) {
        syslog(LOG_ERR, "[FFT] Failed to create FFT plan");
        return false;
    }
    
    for (int i = 0; i < N_FFT; i++) {
        hann_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (N_FFT - 1)));
    }
    
    fft_initialized = true;
    syslog(LOG_INFO, "[FFT] FFT workspace initialized successfully");
    return true;
}

/**
 * Initialize mel filter bank matrix (librosa-compatible)
 */
static bool init_mel_filter_bank(void) {
    if (mel_filters_initialized) {
        return true;
    }
    
    syslog(LOG_INFO, "[MEL] Initializing mel filter bank: %d mels, %d FFT bins", N_MELS, N_FFT_BINS);
    
    memset(mel_filter_bank, 0, sizeof(mel_filter_bank));
    
    float mel_min = hz_to_mel(MEL_FMIN);
    float mel_max = hz_to_mel(MEL_FMAX);
    
    float mel_points[N_MELS + 2];
    for (int i = 0; i < N_MELS + 2; i++) {
        mel_points[i] = mel_min + (mel_max - mel_min) * i / (N_MELS + 1);
    }
    
    float hz_points[N_MELS + 2];
    for (int i = 0; i < N_MELS + 2; i++) {
        hz_points[i] = mel_to_hz(mel_points[i]);
    }
    
    int bin_points[N_MELS + 2];
    for (int i = 0; i < N_MELS + 2; i++) {
        bin_points[i] = (int)floorf(hz_points[i] * N_FFT / TARGET_SAMPLE_RATE);
        if (bin_points[i] >= N_FFT_BINS) {
            bin_points[i] = N_FFT_BINS - 1;
        }
    }
    
    for (int m = 0; m < N_MELS; m++) {
        int left = bin_points[m];
        int center = bin_points[m + 1];
        int right = bin_points[m + 2];
        
        for (int k = left; k < center; k++) {
            if (center > left) {
                mel_filter_bank[m][k] = (float)(k - left) / (center - left);
            }
        }
        
        for (int k = center; k < right; k++) {
            if (right > center) {
                mel_filter_bank[m][k] = (float)(right - k) / (right - center);
            }
        }
        
        if (MEL_NORM_SLANEY) {
            float area = 0.0f;
            for (int k = 0; k < N_FFT_BINS; k++) {
                area += mel_filter_bank[m][k];
            }
            if (area > 0.0f) {
                for (int k = 0; k < N_FFT_BINS; k++) {
                    mel_filter_bank[m][k] /= area;
                }
            }
        }
    }
    
    mel_filters_initialized = true;
    syslog(LOG_INFO, "[MEL] Mel filter bank initialized successfully");
    return true;
}

/**
 * Compute mel-spectrogram for audio (librosa-compatible version)
 */
static void compute_mel_spectrogram(const float *audio, size_t num_samples, float *output) {
    if (!init_fft_workspace() || !init_mel_filter_bank()) {
        syslog(LOG_ERR, "[MEL] Failed to initialize FFT workspace or mel filter bank");
        memset(output, 0, EXPECTED_INPUT_SIZE * sizeof(float));
        return;
    }
    
    memset(output, 0, EXPECTED_INPUT_SIZE * sizeof(float));
    
    float power_spectrum[N_FFT_BINS];
    
    int frame_count = 0;
    for (int start = 0; start < (int)num_samples - N_FFT && frame_count < N_FRAMES; start += HOP_LENGTH) {
        for (int i = 0; i < N_FFT; i++) {
            if (start + i < (int)num_samples) {
                fft_in[i] = audio[start + i] * hann_window[i];
            } else {
                fft_in[i] = 0.0f;
            }
        }
        
        fftwf_execute(fft_plan);
        
        for (int i = 0; i < N_FFT_BINS; i++) {
            float real = crealf(fft_out[i]);
            float imag = cimagf(fft_out[i]);
            power_spectrum[i] = real * real + imag * imag;
        }
        
        for (int m = 0; m < N_MELS; m++) {
            float mel_energy = 0.0f;
            for (int k = 0; k < N_FFT_BINS; k++) {
                mel_energy += mel_filter_bank[m][k] * power_spectrum[k];
            }
            
            float mel_db = 10.0f * log10f(fmaxf(mel_energy, 1e-10f));
            float mel_normalized = (mel_db - (-80.0f)) / (0.0f - (-80.0f));
            
            if (mel_normalized < 0.0f) mel_normalized = 0.0f;
            if (mel_normalized > 1.0f) mel_normalized = 1.0f;
            
            output[frame_count * N_MELS + m] = mel_normalized;
        }
        
        frame_count++;
    }
    
    syslog(LOG_INFO, "[MEL] Computed mel spectrogram: %d frames, %d mels", frame_count, N_MELS);
}

/**
 * Convert float mel features to int8 with corrected quantization
 */
static void quantize_input(const float *mel_features, int8_t *quantized_output) {
    const float scale = 0.003921568859368563f;
    const int zero_point = -128;
    
    for (int i = 0; i < EXPECTED_INPUT_SIZE; i++) {
        float mel_value = mel_features[i];
        float centered_value = (mel_value - 0.5f) * 2.0f;
        int quantized = (int)roundf(centered_value / scale) + zero_point;
        
        if (quantized < -128) quantized = -128;
        if (quantized > 127) quantized = 127;
        
        quantized_output[i] = (int8_t)quantized;
    }
}

/**
 * Process audio frame and run gunshot detection inference
 */
static bool process_gunshot_detection(const float *audio_samples, size_t num_samples) {
    if (!ml_ready) {
        return false;
    }
    
    // Calculate RMS to check if audio is too quiet
    float rms = 0.0f;
    for (size_t i = 0; i < num_samples; i++) {
        rms += audio_samples[i] * audio_samples[i];
    }
    rms = sqrtf(rms / num_samples);
    
    // Skip inference on very quiet audio to prevent false positives
    const float MIN_RMS_THRESHOLD = 0.001f;  // -60 dB
    if (rms < MIN_RMS_THRESHOLD) {
        syslog(LOG_DEBUG, "[SILENCE] Skipping inference on quiet audio (RMS: %.6f < %.6f)", rms, MIN_RMS_THRESHOLD);
        return false;
    }
    
    // Compute mel spectrogram
    float mel_features[EXPECTED_INPUT_SIZE];
    compute_mel_spectrogram(audio_samples, num_samples, mel_features);
    
    // Quantize for model input
    int8_t quantized_input[EXPECTED_INPUT_SIZE];
    quantize_input(mel_features, quantized_input);
    
    // Copy quantized input to tensor memory
    memcpy(inputTensorAddr, quantized_input, inputTensorSize);
    
    // Run inference
    larodError *error = NULL;
    if (larodRunJob(conn, infReq, &error)) {
        // Read results
        int8_t *output_data = (int8_t *)outputTensorAddr;
        float output1 = output_data[0] * 0.003921568859368563f + (-128 * 0.003921568859368563f);
        float output2 = output_data[1] * 0.003921568859368563f + (-128 * 0.003921568859368563f);
        
        // Apply softmax
        float exp1 = expf(output1);
        float exp2 = expf(output2);
        float sum = exp1 + exp2;
        float prob1 = exp1 / sum;
        float prob2 = exp2 / sum;
        
        float gunshot_confidence = prob2 * 100.0f;
        
        inference_count++;
        
        // Detection logic
        if (prob2 > confidence_threshold) {
            detection_count++;
            syslog(LOG_WARNING, "🔫 [GUNSHOT DETECTED - CAMERA AUDIO] Confidence: %.1f%%, RMS: %.3f", 
                   gunshot_confidence, rms);
            syslog(LOG_INFO, "🔫 [CAMERA] Gunshot: %.1f%% (thresh: %.0f%%, RMS: %.3f)", 
                   gunshot_confidence, confidence_threshold * 100.0f, rms);
            
            // Send email notification
            if (email_enabled) {
                send_email_notification(gunshot_confidence, rms);
            }
            
            return true;
        } else {
            syslog(LOG_INFO, "❌ [CAMERA] Gunshot: %.1f%% (thresh: %.0f%%, RMS: %.3f)", 
                   gunshot_confidence, confidence_threshold * 100.0f, rms);
        }
        
        larodClearError(&error);
        return false;
    } else {
        syslog(LOG_ERR, "Failed to run inference: %s", error ? error->msg : "Unknown error");
        larodClearError(&error);
        return false;
    }
}

/**
 * Audio processing callback (adapted from official audiocapture.c)
 */
static void on_process(void *userdata) {
    struct stream_data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *samples;
    uint32_t n_channels, n_samples;

    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        syslog(LOG_WARNING, "Out of buffers for %s", data->name);
        return;
    }

    buf = b->buffer;
    if ((samples = buf->datas[0].data) == NULL) {
        goto done;
    }

    n_channels = buf->datas[0].chunk->size / sizeof(float);
    n_samples = n_channels; // Simplified for now

    // Only process target stream (AudioDevice0Input0.Unprocessed)
    if (data->is_target_stream && ml_ready) {
        // Debug: Log every 1000 audio callbacks to show activity
        if (++debug_counter % 1000 == 1) {
            syslog(LOG_INFO, "[CAMERA] Audio activity: received %u samples, accumulated %u total", 
                   n_samples, samples_accumulated);
        }
        
        // Periodically reload config (every ~5000 callbacks)
        if (debug_counter % 5000 == 0) {
            load_config();
        }
        
        // Add samples to buffer
        if (samples_accumulated + n_samples <= AUDIO_BUFFER_SIZE) {
            memcpy(audio_buffer + samples_accumulated, samples, n_samples * sizeof(float));
            samples_accumulated += n_samples;
            
            // Check for config changes periodically
            check_config_changes();
            
            // Process when we have enough samples (~2 seconds at 48kHz)
            if (samples_accumulated >= INFERENCE_THRESHOLD) {
                static bool first_inference = true;
                if (first_inference) {
                    syslog(LOG_INFO, "*** STARTING REAL CAMERA AUDIO GUNSHOT DETECTION ***");
                    first_inference = false;
                }
                process_gunshot_detection(audio_buffer, AUDIO_BUFFER_SIZE);
                samples_accumulated = 0; // Reset for next batch
            }
        }
    }

done:
    pw_stream_queue_buffer(data->stream, b);
}

/**
 * Stream parameter callback (from official audiocapture.c)
 */
static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    struct stream_data *data = userdata;
    struct spa_audio_info info = { 0 };
    int ret;

    if (id != SPA_PARAM_Format || param == NULL) {
        return;
    }

    if ((ret = spa_format_parse(param, &info.media_type, &info.media_subtype)) < 0) {
        return;
    }

    if (info.media_type != SPA_MEDIA_TYPE_audio ||
        info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }

    spa_format_audio_raw_parse(param, &info.info.raw);

    syslog(LOG_INFO, "[CAMERA] Capturing from node %s, %d channel(s), rate %d.", 
           data->name, info.info.raw.channels, info.info.raw.rate);
    
    // Mark if this is our target stream
    if (strstr(data->name, "AudioDevice0Input0.Unprocessed") != NULL) {
        data->is_target_stream = true;
        syslog(LOG_INFO, "[CAMERA] *** TARGET STREAM FOUND: %s ***", data->name);
    }
}

/**
 * Stream state callback (from official audiocapture.c)
 */
static void on_state_changed(void *userdata, enum pw_stream_state old, 
                           enum pw_stream_state state, const char *error) {
    struct stream_data *data = userdata;
    
    switch (state) {
        case PW_STREAM_STATE_STREAMING:
            syslog(LOG_INFO, "[CAMERA] Stream %s is now streaming", data->name);
            break;
        case PW_STREAM_STATE_ERROR:
            syslog(LOG_ERR, "[CAMERA] Stream %s error: %s", data->name, error);
            running = false;
            break;
        default:
            break;
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process = on_process,
    .state_changed = on_state_changed,
};

/**
 * Registry global callback (adapted from official audiocapture.c)
 */
static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
                                const char *type, uint32_t version,
                                const struct spa_dict *props) {
    const char *media_class, *node_name;
    struct stream_data *stream_data;
    struct pw_properties *stream_props;
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }

    media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);

    if (media_class == NULL || node_name == NULL) {
        return;
    }

    // Log all discovered audio nodes
    syslog(LOG_INFO, "[REGISTRY] Found %s node %s with id %d.", media_class, node_name, id);

    // Only connect to AudioDevice0Input0 nodes (like official example)
    if (!strstr(node_name, "AudioDevice0Input0")) {
        return;
    }

    syslog(LOG_INFO, "[CAMERA] *** CONNECTING TO AUDIO INPUT: %s ***", node_name);

    stream_data = calloc(1, sizeof(struct stream_data));
    strncpy(stream_data->name, node_name, sizeof(stream_data->name) - 1);
    stream_data->is_target_stream = false;

    stream_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_TARGET_OBJECT, node_name,
        NULL);

    stream_data->stream = pw_stream_new(core, "Gunshot Detector", stream_props);

    pw_stream_add_listener(stream_data->stream, &stream_data->stream_listener,
                          &stream_events, stream_data);

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
                                          &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32));

    pw_stream_connect(stream_data->stream,
                     PW_DIRECTION_INPUT,
                     PW_ID_ANY,
                     PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                     params, 1);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
};

/**
 * Initialize LAROD (from v1.1.78 working model)
 */
static bool init_larod(const char *model_path) {
    larodError *error = NULL;
    
    syslog(LOG_INFO, "Initializing LAROD with model: %s", model_path);
    
    if (!larodConnect(&conn, &error)) {
        syslog(LOG_ERR, "Failed to connect to LAROD: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    
    dev = larodGetDevice(conn, "cpu-tflite", 0, &error);
    if (!dev) {
        syslog(LOG_ERR, "CPU-tflite device not available: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    
    int modelFd = open(model_path, O_RDONLY);
    if (modelFd < 0) {
        syslog(LOG_ERR, "Failed to open model file: %s", model_path);
        return false;
    }
    
    model = larodLoadModel(conn, modelFd, dev, LAROD_ACCESS_PRIVATE, 
                          "GunShotModel", NULL, &error);
    close(modelFd);
    
    if (!model) {
        syslog(LOG_ERR, "Failed to load model: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    
    // Create temporary files for tensors
    if (!create_and_map_tmp_file("/tmp/gunshot_input_XXXXXX", inputTensorSize, 
                                 &inputTensorAddr, &inputTensorFd)) {
        return false;
    }
    
    if (!create_and_map_tmp_file("/tmp/gunshot_output_XXXXXX", outputTensorSize, 
                                 &outputTensorAddr, &outputTensorFd)) {
        return false;
    }
    
    // Create model tensors
    inputTensors = larodCreateModelInputs(model, &numInputs, &error);
    if (!inputTensors || numInputs != 1) {
        syslog(LOG_ERR, "Failed to create input tensors");
        larodClearError(&error);
        return false;
    }
    
    outputTensors = larodCreateModelOutputs(model, &numOutputs, &error);
    if (!outputTensors || numOutputs < 1) {
        syslog(LOG_ERR, "Failed to create output tensors");
        larodClearError(&error);
        return false;
    }
    
    // Associate file descriptors with tensors
    if (!larodSetTensorFd(inputTensors[0], inputTensorFd, &error)) {
        syslog(LOG_ERR, "Failed to set input tensor fd: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    
    if (!larodSetTensorFd(outputTensors[0], outputTensorFd, &error)) {
        syslog(LOG_ERR, "Failed to set output tensor fd: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    
    // Create job request
    infReq = larodCreateJobRequest(model, inputTensors, numInputs, 
                                   outputTensors, numOutputs, NULL, &error);
    if (!infReq) {
        syslog(LOG_ERR, "Failed to create job request: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    
    syslog(LOG_INFO, "LAROD inference engine initialized successfully");
    return true;
}

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int sig) {
    syslog(LOG_INFO, "Received signal %d, shutting down", sig);
    running = false;
    pw_main_loop_quit(loop);
}

/**
 * Main function (adapted from official audiocapture.c structure)
 */
int main(void) {
    openlog("gunshot_detector", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Gunshot Detector v1.1.100 starting - Debug Parameter Parsing");
    
    // Initialize curl for email notifications
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Debug parameter file locations
    debug_parameter_locations();
    
    // Load configuration
    load_config();
    
    // Setup safe config file monitoring  
    setup_config_monitoring();
    
    
    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize LAROD
    if (!init_larod("/usr/local/packages/gunshot_detector/gunshot_model_real_audio.tflite")) {
        syslog(LOG_ERR, "Failed to initialize LAROD");
        return 1;
    }
    
    // Initialize FFT and mel filters
    if (!init_fft_workspace() || !init_mel_filter_bank()) {
        syslog(LOG_ERR, "Failed to initialize audio processing");
        return 1;
    }
    
    ml_ready = true;
    syslog(LOG_INFO, "Machine learning pipeline ready");
    
    // Initialize PipeWire (following official audiocapture.c pattern)
    pw_init(NULL, NULL);
    
    loop = pw_main_loop_new(NULL);
    context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
    core = pw_context_connect(context, NULL, 0);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    
    pw_registry_add_listener(registry, &registry_listener, &registry_events, NULL);
    
    syslog(LOG_INFO, "PipeWire initialized - discovering camera audio devices...");
    
    // Run main loop
    pw_main_loop_run(loop);
    
    syslog(LOG_INFO, "Shutting down gunshot detector...");
    
    
    // Cleanup
    if (registry) pw_proxy_destroy((struct pw_proxy*)registry);
    if (core) pw_core_disconnect(core);
    if (context) pw_context_destroy(context);
    if (loop) pw_main_loop_destroy(loop);
    
    pw_deinit();
    
    // Cleanup curl
    curl_global_cleanup();
    
    syslog(LOG_INFO, "Gunshot detector stopped");
    closelog();
    
    return 0;
}