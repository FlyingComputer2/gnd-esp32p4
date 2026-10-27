// Guition ground radio: Ethernet/OTA, C6 SDIO proxy, L3 video upstream and USB.
// USB IDs: 0 video, 1 MSP DisplayPort (TBA), 2 MSP RC telemetry (TBA).
#include <inttypes.h>
#include <stdio.h>

#include "WinjectL3.h"
#include "esp_eth.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <vector>
#include <string>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
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
#include <thread>

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


#define ETH_PHY_ADDR 1
#define ETH_PHY_RESET_GPIO 51

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_LOST_IP) { ground_video_set_ethernet(0); return; }
    const auto *ev = static_cast<const ip_event_got_ip_t *>(data);
    ground_video_set_ethernet(ev->ip_info.ip.addr);
    printf("GROUND_IP ip=" IPSTR "\n", IP2STR(&ev->ip_info.ip));
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
    cfg.console_in = "127.0.0.1:2400";
    winject::UpstreamConfig video;
    video.index = 1; // Existing rover video upstream; USB ID is independently 0.
    video.mode = winject::UpstreamMode::queue;
    video.bus_tx = 0xd4;
    video.bus_rx = 0xc3;
    video.rx_queue_capacity = 256;
    video.scheduler_budget = 4096;
    video.fec_type = winject::FecType::none; // vstreamer FEC terminates at the P4 receiver.
    cfg.upstreams.push_back(video);
    cfg.on_upstream_queue = [](size_t index, std::shared_ptr<winject::UpstreamQueue> up)
    {
        if (index == 1)
        {
            ESP_ERROR_CHECK(ground_video_start(std::move(up)));
            ESP_ERROR_CHECK(usb_upstream_start());
            printf("GROUND_READY usb=v2 video=0 msp_displayport=1:tba msp_rc_telemetry=2:tba\n");
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
