#include "ground_video.hpp"
#include "video_fanout.hpp"
#include "multicast_egress.hpp"
#include "usb_upstream.hpp"
#include "components/stream_receiver.hpp"
#include "core/rs_encode_scalar.hpp"
#include "vstreamer_idf/live_mplane.hpp"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>

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
live_mplane mplane;
constexpr int mplane_port = 5090; // Same port as the rover's vstreamer mplane.
constexpr vstreamer::thread_opts mplane_thread{"vs_mplane", 6 * 1024, 0, 5};

// Per completed AU (RTP marker): capture age (the ground's CLOCK_REALTIME is
// the sync reference) and, at receiver output(), the receiver's own hold
// (datagram enqueue -> output()). Percentiles cover the AUs since the
// previous stats query.
class latency_window {
public:
    void add(float latency_ms, float node_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        if (latency_ms < 0 || latency_ms > 10000) { ++rejected_; return; }
        last_ = {latency_ms, node_ms};
        if (window_.size() < 4096) window_.push_back(last_);
    }
    float last_latency_ms() const { std::lock_guard<std::mutex> lock(mu_); return last_.latency_ms; }
    float last_node_ms() const { std::lock_guard<std::mutex> lock(mu_); return last_.node_ms; }
    std::string stats(bool with_node) {
        std::vector<sample> window;
        uint64_t rejected;
        {
            std::lock_guard<std::mutex> lock(mu_);
            window.swap(window_);
            rejected = rejected_;
        }
        char buf[256];
        if (window.empty()) {
            snprintf(buf, sizeof(buf), "n=0 rejected=%llu", (unsigned long long)rejected);
            return buf;
        }
        std::vector<float> lat, node;
        for (const auto &w : window) { lat.push_back(w.latency_ms); node.push_back(w.node_ms); }
        std::sort(lat.begin(), lat.end());
        std::sort(node.begin(), node.end());
        const auto at = [](const std::vector<float> &v, double q) { return v[std::min(v.size() - 1, size_t(q * v.size()))]; };
        int n = snprintf(buf, sizeof(buf), "n=%u min=%.1f p50=%.1f p95=%.1f max=%.1f",
            unsigned(lat.size()), lat.front(), at(lat, 0.5), at(lat, 0.95), lat.back());
        if (with_node)
            n += snprintf(buf + n, sizeof(buf) - n, " node_p50=%.1f node_p95=%.1f node_max=%.1f",
                at(node, 0.5), at(node, 0.95), node.back());
        snprintf(buf + n, sizeof(buf) - n, " rejected=%llu", (unsigned long long)rejected);
        return buf;
    }
private:
    struct sample { float latency_ms, node_ms; };
    mutable std::mutex mu_;
    std::vector<sample> window_;
    sample last_{-1, -1};
    uint64_t rejected_ = 0;
};
// CPU share per FreeRTOS task since the previous query, busiest first:
// "<name>:<core>:<prio>:<percent>" (core '-' = unpinned). Percent is of one core.
std::string task_cpu_stats() {
    static std::mutex mu;
    static std::map<TaskHandle_t, uint32_t> last_counter; // names repeat
    static uint32_t last_total = 0;
    std::lock_guard<std::mutex> lock(mu);
    std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 4);
    configRUN_TIME_COUNTER_TYPE total = 0;
    tasks.resize(uxTaskGetSystemState(tasks.data(), tasks.size(), &total));
    const uint32_t elapsed = uint32_t(total) - last_total;
    last_total = uint32_t(total);
    struct row { std::string name; int core; unsigned prio; float pct; };
    std::vector<row> rows;
    std::map<TaskHandle_t, uint32_t> counter;
    for (const auto &t : tasks) {
        const std::string name = t.pcTaskName;
        const uint32_t now = uint32_t(t.ulRunTimeCounter);
        const auto it = last_counter.find(t.xHandle);
        const uint32_t delta = it == last_counter.end() ? now : now - it->second;
        counter[t.xHandle] = now;
        rows.push_back({name, int(xTaskGetCoreID(t.xHandle)), unsigned(t.uxCurrentPriority),
            elapsed ? 100.f * float(delta) / float(elapsed) : 0.f});
    }
    last_counter.swap(counter);
    std::sort(rows.begin(), rows.end(), [](const row &a, const row &b) { return a.pct > b.pct; });
    std::string out = "window_ms=" + std::to_string(elapsed / 1000);
    char buf[64];
    for (const auto &r : rows) {
        if (r.core == tskNO_AFFINITY) snprintf(buf, sizeof(buf), " %s:-:%u:%.1f", r.name.c_str(), r.prio, r.pct);
        else snprintf(buf, sizeof(buf), " %s:%d:%u:%.1f", r.name.c_str(), r.core, r.prio, r.pct);
        out += buf;
    }
    return out;
}

latency_window rx_latency;     // at receiver output()
latency_window mcast_latency;  // after a successful multicast sendto()

int64_t clock_ns(clockid_t id) {
    timespec ts{};
    clock_gettime(id, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// RFC 5285 one-byte extension id 2: capture instant, CLOCK_REALTIME ns (BE u64).
int64_t rtp_capture_rt_ns(const uint8_t *b, size_t size) {
    if (size < 12 || (b[0] & 0xc0) != 0x80 || !(b[0] & 0x10)) return 0;
    const size_t start = 12 + 4 * size_t(b[0] & 0x0f);
    if (size < start + 4 || b[start] != 0xbe || b[start + 1] != 0xde) return 0;
    const size_t end = start + 4 + 4 * (size_t(b[start + 2]) << 8 | b[start + 3]);
    if (size < end) return 0;
    for (size_t i = start + 4; i < end;) {
        if (b[i] == 0) { ++i; continue; }
        const uint8_t id = b[i] >> 4;
        const size_t len = size_t(b[i] & 0x0f) + 1;
        if (id == 15 || i + 1 + len > end) return 0;
        if (id == 2 && len == 8) {
            uint64_t ns = 0;
            for (size_t k = 0; k < 8; ++k) ns = ns << 8 | b[i + 1 + k];
            return int64_t(ns);
        }
        i += 1 + len;
    }
    return 0;
}

// Capture age of a completed AU, or -1 for any other packet.
float au_capture_age_ms(const bfc::shared_sized_buffer &packet) {
    const auto *b = packet.u8();
    if (packet.size() < 12 || !(b[1] & 0x80)) return -1; // marker: the AU is complete
    const int64_t capture = rtp_capture_rt_ns(b, packet.size());
    if (capture <= 0) return -1;
    const float age = float((clock_ns(CLOCK_REALTIME) - capture) / 1e6);
    return age < 0 ? 1e9f : age; // negative ages count as rejected
}

std::string format_ms(float ms) {
    if (ms < 0) return "-1";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", ms);
    return buf;
}

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
                if (sent == ssize_t(packet.size())) {
                    const float age = au_capture_age_ms(packet);
                    if (age >= 0) mcast_latency.add(age, -1);
                    return 0;
                }
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
                if (const float age = au_capture_age_ms(packet.sdu); age >= 0) {
                    const float node = float((clock_ns(CLOCK_MONOTONIC) / 1000 - int64_t(packet.ts_us)) / 1e3);
                    rx_latency.add(age, node);
                }
            }
            fanout.push(packet.sdu);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
}

std::string ground_video_stats() {
    std::string stats; receiver->query("stats", &stats);
    // Heap buffer: this runs on app_main's small stack.
    std::vector<char> buf(1536);
    snprintf(buf.data(), buf.size(), "%s recovered_rtp_gaps=%llu recovered_rtp_reordered=%llu radio_queue_evict=%llu l3_rx=%llu l3_evict=%llu fec_input=%llu feedback_drop=%llu usb_evict=%llu multicast_evict=%llu multicast_sent=%llu multicast_offline=%llu multicast_send_errors=%llu multicast_send_retries=%llu multicast_last_errno=%d usb_depth=%u multicast_depth=%u",
           stats.c_str(), (unsigned long long)recovered_rtp_gaps.load(),
           (unsigned long long)recovered_rtp_reordered.load(), (unsigned long long)upstream->radio_queue_evictions(),
           (unsigned long long)upstream->rx_packets.load(),
           (unsigned long long)upstream->rx_evictions.load(), (unsigned long long)fec_input.load(),
           (unsigned long long)feedback_drop.load(),
           (unsigned long long)fanout.drops(0), (unsigned long long)fanout.drops(1),
           (unsigned long long)multicast_sent.load(), (unsigned long long)multicast_offline.load(),
           (unsigned long long)multicast_send_errors.load(), (unsigned long long)multicast_send_retries.load(),
           multicast_last_errno.load(), unsigned(fanout.depth(0)), unsigned(fanout.depth(1)));
    return buf.data();
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
    mplane.add_node("rcv", receiver.get());
    mplane.add_query("rcv", "latency_ms", [] { return format_ms(rx_latency.last_latency_ms()); });
    mplane.add_query("rcv", "node_latency_ms", [] { return format_ms(rx_latency.last_node_ms()); });
    mplane.add_query("rcv", "latency_stats", [] { return rx_latency.stats(true); });
    mplane.add_query("mcast", "latency_ms", [] { return format_ms(mcast_latency.last_latency_ms()); });
    mplane.add_query("mcast", "latency_stats", [] { return mcast_latency.stats(false); });
    mplane.add_query("gnd", "stats", ground_video_stats);
    mplane.add_query("gnd", "tasks", task_cpu_stats);
    mplane.add_query("usb", "prof", usb_upstream_profile);
    if (const int r = mplane.start(mplane_port, mplane_thread); r < 0)
        printf("GROUND_MPLANE bind :%d failed (%d)\n", mplane_port, r);
    else
        printf("GROUND_MPLANE udp :%d node=rcv\n", mplane_port);
    ready.store(true);
    printf("GROUND_VIDEO format=rtp-h264-v1 multicast=%s:%u ttl=1 receiver=l3-queue\n",
           multicast_group, multicast_port);
    return ESP_OK;
}
void ground_video_log_stats() {
    if (!ready.load()) return;
    std::string given_up, late, rs_errors, spread, jitter;
    receiver->query("fec_given_up_blocks", &given_up);
    receiver->query("fec_late_blocks", &late);
    receiver->query("fec_rs_failures", &rs_errors);
    receiver->query("fec_spread_est_ms", &spread);
    receiver->query("fec_jitter_est_ms", &jitter);
    printf("GROUND_FEC backend=%s given_up=%s late=%s rs_errors=%s spread_ms=%s jitter_ms=%s margin_floor_ms=40\n",
        vstreamer::rs_encode_scalar_impl(), given_up.c_str(), late.c_str(), rs_errors.c_str(), spread.c_str(), jitter.c_str());
    printf("GROUND_VIDEO %s\n", ground_video_stats().c_str());
}
