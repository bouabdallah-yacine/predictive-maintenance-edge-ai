#!/usr/bin/env python3
"""
UART → MQTT gateway
===================
Stands in for the ESP32 when the STM32 is simulated in Renode (or plugged in over USB):
reads "$MM,...*CS" frames and publishes the same JSON as the ESP32 firmware.

  python uart_gateway.py --tcp localhost:3456          # Renode
  python uart_gateway.py --serial COM5                 # real board (Windows)
  python uart_gateway.py --serial /dev/ttyACM0         # real board (Linux)
  python uart_gateway.py --stdin < frames.txt          # offline test

Dependencies: pip install paho-mqtt pyserial
"""
import argparse
import json
import socket
import sys
import time

LEVELS = ["NORMAL", "WARNING", "CRITICAL"]


class FrameParser:
    """State-machine parser, identical to Core/Src/protocol.c."""

    def __init__(self):
        self.state = "WAIT"
        self.body = ""
        self.cs_txt = ""
        self.ok = self.err_cs = self.err_fmt = 0

    def feed(self, ch):
        if ch == "$":
            self.state, self.body, self.cs_txt = "BODY", "", ""
            return None
        if self.state == "BODY":
            if ch == "*":
                self.state = "CS"
            elif ch in "\r\n" or len(self.body) > 94:
                self.err_fmt += 1
                self.state = "WAIT"
            else:
                self.body += ch
        elif self.state == "CS":
            self.cs_txt += ch
            if len(self.cs_txt) == 2:
                self.state = "WAIT"
                return self._finish()
        return None

    def _finish(self):
        cs = 0
        for c in self.body:
            cs ^= ord(c)
        try:
            if int(self.cs_txt, 16) != cs:
                self.err_cs += 1
                return None
            tag, *fields = self.body.split(",")
            if tag != "MM" or len(fields) != 7:
                raise ValueError
            seq, temp_d, hum_d, vib, peak, curr, flags = map(int, fields)
        except ValueError:
            self.err_fmt += 1
            return None
        self.ok += 1
        return {
            "seq": seq,
            "temperature": temp_d / 10,
            "humidity": hum_d / 10,
            "vibRms": vib / 1000,
            "vibPeak": peak / 1000,
            "current": curr / 1000,
            "state": LEVELS[min((flags >> 4) & 3, 2)],
            "faultInjected": bool(flags & 4),
            "health": {"dht": bool(flags & 1), "mpu": bool(flags & 2)},
        }


def byte_source(args):
    """Character generator for the selected source (auto-reconnect)."""
    if args.stdin:
        for line in sys.stdin:
            yield from line
        return
    if args.serial:
        import serial  # pyserial
        with serial.Serial(args.serial, args.baud, timeout=1) as port:
            print(f"[GW] serial port {args.serial} @ {args.baud}")
            while True:
                data = port.read(64)
                yield from data.decode("ascii", errors="ignore")
    host, port = args.tcp.split(":")
    while True:
        try:
            with socket.create_connection((host, int(port)), timeout=5) as s:
                print(f"[GW] connected to Renode {host}:{port}")
                s.settimeout(None)
                while True:
                    data = s.recv(256)
                    if not data:
                        break
                    yield from data.decode("ascii", errors="ignore")
        except OSError as e:
            print(f"[GW] Renode unreachable ({e}), retrying in 2 s...")
            time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description="UART → MQTT gateway")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--tcp", default="localhost:3456", help="host:port of the Renode terminal")
    src.add_argument("--serial", help="serial port (COM5, /dev/ttyACM0...)")
    src.add_argument("--stdin", action="store_true", help="read frames from standard input")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--broker", default="broker.hivemq.com")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--prefix", default="pfe-monitor-7f3a")
    ap.add_argument("--device", default="stm32-01")
    ap.add_argument("--dry-run", action="store_true", help="print the JSON without publishing")
    args = ap.parse_args()

    client = None
    topic = f"{args.prefix}/{args.device}/telemetry"
    if not args.dry_run:
        import paho.mqtt.client as mqtt
        try:
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"gw-{args.device}")
        except AttributeError:  # paho-mqtt < 2.0
            client = mqtt.Client(client_id=f"gw-{args.device}")
        client.will_set(f"{args.prefix}/{args.device}/status", "offline", retain=True)
        client.connect(args.broker, args.port, keepalive=30)
        client.loop_start()
        client.publish(f"{args.prefix}/{args.device}/status", "online", retain=True)
        print(f"[GW] MQTT {args.broker}:{args.port} → {topic}")

    parser = FrameParser()
    last_seq = None
    t0 = time.time()
    for ch in byte_source(args):
        frame = parser.feed(ch)
        if not frame:
            continue
        if last_seq is not None and frame["seq"] != (last_seq + 1) % 65536:
            print(f"[GW] ⚠ lost frame(s): {last_seq} → {frame['seq']}")
        last_seq = frame["seq"]
        frame["deviceId"] = args.device
        frame["uptimeMs"] = int((time.time() - t0) * 1000)
        payload = json.dumps(frame, ensure_ascii=False)
        if client:
            client.publish(topic, payload)
        print(payload)

    print(f"[GW] done: {parser.ok} frames OK, {parser.err_cs} checksum errors, {parser.err_fmt} format errors")


if __name__ == "__main__":
    main()
