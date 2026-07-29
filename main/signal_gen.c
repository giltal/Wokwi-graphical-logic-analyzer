// SPDX-License-Identifier: MIT
//
// Test signal generator for the chip-logic-scope custom chip.
//
// D0/D1 are a free-running binary counter, so each channel is a square wave at
// half the frequency of the one below it. D2..D4 are an SPI bus, D5/D6 an I2C
// bus and D7 a bit-banged 9600 8N1 serial line, so every protocol decoder has
// something to chew on.
//
//   GPIO4  -> D0   toggles every 1 x TICK
//   GPIO5  -> D1   toggles every 2 x TICK
//   GPIO6  -> D2   SPI SCK  (200 kHz, mode 0)
//   GPIO7  -> D3   SPI MOSI
//   GPIO15 -> D4   SPI CS (active low)
//   GPIO16 -> D5   I2C SDA (MPU6050 @ 0x68, 100 kHz)
//   GPIO17 -> D6   I2C SCL
//   GPIO18 -> D7   serial TX, "Hello " at 9600 baud

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CHANNEL_COUNT 2

// Half-period of the fastest channel, in microseconds.
// 100 us => D0 = 5 kHz, D1 = 2.5 kHz.
#define TICK_US 100

// Bit-banged serial line on D7.
#define UART_PIN GPIO_NUM_18
#define UART_BAUD 9600
#define UART_BIT_US (1000000 / UART_BAUD)

// SPI master on D2/D3/D4. 200 kHz keeps a 3-byte burst readable at 100 us/div.
#define SPI_SCLK_PIN GPIO_NUM_6
#define SPI_MOSI_PIN GPIO_NUM_7
#define SPI_CS_PIN GPIO_NUM_15
#define SPI_HZ 200000

// I2C master on D5/D6, talking to the MPU6050 in the diagram.
#define I2C_SDA_PIN GPIO_NUM_16
#define I2C_SCL_PIN GPIO_NUM_17
#define I2C_HZ 100000
#define MPU6050_ADDR 0x68
#define MPU6050_WHO_AM_I 0x75

static const gpio_num_t kChannelPins[CHANNEL_COUNT] = {
    GPIO_NUM_4, GPIO_NUM_5,
};

static portMUX_TYPE s_uart_mux = portMUX_INITIALIZER_UNLOCKED;

static uint64_t channel_mask(void) {
  uint64_t mask = 1ULL << UART_PIN;
  for (int i = 0; i < CHANNEL_COUNT; i++) {
    mask |= 1ULL << kChannelPins[i];
  }
  return mask;
}

// 8N1, LSB first, idle high. The critical section keeps the frame free of
// scheduler jitter, which would otherwise show up as framing errors.
static void uart_send_byte(uint8_t value) {
  taskENTER_CRITICAL(&s_uart_mux);
  gpio_set_level(UART_PIN, 0);
  esp_rom_delay_us(UART_BIT_US);
  for (int i = 0; i < 8; i++) {
    gpio_set_level(UART_PIN, (value >> i) & 1u);
    esp_rom_delay_us(UART_BIT_US);
  }
  gpio_set_level(UART_PIN, 1);
  esp_rom_delay_us(UART_BIT_US);
  taskEXIT_CRITICAL(&s_uart_mux);
}

static void uart_task(void *arg) {
  (void)arg;
  gpio_set_level(UART_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(10));

  for (;;) {
    for (const char *p = "Hello "; *p != '\0'; p++) {
      uart_send_byte((uint8_t)*p);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// One short transaction every 50 ms: address the MPU6050, write the register
// pointer, repeated START, then read one byte back. That exercises START,
// address + R/W, ACK, data and STOP in a window a few hundred us wide.
static void i2c_task(void *arg) {
  (void)arg;

  const i2c_master_bus_config_t bus_cfg = {
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .i2c_port = I2C_NUM_0,
      .scl_io_num = I2C_SCL_PIN,
      .sda_io_num = I2C_SDA_PIN,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus = NULL;
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

  const i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = MPU6050_ADDR,
      .scl_speed_hz = I2C_HZ,
  };
  i2c_master_dev_handle_t dev = NULL;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

  for (;;) {
    const uint8_t reg = MPU6050_WHO_AM_I;
    uint8_t value = 0;
    i2c_master_transmit_receive(dev, &reg, 1, &value, 1, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Three bytes every 30 ms with the driver toggling CS around the burst, so the
// decoder sees a clean select / 24 clocks / deselect sequence.
static void spi_task(void *arg) {
  (void)arg;

  const spi_bus_config_t bus_cfg = {
      .mosi_io_num = SPI_MOSI_PIN,
      .miso_io_num = -1,
      .sclk_io_num = SPI_SCLK_PIN,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 32,
  };
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_DISABLED));

  const spi_device_interface_config_t dev_cfg = {
      .clock_speed_hz = SPI_HZ,
      .mode = 0, // CPOL = 0, CPHA = 0
      .spics_io_num = SPI_CS_PIN,
      .queue_size = 1,
  };
  spi_device_handle_t dev = NULL;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev));

  static const uint8_t payload[3] = {'S', 'P', 'I'};
  for (;;) {
    spi_transaction_t tx = {
        .length = 8 * sizeof(payload),
        .tx_buffer = payload,
    };
    spi_device_polling_transmit(dev, &tx);
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

static void counter_task(void *arg) {  (void)arg;
  uint32_t counter = 0;

  for (;;) {
    for (int i = 0; i < CHANNEL_COUNT; i++) {
      gpio_set_level(kChannelPins[i], (counter >> i) & 1u);
    }
    counter++;

    esp_rom_delay_us(TICK_US);

    // Yield periodically so the idle task can feed the watchdog.
    if ((counter & 0x3ff) == 0) {
      vTaskDelay(1);
    }
  }
}

void app_main(void) {
  const gpio_config_t io_conf = {
      .pin_bit_mask = channel_mask(),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));

  printf("logic-scope demo: %d counter channels (tick %d us) + %d baud UART"
         " + I2C @ %d Hz + SPI @ %d Hz\n",
         CHANNEL_COUNT, TICK_US, UART_BAUD, I2C_HZ, SPI_HZ);

  xTaskCreate(counter_task, "counter", 2048, NULL, 5, NULL);
  xTaskCreate(uart_task, "uart", 2048, NULL, 6, NULL);
  xTaskCreate(i2c_task, "i2c", 4096, NULL, 6, NULL);
  xTaskCreate(spi_task, "spi", 4096, NULL, 6, NULL);
}
