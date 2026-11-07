#include <cinttypes>

#include "ConnectionHandler.hpp"
#include "GateController.hpp"
#include "MqttHandler.hpp"
#include "HttpServer.hpp"
#include "TimeSync.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

static constexpr const char *TAG = "mqtt_example";

// ESP-IDF calls this entry point from C.
extern "C" void app_main()
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    static ConnectionHandler connection;
    static GateController gate;
    static MqttHandler mqtt(gate);
    static HttpServer http(connection, mqtt, gate);
    static TimeSync timeSync;
    ESP_ERROR_CHECK(gate.initialize());
    ESP_ERROR_CHECK(timeSync.initialize());

    struct NetworkServices {
        MqttHandler &mqtt;
        TimeSync &timeSync;
    };
    static NetworkServices services{mqtt, timeSync};

    connection.setConnectionListener([](void *context, bool connected) {
        auto &services = *static_cast<NetworkServices *>(context);
        services.mqtt.setNetworkAvailable(connected);
        services.timeSync.setNetworkAvailable(connected);
    }, &services);

    ESP_ERROR_CHECK(http.start());
    ESP_ERROR_CHECK(mqtt.connect());
    ESP_ERROR_CHECK(connection.connect());
}
