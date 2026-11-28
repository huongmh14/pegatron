#include <stdio.h>
#include "pico/stdlib.h"

static constexpr uint RELAY_PIN = 26;  // Chan vat ly 31

static void set_low() {
    gpio_set_dir(RELAY_PIN, GPIO_OUT);
    gpio_put(RELAY_PIN, 0);
    printf("[RELAY] GP26 = LOW  (0 V)   -> relay phai HUT\n");
}

static void set_high() {
    gpio_set_dir(RELAY_PIN, GPIO_OUT);
    gpio_put(RELAY_PIN, 1);
    printf("[RELAY] GP26 = HIGH (3.3 V) -> kiem tra relay co NHA khong\n");
}

static void set_hiz() {
    gpio_disable_pulls(RELAY_PIN);
    gpio_set_dir(RELAY_PIN, GPIO_IN);
    printf("[RELAY] GP26 = HI-Z (tha noi) -> kiem tra relay co NHA khong\n");
}

int main() {
    stdio_init_all();
    gpio_init(RELAY_PIN);
    set_low();

    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);
    printf("\n=== TEST RELAY GP26 (chan vat ly 31) ===\n");
    printf("Phim: 0=LOW(0V) | 1=HIGH(3.3V) | z=HI-Z | a=Auto LOW<->HIGH | b=Auto LOW<->HI-Z\n");

    int auto_mode = 1;  // 0=tay, 1=LOW<->HIGH, 2=LOW<->HI-Z
    bool state = false;
    uint32_t last_toggle = to_ms_since_boot(get_absolute_time());

    while (true) {
        const int ch = getchar_timeout_us(0);
        if (ch == '0') { auto_mode = 0; set_low(); }
        else if (ch == '1') { auto_mode = 0; set_high(); }
        else if (ch == 'z' || ch == 'Z') { auto_mode = 0; set_hiz(); }
        else if (ch == 'a' || ch == 'A') { auto_mode = 1; printf("[MODE] Auto LOW <-> HIGH (2s)\n"); }
        else if (ch == 'b' || ch == 'B') { auto_mode = 2; printf("[MODE] Auto LOW <-> HI-Z (2s)\n"); }

        const uint32_t now = to_ms_since_boot(get_absolute_time());
        if (auto_mode != 0 && now - last_toggle >= 2000) {
            last_toggle = now;
            state = !state;
            if (!state) set_low();
            else if (auto_mode == 1) set_high();
            else set_hiz();
        }
        sleep_ms(10);
    }
}
