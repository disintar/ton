#!/usr/bin/env python3
"""
Monitor TON block propagation from node trace logs and shard lag from /metrics.

The log part needs nodes built with the DTON_TRACE_BLOCK_PROPAGATION trace and
the env enabled on the deployments that should participate in real per-block
latency measurements. The metrics part works with the regular /metrics endpoint.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import json
import math
import re
import signal
import subprocess
import threading
import time
import urllib.request
from collections import defaultdict
from typing import Iterable


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
TRACE_TS_RE = re.compile(r"\]\[(\d+\.\d+)\]\[[^\]]+\].*\[block-propagation\]")
METRIC_RE = re.compile(
    r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{([^}]*)\})?\s+([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)"
)
LABEL_RE = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:\\.|[^"\\])*)"')
FIELD_RE = re.compile(r"(?:^| )([a-zA-Z_][a-zA-Z0-9_]*)=([^ ]*)")


DEFAULT_STAGES = (
    "custom.recv",
    "custom.deserialize",
    "apply.applied_set",
    "shardclient.applied_all_shards",
    "shardclient.saved_to_db",
)


@dataclasses.dataclass(frozen=True)
class Target:
    node: str
    pod: str
    container: str
    pod_ip: str


@dataclasses.dataclass
class MetricSample:
    name: str
    labels: dict[str, str]
    value: float


@dataclasses.dataclass
class MetricsSnapshot:
    node: str
    sampled_at: float
    values: dict[str, float]
    shards: dict[str, int]
    actor_delay_10s: tuple[float, str] | None
    actor_delay_10m: tuple[float, str] | None
    scrape_error: str | None = None


@dataclasses.dataclass
class LogEvent:
    node: str
    ts: float
    stage: str
    block: str
    wc: str
    shard: str
    seqno: str
    source: str
    src: str
    result: str
    ms_from_custom: float | None
    ms_stage: float | None
    line: str


def run_kubectl(args: list[str], timeout: float = 10.0) -> str:
    command = ["kubectl", *args]
    completed = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )
    return completed.stdout


def parse_nodes(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def is_ready(pod: dict) -> bool:
    conditions = pod.get("status", {}).get("conditions", [])
    return any(cond.get("type") == "Ready" and cond.get("status") == "True" for cond in conditions)


def discover_targets(namespace: str, nodes: list[str]) -> dict[str, Target]:
    pods = json.loads(run_kubectl(["-n", namespace, "get", "pods", "-o", "json"], timeout=20.0))
    targets: dict[str, Target] = {}
    for node in nodes:
        candidates = []
        for pod in pods.get("items", []):
            name = pod.get("metadata", {}).get("name", "")
            phase = pod.get("status", {}).get("phase")
            deleting = pod.get("metadata", {}).get("deletionTimestamp") is not None
            if not name.startswith(f"{node}-") or phase != "Running" or deleting:
                continue
            candidates.append(pod)
        candidates.sort(
            key=lambda item: (
                is_ready(item),
                item.get("metadata", {}).get("creationTimestamp", ""),
            ),
            reverse=True,
        )
        if not candidates:
            continue
        pod = candidates[0]
        containers = pod.get("spec", {}).get("containers", [])
        container = next(
            (item["name"] for item in containers if node in item.get("name", "")),
            containers[0]["name"] if containers else "",
        )
        targets[node] = Target(
            node=node,
            pod=pod["metadata"]["name"],
            container=container,
            pod_ip=pod.get("status", {}).get("podIP", ""),
        )
    missing = [node for node in nodes if node not in targets]
    if missing:
        raise RuntimeError(f"no running pods found for nodes: {', '.join(missing)}")
    return targets


def parse_labels(raw: str | None) -> dict[str, str]:
    if not raw:
        return {}
    labels = {}
    for key, value in LABEL_RE.findall(raw):
        labels[key] = bytes(value, "utf-8").decode("unicode_escape")
    return labels


def parse_metrics(text: str) -> list[MetricSample]:
    samples = []
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        match = METRIC_RE.match(line)
        if not match:
            continue
        samples.append(
            MetricSample(
                name=match.group(1),
                labels=parse_labels(match.group(2)),
                value=float(match.group(3)),
            )
        )
    return samples


def scrape_metrics(
    namespace: str,
    target: Target,
    port: int,
    mode: str,
    timeout: float,
) -> MetricsSnapshot:
    sampled_at = time.time()
    try:
        if mode == "apiserver-proxy":
            text = run_kubectl(
                [
                    "-n",
                    namespace,
                    "get",
                    "--raw",
                    f"/api/v1/namespaces/{namespace}/pods/{target.pod}:{port}/proxy/metrics",
                ],
                timeout=timeout,
            )
        elif mode == "direct":
            if not target.pod_ip:
                raise RuntimeError("pod IP is empty")
            with urllib.request.urlopen(f"http://{target.pod_ip}:{port}/metrics", timeout=timeout) as response:
                text = response.read().decode("utf-8", errors="replace")
        elif mode == "exec":
            text = run_kubectl(
                [
                    "-n",
                    namespace,
                    "exec",
                    f"pod/{target.pod}",
                    "-c",
                    target.container,
                    "--",
                    "/bin/sh",
                    "-c",
                    f"curl -fsS --max-time {max(1, int(timeout))} http://127.0.0.1:{port}/metrics",
                ],
                timeout=timeout + 2.0,
            )
        else:
            raise RuntimeError(f"unsupported metrics mode: {mode}")
        sampled_at = time.time()
    except Exception as exc:  # noqa: BLE001 - diagnostic script should keep running.
        return MetricsSnapshot(
            node=target.node,
            sampled_at=sampled_at,
            values={},
            shards={},
            actor_delay_10s=None,
            actor_delay_10m=None,
            scrape_error=str(exc),
        )

    values: dict[str, float] = {}
    shards: dict[str, int] = {}
    actor_10s: tuple[float, str] | None = None
    actor_10m: tuple[float, str] | None = None
    for sample in parse_metrics(text):
        if not sample.labels:
            values[sample.name] = sample.value
        if sample.name == "node_shard_seqno":
            shard_id = sample.labels.get("shard_id")
            if shard_id:
                shards[shard_id] = int(sample.value)
        elif sample.name == "actor_max_delay_seconds":
            period = sample.labels.get("period")
            actor = sample.labels.get("actor", "-")
            if period == "10s" and (actor_10s is None or sample.value > actor_10s[0]):
                actor_10s = (sample.value, actor)
            if period == "10m" and (actor_10m is None or sample.value > actor_10m[0]):
                actor_10m = (sample.value, actor)
    return MetricsSnapshot(
        node=target.node,
        sampled_at=sampled_at,
        values=values,
        shards=shards,
        actor_delay_10s=actor_10s,
        actor_delay_10m=actor_10m,
    )


def parse_optional_float(value: str | None) -> float | None:
    if value is None or value == "-" or value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def parse_log_event(node: str, raw_line: str) -> LogEvent | None:
    line = ANSI_RE.sub("", raw_line.rstrip("\n"))
    if "[block-propagation]" not in line:
        return None
    ts_match = TRACE_TS_RE.search(line)
    ts = float(ts_match.group(1)) if ts_match else time.time()
    fields = {key: value for key, value in FIELD_RE.findall(line)}
    stage = fields.get("stage")
    block = fields.get("block")
    if not stage or not block:
        return None
    return LogEvent(
        node=node,
        ts=ts,
        stage=stage,
        block=block,
        wc=fields.get("wc", "-"),
        shard=fields.get("shard", "-"),
        seqno=fields.get("seqno", "-"),
        source=fields.get("source", "-"),
        src=fields.get("src", "-"),
        result=fields.get("result", "-"),
        ms_from_custom=parse_optional_float(fields.get("ms_from_custom")),
        ms_stage=parse_optional_float(fields.get("ms_stage")),
        line=line,
    )


def percentile(values: Iterable[float], q: float) -> float | None:
    ordered = sorted(values)
    if not ordered:
        return None
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * q) - 1))
    return ordered[index]


def fmt_ms(value: float | None) -> str:
    if value is None:
        return "-"
    return f"{value:.1f}ms"


def fmt_sec(value: float | None) -> str:
    if value is None:
        return "-"
    return f"{value:.2f}s"


class MonitorState:
    def __init__(
        self,
        nodes: list[str],
        stages: set[str],
        window_seconds: float,
        propagation_warn_ms: float,
        apply_warn_ms: float,
        min_nodes: int,
        logs_enabled: bool,
    ) -> None:
        self.nodes = nodes
        self.stages = stages
        self.window_seconds = window_seconds
        self.propagation_warn_ms = propagation_warn_ms
        self.apply_warn_ms = apply_warn_ms
        self.min_nodes = min_nodes
        self.logs_enabled = logs_enabled
        self.lock = threading.Lock()
        self.print_lock = threading.Lock()
        self.stage_events: dict[str, dict[str, dict[str, LogEvent]]] = defaultdict(lambda: defaultdict(dict))
        self.stage_spreads: dict[str, dict[str, tuple[float, float, int, str]]] = defaultdict(dict)
        self.alerted_stage_blocks: set[tuple[str, str]] = set()
        self.node_block_events: dict[str, dict[str, dict[str, LogEvent]]] = defaultdict(lambda: defaultdict(dict))
        self.apply_latencies: dict[str, dict[str, tuple[float, float, str]]] = defaultdict(dict)
        self.alerted_apply_blocks: set[tuple[str, str]] = set()
        self.latest_metrics: dict[str, MetricsSnapshot] = {}
        self.shard_observed: dict[str, dict[int, dict[str, float]]] = defaultdict(lambda: defaultdict(dict))
        self.metrics_spreads: dict[str, dict[int, tuple[float, float, int]]] = defaultdict(dict)
        self.last_log_event_at: dict[str, float] = {}

    def emit(self, text: str) -> None:
        with self.print_lock:
            print(text, flush=True)

    def prune_locked(self, now: float) -> None:
        cutoff = now - self.window_seconds
        for stage in list(self.stage_spreads):
            for block, (_, observed_at, _, _) in list(self.stage_spreads[stage].items()):
                if observed_at < cutoff:
                    self.stage_spreads[stage].pop(block, None)
                    self.stage_events[stage].pop(block, None)
                    self.alerted_stage_blocks.discard((stage, block))
        for node in list(self.apply_latencies):
            for block, (_, observed_at, _) in list(self.apply_latencies[node].items()):
                if observed_at < cutoff:
                    self.apply_latencies[node].pop(block, None)
                    self.node_block_events[node].pop(block, None)
                    self.alerted_apply_blocks.discard((node, block))

    def add_log_event(self, event: LogEvent) -> None:
        if event.stage not in self.stages:
            return
        now = time.time()
        alerts: list[str] = []
        with self.lock:
            self.last_log_event_at[event.node] = now
            events_for_block = self.stage_events[event.stage][event.block]
            previous = events_for_block.get(event.node)
            stage_changed = False
            if previous is None or event.ts < previous.ts:
                events_for_block[event.node] = event
                stage_changed = True
            count = len(events_for_block)
            if stage_changed and count >= self.min_nodes:
                min_ts = min(item.ts for item in events_for_block.values())
                max_ts = max(item.ts for item in events_for_block.values())
                spread_ms = (max_ts - min_ts) * 1000.0
                self.stage_spreads[event.stage][event.block] = (spread_ms, now, count, event.seqno)
                alert_key = (event.stage, event.block)
                if spread_ms >= self.propagation_warn_ms and alert_key not in self.alerted_stage_blocks:
                    self.alerted_stage_blocks.add(alert_key)
                    first = min(events_for_block.values(), key=lambda item: item.ts)
                    last = max(events_for_block.values(), key=lambda item: item.ts)
                    alerts.append(
                        "[propagation-delay] "
                        f"stage={event.stage} spread={fmt_ms(spread_ms)} nodes={count} "
                        f"first={first.node} last={last.node} seqno={event.seqno} block={event.block}"
                    )

            node_events = self.node_block_events[event.node][event.block]
            previous_node_event = node_events.get(event.stage)
            node_changed = False
            if previous_node_event is None or event.ts < previous_node_event.ts:
                node_events[event.stage] = event
                node_changed = True
            recv = node_events.get("custom.recv")
            applied = node_events.get("apply.applied_set")
            if node_changed and recv and applied and applied.result == "ok" and applied.ts >= recv.ts:
                latency_ms = (applied.ts - recv.ts) * 1000.0
                self.apply_latencies[event.node][event.block] = (latency_ms, now, applied.seqno)
                alert_key = (event.node, event.block)
                if latency_ms >= self.apply_warn_ms and alert_key not in self.alerted_apply_blocks:
                    self.alerted_apply_blocks.add(alert_key)
                    alerts.append(
                        "[local-apply-delay] "
                        f"node={event.node} latency={fmt_ms(latency_ms)} seqno={applied.seqno} "
                        f"ms_from_custom={applied.ms_from_custom} block={event.block}"
                    )
            self.prune_locked(now)
        for alert in alerts:
            self.emit(alert)

    def add_metrics(self, snapshots: list[MetricsSnapshot], args: argparse.Namespace) -> None:
        now = time.time()
        lines: list[str] = []
        with self.lock:
            for snapshot in snapshots:
                self.latest_metrics[snapshot.node] = snapshot
                if snapshot.scrape_error:
                    lines.append(f"[metrics-error] node={snapshot.node} error={snapshot.scrape_error}")
                    continue
                for shard_id, seqno in snapshot.shards.items():
                    self.shard_observed[shard_id][seqno].setdefault(snapshot.node, snapshot.sampled_at)
                    nodes_seen = self.shard_observed[shard_id][seqno]
                    if len(nodes_seen) >= self.min_nodes:
                        spread_ms = (max(nodes_seen.values()) - min(nodes_seen.values())) * 1000.0
                        self.metrics_spreads[shard_id][seqno] = (spread_ms, now, len(nodes_seen))
                        if spread_ms >= args.metrics_spread_warn_ms:
                            lines.append(
                                "[metrics-observed-spread] "
                                f"shard={shard_id} seqno={seqno} spread={fmt_ms(spread_ms)} "
                                f"nodes={len(nodes_seen)} note=scrape-limited"
                            )

            summaries = []
            for node in self.nodes:
                snapshot = self.latest_metrics.get(node)
                if not snapshot or snapshot.scrape_error:
                    summaries.append(f"{node}:no-metrics")
                    continue
                values = snapshot.values
                status_time = values.get("ton_node_status_unixtime")
                shard_at = values.get("ton_node_status_shard_client_at")
                status_lag = status_time - shard_at if status_time is not None and shard_at is not None else None
                wall_lag = max(0.0, snapshot.sampled_at - shard_at) if shard_at is not None else None
                last_mc = values.get("ton_node_status_last_masterchain_block_seqno")
                shard_mc = values.get("ton_node_status_shard_client_masterchain_seqno")
                mc_lag = int(last_mc - shard_mc) if last_mc is not None and shard_mc is not None else None
                actor10 = snapshot.actor_delay_10s[0] * 1000.0 if snapshot.actor_delay_10s else None
                summaries.append(
                    f"{node}:status_lag={fmt_sec(status_lag)} wall_lag={fmt_sec(wall_lag)} "
                    f"mc_lag={mc_lag if mc_lag is not None else '-'} actor10={fmt_ms(actor10)}"
                )
                if status_lag is not None and status_lag >= args.shard_lag_warn_sec:
                    lines.append(
                        "[shard-lag] "
                        f"node={node} status_lag={fmt_sec(status_lag)} wall_lag={fmt_sec(wall_lag)} "
                        f"warn={fmt_sec(args.shard_lag_warn_sec)}"
                    )
                if mc_lag is not None and mc_lag >= args.mc_lag_warn:
                    lines.append(f"[mc-lag] node={node} mc_lag={mc_lag} warn={args.mc_lag_warn}")

            shard_ids = sorted({shard for snapshot in self.latest_metrics.values() for shard in snapshot.shards})
            for shard_id in shard_ids:
                values = {
                    node: snapshot.shards[shard_id]
                    for node, snapshot in self.latest_metrics.items()
                    if not snapshot.scrape_error and shard_id in snapshot.shards
                }
                if len(values) < self.min_nodes:
                    continue
                min_node = min(values, key=values.get)
                max_node = max(values, key=values.get)
                spread_seqno = values[max_node] - values[min_node]
                if spread_seqno >= args.shard_seqno_spread_warn:
                    def node_state(node: str) -> tuple[int | None, int | None, int | None]:
                        snapshot = self.latest_metrics[node]
                        sample_values = snapshot.values
                        last_mc = sample_values.get("ton_node_status_last_masterchain_block_seqno")
                        shard_mc = sample_values.get("ton_node_status_shard_client_masterchain_seqno")
                        status_time = sample_values.get("ton_node_status_unixtime")
                        shard_at = sample_values.get("ton_node_status_shard_client_at")
                        status_lag = (
                            int(status_time - shard_at)
                            if status_time is not None and shard_at is not None
                            else None
                        )
                        return (
                            int(last_mc) if last_mc is not None else None,
                            int(shard_mc) if shard_mc is not None else None,
                            status_lag,
                        )

                    min_last_mc, min_shard_mc, min_status_lag = node_state(min_node)
                    max_last_mc, max_shard_mc, max_status_lag = node_state(max_node)
                    mc_values = [node_state(node)[0] for node in values]
                    known_mc_values = [value for value in mc_values if value is not None]
                    mc_spread = (
                        max(known_mc_values) - min(known_mc_values)
                        if len(known_mc_values) >= self.min_nodes
                        else None
                    )
                    reason = "different_masterchain_states" if mc_spread and mc_spread > 0 else "same_masterchain_state"
                    lines.append(
                        "[shard-seqno-spread] "
                        f"shard={shard_id} spread_seqno={spread_seqno} "
                        f"min={values[min_node]}@{min_node} max={values[max_node]}@{max_node} "
                        f"mc_spread={mc_spread if mc_spread is not None else '-'} reason={reason} "
                        f"min_state=last_mc:{min_last_mc},shard_mc:{min_shard_mc},status_lag:{min_status_lag}s "
                        f"max_state=last_mc:{max_last_mc},shard_mc:{max_shard_mc},status_lag:{max_status_lag}s"
                    )

        self.emit(f"[metrics] {' | '.join(summaries)}")
        for line in lines:
            self.emit(line)

    def summary(self) -> None:
        now = time.time()
        lines: list[str] = []
        with self.lock:
            self.prune_locked(now)
            for stage in sorted(self.stages):
                rows = list(self.stage_spreads.get(stage, {}).values())
                values = [row[0] for row in rows]
                if not values:
                    continue
                lines.append(
                    "[propagation-summary] "
                    f"stage={stage} blocks={len(values)} "
                    f"p50={fmt_ms(percentile(values, 0.50))} "
                    f"p95={fmt_ms(percentile(values, 0.95))} "
                    f"max={fmt_ms(max(values))}"
                )
            for node in self.nodes:
                rows = list(self.apply_latencies.get(node, {}).values())
                values = [row[0] for row in rows]
                if values:
                    lines.append(
                        "[local-apply-summary] "
                        f"node={node} blocks={len(values)} "
                        f"p50={fmt_ms(percentile(values, 0.50))} "
                        f"p95={fmt_ms(percentile(values, 0.95))} "
                        f"max={fmt_ms(max(values))}"
                    )
                if self.logs_enabled:
                    last = self.last_log_event_at.get(node)
                    if last is None:
                        lines.append(f"[trace-status] node={node} no-trace-events-seen")
                    elif now - last > 60.0:
                        lines.append(f"[trace-status] node={node} last_trace_event_ago={fmt_sec(now - last)}")
        for line in lines:
            self.emit(line)


def metrics_loop(
    stop: threading.Event,
    namespace: str,
    targets: dict[str, Target],
    state: MonitorState,
    args: argparse.Namespace,
) -> None:
    while not stop.is_set():
        started = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(targets)) as pool:
            futures = [
                pool.submit(
                    scrape_metrics,
                    namespace,
                    target,
                    args.metrics_port,
                    args.metrics_mode,
                    args.kubectl_timeout,
                )
                for target in targets.values()
            ]
            snapshots = [future.result() for future in concurrent.futures.as_completed(futures)]
        state.add_metrics(snapshots, args)
        elapsed = time.monotonic() - started
        stop.wait(max(0.1, args.metrics_interval - elapsed))


def log_loop(
    stop: threading.Event,
    namespace: str,
    target: Target,
    state: MonitorState,
    args: argparse.Namespace,
) -> None:
    command = [
        "kubectl",
        "-n",
        namespace,
        "logs",
        f"deploy/{target.node}",
        "-c",
        target.container,
        f"--since={args.log_since}",
        "-f",
    ]
    while not stop.is_set():
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None
        assert process.stderr is not None

        def stderr_reader() -> None:
            for err_line in process.stderr:
                if stop.is_set():
                    break
                text = err_line.strip()
                if text:
                    state.emit(f"[logs-error] node={target.node} {text}")

        err_thread = threading.Thread(target=stderr_reader, daemon=True)
        err_thread.start()
        for line in process.stdout:
            if stop.is_set():
                break
            event = parse_log_event(target.node, line)
            if event:
                state.add_log_event(event)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
        if not stop.is_set():
            state.emit(f"[logs-restart] node={target.node} kubectl logs exited, reconnecting")
            stop.wait(2.0)


def summary_loop(stop: threading.Event, state: MonitorState, interval: float) -> None:
    while not stop.wait(interval):
        state.summary()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Monitor real block propagation from trace logs and shard lag from TON /metrics endpoints."
    )
    parser.add_argument("--namespace", default="nodes")
    parser.add_argument("--nodes", default="worm,london,echo,solana")
    parser.add_argument("--metrics-port", type=int, default=8321)
    parser.add_argument(
        "--metrics-mode",
        choices=("apiserver-proxy", "direct", "exec"),
        default="apiserver-proxy",
        help="How to read /metrics. apiserver-proxy uses kubectl get --raw and usually works from outside cluster.",
    )
    parser.add_argument("--metrics-interval", type=float, default=2.0)
    parser.add_argument("--summary-interval", type=float, default=15.0)
    parser.add_argument("--window-seconds", type=float, default=300.0)
    parser.add_argument("--duration", type=float, default=0.0, help="Stop after N seconds. 0 means run forever.")
    parser.add_argument("--log-since", default="2m")
    parser.add_argument("--stages", default=",".join(DEFAULT_STAGES))
    parser.add_argument("--min-nodes", type=int, default=2)
    parser.add_argument("--propagation-warn-ms", type=float, default=200.0)
    parser.add_argument("--apply-warn-ms", type=float, default=250.0)
    parser.add_argument("--shard-lag-warn-sec", type=float, default=3.0)
    parser.add_argument("--mc-lag-warn", type=int, default=1)
    parser.add_argument("--shard-seqno-spread-warn", type=int, default=1)
    parser.add_argument("--metrics-spread-warn-ms", type=float, default=2500.0)
    parser.add_argument("--kubectl-timeout", type=float, default=8.0)
    parser.add_argument("--no-logs", action="store_true")
    parser.add_argument("--no-metrics", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    nodes = parse_nodes(args.nodes)
    stages = set(parse_nodes(args.stages))
    if args.min_nodes < 2:
        raise SystemExit("--min-nodes must be >= 2")
    if args.min_nodes > len(nodes):
        raise SystemExit("--min-nodes cannot be greater than --nodes count")

    targets = discover_targets(args.namespace, nodes)
    print(
        "[targets] "
        + " | ".join(
            f"{node}:pod={target.pod},container={target.container},ip={target.pod_ip}"
            for node, target in targets.items()
        ),
        flush=True,
    )
    state = MonitorState(
        nodes=nodes,
        stages=stages,
        window_seconds=args.window_seconds,
        propagation_warn_ms=args.propagation_warn_ms,
        apply_warn_ms=args.apply_warn_ms,
        min_nodes=args.min_nodes,
        logs_enabled=not args.no_logs,
    )
    stop = threading.Event()

    def handle_signal(signum: int, _frame: object) -> None:
        print(f"[stop] signal={signum}", flush=True)
        stop.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    threads: list[threading.Thread] = []
    if not args.no_metrics:
        threads.append(
            threading.Thread(
                target=metrics_loop,
                args=(stop, args.namespace, targets, state, args),
                daemon=True,
            )
        )
    if not args.no_logs:
        for target in targets.values():
            threads.append(
                threading.Thread(
                    target=log_loop,
                    args=(stop, args.namespace, target, state, args),
                    daemon=True,
                )
            )
    threads.append(threading.Thread(target=summary_loop, args=(stop, state, args.summary_interval), daemon=True))
    for thread in threads:
        thread.start()

    started = time.monotonic()
    try:
        while not stop.is_set():
            if args.duration and time.monotonic() - started >= args.duration:
                stop.set()
                break
            time.sleep(0.2)
    finally:
        stop.set()
        for thread in threads:
            thread.join(timeout=3.0)
        state.summary()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
