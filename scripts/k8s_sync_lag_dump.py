#!/usr/bin/env python3
"""Keep a short pod-log history and write a separate trace when sync lag spikes."""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import fcntl
import json
import os
import pathlib
import signal
import subprocess
import threading
import time


METRICS = (
    "ton_node_status_unixtime",
    "ton_node_status_last_masterchain_block_at",
    "ton_node_status_shard_client_at",
    "ton_node_status_last_masterchain_block_seqno",
    "ton_node_status_shard_client_masterchain_seqno",
)


def kubectl(*args: str, timeout: float = 15.0) -> str:
    return subprocess.run(
        ["kubectl", *args], check=True, capture_output=True, text=True, timeout=timeout
    ).stdout


def current_pod(namespace: str, node: str) -> tuple[str, str]:
    data = json.loads(kubectl("-n", namespace, "get", "pods", "-l", f"node={node}", "-o", "json"))
    pods = [
        pod for pod in data["items"]
        if pod["status"].get("phase") == "Running"
        and not pod["metadata"].get("deletionTimestamp")
    ]
    if not pods:
        raise RuntimeError(f"no running pod for {node}")
    pods.sort(key=lambda pod: pod["metadata"]["creationTimestamp"], reverse=True)
    pod = pods[0]
    return pod["metadata"]["name"], pod["spec"]["containers"][0]["name"]


def scrape(namespace: str, pod: str, port: int) -> tuple[dict[str, float], str]:
    try:
        raw = kubectl(
            "-n", namespace, "get", "--raw",
            f"/api/v1/namespaces/{namespace}/pods/{pod}:{port}/proxy/metrics",
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        raw = kubectl(
            "-n", namespace, "exec", f"pod/{pod}", "--", "curl", "-fsS", "--max-time", "5",
            f"http://127.0.0.1:{port}/metrics",
        )
    values = {}
    for line in raw.splitlines():
        name, _, value = line.partition(" ")
        if name in METRICS:
            values[name] = float(value)
    if len(values) != len(METRICS):
        raise RuntimeError(f"missing sync metrics: {set(METRICS) - values.keys()}")
    return values, raw


def log_time(line: str) -> float:
    try:
        return dt.datetime.fromisoformat(line.split(" ", 1)[0].replace("Z", "+00:00")).timestamp()
    except ValueError:
        return time.time()


class Dump:
    def __init__(self, directory: pathlib.Path, pre_seconds: int, post_seconds: int,
                 max_seconds: int, cooldown_seconds: int) -> None:
        self.directory = directory
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.pre_seconds = pre_seconds
        self.post_seconds = post_seconds
        self.max_seconds = max_seconds
        self.cooldown_seconds = cooldown_seconds
        self.lines: collections.deque[tuple[float, str]] = collections.deque(maxlen=50000)
        self.lock = threading.Lock()
        self.log_file = None
        self.metrics_file = None
        self.started = 0.0
        self.until = 0.0
        self.last_finished = 0.0
        self.path: pathlib.Path | None = None
        self.armed = True
        self.clear_samples = 0

    def add_line(self, line: str) -> None:
        with self.lock:
            self.lines.append((log_time(line), line))
            cutoff = time.time() - self.pre_seconds
            while self.lines and self.lines[0][0] < cutoff:
                self.lines.popleft()
            if self.log_file:
                self.log_file.write(line)

    def sample(self, pod: str, values: dict[str, float], raw: str, threshold: float,
               consecutive: int) -> bool:
        now = time.monotonic()
        master_lag = values["ton_node_status_unixtime"] - values["ton_node_status_last_masterchain_block_at"]
        shard_lag = values["ton_node_status_unixtime"] - values["ton_node_status_shard_client_at"]
        gap = (values["ton_node_status_last_masterchain_block_seqno"]
               - values["ton_node_status_shard_client_masterchain_seqno"])
        triggered = max(master_lag, shard_lag) >= threshold and consecutive >= 2
        sample = {"at": dt.datetime.now(dt.timezone.utc).isoformat(), "pod": pod,
                  "master_lag_seconds": master_lag, "shard_lag_seconds": shard_lag,
                  "masterchain_gap": gap, "metrics": values}
        with self.lock:
            if max(master_lag, shard_lag) < threshold:
                self.clear_samples += 1
                if self.clear_samples >= 3:
                    self.armed = True
            else:
                self.clear_samples = 0
            if triggered and self.armed and not self.log_file and now - self.last_finished >= self.cooldown_seconds:
                stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
                self.path = self.directory / f"{stamp}-{pod}"
                self.log_file = pathlib.Path(f"{self.path}.log").open("x", buffering=1)
                self.metrics_file = pathlib.Path(f"{self.path}.jsonl").open("x", buffering=1)
                pathlib.Path(f"{self.path}.prom").write_text(raw)
                for _, line in self.lines:
                    self.log_file.write(line)
                self.started = now
                self.until = now + self.post_seconds
                self.armed = False
                print(f"[capture-start] pod={pod} master={master_lag:.0f}s shard={shard_lag:.0f}s "
                      f"log={self.path}.log", flush=True)
            if self.log_file:
                assert self.metrics_file is not None
                self.metrics_file.write(json.dumps(sample, sort_keys=True) + "\n")
                if triggered:
                    self.until = min(self.started + self.max_seconds, now + self.post_seconds)
                if now >= self.until:
                    self.finish_locked()
        return triggered

    def finish_locked(self) -> None:
        if self.log_file:
            self.log_file.close()
            assert self.metrics_file is not None
            self.metrics_file.close()
            print(f"[capture-done] log={self.path}.log", flush=True)
            self.log_file = None
            self.metrics_file = None
            self.last_finished = time.monotonic()

    def close(self) -> None:
        with self.lock:
            self.finish_locked()


def follow_logs(namespace: str, node: str, dump: Dump, stop: threading.Event) -> None:
    last_pod = None
    last_timestamp = None
    last_line = None
    while not stop.is_set():
        process = None
        try:
            pod, container = current_pod(namespace, node)
            if pod != last_pod:
                last_pod = pod
                last_timestamp = None
                last_line = None
            since = [f"--since-time={last_timestamp}"] if last_timestamp else [f"--since={dump.pre_seconds}s"]
            process = subprocess.Popen(
                ["kubectl", "-n", namespace, "logs", f"pod/{pod}", "-c", container,
                 "--timestamps", *since, "-f"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
            )
            assert process.stdout is not None
            print(f"[log-follow] pod={pod}", flush=True)
            for line in process.stdout:
                if stop.is_set():
                    break
                if line == last_line:
                    continue
                dump.add_line(line)
                last_timestamp = line.split(" ", 1)[0]
                last_line = line
            if process.poll() is not None and process.stderr is not None:
                error = process.stderr.read().strip()
                if error:
                    print(f"[log-follow-error] pod={pod} {error[:500]}", flush=True)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            print(f"[log-follow-error] {exc}", flush=True)
        finally:
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
        stop.wait(2)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--namespace", default="nodes")
    parser.add_argument("--node", default="echo")
    parser.add_argument("--port", type=int, default=8321)
    parser.add_argument("--directory", type=pathlib.Path, required=True)
    parser.add_argument("--threshold", type=float, default=2.0)
    parser.add_argument("--interval", type=float, default=2.0)
    parser.add_argument("--pre-seconds", type=int, default=120)
    parser.add_argument("--post-seconds", type=int, default=45)
    parser.add_argument("--max-seconds", type=int, default=300)
    parser.add_argument("--cooldown-seconds", type=int, default=20)
    parser.add_argument("--duration", type=float, default=0.0)
    args = parser.parse_args()
    os.umask(0o077)
    args.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    lock = (args.directory / "collector.lock").open("a+")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit(f"collector already running for {args.directory}")
    lock.seek(0)
    lock.truncate()
    lock.write(f"{os.getpid()}\n")
    lock.flush()
    dump = Dump(args.directory, args.pre_seconds, args.post_seconds,
                args.max_seconds, args.cooldown_seconds)
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGHUP, lambda *_: stop.set())
    follower = threading.Thread(target=follow_logs, args=(args.namespace, args.node, dump, stop), daemon=True)
    follower.start()
    deadline = time.monotonic() + args.duration if args.duration else float("inf")
    consecutive = 0
    try:
        while not stop.is_set() and time.monotonic() < deadline:
            try:
                pod, _ = current_pod(args.namespace, args.node)
                values, raw = scrape(args.namespace, pod, args.port)
                lag = max(values["ton_node_status_unixtime"] - values["ton_node_status_last_masterchain_block_at"],
                          values["ton_node_status_unixtime"] - values["ton_node_status_shard_client_at"])
                consecutive = consecutive + 1 if lag >= args.threshold else 0
                dump.sample(pod, values, raw, args.threshold, consecutive)
            except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                print(f"[metrics-error] {exc}", flush=True)
            stop.wait(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        follower.join(timeout=3)
        dump.close()
        lock.close()


if __name__ == "__main__":
    main()
