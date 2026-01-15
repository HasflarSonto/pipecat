/*
 * PMU Manager for ESP32-Luna
 * AXP2101 power management interface
 * Based on 01_AXP2101 example and XPowersLib
 */

#include "pmu_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "pmu_manager";

// AXP2101 I2C configuration
#define AXP2101_ADDR            0x34
#define I2C_MASTER_PORT         I2C_NUM_0
#define I2C_MASTER_FREQ_HZ      400000
#define I2C_TIMEOUT_MS          1000

// PMU GPIO (from hardware docs)
#define PMU_SDA_PIN             GPIO_NUM_15
#define PMU_SCL_PIN             GPIO_NUM_16
#define PMU_IRQ_PIN             GPIO_NUM_4

// AXP2101 register addresses (from AXP2101Constants.h)
#define REG_STATUS1             0x00
#define REG_STATUS2             0x01
#define REG_IC_TYPE             0x03
#define REG_ADC_CHANNEL         0x30
#define REG_ADC_DATA0           0x34
#define REG_ADC_DATA1           0x35
#define REG_ADC_DATA2           0x36
#define REG_ADC_DATA3           0x37
#define REG_ADC_DATA4           0x38
#define REG_ADC_DATA5           0x39
#define REG_ADC_DATA6           0x3A
#define REG_ADC_DATA7           0x3B
#define REG_ADC_DATA8           0x3C
#define REG_ADC_DATA9           0x3D
#define REG_INTEN1              0x40
#define REG_INTEN2              0x41
#define REG_INTEN3              0x42
#define REG_INTSTS1             0x48
#define REG_INTSTS2             0x49
#define REG_INTSTS3             0x4A
#define REG_TS_PIN_CTRL         0x50
#define REG_ICC_CHG_SET         0x62
#define REG_CV_CHG_VOL          0x64
#define REG_BAT_PERCENT         0xA4
#define REG_PWROFF_EN           0x22
#define REG_DC_ONOFF            0x80
#define REG_LDO_ONOFF0          0x90
#define REG_LDO_ONOFF1          0x91

// ADC enable bits
#define ADC_EN_VBUS             (1 << 0)
#define ADC_EN_BATT             (1 << 1)
#define ADC_EN_SYS              (1 << 2)
#define ADC_EN_TEMP             (1 << 4)

// Status1 bits
#define STATUS1_VBUS_PRESENT    (1 << 5)
#define STATUS1_VBUS_GOOD       (1 << 4)
#define STATUS1_BATT_PRESENT    (1 << 3)
#define STATUS1_CHARGING        (1 << 2)

// Charger status (from status register)
#define CHG_TRI_STATE           0
#define CHG_PRE_STATE           1
#define CHG_CC_STATE            2
#define CHG_CV_STATE            3
#define CHG_DONE_STATE          4
#define CHG_STOP_STATE          5

// IRQ bits
#define IRQ1_VBUS_INSERT        (1 << 3)
#define IRQ1_VBUS_REMOVE        (1 << 2)
#define IRQ1_BAT_INSERT         (1 << 1)
#define IRQ1_BAT_REMOVE         (1 << 0)
#define IRQ2_CHG_START          (1 << 1)
#define IRQ2_CHG_DONE           (1 << 0)
#define IRQ2_BAT_LOW            (1 << 3)
#define IRQ3_PKEY_SHORT         (1 << 0)
#define IRQ3_PKEY_LONG          (1 << 1)

// PMU state
static struct {
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t i2c_dev;
    pmu_event_cb_t callback;
    void *callback_ctx;
    TaskHandle_t monitor_task;
    bool initialized;
    bool running;
} s_pmu = {0};

// I2C helper functions
static esp_err_t pmu_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    if (s_pmu.i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_pmu.i2c_dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

static esp_err_t pmu_write_reg(uint8_t reg, uint8_t data)
{
    if (s_pmu.i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(s_pmu.i2c_dev, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t pmu_read_reg8(uint8_t reg, uint8_t *value)
{
    return pmu_read_reg(reg, value, 1);
}

static esp_err_t pmu_set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t value;
    esp_err_t ret = pmu_read_reg8(reg, &value);
    if (ret != ESP_OK) return ret;
    return pmu_write_reg(reg, value | mask);
}

static esp_err_t pmu_clear_bits(uint8_t reg, uint8_t mask)
{
    uint8_t value;
    esp_err_t ret = pmu_read_reg8(reg, &value);
    if (ret != ESP_OK) return ret;
    return pmu_write_reg(reg, value & ~mask);
}

static void fire_event(pmu_event_t event)
{
    if (s_pmu.callback) {
        s_pmu.callback(event, s_pmu.callback_ctx);
    }
}

static void pmu_check_irq(void)
{
    uint8_t irq1, irq2, irq3;

    // Read interrupt status
    if (pmu_read_reg8(REG_INTSTS1, &irq1) != ESP_OK) return;
    if (pmu_read_reg8(REG_INTSTS2, &irq2) != ESP_OK) return;
    if (pmu_read_reg8(REG_INTSTS3, &irq3) != ESP_OK) return;

    // Clear interrupts by writing back
    pmu_write_reg(REG_INTSTS1, irq1);
    pmu_write_reg(REG_INTSTS2, irq2);
    pmu_write_reg(REG_INTSTS3, irq3);

    // Fire events
    if (irq1 & IRQ1_VBUS_INSERT) {
        ESP_LOGI(TAG, "VBUS inserted");
        fire_event(PMU_EVENT_VBUS_INSERT);
    }
    if (irq1 & IRQ1_VBUS_REMOVE) {
        ESP_LOGI(TAG, "VBUS removed");
        fire_event(PMU_EVENT_VBUS_REMOVE);
    }
    if (irq2 & IRQ2_CHG_START) {
        ESP_LOGI(TAG, "Charging started");
        fire_event(PMU_EVENT_CHARGE_START);
    }
    if (irq2 & IRQ2_CHG_DONE) {
        ESP_LOGI(TAG, "Charging complete");
        fire_event(PMU_EVENT_CHARGE_DONE);
    }
    if (irq2 & IRQ2_BAT_LOW) {
        ESP_LOGW(TAG, "Battery low");
        fire_event(PMU_EVENT_BATTERY_LOW);
    }
    if (irq3 & IRQ3_PKEY_SHORT) {
        ESP_LOGI(TAG, "Power button short press");
        fire_event(PMU_EVENT_BUTTON_PRESS);
    }
    if (irq3 & IRQ3_PKEY_LONG) {
        ESP_LOGI(TAG, "Power button long press");
        fire_event(PMU_EVENT_BUTTON_LONG);
    }
}

static void pmu_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "PMU monitor task started");

    while (s_pmu.running) {
        pmu_check_irq();
        vTaskDelay(pdMS_TO_TICKS(500));  // Check every 500ms
    }

    ESP_LOGI(TAG, "PMU monitor task stopped");
    vTaskDelete(NULL);
}

esp_err_t pmu_manager_init(void)
{
    if (s_pmu.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing PMU manager...");

    // Initialize I2C bus
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = PMU_SDA_PIN,
        .scl_io_num = PMU_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
        }
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &s_pmu.i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %d", ret);
        return ret;
    }

    // Add AXP2101 device
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    ret = i2c_master_bus_add_device(s_pmu.i2c_bus, &dev_config, &s_pmu.i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %d", ret);
        i2c_del_master_bus(s_pmu.i2c_bus);
        return ret;
    }

    // Verify chip ID
    uint8_t chip_id;
    ret = pmu_read_reg8(REG_IC_TYPE, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read chip ID: %d", ret);
        return ret;
    }

    if (chip_id != 0x4A) {
        ESP_LOGW(TAG, "Unexpected chip ID: 0x%02X (expected 0x4A)", chip_id);
    } else {
        ESP_LOGI(TAG, "AXP2101 detected (ID: 0x%02X)", chip_id);
    }

    // Enable ADC measurements
    pmu_write_reg(REG_ADC_CHANNEL, ADC_EN_VBUS | ADC_EN_BATT | ADC_EN_SYS | ADC_EN_TEMP);

    // Disable TS pin measurement (no battery temp sensor)
    pmu_set_bits(REG_TS_PIN_CTRL, 0x03);  // Disable TS pin

    // Clear all interrupts
    pmu_write_reg(REG_INTSTS1, 0xFF);
    pmu_write_reg(REG_INTSTS2, 0xFF);
    pmu_write_reg(REG_INTSTS3, 0xFF);

    // Enable desired interrupts
    pmu_write_reg(REG_INTEN1, IRQ1_VBUS_INSERT | IRQ1_VBUS_REMOVE |
                              IRQ1_BAT_INSERT | IRQ1_BAT_REMOVE);
    pmu_write_reg(REG_INTEN2, IRQ2_CHG_START | IRQ2_CHG_DONE | IRQ2_BAT_LOW);
    pmu_write_reg(REG_INTEN3, IRQ3_PKEY_SHORT | IRQ3_PKEY_LONG);

    // Set charging current to 400mA (safe default)
    pmu_manager_set_charge_current(400);

    // Set charge target voltage to 4.2V
    pmu_write_reg(REG_CV_CHG_VOL, 0x03);  // 4.2V

    s_pmu.initialized = true;
    s_pmu.running = true;

    // Start monitor task
    BaseType_t task_ret = xTaskCreate(pmu_monitor_task, "pmu_monitor",
                                       3 * 1024, NULL, 3, &s_pmu.monitor_task);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create monitor task");
    }

    // Log initial status
    pmu_status_t status;
    if (pmu_manager_get_status(&status) == ESP_OK) {
        ESP_LOGI(TAG, "PMU initialized - Battery: %d%% (%.2fV), VBUS: %s",
                 status.battery_percent,
                 status.battery_voltage,
                 status.vbus_connected ? "Connected" : "Disconnected");
    }

    return ESP_OK;
}

esp_err_t pmu_manager_deinit(void)
{
    if (!s_pmu.initialized) {
        return ESP_OK;
    }

    s_pmu.running = false;

    if (s_pmu.monitor_task) {
        vTaskDelay(pdMS_TO_TICKS(600));  // Wait for task to exit
        s_pmu.monitor_task = NULL;
    }

    if (s_pmu.i2c_dev) {
        i2c_master_bus_rm_device(s_pmu.i2c_dev);
        s_pmu.i2c_dev = NULL;
    }

    if (s_pmu.i2c_bus) {
        i2c_del_master_bus(s_pmu.i2c_bus);
        s_pmu.i2c_bus = NULL;
    }

    s_pmu.initialized = false;
    ESP_LOGI(TAG, "PMU manager deinitialized");

    return ESP_OK;
}

esp_err_t pmu_manager_get_status(pmu_status_t *status)
{
    if (!s_pmu.initialized || status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(status, 0, sizeof(pmu_status_t));

    // Read status registers
    uint8_t status1, status2;
    esp_err_t ret = pmu_read_reg8(REG_STATUS1, &status1);
    if (ret != ESP_OK) return ret;

    ret = pmu_read_reg8(REG_STATUS2, &status2);
    if (ret != ESP_OK) return ret;

    status->vbus_connected = (status1 & STATUS1_VBUS_PRESENT) != 0;
    status->battery_present = (status1 & STATUS1_BATT_PRESENT) != 0;
    status->is_charging = (status1 & STATUS1_CHARGING) != 0;

    // Get charger state
    uint8_t chg_status = (status2 >> 5) & 0x07;
    switch (chg_status) {
        case CHG_TRI_STATE:
        case CHG_PRE_STATE:
            status->charge_state = PMU_CHARGE_TRICKLE;
            break;
        case CHG_CC_STATE:
            status->charge_state = PMU_CHARGE_CC;
            break;
        case CHG_CV_STATE:
            status->charge_state = PMU_CHARGE_CV;
            break;
        case CHG_DONE_STATE:
            status->charge_state = PMU_CHARGE_DONE;
            break;
        default:
            status->charge_state = PMU_CHARGE_NONE;
            break;
    }

    // Read ADC values
    uint8_t adc[10];
    ret = pmu_read_reg(REG_ADC_DATA0, adc, 10);
    if (ret == ESP_OK) {
        // Battery voltage: ADC[0:1] - 14 bits, LSB = 1mV
        uint16_t batt_raw = ((uint16_t)adc[0] << 6) | (adc[1] & 0x3F);
        status->battery_voltage = batt_raw / 1000.0f;

        // VBUS voltage: ADC[4:5] - 14 bits, LSB = 1mV
        uint16_t vbus_raw = ((uint16_t)adc[4] << 6) | (adc[5] & 0x3F);
        status->vbus_voltage = vbus_raw / 1000.0f;

        // System voltage: ADC[6:7] - 14 bits, LSB = 1mV
        uint16_t sys_raw = ((uint16_t)adc[6] << 6) | (adc[7] & 0x3F);
        status->system_voltage = sys_raw / 1000.0f;

        // Temperature: ADC[8:9] - conversion formula from constants
        uint16_t temp_raw = ((uint16_t)adc[8] << 6) | (adc[9] & 0x3F);
        status->chip_temp = 22.0f + (7274.0f - temp_raw) / 20.0f;
    }

    // Read battery percentage
    uint8_t percent;
    ret = pmu_read_reg8(REG_BAT_PERCENT, &percent);
    if (ret == ESP_OK) {
        status->battery_percent = percent & 0x7F;  // 7-bit value
        if (status->battery_percent > 100) {
            status->battery_percent = 100;
        }
    }

    return ESP_OK;
}

int pmu_manager_get_battery_percent(void)
{
    if (!s_pmu.initialized) {
        return -1;
    }

    uint8_t percent;
    if (pmu_read_reg8(REG_BAT_PERCENT, &percent) != ESP_OK) {
        return -1;
    }

    int value = percent & 0x7F;
    return (value > 100) ? 100 : value;
}

bool pmu_manager_is_charging(void)
{
    if (!s_pmu.initialized) {
        return false;
    }

    uint8_t status1;
    if (pmu_read_reg8(REG_STATUS1, &status1) != ESP_OK) {
        return false;
    }

    return (status1 & STATUS1_CHARGING) != 0;
}

bool pmu_manager_is_vbus_connected(void)
{
    if (!s_pmu.initialized) {
        return false;
    }

    uint8_t status1;
    if (pmu_read_reg8(REG_STATUS1, &status1) != ESP_OK) {
        return false;
    }

    return (status1 & STATUS1_VBUS_PRESENT) != 0;
}

float pmu_manager_get_temperature(void)
{
    if (!s_pmu.initialized) {
        return -273.15f;  // Absolute zero = error
    }

    uint8_t adc[2];
    if (pmu_read_reg(REG_ADC_DATA8, adc, 2) != ESP_OK) {
        return -273.15f;
    }

    uint16_t temp_raw = ((uint16_t)adc[0] << 6) | (adc[1] & 0x3F);
    return 22.0f + (7274.0f - temp_raw) / 20.0f;
}

void pmu_manager_set_event_callback(pmu_event_cb_t callback, void *ctx)
{
    s_pmu.callback = callback;
    s_pmu.callback_ctx = ctx;
}

void pmu_manager_power_off(void)
{
    if (!s_pmu.initialized) {
        return;
    }

    ESP_LOGW(TAG, "Powering off...");

    // Set power off bit
    pmu_set_bits(REG_PWROFF_EN, 0x01);
}

esp_err_t pmu_manager_set_charge_current(uint16_t ma)
{
    if (!s_pmu.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // AXP2101 charge current settings (from constants)
    // Register value = (current_mA - 0) / 25, max 1000mA
    uint8_t reg_val;

    if (ma < 25) {
        reg_val = 0;
    } else if (ma >= 1000) {
        reg_val = 40;  // Max 1000mA
    } else {
        reg_val = ma / 25;
    }

    ESP_LOGI(TAG, "Setting charge current to %d mA (reg=0x%02X)", ma, reg_val);
    return pmu_write_reg(REG_ICC_CHG_SET, reg_val);
}
