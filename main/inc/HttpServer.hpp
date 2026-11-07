#pragma once

#include "ConnectionHandler.hpp"
#include "MqttHandler.hpp"
#include "esp_http_server.h"

class HttpServer {
public:
    HttpServer(const ConnectionHandler &connection, const MqttHandler &mqtt, GateController &gate);
    ~HttpServer();
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    // Keep the server and its handler references alive until device restart.
    esp_err_t start();

private:
    static esp_err_t index(httpd_req_t *request);
    static esp_err_t status(httpd_req_t *request);
    static esp_err_t openGate(httpd_req_t *request);
    static esp_err_t closeGate(httpd_req_t *request);
    static esp_err_t gateStatus(httpd_req_t *request);
    static esp_err_t commandGate(httpd_req_t *request, bool open);

    const ConnectionHandler &connection_;
    const MqttHandler &mqtt_;
    GateController &gate_;
    httpd_handle_t server_ = nullptr;
};
