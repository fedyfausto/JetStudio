#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Serial port for the Serial monitor (termios, or Win32 COM ports), free of Godot types so the tests can drive it through a pty.
namespace jetstudio{

    struct SerialPortInfo{
        // stable path when available (/dev/serial/by-id/...), COM3 on Windows
        std::string path;
        // board name from the USB descriptor, or the device
        std::string description;
    };

    class SerialPort{
        public:
            SerialPort() = default;
            SerialPort(const SerialPort&) = delete;
            SerialPort& operator=(const SerialPort&) = delete;
            ~SerialPort();

            static std::vector<SerialPortInfo> list();
            bool open(const std::string& path, int baud, std::string& error);
            void close();
#ifdef _WIN32
            bool is_open() const{ return handle_ != nullptr; }
#else
            bool is_open() const{ return fd_ >= 0; }
#endif
            const std::string& path() const{ return path_; }
            // Bytes available now (never blocks): >0 read, 0 nothing, -1 the device went away.
            long read(uint8_t* buffer, size_t size);
            bool write(const uint8_t* data, size_t size);
            // Hard reset through the USB auto-reset lines (RTS pulse with DTR low), as esptool does.
            bool reset_board();

        private:
#ifdef _WIN32
            void* handle_ = nullptr;
#else
            int fd_ = -1;
#endif
            std::string path_;
    };

}
