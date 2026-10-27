#include "ground_video.hpp"
#include "video_fanout.hpp"
#include "multicast_egress.hpp"
#include "usb_upstream.hpp"
#include "components/stream_receiver.hpp"
#include "core/rs_encode_scalar.hpp"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdio>

namespace {
constexpr uint16_t multicast_port = 5600;
constexpr const char *multicast_group = "239.255.42.1";
rover::VideoFanout fanout;
std::shared_ptr<winject::UpstreamQueue> upstream;
std::unique_ptr<vstreamer::stream_receiver> receiver;
std::atomic<uint32_t> ethernet{0};
std::atomic<bool> ready{false};
std::atomic<uint64_t> fec_input{0}, recovered_rtp_gaps{0}, recovered_rtp_reordered{0};
std::atomic<uint64_t> feedback_drop{0}, multicast_sent{0}, multicast_offline{0}, multicast_send_errors{0}, multicast_send_retries{0};
std::atomic<int> multicast_last_errno{0};


void multicast_task(void *) {
    rover::MulticastEgress egress;
    int fd = -1;
    uint32_t bound_address = 0;
    sockaddr_in destination{};
    destination.sin_family = AF_INET; destination.sin_port = htons(multicast_port);
    inet_pton(AF_INET, multicast_group, &destination.sin_addr);
    for (;;) {
        const uint32_t address = ethernet.load();
        if (address != bound_address || (address && fd < 0)) {
            if (fd >= 0) close(fd);
            if (egress.reset()) ++multicast_offline;
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
        // Pace recovery bursts to two packets per tick. This still exceeds
        // 10 Mbps while reducing bursts into downstream multicast queues.
        // A transient send failure is retried after yielding, in order.
        for (unsigned i = 0; i < 2; ++i) {
            if (fd < 0) {
                if (!fanout.pop(1)) break;
                ++multicast_offline;
                continue;
            }
            const auto result = egress.step(fanout, [&](const bfc::shared_sized_buffer &packet) {
                const auto sent = sendto(fd, packet.u8(), packet.size(), MSG_DONTWAIT,
                    reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
                if (sent == ssize_t(packet.size())) return 0;
                const int error = sent < 0 ? errno : EIO;
                multicast_last_errno.store(error);
                return error;
            });
            if (result == rover::MulticastEgress::Result::empty) break;
            if (result == rover::MulticastEgress::Result::sent) ++multicast_sent;
            else if (result == rover::MulticastEgress::Result::retry) {
                ++multicast_send_retries;
                break;
            } else ++multicast_send_errors;
        }
        vTaskDelay(pdMS_TO_TICKS(fd < 0 && address ? 100 : 1));
    }
}
void video_task(void *) {
    bool have_rtp = false;
    uint16_t last_sequence = 0;
    uint32_t last_ssrc = 0;
    for (;;) {
        for (unsigned i = 0; i < 64; ++i) {
            vstreamer::component_pdu packet;
            if (receiver->output(packet) < 0) break;
            // This boundary precedes USB and Ethernet. RTP gaps here cannot
            // be attributed to either output transport by a downstream client.
            if (packet.sdu.size() >= 12) {
                const auto *b = packet.sdu.u8();
                const uint16_t sequence = uint16_t(b[2]) << 8 | b[3];
                const uint32_t ssrc = uint32_t(b[8]) << 24 | uint32_t(b[9]) << 16 |
                                      uint32_t(b[10]) << 8 | b[11];
                if (have_rtp && ssrc == last_ssrc) {
                    const int16_t step = uint16_t(sequence - last_sequence);
                    if (step > 1) recovered_rtp_gaps += uint16_t(step - 1);
                    if (step <= 0) ++recovered_rtp_reordered;
                    else last_sequence = sequence;
                } else {
                    have_rtp = true; last_ssrc = ssrc; last_sequence = sequence;
                }
            }
            fanout.push(packet.sdu);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
}

void ground_video_set_ethernet(uint32_t address) { ethernet.store(address); }
esp_err_t ground_video_start(std::shared_ptr<winject::UpstreamQueue> up) {
    if (!up || receiver) return ESP_ERR_INVALID_STATE;
    auto rx = std::make_unique<vstreamer::stream_receiver>();
    if (rx->set_datagram_transport(
            [up, checked = false]() mutable {
                // Run on the already placed receive worker, before video input.
                // PIE first-use may bind a FreeRTOS task to its current core.
                if (!checked) {
                    for (int rows : {1, 2, 10}) {
                        int64_t fast_us = 0, table_us = 0;
                        const int bad = vstreamer::rs_encode_selftest(21, rows, 1403, 2,
                            esp_timer_get_time, &fast_us, &table_us);
                        printf("GROUND_RS_SELFTEST rows=%d mismatches=%d backend=%s fast_us=%lld table_us=%lld\n",
                            rows, bad, vstreamer::rs_encode_scalar_impl(),
                            (long long)fast_us, (long long)table_us);
                    }
                    checked = true;
                }
                auto packet = up->receive(); if (packet) ++fec_input; return packet;
            },
            [up](const uint8_t *bytes, size_t size) {
                const bool sent = up->try_send(bytes, size);
                if (!sent) ++feedback_drop;
                return sent;
            }) < 0 || rx->configure("max_datagram", "1445") < 0 ||
        rx->configure("recv_cpu", "1") < 0 || rx->configure("recv_prio", "13") < 0 || rx->open() < 0) return ESP_FAIL;
    upstream = std::move(up); receiver = std::move(rx);
    auto err = usb_upstream_register_source(0, "video-rtp-h264",
        [] { return fanout.pop(0); }, [] { return upstream->connected(); }, "rtp-h264-v1");
    if (err != ESP_OK) return err;
    if (xTaskCreate(multicast_task, "video_multicast", 4096, nullptr, 15, nullptr) != pdPASS ||
        xTaskCreate(video_task, "video_fanout", 8192, nullptr, 12, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    ready.store(true);
    printf("GROUND_VIDEO format=rtp-h264-v1 multicast=%s:%u ttl=1 receiver=l3-queue\n",
           multicast_group, multicast_port);
    return ESP_OK;
}
void ground_video_log_stats() {
    if (!ready.load()) return;
    std::string stats; receiver->query("stats", &stats);
    std::string given_up, late, rs_errors, spread, jitter;
    receiver->query("fec_given_up_blocks", &given_up);
    receiver->query("fec_late_blocks", &late);
    receiver->query("fec_rs_failures", &rs_errors);
    receiver->query("fec_spread_est_ms", &spread);
    receiver->query("fec_jitter_est_ms", &jitter);
    printf("GROUND_FEC backend=%s given_up=%s late=%s rs_errors=%s spread_ms=%s jitter_ms=%s margin_floor_ms=40\n",
        vstreamer::rs_encode_scalar_impl(), given_up.c_str(), late.c_str(), rs_errors.c_str(), spread.c_str(), jitter.c_str());
    printf("GROUND_VIDEO %s recovered_rtp_gaps=%llu recovered_rtp_reordered=%llu radio_queue_evict=%llu l3_rx=%llu l3_evict=%llu fec_input=%llu feedback_drop=%llu usb_evict=%llu multicast_evict=%llu multicast_sent=%llu multicast_offline=%llu multicast_send_errors=%llu multicast_send_retries=%llu multicast_last_errno=%d\n",
           stats.c_str(), (unsigned long long)recovered_rtp_gaps.load(),
           (unsigned long long)recovered_rtp_reordered.load(), (unsigned long long)upstream->radio_queue_evictions(),
           (unsigned long long)upstream->rx_packets.load(),
           (unsigned long long)upstream->rx_evictions.load(), (unsigned long long)fec_input.load(),
           (unsigned long long)feedback_drop.load(),
           (unsigned long long)fanout.drops(0), (unsigned long long)fanout.drops(1),
           (unsigned long long)multicast_sent.load(), (unsigned long long)multicast_offline.load(),
           (unsigned long long)multicast_send_errors.load(), (unsigned long long)multicast_send_retries.load(),
           multicast_last_errno.load());
}
