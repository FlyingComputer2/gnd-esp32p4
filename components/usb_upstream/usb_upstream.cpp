#include "usb_upstream.hpp"
#include "usb_protocol.hpp"
#ifndef ROVER_USB_HOST_TEST
#include "cJSON.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#endif
#include "cJSON.h"
#include <array>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace
{
using namespace rover::usb;
struct Channel
{
    uint16_t id = 0, mtu = 1445, tx_credit = 0, rx_credit = 0, pending_credit = 0, window = 0;
    std::string name;
    std::shared_ptr<winject::UpstreamQueue> upstream;
    UsbPacketSource source;
    std::function<bool()> source_connected;
    std::string format;
    bool connected() const { return upstream ? upstream->connected() : source_connected(); }
    auto receive() { return upstream ? upstream->receive() : source(); }
    bool open = false, closed = false;
    uint32_t sequence = 0, down = 0, up = 0, dropped = 0;
};
std::mutex io;
std::array<Channel, 16> channels;
size_t count = 0, cursor = 0;
uint32_t session = 0;
bool hello = false, installed = false;
Parser parser;
std::deque<std::vector<uint8_t>> controls;
std::vector<uint8_t> pending;
size_t offset = 0;
unsigned control_burst = 0;
char serial[13];
Channel *find(uint16_t id)
{
    for (size_t i = 0; i < count; ++i)
        if (channels[i].id == id)
            return &channels[i];
    return nullptr;
}
void reset(uint32_t id)
{
    session = id;
    hello = false;
    parser.reset(id);
    controls.clear();
    pending.clear();
    offset = 0;
    for (size_t i = 0; i < count; ++i)
    {
        auto &c = channels[i];
        c.open = c.closed = false;
        c.tx_credit = c.rx_credit = c.pending_credit = 0;
        c.sequence = 0;
        for (unsigned j = 0; j < 32 && c.receive(); ++j)
        {
        }
    }
}
void fail() { reset(0); }
void respond(uint32_t seq, cJSON *obj)
{
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!text)
    {
        fail();
        return;
    }
    std::string str(text);
    cJSON_free(text);
    if (str.size() > 1024 || controls.size() >= 4)
    {
        fail();
        return;
    }
    controls.push_back(encode({reply, 0, session, seq, {str.begin(), str.end()}}));
}
cJSON *result(bool ok, const char *error = nullptr)
{
    auto *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    if (error)
        cJSON_AddStringToObject(o, "error", error);
    return o;
}
bool integer(cJSON *o, const char *key, int min, int max, int &out)
{
    auto *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max ||
        v->valuedouble != v->valueint)
        return false;
    out = v->valueint;
    return true;
}
// Bound recursive JSON parsing to the USB worker's stack budget.
bool bounded_json(const std::vector<uint8_t> &bytes)
{
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (uint8_t ch : bytes)
    {
        if (!ch)
            return false;
        if (quoted)
        {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
        }
        else if (ch == '"')
            quoted = true;
        else if (ch == '{' || ch == '[')
        {
            if (++depth > 8)
                return false;
        }
        else if (ch == '}' || ch == ']')
        {
            if (!depth)
                return false;
            --depth;
        }
    }
    return !quoted && depth == 0;
}
void on_record(const Record &r)
{
    if (r.kind == request)
    {
        if (!r.sequence || r.payload.empty())
        {
            fail();
            return;
        }
        if (!bounded_json(r.payload))
        {
            respond(r.sequence, result(false, "BAD_REQUEST"));
            return;
        }
        auto *cmd = cJSON_ParseWithLength(reinterpret_cast<const char *>(r.payload.data()),
                                          r.payload.size());
        auto *op = cmd ? cJSON_GetObjectItemCaseSensitive(cmd, "op") : nullptr;
        if (!cJSON_IsObject(cmd) || !cJSON_IsString(op))
        {
            cJSON_Delete(cmd);
            respond(r.sequence, result(false, "BAD_REQUEST"));
            return;
        }
        auto *out = result(true);
        int id = 0, window = 0;
        if (!strcmp(op->valuestring, "hello"))
        {
            int version = 0;
            if (!integer(cmd, "version", 2, 2, version))
            {
                cJSON_Delete(out);
                out = result(false, "VERSION");
            }
            else
            {
                hello = true;
                cJSON_AddNumberToObject(out, "version", 2);
                cJSON_AddNumberToObject(out, "max_payload", 2048);
            }
        }
        else if (!hello)
        {
            cJSON_Delete(out);
            out = result(false, "HELLO_REQUIRED");
        }
        else if (!strcmp(op->valuestring, "list"))
        {
            int start = 0, limit = 4;
            if ((cJSON_HasObjectItem(cmd, "offset") && !integer(cmd, "offset", 0, 16, start)) ||
                (cJSON_HasObjectItem(cmd, "limit") && !integer(cmd, "limit", 1, 4, limit)))
            {
                cJSON_Delete(cmd);
                cJSON_Delete(out);
                respond(r.sequence, result(false, "BAD_REQUEST"));
                return;
            }
            auto *list = cJSON_AddArrayToObject(out, "channels");
            size_t end = std::min(count, size_t(start + limit));
            for (size_t i = start; i < end; ++i)
            {
                auto &c = channels[i];
                auto *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", c.id);
                cJSON_AddStringToObject(item, "name", c.name.c_str());
                cJSON_AddNumberToObject(item, "max_payload", c.mtu);
                cJSON_AddBoolToObject(item, "ready", c.connected());
                if (!c.format.empty()) cJSON_AddStringToObject(item, "format", c.format.c_str());
                cJSON_AddBoolToObject(item, "read_only", !c.upstream);
                cJSON_AddItemToArray(list, item);
            }
            if (end < count)
                cJSON_AddNumberToObject(out, "next_offset", end);
            else
                cJSON_AddNullToObject(out, "next_offset");
        }
        else if (!strcmp(op->valuestring, "open"))
        {
            Channel *c = nullptr;
            const char *error = nullptr;
            if (!integer(cmd, "channel", 0, 65535, id) || !integer(cmd, "rx_window", 1, 16, window))
                error = "BAD_REQUEST";
            else if (!(c = find(id)))
                error = "NO_REMOTE_HANDLER";
            else if (c->open || c->closed)
                error = "ALREADY_OPEN_OR_CLOSED";
            else if (!c->connected())
                error = "BACKEND_NOT_READY";
            if (error)
            {
                cJSON_Delete(out);
                out = result(false, error);
            }
            else
            {
                for (unsigned j = 0; j < 32 && c->receive(); ++j)
                {
                }
                c->open = true;
                c->window = window;
                c->tx_credit = window;
                c->rx_credit = 8;
                c->sequence = 0;
                c->pending_credit = 0;
                cJSON_AddNumberToObject(out, "channel", id);
                cJSON_AddNumberToObject(out, "rx_window", 8);
                cJSON_AddNumberToObject(out, "max_payload", c->mtu);
                if (!c->format.empty()) cJSON_AddStringToObject(out, "format", c->format.c_str());
                cJSON_AddBoolToObject(out, "read_only", !c->upstream);
            }
        }
        else if (!strcmp(op->valuestring, "close"))
        {
            auto *c = integer(cmd, "channel", 0, 65535, id) ? find(id) : nullptr;
            if (!c || !c->open)
            {
                cJSON_Delete(out);
                out = result(false, "NOT_OPEN");
            }
            else
            {
                c->open = false;
                c->closed = true;
                c->pending_credit = 0;
            }
        }
        else if (!strcmp(op->valuestring, "stats"))
        {
            int start = 0, limit = 4;
            if ((cJSON_HasObjectItem(cmd, "offset") && !integer(cmd, "offset", 0, 16, start)) ||
                (cJSON_HasObjectItem(cmd, "limit") && !integer(cmd, "limit", 1, 4, limit)))
            {
                cJSON_Delete(cmd);
                cJSON_Delete(out);
                respond(r.sequence, result(false, "BAD_REQUEST"));
                return;
            }
            auto *a = cJSON_AddArrayToObject(out, "channels");
            size_t end = std::min(count, size_t(start + limit));
            for (size_t i = start; i < end; ++i)
            {
                auto &c = channels[i];
                auto *o = cJSON_CreateObject();
                cJSON_AddNumberToObject(o, "id", c.id);
                cJSON_AddNumberToObject(o, "up", c.up);
                cJSON_AddNumberToObject(o, "down", c.down);
                cJSON_AddNumberToObject(o, "drop", c.dropped);
                cJSON_AddNumberToObject(o, "tx_credit", c.tx_credit);
                cJSON_AddItemToArray(a, o);
            }
            if (end < count)
                cJSON_AddNumberToObject(out, "next_offset", end);
            else
                cJSON_AddNullToObject(out, "next_offset");
        }
        else
        {
            cJSON_Delete(out);
            out = result(false, "UNKNOWN_OP");
        }
        cJSON_Delete(cmd);
        respond(r.sequence, out);
        return;
    }
    auto *c = find(r.channel);
    if (!hello || !c || !c->open || !c->connected())
    {
        fail();
        return;
    }
    if (r.kind == credit)
    {
        unsigned n = u16(r.payload.data());
        if (!n || c->tx_credit + n > c->window)
        {
            fail();
            return;
        }
        c->tx_credit += n;
    }
    else if (r.kind == data)
    {
        if (!c->upstream || !c->rx_credit || r.payload.size() > c->mtu)
        {
            fail();
            return;
        }
        --c->rx_credit;
        if (c->upstream->try_send(r.payload.data(), r.payload.size()))
            ++c->down;
        else
            ++c->dropped;
        ++c->pending_credit;
    }
    else
        fail();
}
std::vector<uint8_t> next_record()
{
    if (!session)
        return {};
    // Control priority is bounded, preserving DATA progress under valid load.
    if (!controls.empty() && control_burst < 4)
    {
        auto p = std::move(controls.front());
        controls.pop_front();
        ++control_burst;
        return p;
    }
    for (size_t i = 0; i < count; ++i)
    {
        auto &c = channels[i];
        if (c.open && c.pending_credit)
        {
            uint16_t n = c.pending_credit;
            c.pending_credit = 0;
            c.rx_credit += n;
            std::vector<uint8_t> p(2);
            put16(p.data(), n);
            return encode({credit, c.id, session, 0, std::move(p)});
        }
    }
    for (size_t i = 0; i < count; ++i)
    {
        auto &c = channels[cursor++ % count];
        if (!c.open || !c.connected())
        {
            if (c.open)
            {
                fail();
                return {};
            }
            for (unsigned j = 0; j < 32 && c.receive(); ++j)
                ++c.dropped;
            continue;
        }
        if (!c.tx_credit)
            continue;
        auto packet = c.receive();
        if (!packet)
            continue;
        ++c.sequence;
        if (packet->empty() || packet->size() > c.mtu)
        {
            ++c.dropped;
            continue;
        }
        --c.tx_credit;
        ++c.up;
        control_burst = 0;
        return encode(
            {data, c.id, session, c.sequence, {packet->u8(), packet->u8() + packet->size()}});
    }
    control_burst = 0;
    if (!controls.empty())
    {
        auto p = std::move(controls.front());
        controls.pop_front();
        return p;
    }
    return {};
}
#ifndef ROVER_USB_HOST_TEST
void task(void *)
{
    uint8_t bytes[512];
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(io);
            if (tud_vendor_mounted())
            {
                for (unsigned i = 0; i < 16 && tud_vendor_available(); ++i)
                {
                    auto n = tud_vendor_read(bytes, sizeof(bytes));
                    parser.feed(bytes, n, on_record);
                    if (parser.failed)
                    {
                        fail();
                        break;
                    }
                }
                for (unsigned i = 0; i < 32; ++i)
                {
                    if (pending.empty())
                    {
                        pending = next_record();
                        offset = 0;
                    }
                    if (pending.empty())
                        break;
                    offset += tud_vendor_write(pending.data() + offset, pending.size() - offset);
                    tud_vendor_write_flush();
                    if (offset != pending.size())
                        break;
                    pending.clear();
                    offset = 0;
                }
            }
            else
            {
                for (size_t i = 0; i < count; ++i)
                    for (unsigned j = 0; j < 32 && channels[i].receive(); ++j)
                        ++channels[i].dropped;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
void event_cb(tinyusb_event_t *ev, void *)
{
    if (ev->id == TINYUSB_EVENT_ATTACHED || ev->id == TINYUSB_EVENT_DETACHED)
    {
        std::lock_guard<std::mutex> lock(io);
        reset(0);
    }
}
const tusb_desc_device_t device = {.bLength = sizeof(tusb_desc_device_t),
                                   .bDescriptorType = TUSB_DESC_DEVICE,
                                   .bcdUSB = 0x0200,
                                   .bDeviceClass = 0,
                                   .bDeviceSubClass = 0,
                                   .bDeviceProtocol = 0,
                                   .bMaxPacketSize0 = 64,
                                   .idVendor = 0x303a,
                                   .idProduct = 0x4001,
                                   .bcdDevice = 0x0200,
                                   .iManufacturer = 1,
                                   .iProduct = 2,
                                   .iSerialNumber = 3,
                                   .bNumConfigurations = 1};
const tusb_desc_device_qualifier_t qualifier = {.bLength = sizeof(tusb_desc_device_qualifier_t),
                                                .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
                                                .bcdUSB = 0x0200,
                                                .bDeviceClass = 0,
                                                .bDeviceSubClass = 0,
                                                .bDeviceProtocol = 0,
                                                .bMaxPacketSize0 = 64,
                                                .bNumConfigurations = 1,
                                                .bReserved = 0};
const uint8_t fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, 4, 1, 0x81, 64)};
const uint8_t hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, 4, 1, 0x81, 512)};
const char *strings[] = {"\x09\x04", "Rover", "Ground P4 upstream proxy", serial,
                         "Upstream packets v2"};
#endif
} // namespace
#ifndef ROVER_USB_HOST_TEST
extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                           const tusb_control_request_t *req)
{
    if (stage != CONTROL_STAGE_SETUP)
        return true;
    if (req->bmRequestType != 0xc0 || req->bRequest != 1 || req->wValue || req->wIndex ||
        req->wLength != 4)
        return false;
    std::lock_guard<std::mutex> lock(io);
    static uint8_t response[4];
    uint32_t id = esp_random();
    if (!id || id == session)
        id = session + 1;
    if (!id)
        id = 1;
    reset(id);
    tud_vendor_read_flush();
    tud_vendor_write_clear();
    put32(response, id);
    return tud_control_xfer(rhport, req, response, sizeof(response));
}
#endif
esp_err_t usb_upstream_register(uint16_t id, const char *name,
                                std::shared_ptr<winject::UpstreamQueue> up, uint16_t mtu)
{
    std::lock_guard<std::mutex> lock(io);
    if (!up || !name || strlen(name) > 31 || !mtu || mtu > 1445 || count == channels.size())
        return ESP_ERR_INVALID_ARG;
    if (find(id))
        return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < count; ++i)
        if (channels[i].upstream == up)
            return ESP_ERR_INVALID_STATE;
    auto &c = channels[count++];
    c.id = id;
    c.name = name;
    c.mtu = mtu;
    c.upstream = std::move(up);
    return ESP_OK;
}
esp_err_t usb_upstream_register_source(uint16_t id, const char *name,
    UsbPacketSource source, std::function<bool()> connected, const char *format, uint16_t mtu)
{
    std::lock_guard<std::mutex> lock(io);
    if (!source || !connected || !name || strlen(name) > 31 || !format ||
        strlen(format) > 31 || !mtu || mtu > 1445 || count == channels.size())
        return ESP_ERR_INVALID_ARG;
    if (find(id)) return ESP_ERR_INVALID_STATE;
    auto &c = channels[count++];
    c.id = id; c.name = name; c.mtu = mtu; c.format = format;
    c.source = std::move(source); c.source_connected = std::move(connected);
    return ESP_OK;
}
#ifndef ROVER_USB_HOST_TEST
esp_err_t usb_upstream_start()
{
    if (installed)
        return ESP_ERR_INVALID_STATE;
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BASE));
    snprintf(serial, sizeof(serial), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    auto cfg = TINYUSB_DEFAULT_CONFIG(event_cb);
    cfg.task.priority = 16;
    cfg.descriptor.device = &device;
    cfg.descriptor.qualifier = &qualifier;
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = 5;
    cfg.descriptor.full_speed_config = fs;
    cfg.descriptor.high_speed_config = hs;
    auto err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK)
        return err;
    if (xTaskCreate(task, "usb_upstream", 8192, nullptr, 14, nullptr) != pdPASS)
    {
        tinyusb_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    installed = true;
    return ESP_OK;
}
#endif
void usb_upstream_log_stats()
{
    std::lock_guard<std::mutex> lock(io);
    for (size_t i = 0; i < count; ++i)
    {
        auto &c = channels[i];
        printf("GROUND_USB channel=%u open=%u up=%lu down=%lu drop=%lu credits=%u\n", c.id, c.open,
               (unsigned long)c.up, (unsigned long)c.down, (unsigned long)c.dropped, c.tx_credit);
    }
}
