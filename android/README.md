# Android app

The Android client now lives in
[FlyingComputer2/gnd-android](https://github.com/FlyingComputer2/gnd-android).
It implements matching RGP4 v2 bulk framing, u16 channel handlers, packet credits,
and the native RTP/H.264 depay feeding Android MediaCodec. P4 owns FEC and feedback;
channel 0 requires `rtp-h264-v1` in OPEN. Update both peers together.
