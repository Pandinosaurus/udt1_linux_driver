#!/bin/bash
modprobe udt1cri_usb

if [[ ! -f "/dev/can0" ]]; then
    sudo ip link set can0 type can bitrate 1000000
    sudo ip link set can0 up
fi
if [[ ! -f "/dev/can1" ]]; then
    sudo ip link set can1 type can bitrate 1000000
    sudo ip link set can1 up
fi
if [[ ! -f "/dev/can2" ]]; then
    sudo ip link set can2 type can bitrate 1000000
    sudo ip link set can2 up
fi
if [[ ! -f "/dev/can3" ]]; then
    sudo ip link set can3 type can bitrate 1000000
    sudo ip link set can3 up
fi
