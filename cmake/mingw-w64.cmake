# Windows x86_64 from Linux with MinGW-w64: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# The posix thread model: std::thread and std::mutex need it with older GCCs.
find_program(CMAKE_C_COMPILER NAMES x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# ctest runs the Windows tests through Wine when it is installed.
find_program(WINE NAMES wine64 wine)
if(WINE)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${WINE})
endif()
