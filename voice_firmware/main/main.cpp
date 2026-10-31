#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "freertos/idf_additions.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "flite.h"
extern "C" {
#include "flite_g2p.h"
#include "cst_synth.h"
cst_voice *register_cmu_us_kal(const char *voxdir);
cst_utterance *flite_tts_apply_synth_method(
    cst_utterance *utterance, const cst_synth_module modules[]);
cst_utterance *flite_tts_default_lexical_insertion(cst_utterance *utterance);
cst_utterance *flite_tts_default_textanalysis(cst_utterance *utterance);
cst_utterance *flite_tts_default_tokenization(cst_utterance *utterance);
cst_utterance *flite_tts_utt_init(cst_utterance *utterance, cst_voice *voice);
}
#include "model_path.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#if __has_include("../../include/wifi_secrets.h")
#include "../../include/wifi_secrets.h"
#else
#define CHATBOT_WIFI_SSID ""
#define CHATBOT_WIFI_PASSWORD ""
#endif

extern "C" void app_main(void);

namespace {

constexpr char kTag[] = "chatbot_voice";
constexpr char kFaqPath[] = "/spiffs/faq.txt";
constexpr uint16_t kAudioStreamPort = 3333;
constexpr size_t kMaximumFaqEntries = 192;
constexpr size_t kMaximumWavBytes = 640 * 1024;
constexpr size_t kMaximumTtsTextBytes = 1023;
constexpr size_t kAudioTransferChunkBytes = 1024;
constexpr uint32_t kExpectedSampleRate = 16000;
constexpr TickType_t kConversationTimeout = pdMS_TO_TICKS(15000);

struct FaqEntry {
    std::string id;
    std::string question;
    std::string answer;
};

struct WavPcm {
    size_t offset = 0;
    size_t size = 0;
};

std::vector<FaqEntry> g_faq;
srmodel_list_t *g_models = nullptr;
const esp_mn_iface_t *g_multinet = nullptr;
model_iface_data_t *g_model_data = nullptr;
const esp_afe_sr_iface_t *g_afe_handle = nullptr;
esp_afe_sr_data_t *g_afe_data = nullptr;
SemaphoreHandle_t g_model_mutex = nullptr;
SemaphoreHandle_t g_tts_mutex = nullptr;
cst_voice *g_tts_voice = nullptr;
int g_afe_feed_samples = 0;
int g_afe_feed_channels = 0;
int g_multinet_chunk_samples = 0;
int16_t *g_multinet_frame = nullptr;
std::vector<int16_t> g_pending_samples;
bool g_ready = false;

bool parseFaqLine(char *line, FaqEntry &entry)
{
    char *first = std::strchr(line, '|');
    if (first == nullptr) {
        return false;
    }
    *first = '\0';
    char *second = std::strchr(first + 1, '|');
    if (second == nullptr) {
        return false;
    }
    *second = '\0';
    char *third = std::strchr(second + 1, '|');
    if (third == nullptr || std::strchr(third + 1, '|') != nullptr) {
        return false;
    }
    *third = '\0';
    if (line[0] == '\0' || first[1] == '\0' || second[1] == '\0') {
        return false;
    }

    entry.id = line;
    entry.question = first + 1;
    entry.answer = second + 1;
    return true;
}

bool hasVariantSuffix(const std::string &id, const char *suffix)
{
    const size_t suffix_length = std::strlen(suffix);
    return id.size() > suffix_length &&
           id.compare(id.size() - suffix_length, suffix_length, suffix) == 0;
}

bool hasNumberedVariantSuffix(const std::string &id)
{
    const size_t separator = id.rfind('_');
    return separator != std::string::npos && id.size() - separator == 3 &&
           id[separator + 1] >= '0' && id[separator + 1] <= '9' &&
           id[separator + 2] >= '0' && id[separator + 2] <= '9';
}

bool isPrimaryVoicePhrase(const std::string &id)
{
    if (id == "time") {
        return false;
    }
    if (hasNumberedVariantSuffix(id)) {
        return hasVariantSuffix(id, "_01") && id != "affordance_01";
    }
    return true;
}

bool isExtraVoicePhrase(const std::string &id)
{
    constexpr std::array<const char *, 19> kAliasTopics = {
        "hci",           "hmi",             "user_interface",
        "user_experience", "interaction_design", "design_thinking",
        "usability",     "accessibility",   "wcag",
        "prototype",     "usability_testing", "user_research",
        "user_interview", "persona",         "journey_map",
        "wireframe",     "voice_interface", "microcontroller",
        "wake_word"};

    if (!hasVariantSuffix(id, "_02") && !hasVariantSuffix(id, "_03") &&
        !hasVariantSuffix(id, "_10")) {
        return false;
    }
    for (const char *topic : kAliasTopics) {
        const std::string prefix = std::string(topic) + "_";
        if (id.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

bool loadFaq()
{
    FILE *file = std::fopen(kFaqPath, "r");
    if (file == nullptr) {
        ESP_LOGE(kTag, "Could not open %s; build and flash the FAQ image.", kFaqPath);
        return false;
    }

    char line[2048];
    size_t line_number = 0;
    size_t skipped = 0;
    std::vector<FaqEntry> extra_voice_phrases;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        ++line_number;
        size_t length = std::strlen(line);
        if (length > 0 && line[length - 1] == '\n') {
            line[--length] = '\0';
        }
        if (length > 0 && line[length - 1] == '\r') {
            line[--length] = '\0';
        }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
            ESP_LOGW(kTag, "Skipping overlong FAQ line %u", (unsigned)line_number);
            ++skipped;
            continue;
        }
        FaqEntry entry;
        if (!parseFaqLine(line, entry)) {
            ESP_LOGW(kTag, "Skipping malformed FAQ line %u",
                     static_cast<unsigned>(line_number));
            ++skipped;
            continue;
        }
        const bool extra_voice_phrase = isExtraVoicePhrase(entry.id);
        if (!extra_voice_phrase && !isPrimaryVoicePhrase(entry.id)) {
            continue;
        }
        if (entry.question.size() > ESP_MN_MAX_PHRASE_LEN) {
            ESP_LOGW(kTag, "Skipping overlong voice phrase on FAQ line %u",
                     static_cast<unsigned>(line_number));
            ++skipped;
            continue;
        }
        if (extra_voice_phrase) {
            extra_voice_phrases.push_back(std::move(entry));
            continue;
        }
        if (g_faq.size() < kMaximumFaqEntries) {
            g_faq.push_back(std::move(entry));
        }
    }
    std::fclose(file);

    for (FaqEntry &entry : extra_voice_phrases) {
        if (g_faq.size() == kMaximumFaqEntries) {
            break;
        }
        g_faq.push_back(std::move(entry));
    }

    if (g_faq.empty()) {
        ESP_LOGE(kTag, "No usable FAQ questions were loaded.");
        return false;
    }
    ESP_LOGI(kTag, "Loaded %u voice phrases (%u entries skipped).",
             (unsigned)g_faq.size(), (unsigned)skipped);
    return true;
}

uint16_t readLe16(const uint8_t *bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readLe32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

bool findPcmData(const uint8_t *wav, size_t length, WavPcm &pcm)
{
    if (length < 12 || std::memcmp(wav, "RIFF", 4) != 0 ||
        std::memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool valid_format = false;
    bool found_data = false;
    size_t offset = 12;
    while (offset + 8 <= length) {
        const uint8_t *chunk = wav + offset;
        const uint32_t chunk_size = readLe32(chunk + 4);
        const size_t payload = offset + 8;
        if (chunk_size > length - payload) {
            return false;
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                return false;
            }
            valid_format = readLe16(wav + payload) == 1 &&
                           readLe16(wav + payload + 2) == 1 &&
                           readLe32(wav + payload + 4) == kExpectedSampleRate &&
                           readLe16(wav + payload + 14) == 16;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            pcm.offset = payload;
            pcm.size = chunk_size;
            found_data = true;
        }

        const size_t padded_size = static_cast<size_t>(chunk_size) + (chunk_size & 1);
        if (padded_size > length - payload) {
            return false;
        }
        offset = payload + padded_size;
    }

    return valid_format && found_data && pcm.size >= 2 && (pcm.size % 2) == 0;
}

bool initializeSpeechRecognition()
{
    g_models = esp_srmodel_init("model");
    if (g_models == nullptr) {
        ESP_LOGE(kTag, "Speech models not found in the model partition.");
        return false;
    }
    char *model_name = esp_srmodel_filter(g_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (model_name == nullptr) {
        ESP_LOGE(kTag, "English MultiNet model is not enabled.");
        return false;
    }
    g_multinet = esp_mn_handle_from_name(model_name);
    if (g_multinet == nullptr) {
        ESP_LOGE(kTag, "Could not load MultiNet interface %s.", model_name);
        return false;
    }
    g_model_data = g_multinet->create(model_name, 6000);
    if (g_model_data == nullptr) {
        ESP_LOGE(kTag, "Could not initialize MultiNet model %s.", model_name);
        return false;
    }
    if (esp_mn_commands_alloc(g_multinet, g_model_data) != ESP_OK) {
        ESP_LOGE(kTag, "Could not allocate the MultiNet command list.");
        return false;
    }

    for (size_t index = 0; index < g_faq.size(); ++index) {
        char *phonemes = flite_g2p(g_faq[index].question.c_str(), 1);
        if (phonemes == nullptr) {
            ESP_LOGW(kTag, "Could not generate phonemes for FAQ phrase: %s",
                     g_faq[index].question.c_str());
            continue;
        }
        const esp_err_t error = esp_mn_commands_phoneme_add(
            static_cast<int>(index), g_faq[index].question.c_str(), phonemes);
        std::free(phonemes);
        if (error != ESP_OK) {
            ESP_LOGW(kTag, "Could not register FAQ phrase %u: %s",
                     (unsigned)index, g_faq[index].question.c_str());
        }
    }
    esp_mn_error_t *command_errors = esp_mn_commands_update();
    if (command_errors != nullptr) {
        ESP_LOGW(kTag, "MultiNet rejected %d FAQ phrase(s).",
                 command_errors->num);
    }

    char *wake_model = esp_srmodel_filter(g_models, ESP_WN_PREFIX, nullptr);
    if (wake_model == nullptr) {
        ESP_LOGE(kTag, "The configured WakeNet model was not found.");
        return false;
    }
    afe_config_t *afe_config =
        afe_config_init("M", g_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == nullptr) {
        ESP_LOGE(kTag, "Could not create the speech front-end configuration.");
        return false;
    }
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = wake_model;
    afe_config->vad_init = true;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    g_afe_handle = esp_afe_handle_from_config(afe_config);
    if (g_afe_handle == nullptr) {
        afe_config_free(afe_config);
        ESP_LOGE(kTag, "Could not load the ESP-SR audio front-end.");
        return false;
    }
    g_afe_data = g_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (g_afe_data == nullptr) {
        ESP_LOGE(kTag, "Could not initialize the ESP-SR audio front-end.");
        return false;
    }
    g_afe_feed_samples = g_afe_handle->get_feed_chunksize(g_afe_data);
    g_afe_feed_channels = g_afe_handle->get_feed_channel_num(g_afe_data);
    g_multinet_chunk_samples =
        g_multinet->get_samp_chunksize(g_model_data);
    if (g_afe_feed_samples <= 0 || g_afe_feed_channels != 1 ||
        g_afe_handle->get_samp_rate(g_afe_data) != kExpectedSampleRate ||
        g_multinet_chunk_samples <= 0) {
        ESP_LOGE(kTag, "ESP-SR returned unsupported audio dimensions.");
        return false;
    }
    g_model_mutex = xSemaphoreCreateMutex();
    g_multinet_frame = static_cast<int16_t *>(heap_caps_malloc(
        static_cast<size_t>(g_multinet_chunk_samples) * sizeof(int16_t),
        MALLOC_CAP_8BIT));
    if (g_model_mutex == nullptr || g_multinet_frame == nullptr) {
        ESP_LOGE(kTag, "Could not allocate streaming recognition resources.");
        return false;
    }
    g_afe_handle->print_pipeline(g_afe_data);
    ESP_LOGI(kTag, "Wake word ready: %s. Stream frames: %d samples.",
             esp_srmodel_get_wake_words(g_models, wake_model),
             g_afe_feed_samples);
    ESP_LOGI(kTag, "English speech recognition ready: %s", model_name);
    return true;
}

bool initializeTts()
{
    if (flite_init() != 0) {
        ESP_LOGE(kTag, "Could not initialize the Flite speech synthesizer.");
        return false;
    }
    g_tts_voice = register_cmu_us_kal(nullptr);
    g_tts_mutex = xSemaphoreCreateMutex();
    if (g_tts_voice == nullptr || g_tts_mutex == nullptr) {
        ESP_LOGE(kTag, "Could not initialize the English Flite voice.");
        return false;
    }
    ESP_LOGI(kTag, "English Flite speech synthesis ready.");
    return true;
}

cst_utterance *synthesizeUtterance(cst_utterance *utterance)
{
    static const cst_synth_module modules[] = {
        {"tokenizer_func", flite_tts_default_tokenization},
        {"textanalysis_func", flite_tts_default_textanalysis},
        {"pos_tagger_func", default_pos_tagger},
        {"phrasing_func", default_phrasing},
        {"lexical_insertion_func", flite_tts_default_lexical_insertion},
        {"pause_insertion_func", default_pause_insertion},
        {"intonation_func", cart_intonation},
        {"postlex_func", nullptr},
        {"duration_model_func", cart_duration},
        {"f0_model_func", nullptr},
        {"wave_synth_func", nullptr},
        {"post_synth_hook_func", nullptr},
        {nullptr, nullptr},
    };
    return flite_tts_apply_synth_method(utterance, modules);
}

cst_wave *synthesizeSpeech(const char *text)
{
    cst_utterance *utterance = new_utterance();
    utt_set_input_text(utterance, text);
    flite_tts_utt_init(utterance, g_tts_voice);
    cst_utterance *synthesized = synthesizeUtterance(utterance);
    if (synthesized == nullptr) {
        delete_utterance(utterance);
        return nullptr;
    }
    utterance = synthesized;

    cst_wave *wave = copy_wave(utt_wave(utterance));
    delete_utterance(utterance);
    return wave;
}

int recognizeQuestion(const uint8_t *wav, const WavPcm &pcm)
{
    const int chunk_samples = g_multinet->get_samp_chunksize(g_model_data);
    if (chunk_samples <= 0) {
        ESP_LOGE(kTag, "MultiNet returned an invalid frame size.");
        return -1;
    }
    auto *frame = static_cast<int16_t *>(heap_caps_malloc(
        static_cast<size_t>(chunk_samples) * sizeof(int16_t), MALLOC_CAP_8BIT));
    if (frame == nullptr) {
        ESP_LOGE(kTag, "Could not allocate the MultiNet input frame.");
        return -1;
    }

    if (xSemaphoreTake(g_model_mutex, portMAX_DELAY) != pdTRUE) {
        heap_caps_free(frame);
        ESP_LOGE(kTag, "Could not lock the speech model.");
        return -1;
    }
    g_multinet->clean(g_model_data);
    const auto *samples = reinterpret_cast<const int16_t *>(wav + pcm.offset);
    const size_t total_samples = pcm.size / sizeof(int16_t);
    int command_id = -1;
    for (size_t offset = 0; offset < total_samples; offset += chunk_samples) {
        const size_t count = std::min(static_cast<size_t>(chunk_samples),
                                      total_samples - offset);
        std::memcpy(frame, samples + offset, count * sizeof(int16_t));
        if (count < static_cast<size_t>(chunk_samples)) {
            std::memset(frame + count, 0,
                        (static_cast<size_t>(chunk_samples) - count) *
                            sizeof(int16_t));
        }
        if (g_multinet->detect(g_model_data, frame) == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *results = g_multinet->get_results(g_model_data);
            if (results != nullptr && results->num > 0) {
                command_id = results->command_id[0];
                ESP_LOGI(kTag, "Recognized command %d: %s (%.2f)",
                         command_id, results->string, results->prob[0]);
            }
            break;
        }
    }
    xSemaphoreGive(g_model_mutex);
    heap_caps_free(frame);
    return command_id;
}

esp_err_t healthHandler(httpd_req_t *request)
{
    const char *body = g_ready ? "ESP32 voice FAQ assistant is ready.\n"
                               : "ESP32 voice assistant is not ready.\n";
    httpd_resp_set_status(request, g_ready ? "200 OK" : "503 Service Unavailable");
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

void writeLe16(uint8_t *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = static_cast<uint8_t>(value);
    buffer[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t *buffer, size_t offset, uint32_t value)
{
    buffer[offset] = static_cast<uint8_t>(value);
    buffer[offset + 1] = static_cast<uint8_t>(value >> 8);
    buffer[offset + 2] = static_cast<uint8_t>(value >> 16);
    buffer[offset + 3] = static_cast<uint8_t>(value >> 24);
}

esp_err_t speakHandler(httpd_req_t *request)
{
    if (!g_ready || g_tts_voice == nullptr) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        httpd_resp_sendstr(request, "English speech synthesis is not ready.");
        return ESP_FAIL;
    }
    if (request->content_len == 0 ||
        request->content_len > kMaximumTtsTextBytes) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "Text must be between 1 and 1023 bytes.");
        return ESP_FAIL;
    }

    char text[kMaximumTtsTextBytes + 1] = {};
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(
            request, text + received, request->content_len - received);
        if (count <= 0) {
            httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT,
                                "The text upload was incomplete.");
            return ESP_FAIL;
        }
        received += static_cast<size_t>(count);
    }
    text[received] = '\0';

    if (xSemaphoreTake(g_tts_mutex, portMAX_DELAY) != pdTRUE) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not lock the speech synthesizer.");
        return ESP_FAIL;
    }
    cst_wave *wave = synthesizeSpeech(text);
    xSemaphoreGive(g_tts_mutex);
    if (wave == nullptr || wave->samples == nullptr || wave->num_samples <= 0 ||
        wave->num_channels != 1 || wave->sample_rate <= 0) {
        delete_wave(wave);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "The speech synthesizer produced no audio.");
        return ESP_FAIL;
    }

    const uint64_t audio_bytes =
        static_cast<uint64_t>(wave->num_samples) * sizeof(int16_t);
    if (audio_bytes > UINT32_MAX - 36) {
        delete_wave(wave);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Synthesized audio is too large.");
        return ESP_FAIL;
    }
    auto *audio_chunk = static_cast<uint8_t *>(heap_caps_malloc(
        kAudioTransferChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (audio_chunk == nullptr) {
        delete_wave(wave);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not allocate an audio transfer buffer.");
        return ESP_FAIL;
    }
    uint8_t wav_header[44] = {};
    std::memcpy(wav_header, "RIFF", 4);
    writeLe32(wav_header, 4, static_cast<uint32_t>(36 + audio_bytes));
    std::memcpy(wav_header + 8, "WAVEfmt ", 8);
    writeLe32(wav_header, 16, 16);
    writeLe16(wav_header, 20, 1);
    writeLe16(wav_header, 22, 1);
    writeLe32(wav_header, 24, static_cast<uint32_t>(wave->sample_rate));
    writeLe32(wav_header, 28,
              static_cast<uint32_t>(wave->sample_rate * sizeof(int16_t)));
    writeLe16(wav_header, 32, sizeof(int16_t));
    writeLe16(wav_header, 34, 16);
    std::memcpy(wav_header + 36, "data", 4);
    writeLe32(wav_header, 40, static_cast<uint32_t>(audio_bytes));

    httpd_resp_set_type(request, "audio/wav");
    esp_err_t result = httpd_resp_send_chunk(
        request, reinterpret_cast<const char *>(wav_header), sizeof(wav_header));
    size_t offset = 0;
    ESP_LOGI(kTag, "Sending HTTP TTS response: %u total bytes, %u PCM bytes.",
             static_cast<unsigned>(44 + audio_bytes),
             static_cast<unsigned>(audio_bytes));
    while (result == ESP_OK && offset < audio_bytes) {
        const size_t count =
            std::min<size_t>(kAudioTransferChunkBytes, audio_bytes - offset);
        std::memcpy(audio_chunk,
                    reinterpret_cast<const uint8_t *>(wave->samples) + offset,
                    count);
        result = httpd_resp_send_chunk(
            request,
            reinterpret_cast<const char *>(audio_chunk), count);
        offset += count;
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, nullptr, 0);
    }
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "HTTP TTS transfer failed at %u/%u PCM bytes: %s.",
                 static_cast<unsigned>(offset),
                 static_cast<unsigned>(audio_bytes), esp_err_to_name(result));
    } else {
        ESP_LOGI(kTag, "HTTP TTS response sent successfully.");
    }
    heap_caps_free(audio_chunk);
    delete_wave(wave);
    return result;
}

esp_err_t voiceHandler(httpd_req_t *request)
{
    if (!g_ready) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_send(request, "Voice assistant is not ready.",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    if (request->content_len < 44 || request->content_len > kMaximumWavBytes) {
        httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE,
                            "WAV must be between 44 bytes and 640 KiB.");
        return ESP_FAIL;
    }

    auto *wav = static_cast<uint8_t *>(heap_caps_malloc(
        request->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (wav == nullptr) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Not enough PSRAM to receive this recording.");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(
            request, reinterpret_cast<char *>(wav + received),
            request->content_len - received);
        if (count <= 0) {
            heap_caps_free(wav);
            httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT,
                                "The WAV upload was incomplete.");
            return ESP_FAIL;
        }
        received += static_cast<size_t>(count);
    }

    WavPcm pcm;
    if (!findPcmData(wav, received, pcm)) {
        heap_caps_free(wav);
        httpd_resp_set_status(request, "415 Unsupported Media Type");
        httpd_resp_send(request, "Expected PCM WAV: mono, 16 kHz, 16-bit.",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    const int command_id = recognizeQuestion(wav, pcm);
    heap_caps_free(wav);
    if (command_id < 0 || static_cast<size_t>(command_id) >= g_faq.size()) {
        httpd_resp_set_status(request, "200 OK");
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        return httpd_resp_send(request,
                               "I could not recognize a configured FAQ question.\n",
                               HTTPD_RESP_USE_STRLEN);
    }

    const FaqEntry &entry = g_faq[command_id];
    std::string response = "Recognized: " + entry.question +
                           "\nAnswer: " + entry.answer + "\n";
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, response.c_str(), response.size());
}

bool receiveAll(int socket_fd, uint8_t *buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        const ssize_t count =
            recv(socket_fd, buffer + received, length - received, 0);
        if (count <= 0) {
            return false;
        }
        received += static_cast<size_t>(count);
    }
    return true;
}

bool sendBytes(int socket_fd, const uint8_t *buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        const ssize_t count =
            send(socket_fd, buffer + sent, length - sent, 0);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<size_t>(count);
    }
    return true;
}

bool sendText(int socket_fd, const std::string &text)
{
    return sendBytes(socket_fd,
                     reinterpret_cast<const uint8_t *>(text.data()),
                     text.size());
}

bool sendStreamSpeech(int socket_fd, const std::string &text)
{
    if (!sendText(socket_fd, "EVENT SPEAKING\n")) {
        return false;
    }
    if (text.empty() || text.size() > kMaximumTtsTextBytes) {
        return sendText(socket_fd,
                        "EVENT TTS_ERROR Answer is too long to synthesize.\n");
    }
    if (xSemaphoreTake(g_tts_mutex, portMAX_DELAY) != pdTRUE) {
        return sendText(socket_fd,
                        "EVENT TTS_ERROR Could not lock the synthesizer.\n");
    }
    cst_wave *wave = synthesizeSpeech(text.c_str());
    xSemaphoreGive(g_tts_mutex);
    if (wave == nullptr || wave->samples == nullptr || wave->num_samples <= 0 ||
        wave->num_channels != 1 || wave->sample_rate <= 0) {
        delete_wave(wave);
        ESP_LOGE(kTag, "Could not synthesize a streaming FAQ answer.");
        return sendText(socket_fd,
                        "EVENT TTS_ERROR Speech synthesis failed.\n");
    }

    const uint64_t audio_bytes =
        static_cast<uint64_t>(wave->num_samples) * sizeof(int16_t);
    if (audio_bytes > UINT32_MAX - 44) {
        delete_wave(wave);
        return sendText(socket_fd,
                        "EVENT TTS_ERROR Synthesized audio is too large.\n");
    }

    uint8_t wav_header[44] = {};
    std::memcpy(wav_header, "RIFF", 4);
    writeLe32(wav_header, 4, static_cast<uint32_t>(36 + audio_bytes));
    std::memcpy(wav_header + 8, "WAVEfmt ", 8);
    writeLe32(wav_header, 16, 16);
    writeLe16(wav_header, 20, 1);
    writeLe16(wav_header, 22, 1);
    writeLe32(wav_header, 24, static_cast<uint32_t>(wave->sample_rate));
    writeLe32(wav_header, 28,
              static_cast<uint32_t>(wave->sample_rate * sizeof(int16_t)));
    writeLe16(wav_header, 32, sizeof(int16_t));
    writeLe16(wav_header, 34, 16);
    std::memcpy(wav_header + 36, "data", 4);
    writeLe32(wav_header, 40, static_cast<uint32_t>(audio_bytes));

    char audio_line[32];
    std::snprintf(audio_line, sizeof(audio_line), "AUDIO %u\n",
                  static_cast<unsigned>(44 + audio_bytes));
    auto *audio_chunk = static_cast<uint8_t *>(heap_caps_malloc(
        kAudioTransferChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (audio_chunk == nullptr) {
        delete_wave(wave);
        return sendText(socket_fd,
                        "EVENT TTS_ERROR Could not allocate an audio transfer buffer.\n");
    }
    ESP_LOGI(kTag, "Sending TTS frame: %u total bytes, %u PCM bytes.",
             static_cast<unsigned>(44 + audio_bytes),
             static_cast<unsigned>(audio_bytes));
    if (!sendText(socket_fd, audio_line)) {
        ESP_LOGE(kTag, "Could not send the TTS frame header (errno %d).", errno);
        heap_caps_free(audio_chunk);
        delete_wave(wave);
        return false;
    }
    if (!sendBytes(socket_fd, wav_header, sizeof(wav_header))) {
        ESP_LOGE(kTag, "Could not send the TTS WAV header (errno %d).", errno);
        heap_caps_free(audio_chunk);
        delete_wave(wave);
        return false;
    }
    ESP_LOGI(kTag, "TTS WAV header sent; streaming PCM audio from %s RAM.",
             esp_ptr_external_ram(wave->samples) ? "external" : "internal");
    size_t offset = 0;
    while (offset < audio_bytes) {
        const size_t count =
            std::min<size_t>(kAudioTransferChunkBytes, audio_bytes - offset);
        std::memcpy(audio_chunk,
                    reinterpret_cast<const uint8_t *>(wave->samples) + offset,
                    count);
        if (!sendBytes(
                socket_fd, audio_chunk, count)) {
            ESP_LOGE(kTag, "TTS PCM send failed at %u/%u bytes (errno %d).",
                     static_cast<unsigned>(offset),
                     static_cast<unsigned>(audio_bytes), errno);
            heap_caps_free(audio_chunk);
            delete_wave(wave);
            return false;
        }
        offset += count;
    }
    heap_caps_free(audio_chunk);
    delete_wave(wave);
    ESP_LOGI(kTag, "TTS audio frame sent successfully.");
    return true;
}

void processAudioStream(int client_socket)
{
    const size_t frame_bytes =
        static_cast<size_t>(g_afe_feed_samples) *
        static_cast<size_t>(g_afe_feed_channels) * sizeof(int16_t);
    auto *input_frame = static_cast<int16_t *>(heap_caps_malloc(
        frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (input_frame == nullptr) {
        ESP_LOGE(kTag, "Could not allocate the microphone stream frame.");
        sendText(client_socket, "ERROR Could not allocate audio frame.\n");
        return;
    }

    char greeting[96];
    std::snprintf(greeting, sizeof(greeting), "READY %u %d %d Hi ESP\n",
                  static_cast<unsigned>(kExpectedSampleRate),
                  g_afe_feed_samples, g_afe_feed_channels);
    if (!sendText(client_socket, greeting) ||
        !sendText(client_socket, "EVENT WAKE_REQUIRED\n")) {
        heap_caps_free(input_frame);
        return;
    }

    bool listening = false;
    bool wait_for_silence = false;
    TickType_t last_speech = xTaskGetTickCount();
    g_pending_samples.clear();
    while (receiveAll(client_socket, reinterpret_cast<uint8_t *>(input_frame),
                      frame_bytes)) {
        if (g_afe_handle->feed(g_afe_data, input_frame) <= 0) {
            ESP_LOGW(kTag, "Audio front-end rejected a microphone frame.");
            continue;
        }
        afe_fetch_result_t *result = g_afe_handle->fetch_with_delay(
            g_afe_data, pdMS_TO_TICKS(40));
        if (result == nullptr || result->data == nullptr ||
            result->data_size <= 0) {
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        if (!listening) {
            if (result->wakeup_state == WAKENET_DETECTED) {
                listening = true;
                wait_for_silence = true;
                last_speech = now;
                if (xSemaphoreTake(g_model_mutex, portMAX_DELAY) == pdTRUE) {
                    g_multinet->clean(g_model_data);
                    xSemaphoreGive(g_model_mutex);
                }
                g_pending_samples.clear();
                ESP_LOGI(kTag, "Wake word detected; conversation listening started.");
                if (!sendText(client_socket, "EVENT WAKE\n")) {
                    break;
                }
            }
            continue;
        }

        if (result->vad_state == VAD_SPEECH) {
            last_speech = now;
        }
        if (wait_for_silence) {
            if (result->vad_state == VAD_SILENCE) {
                wait_for_silence = false;
                g_pending_samples.clear();
                if (xSemaphoreTake(g_model_mutex, portMAX_DELAY) == pdTRUE) {
                    g_multinet->clean(g_model_data);
                    xSemaphoreGive(g_model_mutex);
                }
                if (!sendText(client_socket, "EVENT LISTENING\n")) {
                    break;
                }
            }
            continue;
        }

        if (result->vad_state == VAD_SILENCE &&
            now - last_speech >= kConversationTimeout) {
            listening = false;
            g_pending_samples.clear();
            if (xSemaphoreTake(g_model_mutex, portMAX_DELAY) == pdTRUE) {
                g_multinet->clean(g_model_data);
                xSemaphoreGive(g_model_mutex);
            }
            if (!sendText(client_socket, "EVENT WAKE_REQUIRED\n")) {
                break;
            }
            continue;
        }

        const size_t fetched_samples =
            static_cast<size_t>(result->data_size) / sizeof(int16_t);
        g_pending_samples.insert(g_pending_samples.end(), result->data,
                                 result->data + fetched_samples);
        while (g_pending_samples.size() >=
               static_cast<size_t>(g_multinet_chunk_samples)) {
            std::copy_n(g_pending_samples.begin(), g_multinet_chunk_samples,
                        g_multinet_frame);
            g_pending_samples.erase(
                g_pending_samples.begin(),
                g_pending_samples.begin() + g_multinet_chunk_samples);

            int command_id = -1;
            float confidence = 0.0F;
            bool command_timed_out = false;
            if (xSemaphoreTake(g_model_mutex, portMAX_DELAY) != pdTRUE) {
                ESP_LOGE(kTag, "Could not lock MultiNet during streaming.");
                break;
            }
            const esp_mn_state_t state =
                g_multinet->detect(g_model_data, g_multinet_frame);
            if (state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *results =
                    g_multinet->get_results(g_model_data);
                if (results != nullptr && results->num > 0) {
                    command_id = results->command_id[0];
                    confidence = results->prob[0];
                }
                g_multinet->clean(g_model_data);
            } else if (state == ESP_MN_STATE_TIMEOUT) {
                g_multinet->clean(g_model_data);
                command_timed_out = true;
            }
            xSemaphoreGive(g_model_mutex);

            if (command_timed_out) {
                g_pending_samples.clear();
                continue;
            }
            if (command_id < 0) {
                continue;
            }
            if (static_cast<size_t>(command_id) >= g_faq.size()) {
                ESP_LOGW(kTag, "MultiNet returned invalid command ID %d.",
                         command_id);
                continue;
            }

            const FaqEntry &entry = g_faq[command_id];
            std::string response = "Recognized: " + entry.question +
                                   "\nAnswer: " + entry.answer + "\n";
            ESP_LOGI(kTag, "Recognized FAQ entry %s (confidence %.2f).",
                     entry.id.c_str(), confidence);
            if (!sendText(client_socket, response)) {
                break;
            }
            if (!sendStreamSpeech(client_socket, entry.answer)) {
                break;
            }
        }
    }

    g_pending_samples.clear();
    heap_caps_free(input_frame);
}

void audioStreamServerTask(void *)
{
    const int server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_socket < 0) {
        ESP_LOGE(kTag, "Could not create the audio stream socket.");
        vTaskDelete(nullptr);
        return;
    }

    int reuse_address = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
               sizeof(reuse_address));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(kAudioStreamPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_socket, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0 ||
        listen(server_socket, 1) != 0) {
        ESP_LOGE(kTag, "Could not listen for audio on TCP port %u.",
                 kAudioStreamPort);
        close(server_socket);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag, "Continuous audio stream listening on TCP port %u.",
             kAudioStreamPort);
    while (true) {
        sockaddr_in client_address = {};
        socklen_t address_length = sizeof(client_address);
        const int client_socket =
            accept(server_socket, reinterpret_cast<sockaddr *>(&client_address),
                   &address_length);
        if (client_socket < 0) {
            ESP_LOGW(kTag, "Audio stream accept failed.");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        timeval send_timeout = {};
        send_timeout.tv_sec = 5;
        if (setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                       sizeof(send_timeout)) != 0) {
            ESP_LOGW(kTag, "Could not set audio socket send timeout (errno %d).",
                     errno);
        }
        ESP_LOGI(kTag, "Mac audio stream connected.");
        processAudioStream(client_socket);
        shutdown(client_socket, SHUT_RDWR);
        close(client_socket);
        ESP_LOGI(kTag, "Mac audio stream disconnected.");
    }
}

bool startAudioStreamServer()
{
    const BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        audioStreamServerTask, "audio_stream", 8192, nullptr, 5, nullptr, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS) {
        ESP_LOGE(kTag,
                 "Could not start the audio stream server task "
                 "(internal free: %u bytes, largest block: %u bytes).",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                   MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return false;
    }
    return true;
}

void wifiEventHandler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(kTag, "Wi-Fi disconnected; reconnecting.");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(kTag, "Wi-Fi connected. ESP32 IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}

bool initializeWifi()
{
    if (std::strlen(CHATBOT_WIFI_SSID) == 0 ||
        std::strlen(CHATBOT_WIFI_PASSWORD) == 0) {
        ESP_LOGE(kTag, "Wi-Fi credentials are missing from include/wifi_secrets.h.");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifiEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifiEventHandler, nullptr));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid),
                 CHATBOT_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password),
                 CHATBOT_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    return true;
}

bool initializeFaqFilesystem()
{
    esp_vfs_spiffs_conf_t config = {};
    config.base_path = "/spiffs";
    config.partition_label = "spiffs";
    config.max_files = 4;
    config.format_if_mount_failed = false;
    const esp_err_t error = esp_vfs_spiffs_register(&config);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not mount FAQ partition: %s",
                 esp_err_to_name(error));
        return false;
    }
    return loadFaq();
}

bool startHttpServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(kTag, "Could not start HTTP server.");
        return false;
    }

    const httpd_uri_t health = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = healthHandler,
        .user_ctx = nullptr,
    };
    const httpd_uri_t voice = {
        .uri = "/voice",
        .method = HTTP_POST,
        .handler = voiceHandler,
        .user_ctx = nullptr,
    };
    const httpd_uri_t speak = {
        .uri = "/speak",
        .method = HTTP_POST,
        .handler = speakHandler,
        .user_ctx = nullptr,
    };
    if (httpd_register_uri_handler(server, &health) != ESP_OK ||
        httpd_register_uri_handler(server, &voice) != ESP_OK ||
        httpd_register_uri_handler(server, &speak) != ESP_OK) {
        ESP_LOGE(kTag, "Could not register HTTP endpoints.");
        httpd_stop(server);
        return false;
    }
    ESP_LOGI(kTag, "Voice API listening on /voice and /speak");
    return true;
}

} // namespace

extern "C" void app_main(void)
{
    esp_err_t nvs_error = nvs_flash_init();
    if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_error);

    const bool filesystem_ready = initializeFaqFilesystem();
    if (!filesystem_ready) {
        return;
    }
    if (!initializeSpeechRecognition()) {
        return;
    }
    if (!initializeTts()) {
        return;
    }
    g_ready = true;
    if (!initializeWifi()) {
        g_ready = false;
        return;
    }
    if (!startHttpServer()) {
        g_ready = false;
        return;
    }
    if (!startAudioStreamServer()) {
        g_ready = false;
    }
}
