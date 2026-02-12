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
#include <fcntl.h>
#include <array>
#include <link.h>
#include <string>

// ЖЕЛЕЗОБЕТОННЫЙ МЕТОД 1: Поиск через /proc/self/maps с полным путем
void* find_library_via_maps(const char* lib_name) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        LOGE("Failed to open /proc/self/maps");
        return nullptr;
    }
    
    char line[1024];
    void* handle = nullptr;
    
    while (fgets(line, sizeof(line), maps)) {
        // Ищем библиотеку по имени
        char* lib_path = strstr(line, lib_name);
        if (!lib_path) continue;
        
        // Проверяем что это исполняемая секция (r-xp)
        if (!strstr(line, "r-xp")) continue;
        
        // Извлекаем полный путь (после последнего пробела)
        char* path_start = strrchr(line, ' ');
        if (!path_start) continue;
        path_start++; // Пропускаем пробел
        
        // Убираем перенос строки
        size_t len = strlen(path_start);
        if (len > 0 && path_start[len-1] == '\n') {
            path_start[len-1] = '\0';
        }
        
        LOGI("Found library in maps: %s", path_start);
        
        // Пробуем открыть по полному пути
        handle = dlopen(path_start, RTLD_NOW | RTLD_NOLOAD);
        if (handle) {
            LOGI("dlopen success via maps: %s -> %p", path_start, handle);
            fclose(maps);
            return handle;
        }
        
        // Если RTLD_NOLOAD не сработал, пробуем обычный dlopen
        handle = dlopen(path_start, RTLD_NOW);
        if (handle) {
            LOGI("dlopen success (force) via maps: %s -> %p", path_start, handle);
            fclose(maps);
            return handle;
        }
    }
    
    fclose(maps);
    return nullptr;
}

// ЖЕЛЕЗОБЕТОННЫЙ МЕТОД 2: Поиск через dl_iterate_phdr
static void* g_found_handle = nullptr;
static const char* g_target_lib = nullptr;

static int dl_iterate_callback(struct dl_phdr_info *info, size_t size, void *data) {
    if (!info->dlpi_name || strlen(info->dlpi_name) == 0) {
        return 0;
    }
    
    // Проверяем совпадение имени библиотеки
    const char* lib_name = strrchr(info->dlpi_name, '/');
    if (lib_name) lib_name++;
    else lib_name = info->dlpi_name;
    
    if (strstr(lib_name, g_target_lib)) {
        LOGI("dl_iterate found: %s (base: %p)", info->dlpi_name, (void*)info->dlpi_addr);
        
        // Пробуем открыть
        void* handle = dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD);
        if (!handle) {
            handle = dlopen(info->dlpi_name, RTLD_NOW);
        }
        
        if (handle) {
            g_found_handle = handle;
            return 1; // Остановить итерацию
        }
    }
    
    return 0;
}

void* find_library_via_dl_iterate(const char* lib_name) {
    g_found_handle = nullptr;
    g_target_lib = lib_name;
    
    dl_iterate_phdr(dl_iterate_callback, nullptr);
    
    return g_found_handle;
}

// ЖЕЛЕЗОБЕТОННЫЙ МЕТОД 3: Комбинированный поиск
void* find_library_ultimate(const char* lib_name) {
    void* handle = nullptr;
    
    // Метод 1: xdl_open (быстрый)
    handle = xdl_open(lib_name, 0);
    if (handle) {
        LOGI("xdl_open found: %s", lib_name);
        return handle;
    }
    
    handle = xdl_open(lib_name, XDL_TRY_FORCE_LOAD);
    if (handle) {
        LOGI("xdl_open (force) found: %s", lib_name);
        return handle;
    }
    
    // Метод 2: /proc/self/maps (надежный)
    handle = find_library_via_maps(lib_name);
    if (handle) {
        LOGI("maps method found: %s", lib_name);
        return handle;
    }
    
    // Метод 3: dl_iterate_phdr (универсальный)
    handle = find_library_via_dl_iterate(lib_name);
    if (handle) {
        LOGI("dl_iterate found: %s", lib_name);
        return handle;
    }
    
    // Метод 4: dlopen напрямую (последняя попытка)
    handle = dlopen(lib_name, RTLD_NOW | RTLD_NOLOAD);
    if (handle) {
        LOGI("dlopen (direct NOLOAD) found: %s", lib_name);
        return handle;
    }
    
    return nullptr;
}

void hack_start(const char *game_data_dir) {
    bool load = false;
    // Brawl Stars uses libg.so instead of libil2cpp.so
    const char* lib_names[] = {"libg.so", "libapp.so", "libil2cpp.so", nullptr};
    
    LOGI("Starting library search with ULTIMATE method...");
    
    // Увеличиваем время ожидания - библиотека может загружаться позже
    for (int i = 0; i < 50; i++) {  // 50 попыток (100 секунд максимум)
        for (int j = 0; lib_names[j] != nullptr; j++) {
            // ЖЕЛЕЗОБЕТОННЫЙ комбинированный поиск
            void *handle = find_library_ultimate(lib_names[j]);
            
            if (handle) {
                load = true;
                LOGI("SUCCESS! Found IL2CPP library: %s (handle: %p)", lib_names[j], handle);
                il2cpp_api_init(handle);
                il2cpp_dump(game_data_dir);
                LOGI("Dump completed for: %s", lib_names[j]);
                return;
            }
        }
        
        if (i % 5 == 0) {
            LOGI("Library search attempt %d/50...", i);
        }
        
        sleep(2);  // 2 секунды между попытками
    }
    
    if (!load) {
        LOGE("FAILED: IL2CPP library (libg.so/libapp.so/libil2cpp.so) not found after 50 attempts in thread %d", gettid());
        
        // Последняя попытка - выводим все загруженные библиотеки для отладки
        LOGI("Debug: Listing all loaded libraries...");
        FILE* maps = fopen("/proc/self/maps", "r");
        if (maps) {
            char line[1024];
            int count = 0;
            while (fgets(line, sizeof(line), maps) && count < 20) {
                if (strstr(line, ".so") && strstr(line, "r-xp")) {
                    LOGI("Loaded lib: %s", line);
                    count++;
                }
            }
            fclose(maps);
        }
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
        dlclose(nb);
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
