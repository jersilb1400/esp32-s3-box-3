#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "display/hud_display.h"
#include "esp_lcd_ili9341.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"

#include "aht20.h"
#include "at581x.h"
#include "icm42670.h"
#include "i2c_bus.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <array>

#define TAG "EspBox3Board"

// Init ili9341 by custom cmd
static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},

    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},

    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},

    {0, (uint8_t []){0}, 0xff, 0},
};

class EspBox3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_port_t dock_i2c_port_ = I2C_NUM_0;
    i2c_bus_handle_t dock_i2c_bus_ = nullptr;
    bool dock_i2c_initialized_ = false;
    bool sensor_dock_present_ = false;
    bool radar_enabled_ = true;
    bool radar_presence_ = false;
    bool humiture_valid_ = false;
    bool imu_valid_ = false;
    int64_t radar_last_seen_ms_ = 0;
    float temperature_c_ = 0.0f;
    float humidity_percent_ = 0.0f;
    float imu_temp_c_ = 0.0f;
    icm42670_value_t accel_ = {0};
    icm42670_value_t gyro_ = {0};
    aht20_dev_handle_t aht20_ = nullptr;
    at581x_dev_handle_t radar_ = nullptr;
    icm42670_handle_t imu_ = nullptr;
    TaskHandle_t sensor_task_handle_ = nullptr;
    std::array<gpio_num_t, 8> pmod1_{PMOD1_IO1, PMOD1_IO2, PMOD1_IO3, PMOD1_IO4, PMOD1_IO5, PMOD1_IO6, PMOD1_IO7, PMOD1_IO8};
    std::array<gpio_num_t, 8> pmod2_{PMOD2_IO1, PMOD2_IO2, PMOD2_IO3, PMOD2_IO4, PMOD2_IO5, PMOD2_IO6, PMOD2_IO7, PMOD2_IO8};
    Button boot_button_;
    Display* display_;

    static void SensorTaskEntry(void* arg) {
        static_cast<EspBox3Board*>(arg)->SensorTaskLoop();
    }

    void SensorTaskLoop() {
        constexpr int64_t kRadarPresenceMs = 120000;  // 2 minutes, matches Espressif factory demo behavior.
        int log_tick = 0;
        while (true) {
            const int64_t now_ms = esp_timer_get_time() / 1000;

            if (sensor_dock_present_) {
                const int radar_level = gpio_get_level(RADAR_OUT_PIN);
                if (radar_enabled_ && radar_level) {
                    radar_last_seen_ms_ = now_ms;
                }
                radar_presence_ = radar_enabled_ && ((now_ms - radar_last_seen_ms_) < kRadarPresenceMs);

                if (aht20_ != nullptr) {
                    uint32_t temp_raw = 0;
                    uint32_t humidity_raw = 0;
                    float temp = 0.0f;
                    float humidity = 0.0f;
                    if (aht20_read_temperature_humidity(aht20_, &temp_raw, &temp, &humidity_raw, &humidity) == ESP_OK) {
                        temperature_c_ = temp;
                        humidity_percent_ = humidity;
                        humiture_valid_ = true;
                    }
                }
            } else {
                radar_presence_ = false;
            }

            if (imu_ != nullptr) {
                icm42670_value_t accel = {0};
                icm42670_value_t gyro = {0};
                float imu_temp = 0.0f;
                esp_err_t accel_ok = icm42670_get_acce_value(imu_, &accel);
                esp_err_t gyro_ok = icm42670_get_gyro_value(imu_, &gyro);
                esp_err_t temp_ok = icm42670_get_temp_value(imu_, &imu_temp);
                if (accel_ok == ESP_OK && gyro_ok == ESP_OK && temp_ok == ESP_OK) {
                    accel_ = accel;
                    gyro_ = gyro;
                    imu_temp_c_ = imu_temp;
                    imu_valid_ = true;
                }
            }

            // Emit periodic health/status telemetry so field validation does not depend on
            // interactive MCP calls while connected over serial-only workflows.
            if (++log_tick >= 10) {
                log_tick = 0;
                ESP_LOGI(TAG,
                    "Sensor status: dock=%d radar_en=%d radar_presence=%d humiture_valid=%d "
                    "temp=%.2fC hum=%.2f%% imu_valid=%d accel=(%.2f,%.2f,%.2f) gyro=(%.2f,%.2f,%.2f)",
                    sensor_dock_present_, radar_enabled_, radar_presence_, humiture_valid_,
                    temperature_c_, humidity_percent_, imu_valid_,
                    accel_.x, accel_.y, accel_.z, gyro_.x, gyro_.y, gyro_.z);
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    bool ProbeDockI2cAddress(uint8_t addr) const {
        if (dock_i2c_bus_ == nullptr) {
            return false;
        }
        uint8_t found[16] = {0};
        const uint8_t count = i2c_bus_scan(dock_i2c_bus_, found, 16);
        for (uint8_t i = 0; i < count; ++i) {
            if (found[i] == addr) {
                return true;
            }
        }
        return false;
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeDockI2c() {
        if (dock_i2c_initialized_) {
            return;
        }

        const i2c_config_t i2c_expand_conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = DOCK_I2C_SDA_PIN,
            .scl_io_num = DOCK_I2C_SCL_PIN,
            .sda_pullup_en = GPIO_PULLUP_DISABLE,
            .scl_pullup_en = GPIO_PULLUP_DISABLE,
            .master = {
                .clk_speed = 100000,
            },
            .clk_flags = 0,
        };
        dock_i2c_bus_ = i2c_bus_create(dock_i2c_port_, &i2c_expand_conf);
        ESP_ERROR_CHECK(dock_i2c_bus_ != nullptr ? ESP_OK : ESP_FAIL);
        dock_i2c_initialized_ = true;
    }

    void InitializeIrPins() {
        gpio_config_t ctrl_config = {};
        ctrl_config.pin_bit_mask = 1ULL << IR_CTRL_PIN;
        ctrl_config.mode = GPIO_MODE_OUTPUT;
        ctrl_config.pull_up_en = GPIO_PULLUP_DISABLE;
        ctrl_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        ctrl_config.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&ctrl_config));
        gpio_set_level(IR_CTRL_PIN, 0);  // 0 enables IR TX path in Espressif factory demo.

        gpio_config_t tx_config = {};
        tx_config.pin_bit_mask = 1ULL << IR_TX_PIN;
        tx_config.mode = GPIO_MODE_OUTPUT;
        tx_config.pull_up_en = GPIO_PULLUP_DISABLE;
        tx_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        tx_config.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&tx_config));
        gpio_set_level(IR_TX_PIN, 0);

        gpio_config_t rx_config = {};
        rx_config.pin_bit_mask = 1ULL << IR_RX_PIN;
        rx_config.mode = GPIO_MODE_INPUT;
        rx_config.pull_up_en = GPIO_PULLUP_DISABLE;
        rx_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        rx_config.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&rx_config));
    }

    void InitializeSensorDock() {
        InitializeDockI2c();

        sensor_dock_present_ = ProbeDockI2cAddress(AT581X_ADDRRES_0);
        if (!sensor_dock_present_) {
            ESP_LOGW(TAG, "Sensor dock not detected (AT581x missing on I2C port %d)", dock_i2c_port_);
            return;
        }

        at581x_default_cfg_t radar_default_cfg = ATH581X_INITIALIZATION_CONFIG();
        at581x_i2c_config_t radar_i2c_conf = {
            .bus_inst = dock_i2c_bus_,
            .i2c_addr = AT581X_ADDRRES_0,
            .int_gpio_num = RADAR_OUT_PIN,
            .interrupt_level = 1,
            .interrupt_callback = nullptr,
            .def_conf = &radar_default_cfg,
        };
        esp_err_t radar_ret = at581x_new_sensor(&radar_i2c_conf, &radar_);
        if (radar_ret != ESP_OK) {
            ESP_LOGW(TAG, "AT581x init failed: %s", esp_err_to_name(radar_ret));
        }

        aht20_i2c_config_t aht20_i2c_conf = {
            .bus_inst = dock_i2c_bus_,
            .i2c_addr = AHT20_ADDRRES_0,
        };
        esp_err_t aht_ret = aht20_new_sensor(&aht20_i2c_conf, &aht20_);
        if (aht_ret != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 init failed: %s", esp_err_to_name(aht_ret));
        }

        InitializeIrPins();
        ESP_LOGI(TAG, "Sensor dock initialized on I2C port %d", dock_i2c_port_);
    }

    void InitializeImu() {
        esp_err_t ret = icm42670_create(i2c_bus_, ICM42670_I2C_ADDRESS, &imu_);
        if (ret != ESP_OK) {
            ret = icm42670_create(i2c_bus_, ICM42670_I2C_ADDRESS_1, &imu_);
        }
        if (ret != ESP_OK || imu_ == nullptr) {
            ESP_LOGW(TAG, "ICM42670 init failed: %s", esp_err_to_name(ret));
            return;
        }

        const icm42670_cfg_t imu_cfg = {
            .acce_fs = ACCE_FS_2G,
            .acce_odr = ACCE_ODR_100HZ,
            .gyro_fs = GYRO_FS_2000DPS,
            .gyro_odr = GYRO_ODR_100HZ,
        };
        ESP_ERROR_CHECK(icm42670_config(imu_, &imu_cfg));
        ESP_ERROR_CHECK(icm42670_acce_set_pwr(imu_, ACCE_PWR_LOWNOISE));
        ESP_ERROR_CHECK(icm42670_gyro_set_pwr(imu_, GYRO_PWR_LOWNOISE));
        ESP_LOGI(TAG, "ICM42670 initialized");
    }

    gpio_num_t GetPmodPin(int header, int pin) const {
        if (pin < 1 || pin > 8) {
            return GPIO_NUM_NC;
        }
        if (header == 1) {
            return pmod1_[pin - 1];
        }
        if (header == 2) {
            return pmod2_[pin - 1];
        }
        return GPIO_NUM_NC;
    }

    void RegisterSensorTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.sensor.get_status",
            "Get ESP-BOX-3 sensor and dock status",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return std::string("{\"sensor_dock_present\":") + (sensor_dock_present_ ? "true" : "false") +
                    ",\"radar_enabled\":" + (radar_enabled_ ? std::string("true") : std::string("false")) +
                    ",\"radar_presence\":" + (radar_presence_ ? std::string("true") : std::string("false")) +
                    ",\"humiture_valid\":" + (humiture_valid_ ? std::string("true") : std::string("false")) +
                    ",\"imu_valid\":" + (imu_valid_ ? std::string("true") : std::string("false")) + "}";
            });

        mcp_server.AddTool("self.sensor.get_environment",
            "Read temperature and humidity from ESP32-S3-BOX-3-SENSOR dock",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!sensor_dock_present_ || !humiture_valid_) {
                    return std::string("{\"ok\":false,\"message\":\"sensor dock temperature/humidity unavailable\"}");
                }
                return std::string("{\"ok\":true,\"temperature_c\":") + std::to_string(temperature_c_) +
                    ",\"humidity_percent\":" + std::to_string(humidity_percent_) + "}";
            });

        mcp_server.AddTool("self.sensor.get_imu",
            "Read IMU acceleration and gyroscope from ESP32-S3-BOX-3 main unit",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!imu_valid_) {
                    return std::string("{\"ok\":false,\"message\":\"IMU unavailable\"}");
                }
                return std::string("{\"ok\":true,\"temperature_c\":") + std::to_string(imu_temp_c_) +
                    ",\"accel\":{\"x\":" + std::to_string(accel_.x) + ",\"y\":" + std::to_string(accel_.y) + ",\"z\":" + std::to_string(accel_.z) +
                    "},\"gyro\":{\"x\":" + std::to_string(gyro_.x) + ",\"y\":" + std::to_string(gyro_.y) + ",\"z\":" + std::to_string(gyro_.z) + "}}";
            });

        mcp_server.AddTool("self.sensor.get_radar_presence",
            "Get radar presence state from ESP32-S3-BOX-3-SENSOR dock",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!sensor_dock_present_) {
                    return std::string("{\"ok\":false,\"message\":\"sensor dock not connected\"}");
                }
                return std::string("{\"ok\":true,\"presence\":") + (radar_presence_ ? "true" : "false") + "}";
            });

        mcp_server.AddTool("self.sensor.set_radar_enabled",
            "Enable or disable radar processing on ESP32-S3-BOX-3-SENSOR dock",
            PropertyList({Property("enabled", kPropertyTypeBoolean)}),
            [this](const PropertyList& properties) -> ReturnValue {
                radar_enabled_ = properties["enabled"].value<bool>();
                if (!radar_enabled_) {
                    radar_presence_ = false;
                }
                return true;
            });

        mcp_server.AddTool("self.sensor.get_ir_rx_level",
            "Get infrared receiver GPIO level on ESP32-S3-BOX-3-SENSOR dock",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!sensor_dock_present_) {
                    return std::string("{\"ok\":false,\"message\":\"sensor dock not connected\"}");
                }
                return std::string("{\"ok\":true,\"rx_level\":") + std::to_string(gpio_get_level(IR_RX_PIN)) + "}";
            });

        mcp_server.AddTool("self.sensor.set_ir_tx_enabled",
            "Enable or disable infrared transmitter path on ESP32-S3-BOX-3-SENSOR dock",
            PropertyList({Property("enabled", kPropertyTypeBoolean)}),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!sensor_dock_present_) {
                    return std::string("{\"ok\":false,\"message\":\"sensor dock not connected\"}");
                }
                bool enabled = properties["enabled"].value<bool>();
                gpio_set_level(IR_CTRL_PIN, enabled ? 0 : 1);
                return true;
            });

        mcp_server.AddTool("self.pmod.write_gpio",
            "Set a PMOD GPIO (headers on ESP32-S3-BOX-3-DOCK/BREAD/BRACKET)",
            PropertyList({
                Property("header", kPropertyTypeInteger, 1, 2),
                Property("pin", kPropertyTypeInteger, 1, 8),
                Property("level", kPropertyTypeInteger, 0, 1),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int header = properties["header"].value<int>();
                int pin = properties["pin"].value<int>();
                int level = properties["level"].value<int>();
                gpio_num_t gpio = GetPmodPin(header, pin);
                if (gpio == GPIO_NUM_NC) {
                    return std::string("{\"ok\":false,\"message\":\"invalid PMOD pin\"}");
                }

                gpio_config_t io_conf = {};
                io_conf.pin_bit_mask = 1ULL << gpio;
                io_conf.mode = GPIO_MODE_OUTPUT;
                io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
                io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
                io_conf.intr_type = GPIO_INTR_DISABLE;
                ESP_ERROR_CHECK(gpio_config(&io_conf));
                gpio_set_level(gpio, level);
                return true;
            });

        mcp_server.AddTool("self.pmod.read_gpio",
            "Read a PMOD GPIO (headers on ESP32-S3-BOX-3-DOCK/BREAD/BRACKET)",
            PropertyList({
                Property("header", kPropertyTypeInteger, 1, 2),
                Property("pin", kPropertyTypeInteger, 1, 8),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int header = properties["header"].value<int>();
                int pin = properties["pin"].value<int>();
                gpio_num_t gpio = GetPmodPin(header, pin);
                if (gpio == GPIO_NUM_NC) {
                    return std::string("{\"ok\":false,\"message\":\"invalid PMOD pin\"}");
                }

                gpio_config_t io_conf = {};
                io_conf.pin_bit_mask = 1ULL << gpio;
                io_conf.mode = GPIO_MODE_INPUT;
                io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
                io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
                io_conf.intr_type = GPIO_INTR_DISABLE;
                ESP_ERROR_CHECK(gpio_config(&io_conf));
                return std::string("{\"ok\":true,\"level\":") + std::to_string(gpio_get_level(gpio)) + "}";
            });
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_6;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_7;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeIli9341Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_5;
        io_config.dc_gpio_num = GPIO_NUM_4;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_48;
        panel_config.flags.reset_active_high = 1,
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void *)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#elif CONFIG_USE_HUD_DISPLAY_STYLE
        display_ = new HudLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

public:
    EspBox3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeDockI2c();
        InitializeSpi();
        InitializeIli9341Display();
        InitializeButtons();
        InitializeSensorDock();
        InitializeImu();
        RegisterSensorTools();
        xTaskCreatePinnedToCore(SensorTaskEntry, "box3_sensor_task", 4096, this, 5, &sensor_task_handle_, 1);
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(EspBox3Board);
