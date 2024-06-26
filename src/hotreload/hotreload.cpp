#include <iostream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <dlfcn.h>
#include <signal.h>
#include <condition_variable>
#include <mutex>
#include "arena_allocator.h"
#include "../server/server.h" // Assuming this header includes the ServerState definition

namespace fs = std::filesystem;

typedef int (*FnInit)(ArenaAllocator*, ServerState*);
typedef int (*FnUpdate)(ArenaAllocator*, ServerState*);
typedef void (*FnShutdown)(ArenaAllocator*, ServerState*);

std::condition_variable cv;
std::mutex cv_m;
bool reload_signal = false;

void handle_signal(int sig) {
    if (sig == SIGUSR1) {
        std::unique_lock<std::mutex> lk(cv_m);
        reload_signal = true;
        cv.notify_all();
    }
}

void watch_and_reload(const std::string& dll_path, const std::string& copy_path, ArenaAllocator* allocator, ServerState* state) {
    auto last_write_time = fs::last_write_time(dll_path);
    void* engineLibrary = nullptr;
    FnInit init = nullptr;
    FnUpdate update = nullptr;
    FnShutdown shutdown = nullptr;

    auto load_library = [&]() {
        // Copy the updated DLL
        fs::copy(dll_path, copy_path, fs::copy_options::overwrite_existing);

        // Load the new DLL
        engineLibrary = dlopen(copy_path.c_str(), RTLD_NOW);
        if (!engineLibrary) {
            std::cerr << "Library couldn't be loaded: " << dlerror() << '\n';
            return false;
        }

        // Get function pointers
        init = (FnInit)dlsym(engineLibrary, "init");
        update = (FnUpdate)dlsym(engineLibrary, "update");
        shutdown = (FnShutdown)dlsym(engineLibrary, "server_shutdown");

        if (!init || !update || !shutdown) {
            std::cerr << "Unable to find required functions: " << dlerror() << std::endl;
            dlclose(engineLibrary);
            return false;
        }

        return true;
    };

    // Initial load
    if (!load_library()) {
        return;
    } else{
        // Initialize the new DLL
        if (init(allocator, state) != 0) {
            std::cerr << "Initialization failed" << std::endl;
            dlclose(engineLibrary);
            return;
        }

    }

    while (true) {
        // Wait for signal
        std::unique_lock<std::mutex> lk(cv_m);
        cv.wait(lk, []{ return reload_signal; });

        if (reload_signal) {
            reload_signal = false;

            // Check if the DLL has been updated
            auto current_write_time = fs::last_write_time(dll_path);
            if (current_write_time != last_write_time) {
                std::cout << "DLL updated. Reloading..." << std::endl;
                last_write_time = current_write_time;

                if (engineLibrary) {
                    dlclose(engineLibrary);
                }

                // Load the new DLL
                if (!load_library()) {
                    break;
                }
            }

            // Call the update function
            if (update && update(allocator, state) != 0) {
                std::cerr << "Update failed" << std::endl;
                break;
            }
        }
    }

    // Clean up before exiting
    if (shutdown) {
        shutdown(allocator, state);
    }
    if (engineLibrary) {
        dlclose(engineLibrary);
    }
}

int main() {
    std::string dll_path = "./build/server_lib.so";
    std::string copy_path = "./build/server_lib_copy.so";

    if (!fs::exists(dll_path)) {
        std::cerr << "Original shared library not found: " << dll_path << '\n';
        return -1;
    }

    // Create an arena allocator
    ArenaAllocator allocator(1024 * 1024); // 1 MB arena
    ServerState* state = new ServerState();

    // Write PID to a file
    FILE *pid_file = fopen("/tmp/hotreload.pid", "w");
    if (pid_file) {
        fprintf(pid_file, "%d\n", getpid());
        fclose(pid_file);
    } else {
        perror("Error opening PID file for writing");
        return 1;
    }

    // Set up signal handling
    signal(SIGUSR1, handle_signal);

    printf("Hotreload process running with PID: %d\n", getpid());

    watch_and_reload(dll_path, copy_path, &allocator, state);

    return 0;
}
