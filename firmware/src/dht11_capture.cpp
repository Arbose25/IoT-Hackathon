#include "dht11_capture.h"
#include <driver/gpio.h>
#include <freertos/task.h>

bool IRAM_ATTR Dht11Capture::received(rmt_channel_handle_t, const rmt_rx_done_event_data_t* event,
                                    void* context) {
    auto* capture = static_cast<Dht11Capture*>(context);
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(capture->done_, &event->num_symbols, &wake);
    return wake == pdTRUE;
}

bool Dht11Capture::begin() {
    if (!done_) done_ = xQueueCreate(1, sizeof(size_t));
    if (!done_) return false;

    rmt_rx_channel_config_t config{};
    config.gpio_num = pin_;
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = 1000000; // One tick per microsecond.
    config.mem_block_symbols = 48;  // One ESP32-S3 RMT memory block.
    if (rmt_new_rx_channel(&config, &channel_) != ESP_OK) return false;

    const rmt_rx_event_callbacks_t callbacks{received};
    if (rmt_rx_register_event_callbacks(channel_, &callbacks, this) == ESP_OK &&
        gpio_set_level(pin_, 1) == ESP_OK &&
        gpio_set_direction(pin_, GPIO_MODE_INPUT_OUTPUT_OD) == ESP_OK &&
        gpio_set_pull_mode(pin_, GPIO_PULLUP_ONLY) == ESP_OK &&
        rmt_enable(channel_) == ESP_OK) {
        enabled_ = true;
        return true;
    }

    resetChannel();
    return false;
}

void Dht11Capture::resetChannel() {
    resetPending_ = true;
    if (enabled_) {
        if (rmt_disable(channel_) != ESP_OK) return;
        enabled_ = false;
    }
    if (rmt_del_channel(channel_) != ESP_OK) return;
    channel_ = nullptr;
    resetPending_ = false;
    xQueueReset(done_);
}

bool Dht11Capture::read(float& temperature, float& humidity) {
    if (resetPending_) {
        resetChannel();
        if (resetPending_) return false;
    }
    if (!channel_ && !begin()) return false;

    // The output is open drain. The line is released while the sensor replies.
    gpio_set_level(pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(25) + 1);
    rmt_receive_config_t config{};
    config.signal_range_min_ns = 2000;
    config.signal_range_max_ns = 1000000;
    const auto started = rmt_receive(channel_, symbols_, sizeof(symbols_), &config);
    gpio_set_level(pin_, 1);
    size_t count = 0;
    if (started != ESP_OK || xQueueReceive(done_, &count, pdMS_TO_TICKS(30) + 1) != pdPASS) {
        // Cancel a stalled transaction before reusing its static receive buffer.
        resetChannel();
        return false;
    }
    // A full receive buffer can mean noise truncated the capture.
    if (count >= sizeof(symbols_) / sizeof(symbols_[0])) return false;

    struct Pulse { unsigned width; bool high; };
    Pulse pulses[96];
    size_t length = 0;
    for (size_t i = 0; i < count && i < 48; ++i) {
        const auto& symbol = symbols_[i];
        if (symbol.duration0) pulses[length++] = {symbol.duration0, symbol.level0 != 0};
        if (symbol.duration1) pulses[length++] = {symbol.duration1, symbol.level1 != 0};
    }

    // A partial host-low or pull-up pulse can precede the sensor's 80/80 us preamble.
    for (size_t start = 0; start < 3 && start + 82 <= length; ++start) {
        const auto low = pulses[start], high = pulses[start + 1];
        if (low.high || !high.high || low.width < 60 || low.width > 110 ||
            high.width < 60 || high.width > 110) continue;
        uint8_t bytes[5]{};
        for (size_t bit = 0; bit < 40; ++bit) {
            const auto bitLow = pulses[start + 2 + bit * 2];
            const auto bitHigh = pulses[start + 3 + bit * 2];
            // DHT11 sends a roughly 50 us low followed by 26–28 us (0) or 70 us (1).
            // Leave a gap between the accepted bands so ambiguous pulses fail.
            const bool zero = bitHigh.width >= 16 && bitHigh.width <= 38;
            const bool one = bitHigh.width >= 52 && bitHigh.width <= 90;
            if (bitLow.high || !bitHigh.high || bitLow.width < 35 || bitLow.width > 70 ||
                (!zero && !one)) return false;
            bytes[bit / 8] = (bytes[bit / 8] << 1) | one;
        }
        if (static_cast<uint8_t>(bytes[0] + bytes[1] + bytes[2] + bytes[3]) != bytes[4])
            return false;
        // Accept whole units and the single decimal digit observed on this module.
        // The documented DHT11 range is 20–90 %RH and 0–50 °C.
        if (bytes[1] > 9 || bytes[3] > 9) return false;
        const unsigned humidity10 = bytes[0] * 10U + bytes[1];
        const unsigned temperature10 = bytes[2] * 10U + bytes[3];
        if (humidity10 < 200 || humidity10 > 900 || temperature10 > 500) return false;

        humidity = humidity10 / 10.0f;
        temperature = temperature10 / 10.0f;
        return true;
    }
    return false;
}
