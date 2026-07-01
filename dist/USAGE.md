# Assessment Agent (Linux x86_64)

서버 리소스를 수집해 RabbitMQ(192.168.3.121)로 발행하는 단일 바이너리.

## 1. 설치

```
chmod +x assessment-agent-linux-x86_64
RABBITMQ_HOST=192.168.3.121 RABBITMQ_PASS=assessment ./assessment-agent-linux-x86_64 install < /dev/null
```

## 2. 설치 후 보정

```
sed -i -e 's#^RABBITMQ_VHOST=.*#RABBITMQ_VHOST=/assessment#' -e 's/^RABBITMQ_WORKER_USER=.*/RABBITMQ_WORKER_USER=/' ~/.config/assessment-agent/agent.env
systemctl --user restart assessment-agent
```

## 3. 확인

```
journalctl --user -u assessment-agent -n 10 --no-pager
```

`published inventory` + `worker=off`, exception 없으면 정상.

## 4. 제거

```
./assessment-agent-linux-x86_64 uninstall
```
