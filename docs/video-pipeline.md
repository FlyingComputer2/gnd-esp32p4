# Ground video receive and fanout

P4 runs the pinned vstreamer `stream_receiver` component, not a second codec.
The L3-owned video `UpstreamQueue` (index 1) feeds the receiver directly through
nonblocking datagram callbacks. Reverse receiver reports return using
`UpstreamQueue::try_send()`. Both the proxy-to-L3 and L3-to-FEC data paths use
in-process queues; only the low-rate radio management plane uses loopback UDP.
Android does not send radio receiver reports.

Radio queue callbacks run on CPU0 and the FEC receive worker runs on CPU1.
This prevents heavy reconstruction from falling behind SDIO on one core while
the other core is idle. The L3 app RX queue holds 256 datagrams.

RS repair reconstructs only missing data rows; missing parity rows are used
for matrix selection but are never rebuilt. Present data uses shared views.
This avoids spending most of a core reconstructing parity that is discarded.
The P4 receiver uses a 40 ms minimum expiry margin for scheduler jitter.

The radio proxy's existing borrowed-input ownership copy allocates packet
payloads in PSRAM (`GROUND_VIDEO_PSRAM_PACKETS`). L3 queue delivery preserves
that owner, and the direct FEC input uses subviews instead of copying packets
again. Both fanout consumers treat recovered RTP as immutable. Socket input
still copies its reusable receive buffer. Reconstructed rows are 16-byte aligned
and allocated in PSRAM (`VSTREAMER_RX_PSRAM`); output holds their application
subviews. Only decoder source rows that need padding get temporary storage.

Ground exposes `VSTREAMER_RS_PIE` because it compiles vstreamer through its own
component. Before input, the receive worker compares the PIE kernel with the
scalar reference for 1, 2 and 10 output rows using unaligned inputs and an odd
length. `GROUND_RS_SELFTEST` logs mismatches, selected backend and timings;
a mismatch disables PIE through the kernel's existing fallback. `GROUND_FEC`
also reports the current backend. Single-row PIE recovery need not be faster
than the scalar kernel; measure the full pipeline under the actual loss rate.

USB DATA records are built directly in their final wire buffer from the shared
RTP payload, avoiding an intermediate payload allocation/copy. The existing
pending-record buffer retains ownership across partial TinyUSB writes and is
cleared on session reset; wire framing and credits are unchanged.

The receiver strips stream framing, repairs RS erasures, sequences recovered
SDUs and emits RTP H.264. P4 does not depayload or decode H.264. Recovered RTP
packets, including their original sequence, timestamps and extensions, are
shared by two independently bounded queues:

| Consumer | Queue | Output |
| --- | --- | --- |
| USB | 512 packets, oldest eviction | Channel 0, one RTP packet per DATA |
| Ethernet | 128 packets, oldest eviction | UDP 239.255.42.1:5600, TTL 1 |

USB retains up to 512 packets to absorb credit pauses during codec/bitrate
transitions. Both branches absorb recovery bursts independently. Ethernet drains at most
two packets per scheduler turn. `MulticastEgress` retains the pending head
packet on ENOMEM/ENOBUFS/EAGAIN, yields and retries in order; after 64 attempts
it counts a send failure rather than holding stale video indefinitely.
The EMAC uses 1600-byte DMA buffers and 24 TX descriptors so each MTU-sized
packet needs one descriptor instead of three 512-byte descriptors. Multicast
runs above the fanout worker; TinyUSB runs on CPU0, separate from the FEC worker.

A stalled USB consumer cannot take packets from the Ethernet queue. Offline
Ethernet drops its output and does not stop USB. Nonblocking UDP sends can drop;
radio FEC does not protect losses after recovery. RTP gaps can require waiting
for the next keyframe. DisplayPort/MSP remains TBA.

The receiver queue/pool is compiled at 256 entries instead of desktop's 4096:
CMake generates a pinned-header variant without modifying dependency sources.
The pool slab is approximately 384 KB. RS block storage, tasks and lwIP buffers
add memory; real heap/CPU/throughput must be measured on the P4. Source changes
are guarded when the dependency pin changes. The L3 RX queue is 256 datagrams.

## USB compatibility

RGP4 framing remains version 2. Channel 0 now advertises `format:rtp-h264-v1`
and `read_only:true` in LIST and OPEN. New Android rejects OPEN replies without
the expected format. P4 rejects inbound DATA for this receive-only source.
Update P4 and Android together; the older app expects encapsulated/FEC packets
and sends feedback, so it is incompatible with this video service.

General direct-L3 bidirectional registration remains available for other
channels. Processed video is registered as a local packet source, never as a
fabricated L3 `UpstreamQueue`.

## Ethernet multicast receiver

Multicast is active after Ethernet gets an IPv4 address; socket output is bound
to that Ethernet address using IP_MULTICAST_IF. Link/IP loss disables output;
DHCP address changes recreate the multicast socket. No remote listener needs
to be configured on P4. This is LAN multicast (TTL 1), not routed discovery.

Copy [ground-video.sdp](../tools/video/ground-video.sdp) to a LAN client:

```sdp
v=0
o=- 0 0 IN IP4 127.0.0.1
s=Rover ground H264
c=IN IP4 239.255.42.1/1
t=0 0
m=video 5600 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1
```

On Rockchip ground stations, vstreamer's `multicast_receiver` sample runs
`multicast_src -> rtp_h264_depay -> h264_decoder_mpp -> sdl_sink` (KMSDRM).
Build with the multicast source, RTP depayloader, MPP decoder and SDL enabled,
then run `multicast_receiver --interface end1` (or your Ethernet interface).
The host's former L3/stream_receiver services must be stopped; P4 now owns FEC
and radio feedback.

Example receiver: `ffplay -protocol_whitelist file,udp,rtp -i ground-video.sdp`.
The client must receive the sender's in-band SPS/PPS and a keyframe. There is
no RTSP server, RTCP or multicast-client feedback aggregation. The payload type
96 and 90 kHz clock match the pinned rover RTP packer. Switch multicast/IGMP
configuration and client firewall may affect LAN delivery.

## Verification

Portable integration test:

```sh
cmake -S tests/video -B video-build -DCMAKE_BUILD_TYPE=Debug
cmake --build video-build -j 4
ctest --test-dir video-build --output-on-failure
```

It sends real vstreamer/FEC packets into stock stream_receiver over loopback,
drops shards, verifies byte-identical RTP on both output queues, reconstructs
an IDR, checks reverse feedback and stalls one branch to verify isolation.
The test needs local UDP sockets. Existing `tests/run_host_tests.py` additionally
checks receive-only source metadata, source delivery and invalid inbound DATA.
`owned_ingress_test` and `l3_owned_queue_test` verify original storage identity
and retention after receiver/upstream shutdown. `fec_decode_test` exercises
792 full/partial 21/31 blocks with mixed erasures and misaligned input views,
checks bytes and aligned recovered rows, then releases every input owner while
retaining outputs. Run these under ASan/UBSan when changing ownership.

Runtime logs: GROUND_VIDEO contains receiver counters, recovered RTP sequence
gaps before fanout, L3 queue delivery and evictions, FEC input, feedback failures,
USB and multicast evictions, multicast sends, offline discards, transient send
retries, final send failures and the last socket errno. Offline discards before
DHCP are not wired losses. RTP sequence gaps on a client do not identify the
network segment where packets went missing. GROUND_USB reports USB
credits and sends. Counters distinguish recovered radio loss from branch loss.

Bench acceptance: moving video on Android and an Ethernet multicast client
simultaneously; USB detach/zero credits leaves multicast running; Ethernet
link/DHCP loss leaves USB running; reconnects and sender restart; FEC recovery
and feedback under induced radio loss; CPU, heap, queue drops and latency at
1080p30 / 6 Mbit/s plus FEC. Host/CI success does not establish these results.
