#!/bin/bash
echo "starting camera receivers"

( ./scream_receiver/bin/scream_receiver 10.9.0.3 31110 |& tee reciever.log ) &
P1=$!


( gst-launch-1.0 udpsrc port=30112 ! "application/x-rtp,payload=98,encoding-name=H264" ! rtpjitterbuffer latency=10 ! rtph264depay ! avdec_h264 ! videoconvert ! xvimagesink sync=false ) &
P2=$!

( gst-launch-1.0 udpsrc port=30114 ! "application/x-rtp,payload=98,encoding-name=H264" ! rtpjitterbuffer latency=10 ! rtph264depay ! avdec_h264 ! videoconvert ! xvimagesink sync=false ) &
P3=$!

( gst-launch-1.0 udpsrc port=30116 ! "application/x-rtp,payload=98,encoding-name=H264" ! rtpjitterbuffer latency=10 ! rtph264depay ! avdec_h264 ! videoconvert ! xvimagesink sync=false ) &
P4=$!

( gst-launch-1.0 udpsrc port=30118 ! "application/x-rtp,payload=98,encoding-name=H264" ! rtpjitterbuffer latency=10 ! rtph264depay ! avdec_h264 ! videoconvert ! xvimagesink sync=false ) &
P5=$!


echo "Receiver and camera pipes up!"
wait $P1 $P2 $P3 $P4 $P5

