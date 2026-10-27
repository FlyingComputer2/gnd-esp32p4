#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "c6_bridge";

// P4 UART1 <-> C6 UART0 on GPIO32/33. Guition: GPIO32 -> C6 U0RXD (JP1 pin 20),
// GPIO33 <- C6 U0TXD (JP1 pin 22). The Waveshare is wired the other way round.
#define C6_UART_PORT      UART_NUM_1
#if CONFIG_C6_BRIDGE_BOARD_WAVESHARE
#define C6_UART_TX_GPIO   GPIO_NUM_33
#define C6_UART_RX_GPIO   GPIO_NUM_32
#else
#define C6_UART_TX_GPIO   GPIO_NUM_32
#define C6_UART_RX_GPIO   GPIO_NUM_33
#endif
#define C6_UART_BAUD      115200

#define HOST_UART_PORT    UART_NUM_0
#define HOST_UART_TX_GPIO GPIO_NUM_37
#define HOST_UART_RX_GPIO GPIO_NUM_38

#define C6_RESET_GPIO     GPIO_NUM_54
#if CONFIG_C6_BRIDGE_BOARD_WAVESHARE
#define C6_BOOT_GPIO      GPIO_NUM_22
#define C6_BOOT_GPIO_LDO  -1  // GPIO22 is outside the LDO-powered GPIO39-48 domain
#else
#define C6_BOOT_GPIO      GPIO_NUM_45
#define C6_BOOT_GPIO_LDO  4
#endif

#define BRIDGE_BUF_SIZE   512

static esp_ldo_channel_handle_t s_boot_ldo;

static void boot_strap_ldo_on(void)
{
    if (C6_BOOT_GPIO_LDO < 0 || s_boot_ldo != NULL) {
        return;
    }
    const esp_ldo_channel_config_t cfg = {
        .chan_id = C6_BOOT_GPIO_LDO,
        .voltage_mv = 3300,
    };
    esp_err_t err = esp_ldo_acquire_channel(&cfg, &s_boot_ldo);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LDO_VO%d for GPIO%d: %s", C6_BOOT_GPIO_LDO, C6_BOOT_GPIO, esp_err_to_name(err));
    }
}

static void c6_reset_lines_init(void)
{
    boot_strap_ldo_on();

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << C6_RESET_GPIO) | (1ULL << C6_BOOT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(C6_BOOT_GPIO, 1);
    gpio_set_level(C6_RESET_GPIO, 1);
}

static void c6_pulse_reset(void)
{
    gpio_set_level(C6_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(C6_RESET_GPIO, 1);
}

void c6_enter_uart_download_mode(void)
{
    gpio_set_level(C6_BOOT_GPIO, 0);
    c6_pulse_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(C6_BOOT_GPIO, 1);
    ESP_LOGI(TAG, "C6 held in UART ROM downloader (IO9 low through reset)");
}

static void uart_port_init(uart_port_t port, int tx, int rx)
{
    const uart_config_t cfg = {
        .baud_rate = C6_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(port, BRIDGE_BUF_SIZE * 2, BRIDGE_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void bridge_task(void *arg)
{
    uint8_t buf[BRIDGE_BUF_SIZE];
    for (;;) {
        int n = uart_read_bytes(HOST_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            uart_write_bytes(C6_UART_PORT, buf, n);
        }
        n = uart_read_bytes(C6_UART_PORT, buf, sizeof(buf), 0);
        if (n > 0) {
            uart_write_bytes(HOST_UART_PORT, buf, n);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "USB UART0 (GPIO37/38) <-> C6 UART1 (TX GPIO%d, RX GPIO%d); boot GPIO%d reset GPIO54",
             C6_UART_TX_GPIO, C6_UART_RX_GPIO, C6_BOOT_GPIO);
    c6_reset_lines_init();
    uart_port_init(HOST_UART_PORT, HOST_UART_TX_GPIO, HOST_UART_RX_GPIO);
    uart_port_init(C6_UART_PORT, C6_UART_TX_GPIO, C6_UART_RX_GPIO);
    c6_enter_uart_download_mode();

    vTaskDelay(pdMS_TO_TICKS(100));
    uart_flush_input(HOST_UART_PORT);
    uart_flush_input(C6_UART_PORT);

    xTaskCreate(bridge_task, "bridge", 4096, NULL, 10, NULL);
}
