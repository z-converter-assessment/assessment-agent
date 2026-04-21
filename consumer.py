#!/usr/bin/env python3
import json
import os
import sys

import pika
from dotenv import load_dotenv

load_dotenv()


def main() -> int:
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
    channel = connection.channel()

    channel.exchange_declare(exchange=exchange, exchange_type="direct", durable=True)
    channel.queue_declare(queue=queue, durable=True)
    channel.queue_bind(exchange=exchange, queue=queue, routing_key=routing_key)

    def on_message(ch, method, properties, body):
        print(f"[consumer] received from queue={queue} routing_key={method.routing_key}")
        try:
            payload = json.loads(body)
            print(json.dumps(payload, indent=2, ensure_ascii=False))
        except json.JSONDecodeError:
            print(f"[consumer] non-JSON body: {body!r}")
        ch.basic_ack(delivery_tag=method.delivery_tag)

    channel.basic_consume(queue=queue, on_message_callback=on_message)
    print(f"[consumer] waiting for messages on `{queue}` (Ctrl+C to exit)", file=sys.stderr)
    try:
        channel.start_consuming()
    except KeyboardInterrupt:
        channel.stop_consuming()
    finally:
        connection.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
