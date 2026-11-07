#include "HttpServer.hpp"
#include "NetworkConfig.hpp"

#include <cstdio>

#include "esp_log.h"

namespace {
constexpr const char *TAG = "http_server";
constexpr const char PAGE[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>GofkuCamEmbedded</title></head><body>"
    "<h1>GofkuCamEmbedded</h1><p>The device HTTP server is running.</p>"
    "<form method=\"post\" action=\"/open\"><button>Open gate</button></form>"
    "<form method=\"post\" action=\"/close\"><button>Close gate</button></form>"
    "<p>The gate stays open while MQTT is offline.</p>"
    "<p><a href=\"/gate_status\">Gate status</a> | "
    "<a href=\"/status\">Wi-Fi and MQTT status</a></p></body></html>";

const char *stateName(GateState state)
{
    switch (state) {
    case GateState::Idle: return "initializing";
    case GateState::Open: return "open";
    case GateState::Opening: return "opening";
    case GateState::Closed: return "closed";
    case GateState::Recovering: return "recovering";
    }
    return "unknown";
}

template <typename State>
const char *stateName(State state)
{
    switch (state) {
    case State::Idle: return "idle";
    case State::WaitingForConnection: return "waiting_for_connection";
    case State::Connected: return "connected";
    case State::Recovering: return "recovering";
    }
    return "unknown";
}

esp_err_t readOpeningVelocity(httpd_req_t *request, unsigned &velocity, bool &provided)
{
    provided = false;
    const std::size_t length = httpd_req_get_url_query_len(request);
    if (length == 0) { return ESP_OK; }
    char query[64];
    char value[16];
    if (length >= sizeof(query) || httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = httpd_query_key_value(query, "velocity", value, sizeof(value));
    if (result == ESP_ERR_NOT_FOUND) { return ESP_OK; }
    if (result != ESP_OK || value[0] == '\0') { return ESP_ERR_INVALID_ARG; }
    velocity = 0;
    for (const char *digit = value; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9') { return ESP_ERR_INVALID_ARG; }
        velocity = velocity * 10 + static_cast<unsigned>(*digit - '0');
        if (velocity > 360) { return ESP_ERR_INVALID_ARG; }
    }
    if (velocity == 0) { return ESP_ERR_INVALID_ARG; }
    provided = true;
    return ESP_OK;
}
}

HttpServer::HttpServer(const ConnectionHandler &connection, const MqttHandler &mqtt, GateController &gate)
    : connection_(connection), mqtt_(mqtt), gate_(gate)
{
}

HttpServer::~HttpServer()
{
    configASSERT(server_ == nullptr);
}

esp_err_t HttpServer::start()
{
    if (server_ != nullptr) { return ESP_OK; }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK) { return result; }

    httpd_uri_t routes[5]{};
    routes[0].uri = "/";
    routes[0].method = HTTP_GET;
    routes[0].handler = index;
    routes[1].uri = "/status";
    routes[1].method = HTTP_GET;
    routes[1].handler = status;
    routes[2].uri = "/open";
    routes[2].method = HTTP_POST;
    routes[2].handler = openGate;
    routes[3].uri = "/close";
    routes[3].method = HTTP_POST;
    routes[3].handler = closeGate;
    routes[4].uri = "/gate_status";
    routes[4].method = HTTP_GET;
    routes[4].handler = gateStatus;
    for (auto &route : routes) {
        route.user_ctx = this;
        result = httpd_register_uri_handler(server, &route);
        if (result != ESP_OK) { break; }
    }
    if (result != ESP_OK) {
        httpd_stop(server);
        return result;
    }
    server_ = server;
    ESP_LOGI(TAG, "HTTP server listening on http://%s/", NetworkConfig::ADDRESS);
    return ESP_OK;
}

esp_err_t HttpServer::index(httpd_req_t *request)
{
    esp_err_t result = httpd_resp_set_type(request, "text/html; charset=utf-8");
    if (result == ESP_OK) { result = httpd_resp_set_hdr(request, "Cache-Control", "no-store"); }
    return result == ESP_OK ? httpd_resp_send(request, PAGE, HTTPD_RESP_USE_STRLEN) : result;
}

esp_err_t HttpServer::status(httpd_req_t *request)
{
    const auto *self = static_cast<const HttpServer *>(request->user_ctx);
    char response[256];
    std::snprintf(response, sizeof(response), "{\"ip\":\"%s\",\"wifi\":\"%s\",\"mqtt\":\"%s\"}",
                  NetworkConfig::ADDRESS, stateName(self->connection_.state()), stateName(self->mqtt_.state()));
    esp_err_t result = httpd_resp_set_type(request, "application/json");
    if (result == ESP_OK) { result = httpd_resp_set_hdr(request, "Cache-Control", "no-store"); }
    return result == ESP_OK ? httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN) : result;
}

esp_err_t HttpServer::openGate(httpd_req_t *request) { return commandGate(request, true); }
esp_err_t HttpServer::closeGate(httpd_req_t *request) { return commandGate(request, false); }

esp_err_t HttpServer::commandGate(httpd_req_t *request, bool open)
{
    auto *self = static_cast<HttpServer *>(request->user_ctx);
    esp_err_t command;
    if (open) {
        unsigned velocity = 0;
        bool provided = false;
        command = readOpeningVelocity(request, velocity, provided);
        if (command == ESP_OK) { command = provided ? self->gate_.open(velocity) : self->gate_.open(); }
    } else {
        command = self->gate_.close();
    }
    const char *response;
    const char *status;
    if (command == ESP_OK) {
        status = "202 Accepted";
        response = open ? "{\"accepted\":true,\"command\":\"open\"}" : "{\"accepted\":true,\"command\":\"close\"}";
    } else if (command == ESP_ERR_INVALID_ARG) {
        status = "400 Bad Request";
        response = "{\"accepted\":false,\"error\":\"velocity must be an integer from 1 to 360 degrees/second\"}";
    } else if (!open && !self->gate_.mqttConnected()) {
        status = "409 Conflict";
        response = "{\"accepted\":false,\"error\":\"MQTT is offline; gate must remain open\"}";
    } else {
        status = "503 Service Unavailable";
        response = "{\"accepted\":false,\"error\":\"Gate controller is not started\"}";
    }
    esp_err_t result = httpd_resp_set_status(request, status);
    if (result == ESP_OK) { result = httpd_resp_set_type(request, "application/json"); }
    if (result == ESP_OK) { result = httpd_resp_set_hdr(request, "Cache-Control", "no-store"); }
    return result == ESP_OK ? httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN) : result;
}

esp_err_t HttpServer::gateStatus(httpd_req_t *request)
{
    const auto *self = static_cast<const HttpServer *>(request->user_ctx);
    char response[320];
    std::snprintf(response, sizeof(response),
                  "{\"state\":\"%s\",\"target\":\"%s\",\"commanded_position\":%.2f,\"opening_velocity_dps\":%u,\"mqtt_connected\":%s,\"last_error\":\"%s\"}",
                  stateName(self->gate_.state()), stateName(self->gate_.target()),
                  static_cast<double>(self->gate_.commandedPosition()), self->gate_.openingVelocity(),
                  self->gate_.mqttConnected() ? "true" : "false", esp_err_to_name(self->gate_.lastError()));
    esp_err_t result = httpd_resp_set_type(request, "application/json");
    if (result == ESP_OK) { result = httpd_resp_set_hdr(request, "Cache-Control", "no-store"); }
    return result == ESP_OK ? httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN) : result;
}
