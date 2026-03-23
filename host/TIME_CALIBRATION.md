# Time Calibration And Profiling

## Scope

This document describes the current host-side time calibration and profiling flow used by `Net_Aim_Control`.

It covers:

- which UDP ports are used
- which data flows use those ports
- the clock offset request/response protocol
- the timing fields parsed from board result JSON
- how the host estimates `nominal_send_ms`
- how to determine whether clock offset is working
- common failure modes

This document reflects the current code behavior in `Net_Aim_Control`.

## End-To-End Data Flows

`Net_Aim_Control` currently runs three independent UDP-related flows:

1. Video sender flow
   Host runs `ffmpeg.exe`, captures the center ROI, encodes with `h264_nvenc`, and sends `UDP + MPEG-TS` to the board.

2. Detection result flow
   Board sends one UDP JSON result per frame back to the host.

3. Clock offset flow
   Host sends a lightweight NTP-style UDP JSON request to the board and waits for a UDP JSON response.

These three flows are separate. Clock offset does not reuse the result port.

## Port Map

### 1. Video stream port

- Direction: host -> board
- Protocol: UDP
- Default board target IP: `192.168.7.2`
- Default board target port: `5000`
- Purpose: carry the H264 NVENC MPEG-TS video stream produced by `ffmpeg`

Relevant config:

- `sender_output_ip`
- `sender_output_port`

### 2. Detection result port

- Direction: board -> host
- Protocol: UDP
- Default host listen IP: `0.0.0.0`
- Default host listen port: `6000`
- Purpose: receive per-frame detection result JSON

Relevant config:

- `listen_ip`
- `listen_port`

### 3. Clock offset port

- Direction: host -> board request, board -> host response
- Protocol: UDP
- Default board target IP: `192.168.7.2`
- Current board target port: `45678`
- Purpose: estimate host/board wall clock offset for profiling

Relevant config:

- `clock_offset_ip`
- `clock_offset_port`
- `clock_offset_interval_ms`
- `clock_offset_timeout_ms`

## Important Port Clarification

For clock offset:

- the board listens on fixed port `45678`
- the host does **not** listen on a fixed local port for clock offset
- the host clock offset client binds to local port `0`, which means the OS assigns a temporary ephemeral source port

This means:

- the board must reply to the exact source IP and source port from `recvfrom()`
- the board must **not** hardcode the response destination as host port `6000`
- the board must **not** assume the host clock offset client uses a fixed port

## Host Startup Sequence

At runtime, the host does the following:

1. Initialize UDP receiver for detection results on `listen_ip:listen_port`
2. Initialize mouse DLL and `MoveR(dx, dy)` capability
3. Optionally start the ffmpeg sender service
4. Initialize the timing profiler
5. If clock offset is enabled, initialize the clock offset client with:
   - `clock_offset_ip`
   - `clock_offset_port`
   - `clock_offset_timeout_ms`
6. During the main loop:
   - periodically send clock offset requests
   - poll for clock offset responses
   - receive board detection results
   - update profiling estimates
   - run the existing tracker and `MoveR(dx, dy)` control logic

Clock offset is observation-only. It does not change target selection or mouse control.

## Clock Offset Protocol

The host uses an NTP-style four-timestamp exchange:

- `t1`: host send request time
- `t2`: board receive request time
- `t3`: board send response time
- `t4`: host receive response time

### Offset formula

```text
offset = ((t2 - t1) + (t3 - t4)) / 2
```

### Delay formula

```text
delay = (t4 - t1) - (t3 - t2)
```

### Time unit requirements

All protocol timestamps must use:

- wall clock time
- integer values
- nanoseconds

Do not use:

- milliseconds
- microseconds
- monotonic-only timestamps without wall-clock meaning
- strings or floating-point values

## Clock Offset Request Format

The host sends a UDP JSON request like this:

```json
{
  "type": "clock_offset_request",
  "request_id": 1,
  "t1_ns": 1711111111111111111
}
```

Field requirements:

- `type`: always `"clock_offset_request"`
- `request_id`: integer request identifier
- `t1_ns`: host wall clock timestamp in nanoseconds

## Clock Offset Response Format

The board must reply with a UDP JSON response like this:

```json
{
  "request_id": 1,
  "t1_ns": 1711111111111111111,
  "t2_ns": 1711111111112222222,
  "t3_ns": 1711111111113333333
}
```

Field requirements:

- `request_id`: must match the request
- `t1_ns`: must echo the request value
- `t2_ns`: board receive time in nanoseconds
- `t3_ns`: board response send time in nanoseconds

The host currently treats the response as invalid if any of these fields are missing or not integer values.

## Board-Side Minimum Behavior

The minimum valid board-side clock offset server behavior is:

1. Listen on UDP port `45678`
2. Receive a JSON request
3. Parse `request_id` and `t1_ns`
4. Record `t2_ns` immediately after receipt
5. Build the response JSON
6. Record `t3_ns` immediately before sending
7. Send the response back to the exact source address returned by `recvfrom()`

Pseudo-flow:

```text
recvfrom(sock, request, from_addr)
t2_ns = board_wall_time_ns()
response = { request_id, t1_ns, t2_ns, t3_ns }
sendto(sock, response, from_addr)
```

## Detection Result JSON Timing Fields

The normal board detection result packet is independent from the clock offset packet.

The host currently parses these optional result timing fields:

### Frame-level timing

- `frame_pts`
- `frame_best_effort_ts`
- `frame_time_base_num`
- `frame_time_base_den`

### Nested timing fields

- `timing.board_wall_infer_done_ms`
- `timing.board_preprocess_ms`
- `timing.board_inference_ms`
- `timing.board_postprocess_ms`
- `timing.board_result_send_start_ms`
- `timing.board_result_send_end_ms`

Backward-compatible fallback is also supported for older fields:

- `timing.board_infer_us`
- `timing.board_post_us`

## Example Detection Result JSON

```json
{
  "seq": 123,
  "frame_pts": 456000,
  "frame_best_effort_ts": 456000,
  "frame_time_base_num": 1,
  "frame_time_base_den": 90000,
  "frame_width": 640,
  "frame_height": 640,
  "box_count": 1,
  "timing": {
    "board_wall_infer_done_ms": 1711111111111,
    "board_preprocess_ms": 1.40,
    "board_inference_ms": 12.30,
    "board_postprocess_ms": 0.80,
    "board_result_send_start_ms": 1711111111112,
    "board_result_send_end_ms": 1711111111113
  },
  "boxes": [
    {
      "class_id": 0,
      "score": 0.91,
      "x1": 100,
      "y1": 120,
      "x2": 180,
      "y2": 220,
      "cx": 140,
      "cy": 170
    }
  ]
}
```

## Sender Timing Context

When the sender service starts successfully, the host records sender-side timing context:

- `sender_start_wall_ms`
- `sender_start_wall_ns`
- `sender_framerate`
- ROI width and height
- ROI offsets
- `pts_per_second`

This context is used only for profiling. It does not affect control.

## Why `nominal_send_ms` Uses First Valid Result Anchoring

The host no longer assumes that ffmpeg process startup time is the same as the first frame's media timeline origin.

Instead, when the host receives the first valid result packet containing `frame_pts`, it records:

- `first_frame_pts`
- `first_result_recv_wall_ms`

Then later frames use:

```text
nominal_send_ms = first_result_recv_wall_ms + delta_pts_converted_to_ms
```

This avoids the fixed bias caused by using raw ffmpeg process launch time as the media timeline anchor.

## `nominal_send_ms` Calculation

If `frame_pts` exists:

1. On the first valid packet:
   - store `first_frame_pts`
   - store `first_result_recv_wall_ms`
2. For later packets:
   - compute `delta_pts = frame_pts - first_frame_pts`
3. Convert `delta_pts` to milliseconds:
   - prefer `frame_time_base_num` and `frame_time_base_den`
   - otherwise fall back to `pts_per_second`
4. Compute:

```text
nominal_send_ms = first_result_recv_wall_ms + delta_ms
```

If `frame_pts` is missing, `nominal_send_ms` stays unavailable.

## Derived Profiling Metrics

When enough data is available, the host computes:

### 1. Host receive time

```text
host_result_recv_wall_ms
```

Meaning:

- wall-clock time on the host when the result packet was received

### 2. Nominal send time

```text
nominal_send_ms
```

Meaning:

- estimated time that the corresponding frame entered the sender timeline

### 3. End-to-end to infer done

If all are available:

- `board_wall_infer_done_ms`
- `clock_offset_ms`
- `nominal_send_ms`

Then:

```text
e2e_to_infer_done_ms = (board_wall_infer_done_ms - clock_offset_ms) - nominal_send_ms
```

Meaning:

- estimated elapsed time from sender timeline frame origin to board inference completion, converted into host time domain

### 4. Result return time

If all are available:

- `board_wall_infer_done_ms`
- `clock_offset_ms`
- `host_result_recv_wall_ms`

Then:

```text
result_return_ms = host_result_recv_wall_ms - (board_wall_infer_done_ms - clock_offset_ms)
```

Meaning:

- estimated time for board result packaging and network return after inference completion

### 5. Board stage timings

- `board_pre_ms = timing.board_preprocess_ms`
- `board_infer_ms = timing.board_inference_ms`
- `board_post_ms = timing.board_postprocess_ms`
- `board_result_send_ms = board_result_send_end_ms - board_result_send_start_ms`

## Profiling Output Fields

The host currently prints a low-frequency profiling summary line. Typical fields include:

- `sender=running|stopped`
- `offset=...`
- `offset_ep=<ip>:<port>`
- `req=sent|no`
- `resp=ok|no`
- `delay=<value>|N/A`
- `seq=<value>|N/A`
- `frame_pts=<value>|N/A`
- `nominal_send_ms=<value>|N/A`
- `board_wall_infer_done_ms=<value>|N/A`
- `board_pre_ms=<value>|N/A`
- `e2e_to_infer_done_ms=<value>|N/A`
- `result_return_ms=<value>|N/A`
- `board_infer_ms=<value>|N/A`
- `board_post_ms=<value>|N/A`
- `board_result_send_ms=<value>|N/A`

The `--display` debug panel shows the same timing/profiling state in a compact form.

## Clock Offset Status Meanings

### `offset=disabled`

- clock offset client is not enabled
- no request will be sent

### `offset=waiting`

- request has been sent
- host is waiting for a matching response

### `offset=valid:<x>ms`

- the host received a valid response
- `clock_offset_ms` is available
- timing metrics that depend on offset can now be computed

### `offset=timeout`

- request was sent
- no valid response arrived before timeout

Most likely causes:

- board server not running
- board listening on the wrong port
- board response not sent back to the host request source port
- packet dropped by firewall or routing

### `offset=server_unreachable`

- request could not be sent or receive path reported a socket-level problem
- IP, port, routing, or network reachability is wrong

### `offset=init_failed`

- local client initialization failed
- socket or Winsock setup failed

### `offset=invalid_response`

- board sent something back
- but required fields were missing or invalid

Most likely causes:

- missing `request_id`
- missing `t1_ns`
- missing `t2_ns`
- missing `t3_ns`
- wrong types
- wrong time units

## How To Confirm Clock Offset Is Working

Clock offset is considered working when the host profiling line shows all of the following:

- `offset=valid:<value_ms>`
- `req=sent`
- `resp=ok`
- `delay=<value_ms>`

After that, if result timing fields also exist, these should usually stop being `N/A`:

- `e2e_to_infer_done_ms`
- `result_return_ms`

## Common Failure Modes

### 1. Wrong board port

Symptom:

- `offset=timeout`
- `req=sent`
- `resp=no`

Fix:

- verify the board really listens on `45678`
- verify host `clock_offset_port` is also `45678`

### 2. Board replies to host port `6000`

Symptom:

- result JSON works
- clock offset never becomes valid

Cause:

- board sends offset response to the detection-result port instead of the request source port

Fix:

- reply to the source address returned by `recvfrom()`

### 3. Board timestamps use `ms` or `us`

Symptom:

- response arrives
- offset is obviously wrong or unstable

Fix:

- make `t1_ns`, `t2_ns`, `t3_ns` all nanoseconds

### 4. Invalid response schema

Symptom:

- `offset=invalid_response`

Fix:

- include exactly the required integer fields:
  - `request_id`
  - `t1_ns`
  - `t2_ns`
  - `t3_ns`

### 5. Network or firewall problem

Symptom:

- `offset=server_unreachable`
- or repeated `timeout`

Fix:

- verify host and board can exchange UDP on `45678`
- verify the board replies to the correct ephemeral source port

## Relation To `saved_udp_packets.jsonl`

`saved_udp_packets.jsonl` stores successfully parsed detection result packets only.

It does not store clock offset traffic.

The `sender_port` field inside `saved_udp_packets.jsonl` is:

- the UDP source port of the board result packet
- not the video port
- not the clock offset server port

This is why values like `49414` can appear there. That value is usually the board-side source port for result JSON packets.

## Current Practical Interpretation

The current practical port usage is:

- host receives detection results on `6000`
- host sends video to board `5000`
- host sends clock offset requests to board `45678`
- board replies to the host's temporary request source port

If any one of these is mismatched, the corresponding flow can fail while the others still work.

## Recommended Bring-Up Checklist

1. Confirm video stream path:
   - host -> board `5000`
2. Confirm detection result path:
   - board -> host `6000`
3. Confirm clock offset server path:
   - board listens on `45678`
4. Confirm board response behavior:
   - reply to the source address from `recvfrom()`
5. Confirm protocol fields:
   - request uses `request_id`, `t1_ns`
   - response uses `request_id`, `t1_ns`, `t2_ns`, `t3_ns`
6. Confirm time units:
   - all clock offset timestamps are nanoseconds
7. Confirm host console:
   - `offset=valid:<value>ms`
   - `req=sent`
   - `resp=ok`
   - `delay=<value>ms`

Once these conditions are met, clock offset is considered operational.
