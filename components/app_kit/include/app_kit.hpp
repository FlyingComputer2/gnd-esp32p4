// app_kit: named app reactors and the common thread default.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <bfc/default_reactor.hpp>

#include "esp_pthread.h"

namespace app_kit
{

// std::function is intentional: poll_reactor's removal path captures a
// callback inside another callback, which cannot fit in bfc light_function's
// fixed-size storage.
using reactor_t = bfc::default_reactor<std::function<void()>>;
using reactor_tag = bfc::object_tag<reactor_t>;
using reactor_registry = bfc::object_registry<reactor_tag>;
using cancel_fn = std::function<void()>;

constexpr const char* k_apps_reactor = "apps";

enum class stack_in
{
    psram,
    internal,
};

struct thread_spec
{
    const char* name = nullptr;
    size_t stack_bytes = 4 * 1024;
    int prio = 5;
    int core = -1;
    stack_in stack = stack_in::psram;
    bool inherit = false;
};

esp_pthread_cfg_t pthread_cfg(const thread_spec& spec);

class scoped_pthread_cfg
{
public:
    explicit scoped_pthread_cfg(const esp_pthread_cfg_t& cfg);
    explicit scoped_pthread_cfg(const thread_spec& spec) : scoped_pthread_cfg(pthread_cfg(spec)) {}
    ~scoped_pthread_cfg();
    scoped_pthread_cfg(const scoped_pthread_cfg&) = delete;
    scoped_pthread_cfg& operator=(const scoped_pthread_cfg&) = delete;

private:
    esp_pthread_cfg_t prev {};
    bool had_prev;
};

std::thread spawn(const thread_spec& spec, std::function<void()> fn);
bool start_reactor(const std::string& name, const thread_spec& thread);
std::shared_ptr<reactor_t> find_reactor(const std::string& name);
void post(reactor_t& r, std::function<void()> fn);
cancel_fn every(reactor_t& r, uint32_t period_ms, std::function<void()> fn);
// Empty on bind failure; otherwise calling it unregisters and closes the console.
cancel_fn udp_console(reactor_t& r, const std::string& host_port,
                      std::function<std::string(const std::string& line)> handle);
bool parse_host_port(const std::string& s, void* sockaddr_in_out);

}  // namespace app_kit
