#pragma once
#include <Arduino.h>
#include <algorithm>
#include <vector>

// Write-only software SPI for LovyanGFX, for displays wired to pins that no SPI peripheral drives
// (MOSI on D12 of a Nucleo-144, for one). tools/hardware.py picks it only for those pins.
class SoftSpiBus : public lgfx::IBus{
    public:
        struct config_t{
            // STM32 PinName values
            int16_t pin_sclk = -1, pin_mosi = -1, pin_miso = -1, pin_dc = -1;
        };
        const config_t& config() const{ return cfg_; }
        void config(const config_t& c){ cfg_ = c; }

        lgfx::bus_type_t busType() const override{ return lgfx::bus_type_t::bus_spi; }
        bool init() override{
            sck_ = output(cfg_.pin_sclk, sckMask_);
            mosi_ = output(cfg_.pin_mosi, mosiMask_);
            dc_ = output(cfg_.pin_dc, dcMask_);
            *sck_ = sckMask_ << 16;
            return true;
        }
        void release() override{}
        void beginTransaction() override{}
        void endTransaction() override{ dc(true); }
        void wait() override{}
        bool busy() const override{ return false; }
        void initDMA() override{}
        void addDMAQueue(const uint8_t* data, uint32_t length) override{ writeBytes(data, length, true, false); }
        void execDMAQueue() override{}
        uint8_t* getDMABuffer(uint32_t length) override{ scratch_.resize(length); return scratch_.data(); }
        void flush() override{}

        bool writeCommand(uint32_t data, uint_fast8_t bit_length) override{
            dc(false);

            for(uint_fast8_t n = bit_length >> 3; n; --n, data >>= 8) put(uint8_t(data));

            return true;
        }
        void writeData(uint32_t data, uint_fast8_t bit_length) override{
            dc(true);

            for(uint_fast8_t n = bit_length >> 3; n; --n, data >>= 8) put(uint8_t(data));
        }
        void writeDataRepeat(uint32_t data, uint_fast8_t bit_length, uint32_t count) override{
            while(count--) writeData(data, bit_length);
        }
        void writePixels(lgfx::pixelcopy_t* pc, uint32_t length) override{
            const uint32_t bytes = pc->dst_bits >> 3;
            uint8_t buffer[96];

            while(length){
                const uint32_t n = std::min<uint32_t>(length, sizeof(buffer) / bytes);
                pc->fp_copy(buffer, 0, n, pc);
                writeBytes(buffer, n * bytes, true, false);
                length -= n;
            }
        }
        void writeBytes(const uint8_t* data, uint32_t length, bool dataMode, bool) override{
            dc(dataMode);

            while(length--) put(*data++);
        }

        void beginRead() override{}
        void endRead() override{}
        uint32_t readData(uint_fast8_t) override{ return 0; }
        bool readBytes(uint8_t*, uint32_t, bool) override{ return false; }
        void readPixels(void*, lgfx::pixelcopy_t*, uint32_t) override{}

    private:
        // BSRR: the low half sets pins, the high half clears them, so every edge is a single store.
        static volatile uint32_t* output(int16_t pin, uint32_t& mask){
            const PinName name = PinName(pin);
            pinMode(pinNametoDigitalPin(name), OUTPUT);
            GPIO_TypeDef* port = get_GPIO_Port(STM_PORT(name));
            //very high speed: clean edges at this rate
            port->OSPEEDR |= 3u << (STM_PIN(name) * 2);
            mask = 1u << STM_PIN(name);
            return &port->BSRR;
        }
        void dc(bool data){ *dc_ = data ? dcMask_ : dcMask_ << 16; }
        // SPI mode 0, MSB first: the panel samples MOSI on the rising edge of SCK.
        void put(uint8_t b){
            for(int i = 0; i < 8; ++i, b <<= 1){
                *mosi_ = (b & 0x80) ? mosiMask_ : mosiMask_ << 16;
                *sck_ = sckMask_;
                *sck_ = sckMask_ << 16;
            }
        }

        config_t cfg_;
        volatile uint32_t *sck_ = nullptr, *mosi_ = nullptr, *dc_ = nullptr;
        uint32_t sckMask_ = 0, mosiMask_ = 0, dcMask_ = 0;
        std::vector<uint8_t> scratch_;
};
