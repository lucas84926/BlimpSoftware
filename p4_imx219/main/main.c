#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "esp_log.h"
#include "imx219.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_video_device.h"
#include "esp_cam_sensor_detect.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "jpeg_enc.h"
#include "blimp_control.h"
#include "blimp_udp.h"
#include "mpu6050.h"

static const char *TAG = "app_main";

// Shared by the IMX219 SCCB driver and the MPU6050.
static i2c_master_bus_handle_t s_sensor_bus;

#define I2C_MASTER_SCL_IO           8
#define I2C_MASTER_SDA_IO           7
#define I2C_MASTER_NUM              0
#define I2C_MASTER_FREQ_HZ          100000
#define XCLK_PIN                    20
#define XCLK_FREQ_HZ                24000000

// Sensor Input & Output Settings
#define IMG_WIDTH  1536
#define IMG_HEIGHT 1232

#define OUT_WIDTH  480
#define OUT_HEIGHT 384

// Stream definition
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Queue to pass JPEG buffers from Capture Task to HTTP Task
typedef struct {
    uint8_t *data;
    size_t len;
} jpeg_frame_t;

static QueueHandle_t s_jpeg_queue = NULL;

// Helper to access sensor device from controls
extern esp_cam_sensor_device_t *s_sensor_dev; 
static esp_cam_sensor_device_t *s_sensor = NULL;

static int s_digital_gain = 255; // Max
static int s_wb_red = 140;       // ~1.1x Red Gain (Base 128)
static int s_wb_blue = 160;      // ~1.25x Blue Gain (Base 128)

static int *s_x_lut = NULL;
static int *s_y_lut = NULL;

// XCLK Generation
static void enable_xclk(void)
{
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_1_BIT, 
        .freq_hz = XCLK_FREQ_HZ,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&ledc_timer) != ESP_OK) ESP_LOGE(TAG, "Failed to config LEDC timer");

    ledc_channel_config_t ledc_channel = {
        .channel    = LEDC_CHANNEL_0,
        .duty       = 1, 
        .gpio_num   = XCLK_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint     = 0,
        .timer_sel  = LEDC_TIMER_0,
    };
    if (ledc_channel_config(&ledc_channel) != ESP_OK) ESP_LOGE(TAG, "Failed to config LEDC channel");
    ESP_LOGI(TAG, "Enabled XCLK on GPIO %d at %d Hz", XCLK_PIN, XCLK_FREQ_HZ);
}

// Wi-Fi Connection
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    // Streaming and teleop favor stable latency over station power saving.
    // esp_wifi_set_ps(WIFI_PS_NONE);
}

// HTTP Stream Handler
esp_err_t stream_handler(httpd_req_t *req) {
    esp_err_t res = ESP_OK;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK) return res;

    ESP_LOGI(TAG, "Stream client connected");

    jpeg_frame_t frame;
    
    while(true){
        if (xQueueReceive(s_jpeg_queue, &frame, pdMS_TO_TICKS(1000))) {
            // Send Boundary
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
            if(res != ESP_OK) break;

            // Send Header
            size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, frame.len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
            if(res != ESP_OK) break;

            // Send Data
            res = httpd_resp_send_chunk(req, (const char *)frame.data, frame.len);
            free(frame.data);
            
            if(res != ESP_OK) break;
        }
    }
    ESP_LOGI(TAG, "Stream client disconnected");
    return res;
}

// Control Handler (Port 80)
static esp_err_t control_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char val_str[10];
        if (httpd_query_key_value(buf, "gain", val_str, sizeof(val_str)) == ESP_OK) {
            int val = atoi(val_str);
            if (s_sensor) imx219_set_gain(s_sensor, val);
        }
        if (httpd_query_key_value(buf, "exposure", val_str, sizeof(val_str)) == ESP_OK) {
            int val = atoi(val_str);
            if (s_sensor) imx219_set_exposure(s_sensor, val);
        }
        if (httpd_query_key_value(buf, "digital", val_str, sizeof(val_str)) == ESP_OK) {
            s_digital_gain = atoi(val_str);
        }
        if (httpd_query_key_value(buf, "wbr", val_str, sizeof(val_str)) == ESP_OK) {
            s_wb_red = atoi(val_str);
        }
        if (httpd_query_key_value(buf, "wbb", val_str, sizeof(val_str)) == ESP_OK) {
            s_wb_blue = atoi(val_str);
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    const char *html = 
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>"
        "body{margin:0;background:#222;color:#fff;font-family:sans-serif;display:flex;flex-direction:column;align-items:center;}"
        "img{max-width:100%;max-height:60vh;object-fit:contain;background:#000;}"
        ".controls{padding:20px;width:90%;max-width:400px;background:#333;border-radius:10px;margin-top:10px;}"
        "input{width:100%;margin:10px 0;}"
        "label{display:flex;justify-content:space-between;font-size:0.9em;}"
        "</style></head><body>"
        "<img id='stream_img' alt='Loading Stream...'>"
        "<div class='controls'>"
        "<label>Analog Gain <span id='v_gain'>Max</span></label>"
        "<input type='range' min='0' max='232' value='232' onchange='send(\"gain\",this.value)'>"
        "<label>Exposure Time <span id='v_exp'>Max</span></label>"
        "<input type='range' min='1' max='1750' value='300' onchange='send(\"exposure\",this.value)'>"
        "<label>Digital Brightness <span id='v_dig'>255</span></label>"
        "<input type='range' min='0' max='255' value='255' onchange='send(\"digital\",this.value)'>"
        "<hr>"
        "<label>WB Red <span id='v_wbr'>140</span></label>"
        "<input type='range' min='0' max='255' value='140' onchange='send(\"wbr\",this.value)'>"
        "<label>WB Blue <span id='v_wbb'>160</span></label>"
        "<input type='range' min='0' max='255' value='160' onchange='send(\"wbb\",this.value)'>"
        "</div>"
        "<script>"
        "document.getElementById('stream_img').src = window.location.protocol + '//' + window.location.hostname + ':81/stream';"
        "function send(k,v){"
        " document.getElementById('v_'+k.substr(0,3)).innerText=v;"
        " fetch('/control?'+k+'='+v).catch(e=>{console.log(e)});"
        "}"
        "</script>"
        "</body></html>";
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// Start Main Control Server (Port 80)
static httpd_handle_t start_control_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &index_uri);
        httpd_uri_t ctrl_uri  = { .uri = "/control", .method = HTTP_GET, .handler = control_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &ctrl_uri);
    }
    return server;
}

// Start Stream Server (Port 81)
static httpd_handle_t start_stream_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32769;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &stream_uri);
    }
    return server;
}

static void init_demosaic_luts(int width, int height) {
    s_x_lut = malloc(OUT_WIDTH * sizeof(int));
    s_y_lut = malloc(OUT_HEIGHT * sizeof(int));
    
    for (int y = 0; y < OUT_HEIGHT; y++) {
        int src_y = ((y * height) / OUT_HEIGHT) & ~1;
        s_y_lut[y] = src_y * (width * 5 / 4);
    }
    for (int x = 0; x < OUT_WIDTH; x++) {
        int src_x = ((x * width) / OUT_WIDTH) & ~1;
        s_x_lut[x] = (src_x >> 2) * 5 + (src_x & 3);
    }
}

static void demosaic_rggb_to_rgb565(const uint8_t *raw10, uint16_t *rgb) {
    const int raw_stride = IMG_WIDTH * 5 / 4;
    const uint32_t r_gain = (s_digital_gain * s_wb_red) >> 7;
    const uint32_t g_gain = s_digital_gain;
    const uint32_t b_gain = (s_digital_gain * s_wb_blue) >> 7;
    for (int y = 0; y < OUT_HEIGHT; y++) {
        const uint8_t *row0 = raw10 + s_y_lut[y];
        const uint8_t *row1 = row0 + raw_stride;
        uint16_t *dst = rgb + (OUT_HEIGHT - 1 - y) * OUT_WIDTH + OUT_WIDTH - 1;
        for (int x = 0; x < OUT_WIDTH; x++, dst--) {
            int col = s_x_lut[x];
            // This IMX219 module delivers RGGB: R G on the even row, G B on
            // the odd row. Treating it as BGGR swaps red and blue in the JPEG.
            uint32_t r = (row0[col] * r_gain) >> 7;
            uint32_t g = (((uint32_t)row0[col + 1] + row1[col]) * g_gain) >> 8;
            uint32_t b = (row1[col + 1] * b_gain) >> 7;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            *dst = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("cam_hal", ESP_LOG_WARN);
    esp_log_level_set("esp_video", ESP_LOG_WARN);
    
    // Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Init WiFi
    wifi_init_sta();

    // Flight control is initialized stopped before any network task can run.
    blimp_control_init();

    // One controller owns GPIO 7/8; the camera (0x10) and the IMU (0x68) are
    // just two devices on it. A second bus on the same pads takes the pins away
    // from whichever controller was there first.
    i2c_master_bus_config_t sensor_bus_cfg = {
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&sensor_bus_cfg, &s_sensor_bus));

    mpu6050_start(s_sensor_bus);
    blimp_udp_start();

    init_demosaic_luts(IMG_WIDTH, IMG_HEIGHT);

    // Init JPEG Encoder (Hardware)
    jpeg_enc_init(OUT_WIDTH, OUT_HEIGHT);

    // Create Queue
    s_jpeg_queue = xQueueCreate(1, sizeof(jpeg_frame_t));

    // Start Webservers (Dual Ports)
    start_control_server();
    start_stream_server();

    // Camera Init
    enable_xclk();
    vTaskDelay(pdMS_TO_TICKS(50));
    imx219_force_link();

    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = s_sensor_bus,
            .freq = I2C_MASTER_FREQ_HZ,
        },
        .reset_pin = -1, 
        .pwdn_pin = -1,
    };
    esp_video_init_config_t cam_config = { .csi = &csi_config };
    
    if (esp_video_init(&cam_config) != ESP_OK) {
        ESP_LOGE(TAG, "Video Init Failed");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500)); 

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (fd < 0) return;

    struct v4l2_requestbuffers req = {
        .count = 2,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ioctl(fd, VIDIOC_REQBUFS, &req);

    struct v4l2_buffer buf_q;
    for (int i = 0; i < 2; i++) {
        buf_q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf_q.memory = V4L2_MEMORY_MMAP;
        buf_q.index = i;
        ioctl(fd, VIDIOC_QBUF, &buf_q);
    }

    // Set Format (RAW10)
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = IMG_WIDTH,
        .fmt.pix.height = IMG_HEIGHT,
        .fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR10,
    };
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        ESP_LOGE(TAG, "Failed to set format");
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMON, &type);

    ESP_LOGI(TAG, "Camera Started (RAW10 Mode). Streaming active...");
    
    // Fetch global sensor device for controls
    s_sensor = imx219_get_global_dev();
    if (s_sensor) {
        ESP_LOGI(TAG, "Global Sensor Device Acquired: %p", s_sensor);
    } else {
        ESP_LOGE(TAG, "Global Sensor Device NOT Found!");
    }
    
    struct v4l2_buffer buf_dq = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    
    // Map buffers
    void *mapped_bufs[2];
    for(int i=0; i<2; i++){
         struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
         ioctl(fd, VIDIOC_QUERYBUF, &b);
         mapped_bufs[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
         if (mapped_bufs[i] == MAP_FAILED) ESP_LOGE(TAG, "Map failed");
    }

    uint32_t frame_count = 0;
    uint64_t last_time = esp_timer_get_time();

    // RGB565 halves conversion traffic and feeds the JPEG encoder's 4:2:2 path.
    uint16_t *rgb_buf = heap_caps_malloc(OUT_WIDTH * OUT_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!rgb_buf) ESP_LOGE(TAG, "Failed to alloc RGB565 buffer");

    // Set values for default exposure and gain
    if (s_sensor) {
        imx219_set_exposure(s_sensor, 110);
        imx219_set_gain(s_sensor, 232);
    }

    while (1) {
        if (ioctl(fd, VIDIOC_DQBUF, &buf_dq) == 0) {
            uint8_t *jpg_buf = NULL;
            int jpg_len = 0;
            
            uint8_t *raw_data = (uint8_t*)mapped_bufs[buf_dq.index];

            if (rgb_buf) {
                demosaic_rggb_to_rgb565(raw_data, rgb_buf);
                
                // One JPEG feeds both low-latency UDP and the diagnostic web stream.
                if (jpeg_enc_process(rgb_buf, OUT_WIDTH * OUT_HEIGHT * 2, OUT_WIDTH, OUT_HEIGHT,
                                     V4L2_PIX_FMT_RGB565, &jpg_buf, &jpg_len) == ESP_OK) {
                    blimp_udp_send_jpeg(jpg_buf, jpg_len);
                    jpeg_frame_t jf = { .data = jpg_buf, .len = jpg_len };
                    jpeg_frame_t stale;
                    if (xQueueReceive(s_jpeg_queue, &stale, 0) == pdTRUE) free(stale.data);
                    if (xQueueSend(s_jpeg_queue, &jf, 0) != pdTRUE) {
                        free(jpg_buf);
                    }
                }
            }
            
            frame_count++;
            ioctl(fd, VIDIOC_QBUF, &buf_dq);

        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        uint64_t now = esp_timer_get_time();
        if ((now - last_time) >= 1000000) {
            ESP_LOGI(TAG, "FPS: %lu", frame_count);
            frame_count = 0;
            last_time = now;
        }
    }
}
