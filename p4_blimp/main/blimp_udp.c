#include "blimp_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>
#include "blimp_control.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mpu6050.h"
#include "sdkconfig.h"

#define VIDEO_MTU 1400
#define VIDEO_HEADER 28
#define VIDEO_PAYLOAD (VIDEO_MTU - VIDEO_HEADER)

static const char *TAG = "blimp_udp";
static int s_socket = -1;
static struct sockaddr_in s_base;
static bool s_have_base;
static SemaphoreHandle_t s_peer_lock;
static uint32_t s_video_seq;

static void remember_base(const struct sockaddr_in *source)
{
    xSemaphoreTake(s_peer_lock, portMAX_DELAY);
    s_base = *source;
    s_have_base = true;
    xSemaphoreGive(s_peer_lock);
}

static bool get_base(struct sockaddr_in *base, uint16_t port)
{
    xSemaphoreTake(s_peer_lock, portMAX_DELAY);
    bool have = s_have_base;
    if (have) { *base = s_base; base->sin_port = htons(port); }
    xSemaphoreGive(s_peer_lock);
    return have;
}

static void send_json_to(const struct sockaddr_in *address, const char *json)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "%s\n", json);
    if (n > 0 && n < sizeof(line)) sendto(s_socket, line, n, 0,
                                          (const struct sockaddr *)address, sizeof(*address));
}

static double number_or(cJSON *root, const char *name, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static void command_task(void *arg)
{
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET, .sin_port = htons(CONFIG_BLIMP_COMMAND_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_socket < 0 || bind(s_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "UDP bind failed: errno %d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "NDJSON commands listening on UDP/%d", CONFIG_BLIMP_COMMAND_PORT);
    while (true) {
        char packet[512];
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        int n = recvfrom(s_socket, packet, sizeof(packet) - 1, 0,
                         (struct sockaddr *)&source, &source_len);
        if (n <= 0) continue;
        packet[n] = 0;
        cJSON *root = cJSON_Parse(packet);
        if (!root) continue;
        cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
        const char *type = cJSON_IsString(type_item) ? type_item->valuestring : "";
        remember_base(&source);
        if (!strcmp(type, "control")) {
            blimp_control_set(number_or(root, "left", 0), number_or(root, "right", 0),
                              number_or(root, "left_tilt", 90), number_or(root, "right_tilt", 90));
        } else if (!strcmp(type, "estop")) {
            blimp_control_estop();
        } else if (!strcmp(type, "clear_estop")) {
            blimp_control_clear_estop();
        } else if (!strcmp(type, "gate_open")) {
            blimp_control_set_gate(true);
        } else if (!strcmp(type, "gate_close")) {
            blimp_control_set_gate(false);
        } else if (!strcmp(type, "ping")) {
            char pong[80];
            snprintf(pong, sizeof(pong), "{\"type\":\"pong\",\"n\":%.0f}",
                     number_or(root, "n", 0));
            send_json_to(&source, pong);
        }
        cJSON_Delete(root);
    }
}

static void telemetry_task(void *arg)
{
    while (true) {
        struct sockaddr_in base;
        if (s_socket >= 0 && get_base(&base, CONFIG_BLIMP_COMMAND_PORT)) {
            blimp_output_t output;
            blimp_attitude_t attitude;
            blimp_control_get(&output);
            mpu6050_get(&attitude);
            char json[400];
            snprintf(json, sizeof(json),
                     "{\"type\":\"telemetry\",\"from\":\"blimp\","
                     "\"estop\":%s,\"left\":%.3f,\"right\":%.3f,"
                     "\"left_tilt\":%.1f,\"right_tilt\":%.1f,"
                     "\"gate\":%.1f,\"gate_open\":%s,"
                     "\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f}",
                     output.estop ? "true" : "false", output.left, output.right,
                     output.left_tilt, output.right_tilt,
                     output.gate, output.gate_open ? "true" : "false",
                     attitude.roll, attitude.pitch, attitude.yaw);
            send_json_to(&base, json);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void blimp_udp_start(void)
{
    s_peer_lock = xSemaphoreCreateMutex();
    xTaskCreate(command_task, "blimp_udp_rx", 6144, NULL, 14, NULL);
    xTaskCreate(telemetry_task, "blimp_telemetry", 4096, NULL, 8, NULL);
}

void blimp_udp_send_jpeg(const uint8_t *jpeg, size_t length)
{
    struct sockaddr_in base;
    if (s_socket < 0 || !get_base(&base, CONFIG_BLIMP_VIDEO_PORT) || !jpeg || !length) return;
    uint16_t count = (length + VIDEO_PAYLOAD - 1) / VIDEO_PAYLOAD;
    if (!count) return;
    uint8_t packet[VIDEO_MTU];
    uint32_t seq = s_video_seq++;
    for (uint16_t index = 0; index < count; ++index) {
        memset(packet, 0, VIDEO_HEADER);
        memcpy(packet, "UCV1", 4);
        memcpy(packet + 4, "blimp", 5);
        uint32_t net_seq = htonl(seq);
        uint16_t net_index = htons(index), net_count = htons(count);
        memcpy(packet + 20, &net_seq, 4);
        memcpy(packet + 24, &net_index, 2);
        memcpy(packet + 26, &net_count, 2);
        size_t offset = index * VIDEO_PAYLOAD;
        size_t payload = length - offset;
        if (payload > VIDEO_PAYLOAD) payload = VIDEO_PAYLOAD;
        memcpy(packet + VIDEO_HEADER, jpeg + offset, payload);
        sendto(s_socket, packet, VIDEO_HEADER + payload, 0,
               (const struct sockaddr *)&base, sizeof(base));
    }
}
