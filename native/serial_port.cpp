#include "serial_port.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <devguid.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace jetstudio{

    SerialPort::~SerialPort(){ close(); }

#ifndef _WIN32
    namespace{

        speed_t speedOf(int baud){
            switch(baud){
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

        std::string resolved(const std::string& path){
            char buffer[PATH_MAX];
            return realpath(path.c_str(), buffer) ? std::string(buffer) : path;
        }

        std::vector<std::string> entries(const std::string& folder, const std::string& prefix){
            std::vector<std::string> out;

            if(DIR* dir = opendir(folder.c_str())){
                while(const dirent* entry = readdir(dir)){
                    const std::string name = entry->d_name;

                    if(name.empty() || name[0] == '.' || name.compare(0, prefix.size(), prefix) != 0) continue;

                    out.push_back(folder + "/" + name);
                }

                closedir(dir);
            }

            std::sort(out.begin(), out.end());
            return out;
        }

        // "usb-Espressif_USB_JTAG_serial_debug_unit_DC:54:75-if00" -> "Espressif USB JTAG serial debug unit DC:54:75".
        std::string describe(std::string name){
            if(name.compare(0, 4, "usb-") == 0) name.erase(0, 4);

            const size_t interface = name.rfind("-if");

            if(interface != std::string::npos) name.erase(interface);

            std::replace(name.begin(), name.end(), '_', ' ');
            return name;
        }

        bool setLines(int fd, bool dtr, bool rts){
            int status = 0;

            if(ioctl(fd, TIOCMGET, &status) != 0) return false;

            status = (status & ~(TIOCM_DTR | TIOCM_RTS)) | (dtr ? TIOCM_DTR : 0) | (rts ? TIOCM_RTS : 0);
            //both lines in one request: no reset glitch
            return ioctl(fd, TIOCMSET, &status) == 0;
        }

    }

    std::vector<SerialPortInfo> SerialPort::list(){
        std::vector<SerialPortInfo> ports;
        std::vector<std::string> devices;
        //by-id names first: they survive board resets and say which board it is.
        for(const auto& link : entries("/dev/serial/by-id", "")){
            ports.push_back({link, describe(link.substr(link.rfind('/') + 1))});
            devices.push_back(resolved(link));
        }

        for(const char* prefix : {"ttyACM", "ttyUSB", "cu.usbmodem", "cu.usbserial"})
            for(const auto& device : entries("/dev", prefix))
                if(std::find(devices.begin(), devices.end(), device) == devices.end())
                    ports.push_back({device, device.substr(5)});
        return ports;
    }

    bool SerialPort::open(const std::string& path, int baud, std::string& error){
        close();
        const speed_t speed = speedOf(baud);

        if(!speed){ error = "Unsupported speed: " + std::to_string(baud) + " baud."; return false; }
        const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

        if(fd < 0){
            error = path + ": " + std::strerror(errno);

            if(errno == EACCES)
                error += " (add the user to the dialout group: sudo usermod -aG dialout $USER, then log in again)";
            return false;
        }

        termios tio{};

        if(tcgetattr(fd, &tio) != 0){
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

        if(tcsetattr(fd, TCSANOW, &tio) != 0){
            error = path + ": configuration failed (" + std::strerror(errno) + ").";
            ::close(fd);
            return false;
        }

        //keep DTR and RTS low, like monitor_dtr/monitor_rts = 0 in platformio.ini.
        //otherwise the USB Serial/JTAG auto-reset logic restarts or halts the board.
        //this fails on pseudo-terminals (no modem lines), which is fine.
        setLines(fd, false, false);
        fd_ = fd;
        path_ = path;
        return true;
    }

    void SerialPort::close(){
        if(fd_ >= 0) ::close(fd_);

        fd_ = -1;
    }

    long SerialPort::read(uint8_t* buffer, size_t size){
        if(fd_ < 0) return -1;

        pollfd request{fd_, POLLIN, 0};

        if(poll(&request, 1, 0) < 0) return errno == EINTR ? 0 : -1;

        if(request.revents & (POLLERR | POLLNVAL)) return -1;

        if(request.revents & POLLIN){
            const ssize_t count = ::read(fd_, buffer, size);

            if(count > 0) return long(count);

            if(count < 0 && (errno == EAGAIN || errno == EINTR)) return 0;

            //end of file or I/O error: unplugged or reset
            return -1;
        }

        return (request.revents & POLLHUP) ? -1 : 0;
    }

    bool SerialPort::write(const uint8_t* data, size_t size){
        size_t sent = 0;

        while(fd_ >= 0 && sent < size){
            const ssize_t count = ::write(fd_, data + sent, size - sent);

            if(count > 0){ sent += size_t(count); continue; }

            if(count < 0 && errno != EAGAIN && errno != EINTR) return false;

            pollfd request{fd_, POLLOUT, 0};

            if(poll(&request, 1, 200) <= 0) return false;
        }

        return sent == size;
    }

    bool SerialPort::reset_board(){
        if(fd_ < 0 || !setLines(fd_, false, true)) return false;

        usleep(150 * 1000);
        return setLines(fd_, false, false);
    }

#else

    namespace{

        //the A functions answer in the system code page, Godot reads UTF-8
        std::string utf8Of(const wchar_t* text){
            char out[1024] = {};
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof(out) - 1, nullptr, nullptr);
            return out;
        }

        std::string lastError(){
            wchar_t text[256] = {};
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0, text, 255, nullptr);
            std::string out = utf8Of(text);

            while(!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.')) out.pop_back();

            return out;
        }

        std::string textProperty(HDEVINFO devices, SP_DEVINFO_DATA& device, DWORD property){
            wchar_t text[512] = {};

            if(!SetupDiGetDeviceRegistryPropertyW(devices, &device, property, nullptr, reinterpret_cast<BYTE*>(text), sizeof(text) - sizeof(wchar_t), nullptr))
                return {};

            return utf8Of(text);
        }

        // What the board says about itself over USB ("USB JTAG/serial debug unit"), as UTF-8.
        std::string busDescription(HDEVINFO devices, SP_DEVINFO_DATA& device){
            wchar_t text[256] = {};
            DEVPROPTYPE type = 0;

            if(!SetupDiGetDevicePropertyW(devices, &device, &DEVPKEY_Device_BusReportedDeviceDesc, &type, reinterpret_cast<BYTE*>(text), sizeof(text) - sizeof(wchar_t), nullptr, 0))
                return {};

            return utf8Of(text);
        }

        std::string vendorOf(const std::string& hardwareId){
            const size_t at = hardwareId.find("VID_");
            const std::string vid = at == std::string::npos ? "" : hardwareId.substr(at + 4, 4);

            if(vid == "303A") return "Espressif";

            if(vid == "0483") return "STMicroelectronics";

            if(vid == "2341") return "Arduino";

            return {};
        }

    }

    // The Ports class lists the COM ports. Boards on Windows' own USB serial driver (usbser) all get the same
    // localized name, so they show what they report over USB instead, as the Linux names do.
    std::vector<SerialPortInfo> SerialPort::list(){
        std::vector<SerialPortInfo> ports;
        HDEVINFO devices = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);

        if(devices == INVALID_HANDLE_VALUE) return ports;

        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);

        for(DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &device); ++i){
            char name[64] = {};
            HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);

            if(key == INVALID_HANDLE_VALUE) continue;

            DWORD size = sizeof(name) - 1, type = 0;
            const bool found = RegQueryValueExA(key, "PortName", nullptr, &type, reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS;
            RegCloseKey(key);

            //LPT ports
            if(!found || std::strncmp(name, "COM", 3) != 0) continue;

            std::string description = textProperty(devices, device, SPDRP_FRIENDLYNAME);
            const size_t suffix = description.rfind(" (COM");

            if(suffix != std::string::npos) description.erase(suffix);

            const std::string reported = busDescription(devices, device);

            if(_stricmp(textProperty(devices, device, SPDRP_SERVICE).c_str(), "usbser") == 0 && !reported.empty()){
                const std::string vendor = vendorOf(textProperty(devices, device, SPDRP_HARDWAREID));
                description = vendor.empty() ? reported : vendor + " " + reported;
            }

            ports.push_back({name, description.empty() ? std::string(name) : description});
        }

        SetupDiDestroyDeviceInfoList(devices);
        std::sort(ports.begin(), ports.end(), [](const SerialPortInfo& a, const SerialPortInfo& b){
            return a.path.size() != b.path.size() ? a.path.size() < b.path.size() : a.path < b.path;
        });

        return ports;
    }

    bool SerialPort::open(const std::string& path, int baud, std::string& error){
        close();
        //COM10 and above only open through the device namespace.
        const std::string device = path.compare(0, 4, "\\\\.\\") == 0 ? path : "\\\\.\\" + path;
        HANDLE handle = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

        if(handle == INVALID_HANDLE_VALUE){
            const DWORD code = GetLastError();
            error = path + ": " + lastError();

            if(code == ERROR_ACCESS_DENIED) error += " (another program is using the port)";

            return false;
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);

        if(!GetCommState(handle, &dcb)){
            error = path + ": not a serial port.";
            CloseHandle(handle);
            return false;
        }

        dcb.BaudRate = DWORD(baud);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        //DTR and RTS low, as on Linux: otherwise the USB Serial/JTAG auto-reset restarts or halts the board.
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;

        if(!SetCommState(handle, &dcb)){
            error = path + ": configuration failed (" + lastError() + ").";
            CloseHandle(handle);
            return false;
        }

        //reads return at once with whatever has arrived.
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.WriteTotalTimeoutConstant = 200;
        SetCommTimeouts(handle, &timeouts);
        handle_ = handle;
        path_ = path;
        return true;
    }

    void SerialPort::close(){
        if(handle_) CloseHandle(static_cast<HANDLE>(handle_));

        handle_ = nullptr;
    }

    long SerialPort::read(uint8_t* buffer, size_t size){
        if(!handle_) return -1;

        DWORD errors = 0;
        COMSTAT status{};

        //unplugged
        if(!ClearCommError(static_cast<HANDLE>(handle_), &errors, &status)) return -1;

        if(status.cbInQue == 0) return 0;

        DWORD count = 0;
        const DWORD wanted = DWORD(std::min<size_t>(size, status.cbInQue));

        if(!ReadFile(static_cast<HANDLE>(handle_), buffer, wanted, &count, nullptr)) return -1;

        return long(count);
    }

    bool SerialPort::write(const uint8_t* data, size_t size){
        size_t sent = 0;

        while(handle_ && sent < size){
            DWORD count = 0;

            if(!WriteFile(static_cast<HANDLE>(handle_), data + sent, DWORD(size - sent), &count, nullptr) || count == 0) return false;

            sent += count;
        }

        return sent == size;
    }

    // usbser.sys only sends a line change when DTR changes, so DTR is set again after RTS (esptool does the same).
    bool SerialPort::reset_board(){
        HANDLE handle = static_cast<HANDLE>(handle_);

        if(!handle || !EscapeCommFunction(handle, SETRTS) || !EscapeCommFunction(handle, CLRDTR)) return false;

        Sleep(150);
        return EscapeCommFunction(handle, CLRRTS) && EscapeCommFunction(handle, CLRDTR);
    }

#endif

}
