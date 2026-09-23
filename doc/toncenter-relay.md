# Toncenter relay from the node

An independent C++ module adds a best-effort HTTP path after external-message admission.
Both direct LiteServer sends and validated private-overlay relays submit to it.
Public-overlay input is not sent to Toncenter. The global broadcast-disable flag is respected.
Native TON overlay delivery never waits for the HTTP response.

Enable the build with -DTON_USE_TONCENTER_RELAY=ON (off by default).
Build dependency: libcurl development headers/library (Ubuntu: libcurl4-openssl-dev).
Runtime: libcurl and the system CA certificate bundle.

Enable only on the desired nodes:
- DTON_TONCENTER_RELAY=1
- DTON_TONCENTER_API_KEY from a Kubernetes Secret (optional for unauthenticated API)
- DTON_TONCENTER_RPS, integer 1..100, default 1. Set according to the key's allowance.

Requests use verified HTTPS to https://toncenter.com/api/v3/message with JSON
{"boc":"<base64>"}. A dedicated worker performs HTTP, with 1s connect and 2.5s total
timeouts. No redirects or automatic retries. A 429 response pauses sending for 5s.
The queue is bounded to 64 messages / at most 64 KiB each; messages older than 5s
are dropped. Deduplication holds up to 8192 BOC hashes for 60s. These limits protect
node operation; this is an additional delivery path, not a durable queue.

A response counts as accepted only for HTTP 200 with a valid 32-byte base64
message_hash. Acceptance does not prove on-chain inclusion. Logs contain a SHA-256
of the serialized BOC, response category, HTTP/transport codes and returned message hash.
They do not contain the BOC, API key or raw response body.

## Prometheus

The existing node Prometheus endpoint exports:
- ton_node_toncenter_enabled
- ton_node_toncenter_requests_total (actual HTTP attempts)
- ton_node_toncenter_accepted_total (accepted API responses)
- ton_node_toncenter_network_errors_total
- ton_node_toncenter_http_errors_total (excluding 429)
- ton_node_toncenter_rate_limited_total (429)
- ton_node_toncenter_invalid_responses_total (HTTP 200 with invalid response)
- ton_node_toncenter_enqueued_total
- ton_node_toncenter_duplicates_total
- ton_node_toncenter_queue_full_total
- ton_node_toncenter_expired_total
- ton_node_toncenter_queue_size

Counters reset when the node process restarts. Example hourly count:
sum(increase(ton_node_toncenter_requests_total[1h]))
Accepted count:
sum(increase(ton_node_toncenter_accepted_total[1h]))

## Verification

Build validator-engine and toncenter-relay-client. Run
python3 test/validator/test-toncenter-relay-http.py with the compiled client path.
Tests exercise request JSON/auth, valid responses, 400/429/500, malformed JSON,
invalid hashes, oversized responses, disconnects and the total timeout.

The standalone diagnostic client uses the same HTTP function as the node. Pass one
base64 BOC on stdin; API key comes from DTON_TONCENTER_API_KEY. Default URL is mainnet.
An optional URL argument is intended for the local test server.
Replaying an old signed message may legitimately return rejection or a duplicate response.
