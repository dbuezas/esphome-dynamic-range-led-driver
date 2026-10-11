// Host-test stand-in for ESPHome's log.h: logging does nothing.
#pragma once
template<typename... Args> inline void esp_log_ignore(Args...) {}
#define ESP_LOGD(tag, ...) esp_log_ignore(tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, ...) esp_log_ignore(tag, ##__VA_ARGS__)
