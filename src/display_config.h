#pragma once
#include <LovyanGFX.hpp>

// Waveshare ESP32-S3-Touch-AMOLED-1.64
// Display: CO5300, 280x456, QSPI  (col_offset=20)
// Touch:   FT3168 (FT5x06-compat), I2C1 0x38, SDA=47, SCL=48

struct Panel_CO5300_Waveshare164 : public lgfx::Panel_CO5300 {
    Panel_CO5300_Waveshare164() {
        _cfg.memory_width  = _cfg.panel_width  = 280;
        _cfg.memory_height = _cfg.panel_height = 456;
    }

    const uint8_t* getInitCommands(uint8_t listno) const override {
        // CASET: 20..299  RASET: 0..455
        static constexpr uint8_t list0[] = {
            0xFE, 1, 0x00,
            0xC4, 1, 0x80,
            0x3A, 1, 0x55,                     // pixel format 16-bit RGB565
            0x35, 1, 0x00,                     // TE on
            0x53, 1, 0x20,
            0x63, 1, 0xFF,
            0x2A, 4, 0x00,0x14, 0x01,0x2B,    // col 20..299
            0x2B, 4, 0x00,0x00, 0x01,0xC7,    // row  0..455
            0x11, 0x80, 120,                   // sleep out + delay
            0x51, 1, 0x01,
            0x29, 0x80, 20,                    // display on + delay
            0x51, 1, 0xD0,
            0xFF, 0xFF
        };
        if (listno == 0) return list0;
        return nullptr;
    }
};

class LGFX_WaveshareAMOLED164 : public lgfx::LGFX_Device {
    lgfx::Bus_SPI                _bus_instance;
    Panel_CO5300_Waveshare164    _panel_instance;
    // Touch (FT3168) is read manually via Wire — no LovyanGFX touch driver

public:
    LGFX_WaveshareAMOLED164() {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = SPI_MODE0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.pin_sclk    = 10;
            cfg.pin_io0     = 11;
            cfg.pin_io1     = 12;
            cfg.pin_io2     = 13;
            cfg.pin_io3     = 14;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs        = 9;
            cfg.pin_rst       = 21;
            cfg.pin_busy      = -1;
            cfg.panel_width   = 280;
            cfg.panel_height  = 456;
            cfg.memory_width  = 280;
            cfg.memory_height = 456;
            cfg.offset_x      = 20;
            cfg.offset_y      = 0;
            cfg.readable      = false;
            _panel_instance.config(cfg);
        }
        setPanel(&_panel_instance);
    }

    bool enableFrameBuffer(bool auto_display = true) {
        if (_panel_instance.initPanelFb()) {
            auto* fb = _panel_instance.getPanelFb();
            if (fb) {
                fb->setBus(&_bus_instance);
                fb->setAutoDisplay(auto_display);
                setPanel(fb);
                return true;
            }
        }
        return false;
    }
};
