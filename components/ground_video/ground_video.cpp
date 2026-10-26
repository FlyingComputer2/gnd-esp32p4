#include "ground_video.hpp"
#include "video_fanout.hpp"
#include "usb_upstream.hpp"
#include "components/stream_receiver.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdio>

namespace {
constexpr uint16_t receiver_port = 21082;
constexpr uint16_t multicast_port = 5600;
constexpr const char *multicast_group = "239.255.42.1";
rover::VideoFanout fanout;
std::shared_ptr<winject::UpstreamQueue> upstream;
std::unique_ptr<vstreamer::stream_receiver> receiver;
std::atomic<uint32_t> ethernet{0};
std::atomic<bool> ready{false};
std::atomic<uint64_t> bridge_drop{0}, feedback_drop{0}, multicast_sent{0}, multicast_drop{0};
int bridge_fd = -1;

void multicast_task(void *) {
    int fd = -1;
    uint32_t bound_address = 0;
    sockaddr_in destination{};
    destination.sin_family = AF_INET; destination.sin_port = htons(multicast_port);
    inet_pton(AF_INET, multicast_group, &destination.sin_addr);
    for (;;) {
        const uint32_t address = ethernet.load();
        if (address != bound_address || (address && fd < 0)) {
            if (fd >= 0) close(fd);
            fd = -1; bound_address = address;
            if (address) {
                fd = socket(AF_INET, SOCK_DGRAM, 0);
                in_addr iface{}; iface.s_addr = address;
                uint8_t ttl = 1, loop = 0;
                if (fd >= 0 && (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) < 0 ||
                    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0 ||
                    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0)) {
                    close(fd); fd = -1;
                }
            }
        }
        for (unsigned i = 0; i < 32; ++i) {
            auto packet = fanout.pop(1);
            if (!packet) break;
            if (fd >= 0 && sendto(fd, packet->u8(), packet->size(), MSG_DONTWAIT,
                reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) == ssize_t(packet->size()))
                ++multicast_sent;
            else ++multicast_drop;
        }
        vTaskDelay(pdMS_TO_TICKS(fd < 0 && address ? 100 : 1));
    }
}
void video_task(void *) {
    sockaddr_in destination{};
    destination.sin_family = AF_INET; destination.sin_port = htons(receiver_port);
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    uint8_t feedback[2048];
    for (;;) {
        // Short bursts keep the lwIP loopback mailbox bounded and let RX/FEC run.
        for (unsigned i = 0; i < 8; ++i) {
            auto packet = upstream->receive();
            if (!packet) break;
            if (sendto(bridge_fd, packet->u8(), packet->size(), MSG_DONTWAIT,
                reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) != ssize_t(packet->size()))
                ++bridge_drop;
        }
        for (unsigned i = 0; i < 64; ++i) {
            vstreamer::component_pdu packet;
            if (receiver->output(packet) < 0) break;
            fanout.push(packet.sdu);
        }
        for (unsigned i = 0; i < 4; ++i) {
            const ssize_t n = recv(bridge_fd, feedback, sizeof(feedback), MSG_DONTWAIT);
            if (n < 0) break;
            if (n && !upstream->try_send(feedback, size_t(n))) ++feedback_drop;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
}

void ground_video_set_ethernet(uint32_t address) { ethernet.store(address); }
esp_err_t ground_video_start(std::shared_ptr<winject::UpstreamQueue> up) {
    if (!up || receiver) return ESP_ERR_INVALID_STATE;
    auto rx = std::make_unique<vstreamer::stream_receiver>();
    if (rx->configure("listen", "127.0.0.1:21082") < 0 ||
        rx->configure("max_datagram", "1445") < 0 ||
        rx->configure("recv_prio", "13") < 0 || rx->open() < 0) return ESP_FAIL;
    bridge_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bridge_fd < 0 || bind(bridge_fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
        if (bridge_fd >= 0) close(bridge_fd);
        bridge_fd = -1; return ESP_FAIL;
    }
    upstream = std::move(up); receiver = std::move(rx);
    auto err = usb_upstream_register_source(0, "video-rtp-h264",
        [] { return fanout.pop(0); }, [] { return upstream->connected(); }, "rtp-h264-v1");
    if (err != ESP_OK) return err;
    if (xTaskCreate(multicast_task, "video_multicast", 4096, nullptr, 10, nullptr) != pdPASS ||
        xTaskCreate(video_task, "video_fanout", 8192, nullptr, 12, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    ready.store(true);
    printf("GROUND_VIDEO format=rtp-h264-v1 multicast=%s:%u ttl=1 receiver=127.0.0.1:%u\n",
           multicast_group, multicast_port, receiver_port);
    return ESP_OK;
}
void ground_video_log_stats() {
    if (!ready.load()) return;
    std::string stats; receiver->query("stats", &stats);
    printf("GROUND_VIDEO %s bridge_drop=%llu feedback_drop=%llu usb_evict=%llu multicast_evict=%llu multicast_sent=%llu multicast_drop=%llu\n",
           stats.c_str(), (unsigned long long)bridge_drop.load(), (unsigned long long)feedback_drop.load(),
           (unsigned long long)fanout.drops(0), (unsigned long long)fanout.drops(1),
           (unsigned long long)multicast_sent.load(), (unsigned long long)multicast_drop.load());
}
