#pragma once
#include "serial_port.hpp"
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

// Backs the Serial panel. read() never blocks, and the port closes itself when the board goes away.
class JetSerial : public godot::RefCounted {
    GDCLASS(JetSerial, godot::RefCounted)
public:
    godot::Array list_ports() const;
    godot::String open(const godot::String& path, int baud);
    void close();
    bool is_open() const;
    godot::String port() const;
    godot::PackedByteArray read();
    bool write(const godot::PackedByteArray& data);
    bool reset_board();

private:
    jetstudio::SerialPort port_;

protected:
    static void _bind_methods();
};
