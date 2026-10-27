# USB upstream proxy, RGP4 version 2

The P4 runs winject-l3 and exposes its queue app transports over one USB bulk
endpoint pair. P4 runs vstreamer's FEC receiver; Android depayloads recovered RTP and uses
MediaCodec for video decode. Neither endpoint treats USB transfer boundaries as datagrams.

| Channel (`u16`) | Application | State |
| --- | --- | --- |
| 0 | Video | Implemented; receive-only RTP H.264 (`rtp-h264-v1`) |
| 1 | MSP DisplayPort | TBA |
| 2 | MSP RC telemetry | TBA |

All IDs 0–65535 are valid for application DATA/CREDIT. Management is selected
by record kind. Its channel header field is zero and unused; a management
REQUEST must never enter channel 0's video handler. USB IDs, L3 upstream indexes
and radio buses are independent. Video maps USB 0 to L3 upstream index 1,
ground TX bus d4 / RX bus c3 through the P4 receiver. MSP IDs do not select any radio buses yet.

## Direct L3 transport binding

L3 creates `winject::UpstreamQueue` for a queue upstream whose endpoint and
reactor names are empty, and hands it over through `Config::on_upstream_queue`:

```cpp
video.mode = winject::UpstreamMode::queue;
// Leave named endpoint and reactor fields empty: L3 constructs this transport.
cfg.on_upstream_queue = [](size_t index,
                           std::shared_ptr<winject::UpstreamQueue> upstream) {
    if (index == 1) ground_video_start(std::move(upstream));
};
```

The video service owns the shared handle and feeds stream_receiver. It registers
a local processed source for USB 0 using `usb_upstream_register_source()`, with
`format:rtp-h264-v1` and `read_only:true`. LIST and OPEN expose both fields.
Callbacks pop from the dedicated bounded USB queue and check L3 connected state;
they must not block. No app-created queue pair impersonates an L3 transport.

Other bidirectional services can register the real L3 object directly with
`usb_upstream_register()`. Duplicate IDs and repeated L3 objects are rejected;
there are at most 16 registrations. L3 owns construction/disconnection, and
retained shared handles stay memory-valid after teardown. No automatic manager
restart/rebinding is implemented. L3 loss invalidates an open USB session.

Video manager FEC is disabled. vstreamer FEC ends on P4 and its receiver reports
return through L3, independently of USB. See [video pipeline](video-pipeline.md).

## Record header

All integers below are little endian, encoded explicitly without struct casts.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII RGP4 |
| 4 | 1 | Version 2 |
| 5 | 1 | Kind |
| 6 | 2 | Flags, zero |
| 8 | 2 | Unsigned channel ID |
| 10 | 2 | Payload length |
| 12 | 4 | Nonzero session ID |
| 16 | 4 | Sequence or request ID |
| 20 | length | Payload, without padding |

| Kind | Value | Channel meaning | Payload |
| --- | --- | --- | --- |
| REQUEST | 1 | Field must be zero; management | JSON request, nonzero request ID |
| REPLY | 2 | Field must be zero; management | JSON reply, matching request ID |
| EVENT | 3 | Field must be zero; management | Reserved status event |
| DATA | 4 | Application ID, including 0 | One nonempty upstream datagram |
| CREDIT | 5 | Application ID, including 0 | uint16 slots released; sequence zero |

The hard payload cap is 2048 bytes and management cap is 1024 bytes. DATA must
also fit the channel's negotiated MTU. No application fragmentation, truncation,
USB retransmission layer or additional checksum is added. USB already protects
its transfers; video radio FEC is handled by P4 before fanout.

Parsers handle split headers, split payloads and multiple records per transfer.
Stale bytes left after session reset are skipped until a current-session header
is found. Malformed current-session headers/records invalidate the connection;
the client starts a fresh session. DATA sequences count outgoing records per
channel/direction and start at 1. They are diagnostic, not acknowledgements;
upstream queue loss is tracked separately by L3.

Golden DATA record: channel 65535, session 0x01020304, sequence 0x11223344,
payload 01 02 03:

```text
52475034 02 04 0000 ffff 0300 04030201 44332211 010203
```

C++ and JVM tests assert the same bytes, including unsigned high IDs.

## Session and management

The host claims interface 0, endpoints OUT 01 and IN 81. Before starting bulk
workers it issues this EP0 request:

```text
bmRequestType = 0xc0 (vendor/device IN)
bRequest      = 0x01 (RESET_SESSION)
wValue/index  = 0
wLength       = 4
response      = new nonzero uint32 session ID, little endian
```

Reset serializes with the USB worker, closes channel state and clears proxy
staging, credits, parser state and software FIFOs. Already submitted IN bytes
can still complete; the client discards their old session ID. Reset cannot
retract application packets already admitted into L3. Physical detach or
protocol failure invalidates session state. A new Android connection always
resets, including when the app reopens against a still-attached P4.

Management is JSON on REQUEST/REPLY. Success uses `{"ok":true,...}` and errors
use `{"ok":false,"error":"CODE"}`. At most four requests are outstanding and
replies are bounded. Operations:

- `{"op":"hello","version":2}` negotiates v2. Other operations require HELLO.
- `{"op":"list","offset":0,"limit":4}` returns registered channel IDs,
  names, `max_payload`, readiness, `read_only`, optional `format` and `next_offset`
  (null at end). MSP placeholders
  are absent until their L3 transports exist and are bound.
- `{"op":"open","channel":0,"rx_window":8}` opens video. The host supplies
  P4→Android receive slots; the reply reports the P4's Android→P4 `rx_window`
  (currently 8), the channel and MTU. Video additionally returns
  `format:rtp-h264-v1` and `read_only:true`. OPEN reply precedes DATA.
- `{"op":"close","channel":0}` closes and discards pending channel traffic.
  An ID closed in this session cannot reopen until RESET_SESSION.
- `{"op":"stats","offset":0,"limit":4}` returns local counters and credits.

Errors include `BAD_REQUEST`, `VERSION`, `HELLO_REQUIRED`, `NO_REMOTE_HANDLER`,
`BACKEND_NOT_READY`, `ALREADY_OPEN_OR_CLOSED`, `NOT_OPEN` and `UNKNOWN_OP`.
The Android app currently closes the whole transport on disconnect/error and
reopens with a fresh reset. It does not expose an individual channel-close API.
HTTP OTA remains available through Ethernet; bulk firmware update is not part
of this implementation.

## Credits, queues and scheduling

Each open channel has independent packet credits in both directions. One credit
reserves one receive slot for that channel's maximum datagram. Initial windows
come from OPEN and are limited to 1–16. The sender consumes one credit per DATA
record; zero credits means that channel cannot send. CREDIT and management do
not consume DATA credits. Invalid/overflow grants or uncredited DATA invalidate
the session rather than overflowing buffers.

On P4, a credit is returned once DATA has been submitted to the L3 app transport.
On Android, credit is returned when the handler releases its `Lease`. Pending
releases are coalesced. A lone release is sent without waiting for new DATA.
This controls local receive storage, not radio or remote application delivery.

Unopened P4 backends are drained/discarded. An open backend with zero host
credits retains its bounded outgoing queue; normal oldest-packet eviction
applies under sustained overload. Android's outgoing channel queue has eight
slots; `trySend()` returns false when closed, over MTU or full. Queue contents
are never replayed into a new session.

Both sides schedule DATA round-robin over eligible channels, at complete record
boundaries. Credit returns and management use priority with a bounded management
burst. A partial record finishes before another channel's record is selected.
The P4 USB TX FIFO is limited to 4096 bytes to avoid a large prequeued video
backlog. All channels still share physical endpoint bandwidth/backpressure.

## Android registration and receive pipeline

```java
UsbProxy proxy = new UsbProxy(errorListener);
UsbProxy.Channel video = proxy.registerHandler(0, videoReceiver);
proxy.connect(usbManager, device); // reset, HELLO and discovery
video.open(8);
```

Java validates IDs as unsigned 16-bit integers; a Kotlin caller can convert a
`UShort` with `toInt()`. Duplicate handlers fail. One reader demultiplexes by
record kind/channel; one writer serializes records. Every channel has its own
bounded, serial handler executor. `onOpen` precedes packets; `onClose` follows
accepted callback work. Handlers never call bulk I/O directly.

`onPacket(Lease, Channel)` owns the packet until `close()` releases its credit.
Consumers may transfer the lease to bounded asynchronous work. Copying packets
into unbounded queues defeats flow control. Slow video consumption exhausts
only video credits, while the reader continues to demultiplex other channels.

Channel 0 feeds the native RTP/H.264 depacketizer.
Android validates the `rtp-h264-v1` format in OPEN, then depayloads channel-0
RTP into Annex-B AUs for MediaCodec. It sends no channel-0 DATA or radio reports.
The decoder currently assumes 1080p30. Update firmware and app together; legacy
video envelopes and feedback are incompatible with this receive-only source.

## Verification and remaining bench work

Host tests exercise real USB service routing against the L3 app object:
unsigned IDs, identical golden bytes, split records, management/video separation,
duplicate bindings, credit isolation, stale sessions and invalid traffic. The
P4 receiver integration test drops RS shards, verifies identical fanout RTP and
reverse reports, and reconstructs an IDR. Android tests fragmented RTP/IDRs and
rejects malformed/legacy packets. Android
CI runs JVM tests, native tests and assembles an arm64 debug APK.

The P4 firmware compiles with ESP-IDF 5.5.5 and fits its existing OTA slots.
Phone enumeration, live playback, USB power and sustained CPU/heap/latency still
require an attached P4 and Android device. Build/unit results do not establish
those hardware properties.
