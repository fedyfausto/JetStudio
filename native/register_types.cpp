#include "jet_bridge.hpp"
#include "jet_serial.hpp"
#include <godot_cpp/godot.hpp>
using namespace godot;
void initializeJet(ModuleInitializationLevel level){
    if(level!=MODULE_INITIALIZATION_LEVEL_SCENE) return;

    ClassDB::register_class<JetBridge>();
    ClassDB::register_class<JetSerial>();
}
void uninitializeJet(ModuleInitializationLevel){}
extern "C" GDExtensionBool GDE_EXPORT jet_studio_init(
    GDExtensionInterfaceGetProcAddress get_proc_address, GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization){
    GDExtensionBinding::InitObject init(get_proc_address,library,initialization);
    init.register_initializer(initializeJet); init.register_terminator(uninitializeJet);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
