#pragma once
// POSIX termios port for the Serial monitor, kept free of Godot types so the tests can drive it through a pty.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jetstudio {

struct SerialPortInfo {
    std::string path;        // stable path when available (/dev/serial/by-id/...)
    std::string description; // board name from the USB descriptor, or the device
};

class SerialPort {
public:
    SerialPort() = default;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    ~SerialPort();

    static std::vector<SerialPortInfo> list();
    bool open(const std::string& path, int baud, std::string& error);
    void close();
    bool is_open() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }
    // Bytes available now (never blocks): >0 read, 0 nothing, -1 the device went away.
    long read(uint8_t* buffer, size_t size);
    bool write(const uint8_t* data, size_t size);
    // Hard reset through the USB auto-reset lines (RTS pulse with DTR low), as esptool does.
    bool reset_board();

private:
    int fd_ = -1;
    std::string path_;
};

} // namespace jetstudio
