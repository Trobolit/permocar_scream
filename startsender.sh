#!/bin/bash
echo "setting up rtsp:s"
( ./rtsp/bin/rtsp 10.180.65.26 31111 &> rtspOut1.txt ) &
P1=$!
echo "one done"
( ./rtsp/bin/rtsp 10.180.65.27 31112 &> rtspOut2.txt ) &
P2=$!
echo "two done"
( ./rtsp/bin/rtsp 10.180.65.28 31113 &> rtspOut3.txt ) &
P3=$!
echo "three done"
( ./rtsp/bin/rtsp 10.180.65.29 31114 &> rtspOut4.txt ) &
P4=$!
echo "four done"

# scream_sender <options> auth nsources out_ip out_port in_ip_1 in_port_1 prio_1 .. in_ip_n in_port_n prio_n
echo "starting scream sender!"
./scream_sender/bin/scream_sender -cwvmem 60 -scale 1.0 YWRtaW46YWRtaW5hZG1pbjE= 4 10.9.0.2 31110 10.180.65.28 31113 0.2 10.180.65.27 31112 0.2 10.180.65.26 31111 0.2 10.180.65.29 31114 1.0  &> screamSender.log
#echo "scream sender running, all seems good from bash."
wait $P1 $P2 $P3 $P4


tail -n 35 rtspOut1.txt
