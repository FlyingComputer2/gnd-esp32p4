# Ground video receive and fanout

P4 runs the pinned vstreamer `stream_receiver` component, not a second codec.
The L3-owned video `UpstreamQueue` (index 1) feeds receiver UDP
`127.0.0.1:21082` through a local nonblocking bridge. This adapts the existing
UDP-only receiver without changing its wire/FEC/feedback implementation.
Receiver reports return on the same bridge socket and are sent back using
`UpstreamQueue::try_send()`. Android no longer sends radio receiver reports.

The receiver strips stream framing, repairs RS erasures, sequences recovered
SDUs and emits RTP H.264. P4 does not depayload or decode H.264. Recovered RTP
packets, including their original sequence, timestamps and extensions, are
shared by two independently bounded queues:

| Consumer | Queue | Output |
| --- | --- | --- |
| USB | 32 packets, oldest eviction | Channel 0, one RTP packet per DATA |
| Ethernet | 32 packets, oldest eviction | UDP 239.255.42.1:5600, TTL 1 |

A stalled USB consumer cannot take packets from the Ethernet queue. Offline
Ethernet drops its output and does not stop USB. Nonblocking UDP sends can drop;
radio FEC does not protect losses after recovery. RTP gaps can require waiting
for the next keyframe. DisplayPort/MSP remains TBA.

The receiver queue/pool is compiled at 64 entries instead of desktop's 4096:
CMake generates a pinned-header variant without modifying dependency sources.
The pool slab is approximately 96 KB. RS block storage, tasks and lwIP buffers
add memory; real heap/CPU/throughput must be measured on the P4. Source changes
are guarded when the dependency pin changes. The L3 RX queue is 64 datagrams.

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

Runtime logs: GROUND_VIDEO contains receiver counters, bridge/feedback failures,
USB and multicast evictions, multicast sends and drops. GROUND_USB reports USB
credits and sends. Counters distinguish recovered radio loss from branch loss.

Bench acceptance: moving video on Android and an Ethernet multicast client
simultaneously; USB detach/zero credits leaves multicast running; Ethernet
link/DHCP loss leaves USB running; reconnects and sender restart; FEC recovery
and feedback under induced radio loss; CPU, heap, queue drops and latency at
1080p30 / 6 Mbit/s plus FEC. Host/CI success does not establish these results.
