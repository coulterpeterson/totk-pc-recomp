# TotK Recomp: Android skeleton

Vulkan capability probe for the port. Run from `android/`.

Prerequisites: Android SDK (platform 35), NDK r27 (27.0.12077973), CMake 3.22.1, JDK 17, Gradle 8.9+.

SDL3 source goes in `app/jni/SDL` and is installed for arm64-v8a:

```
git clone --depth 1 --branch release-3.4.18 https://github.com/libsdl-org/SDL app/jni/SDL
cmake -S app/jni/SDL -B build/sdl -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_INSTALL_PREFIX=$PWD/build/sdl-install
cmake --build build/sdl && cmake --install build/sdl
export CMAKE_PREFIX_PATH=$PWD/build/sdl-install
./gradlew assembleDebug
```
