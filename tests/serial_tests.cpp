// Serial monitor port on a pseudo-terminal: the master side plays the board.
#include "serial_port.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

// Everything the port receives within `timeout_ms`.
std::string drain(jetstudio::SerialPort& port, size_t expected, int timeout_ms = 1000) {
    std::string text;
    uint8_t buffer[256];
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (text.size() < expected && std::chrono::steady_clock::now() < end) {
        const long count = port.read(buffer, sizeof(buffer));
        if (count < 0) break;
        if (count == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        else text.append(reinterpret_cast<char*>(buffer), size_t(count));
    }
    return text;
}
} // namespace

int main() {
    using jetstudio::SerialPort;
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        std::cout << "SKIP: no pseudo-terminals available\n";
        return 0;
    }
    const std::string device = ptsname(master);
    SerialPort port;
    std::string error;
    check(!port.open("/dev/jet-studio-missing", 115200, error) && !error.empty(), "missing device reports an error");
    check(!port.open(device, 1234, error) && error.find("1234") != std::string::npos, "unsupported speed is rejected");
    check(port.open(device, 115200, error) && port.is_open() && port.path() == device, "open the pseudo-terminal");
    uint8_t byte;
    check(port.read(&byte, 1) == 0, "read never blocks when nothing arrived");

    // Board -> editor, raw bytes (no newline translation, UTF-8 and escapes intact).
    const std::string log = "\x1b[0;32mI (1234) JetStudio: hello è\x1b[0m\r\n";
    check(write(master, log.data(), log.size()) == ssize_t(log.size()), "master writes");
    check(drain(port, log.size()) == log, "log line arrives unchanged");

    // Editor -> board.
    const std::string command = "help\n";
    check(port.write(reinterpret_cast<const uint8_t*>(command.data()), command.size()), "port writes");
    char received[16] = {};
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(read(master, received, sizeof(received)) == ssize_t(command.size()) && command == received, "board receives the text");

    // Unplugging (master closed) is detected and reported as -1.
    close(master);
    long result = 0;
    for (int i = 0; i < 100 && result == 0; ++i) {
        result = port.read(&byte, 1);
        if (result == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(result < 0, "disconnection is detected");
    port.close();
    check(!port.is_open(), "close");
    SerialPort::list(); // must not crash without devices
    if (failures) { std::cerr << failures << " serial test(s) failed\n"; return 1; }
    std::cout << "PASS: serial port open/read/write/disconnect on a pseudo-terminal\n";
}
