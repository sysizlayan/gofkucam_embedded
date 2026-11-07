idf_component_register(
    SRCS
        "app_main.cpp"
        "lib/MqttHandler.cpp"
        "lib/ConnectionHandler.cpp"
        "lib/HttpServer.cpp"
        "lib/TimeSync.cpp"
        "lib/ServoMotor.cpp"
        "lib/GateController.cpp"
    REQUIRES freertos espressif__mqtt esp_event esp_netif esp_http_server
    PRIV_REQUIRES esp_wifi nvs_flash espressif__servo
    INCLUDE_DIRS "." "inc"
)

