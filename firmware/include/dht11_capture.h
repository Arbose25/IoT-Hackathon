#pragma once

#include <driver/rmt_rx.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class Dht11Capture {
public:
    explicit Dht11Capture(gpio_num_t pin) : pin_(pin) {}
    bool read(float& temperature, float& humidity);

private:
    bool begin();
    void resetChannel();
    static bool IRAM_ATTR received(rmt_channel_handle_t, const rmt_rx_done_event_data_t*, void*);

    gpio_num_t pin_;
    rmt_channel_handle_t channel_ = nullptr;
    QueueHandle_t done_ = nullptr;
    bool enabled_ = false;
    bool resetPending_ = false;
    rmt_symbol_word_t symbols_[48]{};
};
