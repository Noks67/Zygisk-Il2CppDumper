//
// Created by Perfare on 2020/7/4.
//

#include "hack.h"
#include "il2cpp_dump.h"
#include "log.h"
#include "xdl.h"
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <sys/system_properties.h>
#include <dlfcn.h>
#include <jni.h>
#include <thread>
#include <sys/mman.h>
#include <linux/unistd.h>
#include <array>

void hack_start(const char *game_data_dir) {
    bool load = false;
    // Brawl Stars uses libg.so instead of libil2cpp.so
    const char* lib_names[] = {"libg.so", "libapp.so", "libil2cpp.so", nullptr};
    
    for (int i = 0; i < 10; i++) {
        for (int j = 0; lib_names[j] != nullptr; j++) {
            void *handle = xdl_open(lib_names[j], 0);
            if (handle) {
                load = true;
                il2cpp_api_init(handle);
                il2cpp_dump(game_data_dir);
                LOGI("Found IL2CPP library: %s", lib_names[j]);
                return;
            }
        }
        sleep(1);
    }
    if (!load) {
        LOGI("IL2CPP library (libg.so/libapp.so/libil2cpp.so) not found in thread %d", gettid());
    }
}

std::string GetLibDir(JavaVM *vms) {
    JNIEnv *env = nullptr;
    vms->AttachCurrentThread(&env, nullptr);
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    jmethodID currentApplication = env->GetStaticMethodID(activityThread, "currentApplication", "()Landroid/app/Application;");
    jobject application = env->CallStaticObjectMethod(activityThread, currentApplication);
    jclass applicationClass = env->GetObjectClass(application);
    jmethodID getApplicationInfo = env->GetMethodID(applicationClass, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jobject applicationInfo = env->CallObjectMethod(application, getApplicationInfo);
    jclass applicationInfoClass = env->GetObjectClass(applicationInfo);
    jfieldID nativeLibraryDir = env->GetFieldID(applicationInfoClass, "nativeLibraryDir", "Ljava/lang/String;");
    jstring nativeLibraryDirString = (jstring)env->GetObjectField(applicationInfo, nativeLibraryDir);
    const char *nativeLibraryDirChars = env->GetStringUTFChars(nativeLibraryDirString, nullptr);
    std::string result(nativeLibraryDirChars);
    env->ReleaseStringUTFChars(nativeLibraryDirString, nativeLibraryDirChars);
    return result;
}

std::string GetNativeBridgeLibrary() {
    char value[PROP_VALUE_MAX];
    __system_property_get("ro.dalvik.vm.native.bridge", value);
    if (strlen(value) == 0) {
        return {};
    }
    std::array<char, PATH_MAX> path{};
    snprintf(path.data(), PATH_MAX, "/system/lib%s/houdini.so", sizeof(void *) == 8 ? "64" : "");
    if (access(path.data(), F_OK) == 0) {
        return {path.data()};
    }
    snprintf(path.data(), PATH_MAX, "/system/lib%s/libhoudini.so", sizeof(void *) == 8 ? "64" : "");
    if (access(path.data(), F_OK) == 0) {
        return {path.data()};
    }
    return {value};
}

struct NativeBridgeCallbacks {
    uint32_t version;
    void *initialize;

    void *(*loadLibrary)(const char *libpath, int flag);

    void *(*getTrampoline)(void *handle, const char *name, const char *shorty, uint32_t len);

    void *isSupported;
    void *getAppEnv;
    void *isCompatibleWith;
    void *getSignalHandler;
    void *unloadLibrary;
    void *getError;
    void *isPathSupported;
    void *initAnonymousNamespace;
    void *createNamespace;
    void *linkNamespaces;

    void *(*loadLibraryExt)(const char *libpath, int flag, void *ns);
};

bool NativeBridgeLoad(const char *game_data_dir, int api_level, void *data, size_t length) {
    //TODO 等待houdini初始化
    sleep(5);

    auto libart = dlopen("libart.so", RTLD_NOW);
    auto JNI_GetCreatedJavaVMs = (jint (*)(JavaVM **, jsize, jsize *)) dlsym(libart,
                                                                             "JNI_GetCreatedJavaVMs");
    LOGI("JNI_GetCreatedJavaVMs %p", JNI_GetCreatedJavaVMs);
    JavaVM *vms_buf[1];
    JavaVM *vms;
    jsize num_vms;
    jint status = JNI_GetCreatedJavaVMs(vms_buf, 1, &num_vms);
    if (status == JNI_OK && num_vms > 0) {
        vms = vms_buf[0];
    } else {
        LOGE("GetCreatedJavaVMs error");
        return false;
    }

    auto lib_dir = GetLibDir(vms);
    if (lib_dir.empty()) {
        LOGE("GetLibDir error");
        return false;
    }
    if (lib_dir.find("/lib/x86") != std::string::npos) {
        LOGI("no need NativeBridge");
        munmap(data, length);
        return false;
    }

    auto nb = dlopen("libhoudini.so", RTLD_NOW);
    if (!nb) {
        auto native_bridge = GetNativeBridgeLibrary();
        LOGI("native bridge: %s", native_bridge.data());
        nb = dlopen(native_bridge.data(), RTLD_NOW);
    }
    if (nb) {
        LOGI("nb %p", nb);
        auto NativeBridgeItf = (NativeBridgeCallbacks *) dlsym(nb, "NativeBridgeItf");
        if (NativeBridgeItf && NativeBridgeItf->version >= 2) {
            LOGI("NativeBridgeItf version %d", NativeBridgeItf->version);
            auto handle = NativeBridgeItf->loadLibrary("libil2cpp.so", RTLD_NOW);
            if (handle) {
                LOGI("loadLibrary libil2cpp.so %p", handle);
                il2cpp_api_init(handle);
                il2cpp_dump(game_data_dir);
                munmap(data, length);
                return true;
            } else {
                // Try libg.so and libapp.so for Brawl Stars
                handle = NativeBridgeItf->loadLibrary("libg.so", RTLD_NOW);
                if (handle) {
                    LOGI("loadLibrary libg.so %p", handle);
                    il2cpp_api_init(handle);
                    il2cpp_dump(game_data_dir);
                    munmap(data, length);
                    return true;
                }
                handle = NativeBridgeItf->loadLibrary("libapp.so", RTLD_NOW);
                if (handle) {
                    LOGI("loadLibrary libapp.so %p", handle);
                    il2cpp_api_init(handle);
                    il2cpp_dump(game_data_dir);
                    munmap(data, length);
                    return true;
                }
            }
        }
        close(nb);
    } else {
        LOGI("dlopen native bridge error");
        auto fd = open("/proc/self/maps", O_RDONLY);
        if (fd >= 0) {
            char line[1024];
            while (read(fd, line, sizeof(line)) > 0) {
                if (strstr(line, "libhoudini") || strstr(line, "libnb")) {
                    LOGI("maps: %s", line);
                }
            }
            close(fd);
        }
    }
    return false;
}

void hack_prepare(const char *game_data_dir, void *data, size_t length) {
    LOGI("hack thread: %d", gettid());
    int api_level = android_get_device_api_level();
    LOGI("api level: %d", api_level);

#if defined(__i386__) || defined(__x86_64__)
    if (!NativeBridgeLoad(game_data_dir, api_level, data, length)) {
#endif
        hack_start(game_data_dir);
#if defined(__i386__) || defined(__x86_64__)
    }
#endif
}

#if defined(__arm__) || defined(__aarch64__)

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    auto game_data_dir = (const char *) reserved;
    std::thread hack_thread(hack_start, game_data_dir);
    hack_thread.detach();
    return JNI_VERSION_1_6;
}

#endif
