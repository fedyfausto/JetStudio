#include "serial_port.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace jetstudio {

SerialPort::~SerialPort() { close(); }

#ifndef _WIN32
namespace {

speed_t speed_of(int baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return 0;
    }
}

std::string resolved(const std::string& path) {
    char buffer[PATH_MAX];
    return realpath(path.c_str(), buffer) ? std::string(buffer) : path;
}

std::vector<std::string> entries(const std::string& folder, const std::string& prefix) {
    std::vector<std::string> out;
    if (DIR* dir = opendir(folder.c_str())) {
        while (const dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.empty() || name[0] == '.' || name.compare(0, prefix.size(), prefix) != 0) continue;
            out.push_back(folder + "/" + name);
        }
        closedir(dir);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// "usb-Espressif_USB_JTAG_serial_debug_unit_DC:54:75-if00" -> "Espressif USB JTAG serial debug unit DC:54:75".
std::string describe(std::string name) {
    if (name.compare(0, 4, "usb-") == 0) name.erase(0, 4);
    const size_t interface = name.rfind("-if");
    if (interface != std::string::npos) name.erase(interface);
    std::replace(name.begin(), name.end(), '_', ' ');
    return name;
}

bool set_lines(int fd, bool dtr, bool rts) {
    int status = 0;
    if (ioctl(fd, TIOCMGET, &status) != 0) return false;
    status = (status & ~(TIOCM_DTR | TIOCM_RTS)) | (dtr ? TIOCM_DTR : 0) | (rts ? TIOCM_RTS : 0);
    return ioctl(fd, TIOCMSET, &status) == 0; // both lines in one request: no reset glitch
}

} // namespace

std::vector<SerialPortInfo> SerialPort::list() {
    std::vector<SerialPortInfo> ports;
    std::vector<std::string> devices;
    // /dev/serial/by-id first: those names survive board resets and say which board it is.
    for (const auto& link : entries("/dev/serial/by-id", "")) {
        ports.push_back({link, describe(link.substr(link.rfind('/') + 1))});
        devices.push_back(resolved(link));
    }
    for (const char* prefix : {"ttyACM", "ttyUSB", "cu.usbmodem", "cu.usbserial"})
        for (const auto& device : entries("/dev", prefix))
            if (std::find(devices.begin(), devices.end(), device) == devices.end())
                ports.push_back({device, device.substr(5)});
    return ports;
}

bool SerialPort::open(const std::string& path, int baud, std::string& error) {
    close();
    const speed_t speed = speed_of(baud);
    if (!speed) { error = "Unsupported speed: " + std::to_string(baud) + " baud."; return false; }
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = path + ": " + std::strerror(errno);
        if (errno == EACCES)
            error += " (add the user to the dialout group: sudo usermod -aG dialout $USER, then log in again)";
        return false;
    }
    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        error = path + ": not a serial port.";
        ::close(fd);
        return false;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        error = path + ": configuration failed (" + std::strerror(errno) + ").";
        ::close(fd);
        return false;
    }
    // Keep DTR and RTS low, like monitor_dtr/monitor_rts = 0 in platformio.ini.
    // Otherwise the USB Serial/JTAG auto-reset logic restarts or halts the board.
    // This fails on pseudo-terminals (no modem lines), which is fine.
    set_lines(fd, false, false);
    fd_ = fd;
    path_ = path;
    return true;
}

void SerialPort::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

long SerialPort::read(uint8_t* buffer, size_t size) {
    if (fd_ < 0) return -1;
    pollfd request{fd_, POLLIN, 0};
    if (poll(&request, 1, 0) < 0) return errno == EINTR ? 0 : -1;
    if (request.revents & (POLLERR | POLLNVAL)) return -1;
    if (request.revents & POLLIN) {
        const ssize_t count = ::read(fd_, buffer, size);
        if (count > 0) return long(count);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        return -1; // end of file or I/O error: unplugged or reset
    }
    return (request.revents & POLLHUP) ? -1 : 0;
}

bool SerialPort::write(const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (fd_ >= 0 && sent < size) {
        const ssize_t count = ::write(fd_, data + sent, size - sent);
        if (count > 0) { sent += size_t(count); continue; }
        if (count < 0 && errno != EAGAIN && errno != EINTR) return false;
        pollfd request{fd_, POLLOUT, 0};
        if (poll(&request, 1, 200) <= 0) return false;
    }
    return sent == size;
}

bool SerialPort::reset_board() {
    if (fd_ < 0 || !set_lines(fd_, false, true)) return false;
    usleep(150 * 1000);
    return set_lines(fd_, false, false);
}

#else // Windows: not implemented yet.

std::vector<SerialPortInfo> SerialPort::list() { return {}; }
bool SerialPort::open(const std::string&, int, std::string& error) {
    error = "Serial monitor not available on Windows yet.";
    return false;
}
void SerialPort::close() {}
long SerialPort::read(uint8_t*, size_t) { return -1; }
bool SerialPort::write(const uint8_t*, size_t) { return false; }
bool SerialPort::reset_board() { return false; }

#endif

} // namespace jetstudio
