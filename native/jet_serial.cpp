#include "jet_serial.hpp"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

namespace {
std::string utf8(const String& s) { const CharString c = s.utf8(); return std::string(c.get_data(), size_t(c.length())); }
String gd(const std::string& s) { return String::utf8(s.data(), int64_t(s.size())); }
} // namespace

void JetSerial::_bind_methods() {
    ClassDB::bind_method(D_METHOD("list_ports"), &JetSerial::list_ports);
    ClassDB::bind_method(D_METHOD("open", "path", "baud"), &JetSerial::open);
    ClassDB::bind_method(D_METHOD("close"), &JetSerial::close);
    ClassDB::bind_method(D_METHOD("is_open"), &JetSerial::is_open);
    ClassDB::bind_method(D_METHOD("port"), &JetSerial::port);
    ClassDB::bind_method(D_METHOD("read"), &JetSerial::read);
    ClassDB::bind_method(D_METHOD("write", "data"), &JetSerial::write);
    ClassDB::bind_method(D_METHOD("reset_board"), &JetSerial::reset_board);
}

// [{path, name}]
Array JetSerial::list_ports() const {
    Array out;
    for (const auto& info : jetstudio::SerialPort::list()) {
        Dictionary item;
        item["path"] = gd(info.path);
        item["name"] = gd(info.description);
        out.push_back(item);
    }
    return out;
}

// Empty string on success, else the reason.
String JetSerial::open(const String& path, int baud) {
    std::string error;
    return port_.open(utf8(path), baud, error) ? String() : gd(error);
}

void JetSerial::close() { port_.close(); }
bool JetSerial::is_open() const { return port_.is_open(); }
String JetSerial::port() const { return gd(port_.path()); }

PackedByteArray JetSerial::read() {
    PackedByteArray out;
    uint8_t buffer[16384];
    // Drain what is available, bounded per call to keep the editor frame short.
    for (int i = 0; i < 16 && port_.is_open(); ++i) {
        const long count = port_.read(buffer, sizeof(buffer));
        if (count < 0) { port_.close(); break; }
        if (count == 0) break;
        const int64_t start = out.size();
        out.resize(start + count);
        std::copy(buffer, buffer + count, out.ptrw() + start);
    }
    return out;
}

bool JetSerial::write(const PackedByteArray& data) { return port_.write(data.ptr(), size_t(data.size())); }
bool JetSerial::reset_board() { return port_.reset_board(); }
