#!/usr/bin/env bash
set -euo pipefail

docker stop rabbitmq
docker rm rabbitmq
