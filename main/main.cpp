// Guition ground radio: Ethernet/OTA, C6 SDIO proxy, L3 and USB.
// L3 upstreams: 0 clock sync (krono SynchServer, UDP console 5110), 1 video,
// 2 bandwidth tester (app_bwtest, UDP console 5100), 3 rover_sample RC /
// telemetry (bridged, see below). Buses as on the rover, swapped: sync a1/b2,
// video d4/c3, tester e5/f6, rover_sample 28/17.
// USB IDs: 0 video, 1 MSP DisplayPort (TBA), 2 MSP RC telemetry (rover_sample),
// 3 sync (krono synch requests from the Android app, answered against this
// board's clock).
#include <inttypes.h>
#include <stdio.h>

#include "WinjectL3.h"
#include "apps/bwtest/Bwtest.h"
#include "synch/SynchGpio.h"
#include "esp_eth.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <vector>
#include <string>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "usb_upstream.hpp"
#include "ground_video.hpp"
#include "winject_proxy.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <bfc/cv_reactor.hpp>
#include <atomic>
#include <deque>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include "utils/NetUtil.h"

static const char *TAG = "rover_ground";
namespace {
constexpr const char *radio_tx_name = "ground-radio-tx";
constexpr const char *radio_rx_name = "ground-radio-rx";
constexpr const char *radio_reactor_name = "ground-radio";
std::shared_ptr<winject::ReactiveLossyQueue> radio_tx, radio_rx;
std::shared_ptr<bfc::cv_reactor<>> radio_reactor;
void *run_radio_reactor(void *) { radio_reactor->run(); return nullptr; }
void print_cpu() {
    struct Previous { TaskHandle_t task; uint32_t runtime; };
    static std::vector<Previous> previous;
    static int64_t previous_us = 0;
    std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 8);
    configRUN_TIME_COUNTER_TYPE total = 0;
    const auto count = uxTaskGetSystemState(tasks.data(), tasks.size(), &total);
    const auto now = esp_timer_get_time();
    const double delta = double(now - previous_us);
    std::vector<Previous> next;
    std::string line;
    for (UBaseType_t i = 0; i < count; ++i) {
        const auto &task = tasks[i];
        const uint32_t runtime = task.ulRunTimeCounter;
        next.push_back({task.xHandle, runtime});
        if (!previous_us) continue;
        for (const auto &old : previous) if (old.task == task.xHandle) {
            const auto percent = 100.0 * uint32_t(runtime - old.runtime) / delta;
            if (percent >= 0.5 || std::string(task.pcTaskName).find("IDLE") == 0) {
                char entry[80];
                snprintf(entry, sizeof(entry), " %s(p%u)=%.1f%%", task.pcTaskName,
                         unsigned(task.uxCurrentPriority), percent);
                line += entry;
            }
            break;
        }
    }
    previous = std::move(next); previous_us = now;
    printf("GROUND_CPU%s\n", line.c_str());
}

// Runs app_bwtest on its upstream's queue. Its sender and receive poller
// inherit this thread's settings: low priority, unpinned, so video wins.
void start_bwtest(std::shared_ptr<winject::UpstreamQueue> up) {
    static bool started = false;
    if (started) {
        ESP_LOGW(TAG, "bwtest upstream replaced; restart to rebind the tester");
        return;
    }
    started = true;
    esp_pthread_cfg_t prev {};
    const bool had_prev = esp_pthread_get_cfg(&prev) == ESP_OK;
    auto cfg = esp_pthread_get_default_config();
    cfg.stack_size = 6 * 1024;
    cfg.prio = 5;
    cfg.thread_name = "bwtest";
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
    std::thread([up = std::move(up)] {
        app_bwtest::Config bw;
        bw.upstream = up;
        bw.console = "0.0.0.0:5100";
        printf("GROUND_BWTEST_EXIT rc=%d\n", app_bwtest::main(bw));
    }).detach();
    if (had_prev) esp_pthread_set_cfg(&prev);
}

// krono's datagram transport over an L3-owned upstream queue.
krono::Transport synch_transport(std::shared_ptr<winject::UpstreamQueue> up) {
    return krono::Transport{
        [up](const uint8_t *data, size_t len) { return up->try_send(data, len); },
        [up](std::vector<uint8_t> *out) {
            auto packet = up->receive();
            if (!packet) return false;
            const auto *p = reinterpret_cast<const uint8_t *>(packet->data());
            out->assign(p, p + packet->size());
            return true;
        }};
}

// Runs the clock-sync server, the reference clock, on its upstream's queue.
// The rover's sync pulse arrives on GPIO20 (JP1 pin 17): active low, Hi-Z at
// rest, pulled up here. Priority 12 so replies are stamped promptly.
constexpr int synch_capture_gpio = 20;
void start_synch(std::shared_ptr<winject::UpstreamQueue> up) {
    static bool started = false;
    if (started) {
        ESP_LOGW(TAG, "synch upstream replaced; restart to rebind the server");
        return;
    }
    started = true;
    esp_pthread_cfg_t prev {};
    const bool had_prev = esp_pthread_get_cfg(&prev) == ESP_OK;
    auto cfg = esp_pthread_get_default_config();
    cfg.stack_size = 6 * 1024;
    cfg.prio = 12;
    cfg.thread_name = "synch";
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
    std::thread([up = std::move(up)] {
        krono::ServerConfig sc;
        sc.transport = synch_transport(up);
        sc.edges = krono::start_gpio_edge_capture(synch_capture_gpio);
        sc.console = "0.0.0.0:5110";
        printf("GROUND_SYNCH_EXIT rc=%d\n", krono::server_main(sc));
    }).detach();
    if (had_prev) esp_pthread_set_cfg(&prev);
}

// USB channel 3 "sync": the Android app runs a SynchClient against this
// board's clock, the same reference the rover follows, so it can turn the
// rover's RTP capture stamps into latency. Requests are answered as they
// arrive, under the USB lock; replies wait for the channel's next DATA slot.
constexpr uint16_t usb_synch_channel = 3;
std::deque<bfc::shared_sized_buffer> usb_synch_replies;
// Fed only through answer(): rtt requests, no wire.
krono::SynchServer usb_synch({});

bool answer_usb_synch(const uint8_t *data, size_t len) {
    uint8_t reply[krono::k_response_bytes];
    const size_t n = usb_synch.answer(data, len, reply);
    if (n) {
        // A client waits for one reply at a time; keep the newest few.
        if (usb_synch_replies.size() >= 4) usb_synch_replies.pop_front();
        usb_synch_replies.push_back(bfc::shared_sized_buffer::copy_from(reply, n));
    }
    return true;
}

void register_usb_synch() {
    const esp_err_t err = usb_upstream_register_service(usb_synch_channel, "sync",
        []() -> std::optional<bfc::shared_sized_buffer> {
            if (usb_synch_replies.empty()) return std::nullopt;
            auto reply = std::move(usb_synch_replies.front());
            usb_synch_replies.pop_front();
            return reply;
        },
        answer_usb_synch,
        [] { return true; }, "synch-v1", 64);
    if (err != ESP_OK) printf("GROUND_USB_SYNCH register failed err=0x%x\n", err);
}

// Upstream 3 (rover_sample): MSP v2 frames, one per datagram, passed through
// unchanged. Up: USB channel 2 "msp-rc" and UDP 5120 (bench tools) go to the
// rover. Down: telemetry goes to USB channel 2 and to the last UDP sender
// (for 10 s after it last sent). The rover follows one RC source at a time.
constexpr uint16_t usb_rc_channel = 2;
constexpr const char *rc_udp_console = "0.0.0.0:5120";
std::mutex rc_mu;
std::shared_ptr<winject::UpstreamQueue> rc_up;  // set by on_upstream_queue
std::deque<bfc::shared_sized_buffer> usb_rc_down;
std::atomic<uint32_t> rc_up_frames{0}, rc_up_drops{0}, rc_down_frames{0};

std::shared_ptr<winject::UpstreamQueue> rc_upstream() {
    std::lock_guard<std::mutex> lock(rc_mu);
    return rc_up;
}

bool rc_send_up(const uint8_t *data, size_t len) {
    auto up = rc_upstream();
    if (up && up->try_send(data, len)) { ++rc_up_frames; return true; }
    ++rc_up_drops;
    return false;
}

void register_usb_rc() {
    const esp_err_t err = usb_upstream_register_service(usb_rc_channel, "msp-rc",
        []() -> std::optional<bfc::shared_sized_buffer> {
            std::lock_guard<std::mutex> lock(rc_mu);
            if (usb_rc_down.empty()) return std::nullopt;
            auto frame = std::move(usb_rc_down.front());
            usb_rc_down.pop_front();
            return frame;
        },
        rc_send_up,
        [] { auto up = rc_upstream(); return up && up->connected(); }, "rc-v1", 64);
    if (err != ESP_OK) printf("GROUND_USB_RC register failed err=0x%x\n", err);
}

void run_rc_bridge(std::shared_ptr<winject::UpstreamQueue> up) {
    sockaddr_in addr{};
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || !winject::parse_host_port(rc_udp_console, &addr) ||
        bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        printf("GROUND_RC_BRIDGE cannot bind %s\n", rc_udp_console);
        return;
    }
    // Short timeout: the same loop drains telemetry every couple of ms.
    timeval timeout{};
    timeout.tv_usec = 2000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in peer{};
    int64_t peer_until = 0;
    uint8_t buf[256];
    for (;;) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
        const int64_t now = esp_timer_get_time();
        if (n > 0) {
            rc_send_up(buf, size_t(n));
            peer = from;
            peer_until = now + 10000000;
        }
        while (auto frame = up->receive()) {
            ++rc_down_frames;
            if (now < peer_until)
                sendto(fd, frame->data(), frame->size(), 0, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
            std::lock_guard<std::mutex> lock(rc_mu);
            // The phone wants the newest telemetry; keep a few.
            if (usb_rc_down.size() >= 8) usb_rc_down.pop_front();
            usb_rc_down.push_back(std::move(*frame));
        }
    }
}

void start_rc_bridge(std::shared_ptr<winject::UpstreamQueue> up) {
    {
        std::lock_guard<std::mutex> lock(rc_mu);
        if (rc_up) {
            ESP_LOGW(TAG, "rover_sample upstream replaced; restart to rebind the bridge");
            return;
        }
        rc_up = up;
    }
    esp_pthread_cfg_t prev {};
    const bool had_prev = esp_pthread_get_cfg(&prev) == ESP_OK;
    auto cfg = esp_pthread_get_default_config();
    cfg.stack_size = 4 * 1024;
    cfg.prio = 10;
    cfg.thread_name = "rc_bridge";
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
    std::thread(run_rc_bridge, std::move(up)).detach();
    if (had_prev) esp_pthread_set_cfg(&prev);
}

void start_radio_queues() {
    radio_tx = std::make_shared<winject::ReactiveLossyQueue>();
    radio_rx = std::make_shared<winject::ReactiveLossyQueue>();
    radio_tx->set_capacity(256);
    radio_rx->set_capacity(256);
    radio_reactor = std::make_shared<bfc::cv_reactor<>>();
    using Registry = bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;
    ESP_ERROR_CHECK(Registry::get_instance().add(radio_tx_name, radio_tx) &&
                    Registry::get_instance().add(radio_rx_name, radio_rx) &&
                    bfc::cv_reactor_registry::get_instance().add(radio_reactor_name, radio_reactor)
                    ? ESP_OK : ESP_FAIL);
    auto cfg = esp_pthread_get_default_config();
    cfg.stack_size = 32768;
    cfg.prio = 14;
    cfg.pin_to_core = 0;
    cfg.thread_name = "ground_radio";
    ESP_ERROR_CHECK(esp_pthread_set_cfg(&cfg));
    pthread_t thread;
    ESP_ERROR_CHECK(pthread_create(&thread, nullptr, run_radio_reactor, nullptr) == 0 ? ESP_OK : ESP_FAIL);
    pthread_detach(thread);
}
}


// The ground's CLOCK_REALTIME is the rover's sync reference (krono synch), so
// it follows the GS host's chrony (the opi5, which decodes the video): rover
// capture stamps then compare directly with the decoder's clock. Smooth mode
// steps the first sync and slews after; lwIP does not compensate round
// trip, so it sits one LAN one-way delay off.
constexpr const char *sntp_server = "192.168.253.106";
constexpr uint32_t sntp_interval_ms = 16000; // RFC 4330 floor is 15 s.

static void on_sntp_sync(struct timeval *tv)
{
    printf("GROUND_SNTP synced server=%s time=%lld.%06ld\n", sntp_server,
           static_cast<long long>(tv->tv_sec), static_cast<long>(tv->tv_usec));
}

static void start_sntp(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(sntp_server);
    cfg.smooth_sync = true;
    cfg.sync_cb = on_sntp_sync;
    sntp_set_sync_interval(sntp_interval_ms);
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        printf("GROUND_SNTP init failed\n");
    }
}

#define ETH_PHY_ADDR 1
#define ETH_PHY_RESET_GPIO 51

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_LOST_IP) { ground_video_set_ethernet(0); return; }
    const auto *ev = static_cast<const ip_event_got_ip_t *>(data);
    ground_video_set_ethernet(ev->ip_info.ip.addr);
    printf("GROUND_IP ip=" IPSTR "\n", IP2STR(&ev->ip_info.ip));
    start_sntp();
    fflush(stdout);
}

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ETHERNET_EVENT_CONNECTED)
    {
        ESP_LOGI(TAG, "Ethernet link up");
    }
    else if (id == ETHERNET_EVENT_DISCONNECTED)
    {
        ground_video_set_ethernet(0);
        ESP_LOGW(TAG, "Ethernet link down");
    }
}

static esp_err_t start_ethernet(void)
{
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(netif, "rover-ground-radio");

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = ETH_PHY_RESET_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    esp_err_t err = esp_eth_driver_install(&eth_cfg, &eth);
    if (err != ESP_OK)
    {
        return err;
    }
    esp_netif_attach(netif, esp_eth_new_netif_glue(eth));
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_got_ip, NULL);
    return esp_eth_start(eth);
}

static void *l3_main(void *)
{
    winject::Config cfg;
    cfg.channel = 1;
    cfg.modulation = "OFDM_24M";
    cfg.power_dbm = 15;
    cfg.domain = 0xB00B;
    cfg.stats_sec = 10;
    cfg.radio_device.mplane.sin_family = AF_INET;
    cfg.radio_device.mplane.sin_port = htons(22);
    cfg.radio_device.mplane.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cfg.radio_device.dplane = cfg.radio_device.mplane;
    cfg.radio_device.dplane.sin_port = htons(9000);
    cfg.radio_device.dplane_tx = radio_tx_name;
    cfg.radio_device.dplane_rx = radio_rx_name;
    cfg.radio_device.reactor = radio_reactor_name;
    cfg.radio_device.fcs = winject::RadioFcsConfig::signal;
    // Reachable over Ethernet for lur/lut during tests; no authentication.
    cfg.console_in = "0.0.0.0:2400";
    winject::UpstreamConfig synch;
    synch.index = 0; // Rover upstream 0 with the buses swapped.
    synch.mode = winject::UpstreamMode::queue;
    synch.bus_tx = 0xa1;
    synch.bus_rx = 0xb2;
    synch.rx_queue_capacity = 16;
    synch.scheduler_budget = 256;
    synch.fec_type = winject::FecType::none; // A lost exchange is a timeout.
    cfg.upstreams.push_back(synch);
    winject::UpstreamConfig video;
    video.index = 1; // USB ID is independently 0.
    video.mode = winject::UpstreamMode::queue;
    video.bus_tx = 0xd4;
    video.bus_rx = 0xc3;
    video.rx_queue_capacity = 256;
    video.scheduler_budget = 4096;
    video.fec_type = winject::FecType::none; // vstreamer FEC terminates at the P4 receiver.
    cfg.upstreams.push_back(video);
    winject::UpstreamConfig bwtest;
    bwtest.index = 2; // Rover upstream 2 with the buses swapped.
    bwtest.mode = winject::UpstreamMode::queue;
    bwtest.bus_tx = 0xe5;
    bwtest.bus_rx = 0xf6;
    bwtest.rx_queue_capacity = 256;
    bwtest.scheduler_budget = 4096;
    bwtest.fec_type = winject::FecType::none; // Clean loss counts.
    cfg.upstreams.push_back(bwtest);
    winject::UpstreamConfig rover_rc;
    rover_rc.index = 3; // Rover upstream 3 (rover_sample) with the buses swapped.
    rover_rc.mode = winject::UpstreamMode::queue;
    rover_rc.bus_tx = 0x28;
    rover_rc.bus_rx = 0x17;
    rover_rc.rx_queue_capacity = 16;
    rover_rc.scheduler_budget = 256;
    rover_rc.fec_type = winject::FecType::none; // RC repeats at 50 Hz; FEC only adds latency.
    cfg.upstreams.push_back(rover_rc);
    cfg.on_upstream_queue = [](size_t index, std::shared_ptr<winject::UpstreamQueue> up)
    {
        if (index == 0)
        {
            start_synch(std::move(up));
        }
        else if (index == 1)
        {
            ESP_ERROR_CHECK(ground_video_start(std::move(up)));
            ESP_ERROR_CHECK(usb_upstream_start());
            printf("GROUND_READY usb=v2 video=0 msp_displayport=1:tba msp_rc_telemetry=2 sync=3\n");
        }
        else if (index == 2)
        {
            start_bwtest(std::move(up));
        }
        else if (index == 3)
        {
            start_rc_bridge(std::move(up));
        }
    };
    int rc = winject_l3::main(cfg);
    printf("GROUND_L3_EXIT rc=%d\n", rc);
    return nullptr;
}

extern "C" void app_main(void)

{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    err = start_ethernet();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Ethernet: %s", esp_err_to_name(err));
    }

    start_radio_queues();
    register_usb_synch();
    register_usb_rc();
    winject_proxy_config_t cfg = WINJECT_PROXY_CONFIG_DEFAULT();
    cfg.dplane_tx_queue = radio_tx_name;
    cfg.dplane_rx_queue = radio_rx_name;
    cfg.dplane_reactor = radio_reactor_name;
    err = winject_proxy_start(&cfg);
    if (err != ESP_OK)
    {
        printf("GROUND_FAILED err=0x%x\n", err);
        return;
    }
    auto pthread_cfg = esp_pthread_get_default_config();
    pthread_cfg.stack_size = 32768;
    pthread_cfg.prio = 12;
    pthread_cfg.thread_name = "ground_l3";
    ESP_ERROR_CHECK(esp_pthread_set_cfg(&pthread_cfg));
    pthread_t l3;
    if (pthread_create(&l3, nullptr, l3_main, nullptr) != 0)
    {
        ESP_LOGE(TAG, "Cannot start L3");
        return;
    }
    pthread_detach(l3);
    fflush(stdout);

    // Periodic counters on the console.
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        winject_proxy_stats_t ps;
        sdio_link_host_stats_t ls;
        winject_proxy_get_stats(&ps);
        usb_upstream_log_stats();
        printf("GROUND_RC up=%" PRIu32 " up_drop=%" PRIu32 " down=%" PRIu32 "\n", rc_up_frames.load(),
               rc_up_drops.load(), rc_down_frames.load());
        const auto us = usb_synch.stats();
        printf("GROUND_USB_SYNCH requests=%llu replies=%llu bad=%llu\n",
               (unsigned long long)us.requests, (unsigned long long)us.replies,
               (unsigned long long)us.bad);
        print_cpu();
        ground_video_log_stats();
        printf("GROUND_HEAP internal=%u internal_largest=%u psram=%u\n",
               unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
               unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        sdio_link_host_get_stats(&ls);
        printf("GROUND_PROXY_STATS mreq=%" PRIu32 " mrep=%" PRIu32 " mdrop=%" PRIu32
               " ddown=%" PRIu32 " ddown_drop=%" PRIu32 " dup=%" PRIu32 " dup_nopeer=%" PRIu32
               " dup_fail=%" PRIu32 " dup_evict=%" PRIu32 " link_rx_bad=%" PRIu32
               " link_rx_err=%" PRIu32 " link_tx_err=%" PRIu32 " link_downs=%" PRIu32
               " link_ups=%" PRIu32 "\n",
               ps.mplane_requests, ps.mplane_replies, ps.mplane_dropped, ps.dplane_down,
               ps.dplane_down_dropped, ps.dplane_up, ps.dplane_up_no_peer, ps.dplane_up_failed,
               ps.dplane_up_evicted, ls.rx_bad, ls.rx_errors, ls.tx_errors, ls.link_downs,
               ls.link_ups);
        fflush(stdout);
    }
}
