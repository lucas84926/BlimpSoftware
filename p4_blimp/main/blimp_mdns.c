#include "blimp_mdns.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#if CONFIG_BLIMP_MDNS_ENABLE
#include "mdns.h"
#endif

static const char *TAG = "blimp_mdns";

void blimp_mdns_start(void)
{
#if CONFIG_BLIMP_MDNS_ENABLE
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }

    char hostname[64];
    snprintf(hostname, sizeof(hostname), "%s", CONFIG_BLIMP_MDNS_HOSTNAME);
#if CONFIG_BLIMP_MDNS_APPEND_MAC
    // Read the station MAC rather than the base MAC: on the P4 the radio lives
    // on the C6, and this runs after esp_wifi_start() so the remote is up.
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(hostname, sizeof(hostname), "%s-%02x%02x%02x",
                 CONFIG_BLIMP_MDNS_HOSTNAME, mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "MAC unavailable; publishing %s without a suffix", hostname);
    }
#endif
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_hostname_set(hostname));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        mdns_instance_name_set(CONFIG_BLIMP_MDNS_INSTANCE));

    // The ports are advertised in TXT as well as in the SRV record so a client
    // browsing the service learns the video port without a second lookup.
    char command_port[8];
    char video_port[8];
    snprintf(command_port, sizeof(command_port), "%d", CONFIG_BLIMP_COMMAND_PORT);
    snprintf(video_port, sizeof(video_port), "%d", CONFIG_BLIMP_VIDEO_PORT);
    mdns_txt_item_t txt[] = {
        {"id", hostname},
        {"proto", "ndjson"},
        {"command_port", command_port},
        {"video_port", video_port},
    };
    err = mdns_service_add(NULL, "_blimp", "_udp", CONFIG_BLIMP_COMMAND_PORT,
                           txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Advertising %s.local (_blimp._udp on port %d)",
             hostname, CONFIG_BLIMP_COMMAND_PORT);
#else
    ESP_LOGI(TAG, "mDNS disabled; the basestation needs the IP address");
#endif
}
