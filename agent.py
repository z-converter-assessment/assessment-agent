#!/usr/bin/env python3
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

import pika
from dotenv import load_dotenv

load_dotenv()


def _run(cmd: list[str]) -> str:
    result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return result.stdout


def collect_hostname() -> str:
    return _run(["hostname"]).strip()


def collect_nproc() -> str:
    return _run(["nproc"]).strip()


def collect_free_mem_mb() -> int:
    output = _run(["free", "-m"])
    for line in output.splitlines():
        if line.startswith("Mem:"):
            return int(line.split()[1])
    raise RuntimeError("failed to parse `free -m` output")


def collect_lsblk() -> list[dict]:
    output = _run(["lsblk", "--json", "-o", "NAME,SIZE"])
    data = json.loads(output)
    return [{"name": d["name"], "size": d["size"]} for d in data.get("blockdevices", [])]


def collect_internal_ips() -> list[str]:
    output = _run(["ip", "-o", "-4", "addr", "show"])
    ips: list[str] = []
    for line in output.splitlines():
        match = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", line)
        if not match:
            continue
        ip = match.group(1)
        if ip.startswith("127."):
            continue
        ips.append(ip)
    return ips


def collect_external_ip() -> list[str]:
    configured = os.environ.get("AGENT_EXTERNAL_IP")
    if configured:
        return [ip.strip() for ip in configured.split(",") if ip.strip()]
    try:
        with urllib.request.urlopen("https://api.ipify.org", timeout=3) as resp:
            return [resp.read().decode().strip()]
    except Exception:
        return []


def collect_inventory() -> dict:
    return {
        "hostname": collect_hostname(),
        "nproc": collect_nproc(),
        "free": {"mem_total_mb": collect_free_mem_mb()},
        "lsblk_raw": collect_lsblk(),
        "ip_raw": {
            "internal": collect_internal_ips(),
            "external": collect_external_ip(),
        },
    }


def publish_to_rabbitmq(data: dict) -> None:
    host = os.environ.get("RABBITMQ_HOST", "localhost")
    port = int(os.environ.get("RABBITMQ_PORT", "5672"))
    user = os.environ.get("RABBITMQ_USER", "admin")
    password = os.environ.get("RABBITMQ_PASS", "admin")
    exchange = os.environ.get("RABBITMQ_EXCHANGE", "assessment")
    queue = os.environ.get("RABBITMQ_QUEUE", "server.metrics")
    routing_key = os.environ.get("RABBITMQ_ROUTING_KEY", "metrics")

    credentials = pika.PlainCredentials(user, password)
    params = pika.ConnectionParameters(host=host, port=port, credentials=credentials)

    connection = pika.BlockingConnection(params)
    try:
        channel = connection.channel()
        channel.exchange_declare(exchange=exchange, exchange_type="direct", durable=True)
        channel.queue_declare(queue=queue, durable=True)
        channel.queue_bind(exchange=exchange, queue=queue, routing_key=routing_key)
        channel.basic_publish(
            exchange=exchange,
            routing_key=routing_key,
            body=json.dumps(data).encode("utf-8"),
            properties=pika.BasicProperties(
                content_type="application/json",
                delivery_mode=pika.DeliveryMode.Persistent,
            ),
        )
    finally:
        connection.close()


def run_once() -> None:
    data = collect_inventory()
    publish_to_rabbitmq(data)
    print(f"[agent] published inventory for {data['hostname']}", file=sys.stderr)


def main() -> int:
    interval = int(os.environ.get("AGENT_INTERVAL_SEC", "60"))
    if interval <= 0:
        run_once()
        return 0

    print(f"[agent] loop mode: interval={interval}s (Ctrl+C to exit)", file=sys.stderr)
    while True:
        try:
            run_once()
        except Exception as exc:
            print(f"[agent] iteration failed: {exc!r}", file=sys.stderr)
        try:
            time.sleep(interval)
        except KeyboardInterrupt:
            print("[agent] stopped by user", file=sys.stderr)
            return 0


if __name__ == "__main__":
    sys.exit(main())
