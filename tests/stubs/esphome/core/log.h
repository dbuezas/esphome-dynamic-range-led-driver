// Host-test stand-in for ESPHome's log.h: logging does nothing.
#pragma once
#define ESP_LOGD(tag, ...) ((void) (tag))
#define ESP_LOGV(tag, ...) ((void) (tag))
