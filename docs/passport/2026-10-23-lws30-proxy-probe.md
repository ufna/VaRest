# libwebsockets 3.0 — B1 proxy passport

<!-- Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved. -->

Generated from a complete raw-rig matrix; values below are observed, not production policy.

Source commit: `c9e2880c6492de47c1729656da24757f15fdd0c2`. Scope: Linux modular editor raw rig; no native/cooked runtime claim.
Evidence directory: `/w/Unreal/VaRestHost/Saved/B1Proxy/task5/20261023T112356Z/full-1`; `sha256.json` hashes every original artifact.
Working-tree state at run start: `dirty (see manifest)`.

## Results

| Case | Open/echo | Route | APPEND/create/destroy/cert | Source/redirect requests | Seconds |
|---|---|---|---|---|---|
| direct-ws | True/True | direct | 1/1/1/0 | 1/0 | 0.002755 |
| direct-wss | True/True | direct | 1/1/1/2 | 1/0 | 0.043332 |
| explicit-ws | True/True | A | 1/1/1/0 | 1/0 | 0.004239 |
| explicit-wss | True/True | A | 1/1/1/2 | 1/0 | 0.046285 |
| deny-ws | False/False | A | 0/1/1/0 | 0/0 | 0.003887 |
| deny-wss | False/False | A | 0/1/1/0 | 0/0 | 0.002713 |
| close-ws | False/False | A | 0/1/1/0 | 0/0 | 0.013987 |
| close-wss | False/False | A | 0/1/1/0 | 0/0 | 0.001425 |
| silent-ws | False/False | A | 0/1/1/0 | 0/0 | 20.094499 |
| silent-wss | False/False | A | 0/1/1/0 | 0/0 | 20.107336 |
| redirect-ws | True/True | A | 2/2/1/0 | 1/1 | 0.005659 |
| redirect-wss | False/False | A | 1/2/1/3 | 1/0 | 0.008029 |
| env-ws | True/True | B | 1/1/1/0 | 1/0 | 0.003472 |
| env-wss | True/True | B | 1/1/1/2 | 1/0 | 0.046333 |
| precedence-ws | True/True | A | 1/1/1/0 | 1/0 | 0.002746 |
| precedence-wss | True/True | A | 1/1/1/2 | 1/0 | 0.047950 |
| env-noproxy-ws | True/True | B | 1/1/1/0 | 1/0 | 0.004445 |
| env-noproxy-wss | True/True | B | 1/1/1/2 | 1/0 | 0.046132 |
| redirect-cross-ws | False/False | A | 1/2/1/1 | 1/0 | 0.008597 |
| untrusted-wss | False/False | A | 0/1/1/1 | 0/0 | 0.004646 |
| upgrade403-ws | False/False | A | 1/1/1/0 | 1/0 | 0.003726 |
| upgrade403-wss | False/False | A | 1/1/1/2 | 1/0 | 0.004915 |

## Exact callback and journal observations

Each ordered list includes context/unresolved callbacks (conn=0), pseudo-events and repeated callbacks.
Counts use the recorded attempt connection ID. Token lifecycles retain earlier conn=0 CREATE records
when that token later becomes associated; every distinct attempt token must have one recorded destruction.
Certificate callbacks are per verification operation/depth. A count is not a connection count.
Error hex is bounded to 256 bytes; length is raw callback len, and pointer presence is independent.
Silent rows require native destruction before UE shutdown, with client EOF and no fixture deadline/stop.
Root accepted destruction-only silent completion: all tokens must be destroyed before the primary marker
and shutdown. Zero error callbacks stays zero; no separate terminal callback is invented.
Other negative controls retain the separate terminal-before-destruction requirement.
Case mode/TLS/hostname flags are checked against the fixed matrix, and TLS controls require associated certificates.
Redirect rows record the initial hostname-skip option for numeric loopback controls; chain checks remain.
That flag does not establish its effectiveness after a library redirect/reset; inspect the recorded certificate results.
Synthetic Authorization is appended on every raw APPEND; target presence is not proof of automatic replay.

### direct-ws

```json
{
  "case": "direct-ws",
  "certificates": [],
  "connect_a": 0,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 9,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 10,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 11,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 12,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 21,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 22,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 23,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.002755142981186509,
  "redirect_hostname_skip": false,
  "route": "direct",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        21
      ],
      "wsi": 3
    }
  ]
}
```

### direct-wss

```json
{
  "case": "direct-wss",
  "certificates": [
    {
      "conn": 1,
      "depth": 1,
      "preverify": 1,
      "seq": 6,
      "wsi": 3,
      "x509_error": 0
    },
    {
      "conn": 1,
      "depth": 0,
      "preverify": 1,
      "seq": 7,
      "wsi": 3,
      "x509_error": 0
    }
  ],
  "connect_a": 0,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 2,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=1",
      "reason": 58,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=0",
      "reason": 58,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 10,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 11,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 12,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 21,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 22,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 23,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 25,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 26,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.04333210410550237,
  "redirect_hostname_skip": false,
  "route": "direct",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        23
      ],
      "wsi": 3
    }
  ]
}
```

### explicit-ws

```json
{
  "case": "explicit-ws",
  "certificates": [],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [
    {
      "ev": "end",
      "id": 1,
      "t": 1134762.073027926
    }
  ],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 9,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 10,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 11,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 12,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 21,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 22,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 23,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.0042391379829496145,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        21
      ],
      "wsi": 3
    }
  ]
}
```

### explicit-wss

```json
{
  "case": "explicit-wss",
  "certificates": [
    {
      "conn": 1,
      "depth": 1,
      "preverify": 1,
      "seq": 6,
      "wsi": 3,
      "x509_error": 0
    },
    {
      "conn": 1,
      "depth": 0,
      "preverify": 1,
      "seq": 7,
      "wsi": 3,
      "x509_error": 0
    }
  ],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 2,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [
    {
      "ev": "end",
      "id": 1,
      "t": 1134787.267854246
    }
  ],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=1",
      "reason": 58,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=0",
      "reason": 58,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 10,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 11,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 12,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 21,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 22,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 23,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 25,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 26,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.04628506489098072,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        23
      ],
      "wsi": 3
    }
  ]
}
```

### deny-ws

```json
{
  "case": "deny-ws",
  "certificates": [],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 0,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [
    {
      "conn": 1,
      "hex": "",
      "input_present": false,
      "length": 0,
      "seq": 6,
      "wsi": 3
    }
  ],
  "fixture_witnesses": [
    {
      "ev": "denied",
      "id": 1,
      "status": 403,
      "t": 1134815.63759933
    },
    {
      "ev": "end",
      "id": 1,
      "t": 1134815.637602686
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CONNECTION_ERROR",
      "note": "",
      "reason": 1,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 8,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 10,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 11,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.0038872628938406706,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [],
  "source_requests": 0,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        7
      ],
      "wsi": 3
    }
  ]
}
```

### deny-wss

```json
{
  "case": "deny-wss",
  "certificates": [],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 0,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [
    {
      "conn": 1,
      "hex": "",
      "input_present": false,
      "length": 0,
      "seq": 6,
      "wsi": 3
    }
  ],
  "fixture_witnesses": [
    {
      "ev": "denied",
      "id": 1,
      "status": 403,
      "t": 1134851.525855071
    },
    {
      "ev": "end",
      "id": 1,
      "t": 1134851.525861032
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CONNECTION_ERROR",
      "note": "",
      "reason": 1,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 8,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 10,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 11,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.0027134378906339407,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [],
  "source_requests": 0,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        7
      ],
      "wsi": 3
    }
  ]
}
```

### close-ws

```json
{
  "case": "close-ws",
  "certificates": [],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 0,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [
    {
      "conn": 1,
      "hex": "",
      "input_present": false,
      "length": 0,
      "seq": 6,
      "wsi": 3
    }
  ],
  "fixture_witnesses": [
    {
      "ev": "closed_without_response",
      "id": 1,
      "t": 1134884.300473169
    },
    {
      "ev": "end",
      "id": 1,
      "t": 1134884.300475363
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CONNECTION_ERROR",
      "note": "",
      "reason": 1,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 10,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 11,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.013986841076985002,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [],
  "source_requests": 0,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        8
      ],
      "wsi": 3
    }
  ]
}
```

### close-wss

```json
{
  "case": "close-wss",
  "certificates": [],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 0,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [
    {
      "conn": 1,
      "hex": "",
      "input_present": false,
      "length": 0,
      "seq": 6,
      "wsi": 3
    }
  ],
  "fixture_witnesses": [
    {
      "ev": "closed_without_response",
      "id": 1,
      "t": 1134916.552872252
    },
    {
      "ev": "end",
      "id": 1,
      "t": 1134916.552875057
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CONNECTION_ERROR",
      "note": "",
      "reason": 1,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 8,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 10,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 11,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.001425446942448616,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [],
  "source_requests": 0,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        7
      ],
      "wsi": 3
    }
  ]
}
```

### silent-ws

```json
{
  "case": "silent-ws",
  "certificates": [],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 0,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [],
  "fixture_witnesses": [
    {
      "ev": "silent",
      "id": 1,
      "lifetime_seconds": 40.0,
      "t": 1134943.806068113
    },
    {
      "ev": "peer_eof",
      "id": 1,
      "t": 1134963.899220099
    },
    {
      "ev": "end",
      "id": 1,
      "t": 1134963.899235147
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 8,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 9,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 10,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 30.0,
  "primary_seconds": 20.094498595921323,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": true,
  "source_authorization": [],
  "source_requests": 0,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        6
      ],
      "wsi": 3
    }
  ]
}
```

### silent-wss

```json
{
  "case": "silent-wss",
  "certificates": [],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 0,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [],
  "fixture_witnesses": [
    {
      "ev": "silent",
      "id": 1,
      "lifetime_seconds": 40.0,
      "t": 1134992.812425037
    },
    {
      "ev": "peer_eof",
      "id": 1,
      "t": 1135012.917842658
    },
    {
      "ev": "end",
      "id": 1,
      "t": 1135012.917863637
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 8,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 9,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 10,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 30.0,
  "primary_seconds": 20.107336048036814,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": true,
  "source_authorization": [],
  "source_requests": 0,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        6
      ],
      "wsi": 3
    }
  ]
}
```

### redirect-ws

```json
{
  "case": "redirect-ws",
  "certificates": [],
  "connect_a": 2,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 2,
    "cert": 0,
    "create": 2,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [
    {
      "ev": "end",
      "id": 1,
      "t": 1135041.321820711
    },
    {
      "ev": "end",
      "id": 2,
      "t": 1135041.325370928
    }
  ],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 9,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 10,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 11,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 12,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 14,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 15,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 16,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 17,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 18,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 19,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 20,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 21,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 22,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 23,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 24,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 25,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 26,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 27,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.005659088026732206,
  "redirect_hostname_skip": true,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "target_requests": 1,
  "token_lifecycles": [
    {
      "create_sequences": [
        4,
        8
      ],
      "destroy_sequences": [
        24
      ],
      "wsi": 3
    }
  ]
}
```

### redirect-wss

```json
{
  "case": "redirect-wss",
  "certificates": [
    {
      "conn": 1,
      "depth": 1,
      "preverify": 1,
      "seq": 6,
      "wsi": 3,
      "x509_error": 0
    },
    {
      "conn": 1,
      "depth": 0,
      "preverify": 1,
      "seq": 7,
      "wsi": 3,
      "x509_error": 0
    },
    {
      "conn": 1,
      "depth": 0,
      "preverify": 0,
      "seq": 11,
      "wsi": 3,
      "x509_error": 62
    }
  ],
  "connect_a": 2,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 3,
    "create": 2,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [
    {
      "conn": 1,
      "hex": "636c69656e7420636f6e6e656374206661696c6564",
      "input_present": true,
      "length": 21,
      "seq": 12,
      "wsi": 3
    }
  ],
  "fixture_witnesses": [
    {
      "ev": "end",
      "id": 1,
      "t": 1135065.238961603
    },
    {
      "errno": 104,
      "ev": "io_error",
      "id": 2,
      "t": 1135065.242249351
    },
    {
      "ev": "end",
      "id": 2,
      "t": 1135065.242254521
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=1",
      "reason": 58,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=0",
      "reason": 58,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 10,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=0 error=62 depth=0",
      "reason": 58,
      "seq": 11,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "CONNECTION_ERROR",
      "note": "client connect failed",
      "reason": 1,
      "seq": 12,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 13,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 14,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 16,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 17,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.008029075106605887,
  "redirect_hostname_skip": true,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4,
        10
      ],
      "destroy_sequences": [
        13
      ],
      "wsi": 3
    }
  ]
}
```

### env-ws

```json
{
  "case": "env-ws",
  "certificates": [],
  "connect_a": 0,
  "connect_b": 1,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 9,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 10,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 11,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 12,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 21,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 22,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 23,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.0034721908159554005,
  "redirect_hostname_skip": false,
  "route": "B",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        21
      ],
      "wsi": 3
    }
  ]
}
```

### env-wss

```json
{
  "case": "env-wss",
  "certificates": [
    {
      "conn": 1,
      "depth": 1,
      "preverify": 1,
      "seq": 6,
      "wsi": 3,
      "x509_error": 0
    },
    {
      "conn": 1,
      "depth": 0,
      "preverify": 1,
      "seq": 7,
      "wsi": 3,
      "x509_error": 0
    }
  ],
  "connect_a": 0,
  "connect_b": 1,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 2,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=1",
      "reason": 58,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=0",
      "reason": 58,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 10,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 11,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 12,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 21,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 22,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 23,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 25,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 26,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.04633319191634655,
  "redirect_hostname_skip": false,
  "route": "B",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        23
      ],
      "wsi": 3
    }
  ]
}
```

### precedence-ws

```json
{
  "case": "precedence-ws",
  "certificates": [],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [
    {
      "ev": "end",
      "id": 1,
      "t": 1135135.174307308
    }
  ],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 9,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 10,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 11,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 12,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 21,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 22,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 23,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.0027461580466479063,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        21
      ],
      "wsi": 3
    }
  ]
}
```

### precedence-wss

```json
{
  "case": "precedence-wss",
  "certificates": [
    {
      "conn": 1,
      "depth": 1,
      "preverify": 1,
      "seq": 6,
      "wsi": 3,
      "x509_error": 0
    },
    {
      "conn": 1,
      "depth": 0,
      "preverify": 1,
      "seq": 7,
      "wsi": 3,
      "x509_error": 0
    }
  ],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 2,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [
    {
      "ev": "end",
      "id": 1,
      "t": 1135153.568973499
    }
  ],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=1",
      "reason": 58,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=0",
      "reason": 58,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 10,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 11,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 12,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 21,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 22,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 23,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 25,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 26,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.04794981307350099,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        23
      ],
      "wsi": 3
    }
  ]
}
```

### env-noproxy-ws

```json
{
  "case": "env-noproxy-ws",
  "certificates": [],
  "connect_a": 0,
  "connect_b": 1,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 9,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 10,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 11,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 12,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 21,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 22,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 23,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.0044452648144215345,
  "redirect_hostname_skip": false,
  "route": "B",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        21
      ],
      "wsi": 3
    }
  ]
}
```

### env-noproxy-wss

```json
{
  "case": "env-noproxy-wss",
  "certificates": [
    {
      "conn": 1,
      "depth": 1,
      "preverify": 1,
      "seq": 6,
      "wsi": 3,
      "x509_error": 0
    },
    {
      "conn": 1,
      "depth": 0,
      "preverify": 1,
      "seq": 7,
      "wsi": 3,
      "x509_error": 0
    }
  ],
  "connect_a": 0,
  "connect_b": 1,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 2,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": true,
  "errors": [],
  "fixture_witnesses": [],
  "harness_errors": [],
  "opened": true,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=1",
      "reason": 58,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=0",
      "reason": 58,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "FILTER_PRE_ESTABLISH",
      "note": "protocol= status=101",
      "reason": 2,
      "seq": 10,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "ESTABLISHED",
      "note": "bound=varest",
      "reason": 3,
      "seq": 11,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 12,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "command",
      "note": "write",
      "reason": -3,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 14,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "write",
      "note": "outbox ret=7 partial=0",
      "reason": -4,
      "seq": 15,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "RECEIVE",
      "note": "",
      "reason": 8,
      "seq": 16,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "command",
      "note": "close",
      "reason": -3,
      "seq": 17,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 18,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 19,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WRITEABLE",
      "note": "",
      "reason": 10,
      "seq": 20,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "close>",
      "note": "code=1000",
      "reason": -5,
      "seq": 21,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CLIENT_CLOSED",
      "note": "",
      "reason": 75,
      "seq": 22,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 23,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 24,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 25,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 26,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.04613203299231827,
  "redirect_hostname_skip": false,
  "route": "B",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        23
      ],
      "wsi": 3
    }
  ]
}
```

### redirect-cross-ws

```json
{
  "case": "redirect-cross-ws",
  "certificates": [
    {
      "conn": 1,
      "depth": 0,
      "preverify": 0,
      "seq": 9,
      "wsi": 3,
      "x509_error": 62
    }
  ],
  "connect_a": 2,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 1,
    "create": 2,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [
    {
      "conn": 1,
      "hex": "636c69656e7420636f6e6e656374206661696c6564",
      "input_present": true,
      "length": 21,
      "seq": 10,
      "wsi": 3
    }
  ],
  "fixture_witnesses": [
    {
      "ev": "end",
      "id": 1,
      "t": 1135225.422348434
    },
    {
      "errno": 104,
      "ev": "io_error",
      "id": 2,
      "t": 1135225.427282219
    },
    {
      "ev": "end",
      "id": 2,
      "t": 1135225.427290164
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=0 error=62 depth=0",
      "reason": 58,
      "seq": 9,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "CONNECTION_ERROR",
      "note": "client connect failed",
      "reason": 1,
      "seq": 10,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 11,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 12,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 14,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 15,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.008597212145105004,
  "redirect_hostname_skip": true,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4,
        8
      ],
      "destroy_sequences": [
        11
      ],
      "wsi": 3
    }
  ]
}
```

### untrusted-wss

```json
{
  "case": "untrusted-wss",
  "certificates": [
    {
      "conn": 1,
      "depth": 1,
      "preverify": 0,
      "seq": 6,
      "wsi": 3,
      "x509_error": 19
    }
  ],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 0,
    "cert": 1,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [
    {
      "conn": 1,
      "hex": "636c69656e7420636f6e6e656374206661696c6564",
      "input_present": true,
      "length": 21,
      "seq": 7,
      "wsi": 3
    }
  ],
  "fixture_witnesses": [
    {
      "errno": 104,
      "ev": "io_error",
      "id": 1,
      "t": 1135251.48636365
    },
    {
      "ev": "end",
      "id": 1,
      "t": 1135251.486369871
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=0 error=19 depth=1",
      "reason": 58,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "CONNECTION_ERROR",
      "note": "client connect failed",
      "reason": 1,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 10,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 11,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 12,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.004645677981898189,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [],
  "source_requests": 0,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        8
      ],
      "wsi": 3
    }
  ]
}
```

### upgrade403-ws

```json
{
  "case": "upgrade403-ws",
  "certificates": [],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 0,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [
    {
      "conn": 1,
      "hex": "48533a207773207570677261646520666f7262696464656e",
      "input_present": true,
      "length": 24,
      "seq": 8,
      "wsi": 3
    }
  ],
  "fixture_witnesses": [
    {
      "ev": "end",
      "id": 1,
      "t": 1135276.977537102
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:45059",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 7,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CONNECTION_ERROR",
      "note": "HS: ws upgrade forbidden",
      "reason": 1,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 9,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 10,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 11,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 12,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 13,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.003725804155692458,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        9
      ],
      "wsi": 3
    }
  ]
}
```

### upgrade403-wss

```json
{
  "case": "upgrade403-wss",
  "certificates": [
    {
      "conn": 1,
      "depth": 1,
      "preverify": 1,
      "seq": 6,
      "wsi": 3,
      "x509_error": 0
    },
    {
      "conn": 1,
      "depth": 0,
      "preverify": 1,
      "seq": 7,
      "wsi": 3,
      "x509_error": 0
    }
  ],
  "connect_a": 1,
  "connect_b": 0,
  "control_errors": [],
  "counts": {
    "append": 1,
    "cert": 2,
    "create": 1,
    "destroy": 1
  },
  "destroy_wait_completed": true,
  "destroyed": true,
  "destroyed_before_shutdown": true,
  "echoed": false,
  "errors": [
    {
      "conn": 1,
      "hex": "48533a207773207570677261646520666f7262696464656e",
      "input_present": true,
      "length": 24,
      "seq": 10,
      "wsi": 3
    }
  ],
  "fixture_witnesses": [
    {
      "ev": "end",
      "id": 1,
      "t": 1135301.239304684
    }
  ],
  "harness_errors": [],
  "opened": false,
  "ordered": [
    {
      "conn": 0,
      "name": "LOAD_EXTRA_CLIENT_VERIFY_CERTS",
      "note": "",
      "reason": 21,
      "seq": 1,
      "wsi": 1
    },
    {
      "conn": 0,
      "name": "PROTOCOL_INIT",
      "note": "",
      "reason": 27,
      "seq": 2,
      "wsi": 2
    },
    {
      "conn": 1,
      "name": "connect>",
      "note": "localhost:42865",
      "reason": -1,
      "seq": 3,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "WSI_CREATE",
      "note": "",
      "reason": 29,
      "seq": 4,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "connect<",
      "note": "wsi",
      "reason": -2,
      "seq": 5,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=1",
      "reason": 58,
      "seq": 6,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "PERFORM_SERVER_CERT_VERIFICATION",
      "note": "preverify=1 error=0 depth=0",
      "reason": 58,
      "seq": 7,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "APPEND_HANDSHAKE_HEADER",
      "note": "",
      "reason": 24,
      "seq": 8,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "headers+",
      "note": "all",
      "reason": -6,
      "seq": 9,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "CONNECTION_ERROR",
      "note": "HS: ws upgrade forbidden",
      "reason": 1,
      "seq": 10,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "WSI_DESTROY",
      "note": "",
      "reason": 30,
      "seq": 11,
      "wsi": 3
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-primary-finished",
      "reason": -7,
      "seq": 12,
      "wsi": 0
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-begin",
      "reason": -7,
      "seq": 13,
      "wsi": 0
    },
    {
      "conn": 0,
      "name": "PROTOCOL_DESTROY",
      "note": "",
      "reason": 28,
      "seq": 14,
      "wsi": 4
    },
    {
      "conn": 1,
      "name": "marker",
      "note": "proxy-shutdown-end",
      "reason": -7,
      "seq": 15,
      "wsi": 0
    }
  ],
  "primary_budget_seconds": 8.0,
  "primary_seconds": 0.004914658842608333,
  "redirect_hostname_skip": false,
  "route": "A",
  "silent_destroy_only": false,
  "source_authorization": [
    [
      "Bearer b1-probe-marker"
    ]
  ],
  "source_requests": 1,
  "target_authorization": [],
  "target_requests": 0,
  "token_lifecycles": [
    {
      "create_sequences": [
        4
      ],
      "destroy_sequences": [
        11
      ],
      "wsi": 3
    }
  ]
}
```

## B2 decision inputs

Compare direct/explicit ws and wss ordered APPEND, WSI_CREATE and certificate positions before changing any veto.
Compare denial, abrupt close and silence error pointer/length/bytes and terminal/destruction order.
Compare post-tunnel untrusted TLS and target HTTP403 before choosing any contextual empty-error grouping.
Environment rows identify which listener saw CONNECT; no_proxy=* rows are observations, not a promised bypass API.
A denial fixture status is ground truth on the proxy side, not a target HTTP status and not evidence of callback text.
If these public signals coincide, they do not distinguish the cases. A broader proxied pre-upgrade empty-error
ConnectFailed grouping requires an explicit root B2 ruling and preservation of TLS/redirect/cancel/deadline failures.
No production classifier or redirect-veto change was made by B1. Raw redirects were allowed for measurement.
B2 must prove successful production ws/wss proxying and no redirected target request or credential forwarding.

## Remaining release gates

Native Windows, macOS, Android/iOS and cooked/shared-copy runtime measurements were not run by this Linux editor matrix.
The independent W1a audit completed NoPCH link/runtime validation (572/0) and normal restoration (Module 1/0);
hqx.2 was closed by root. Those attributed results are not B1 execution evidence. Future build guards remain.
VaRest-rti residual and the WebSocket parent epic remain outside B1 completion.

## Controller handoff — manually reviewed B2 inputs

Reviewed 2026-10-23 against the immutable `full-1` artifacts above. Sequence numbers are local to each raw trace; Python and UE clocks are not compared. This handoff interprets recorded public callbacks and fixture journals. It does not implement a production policy.

- **Successful proxying adds no recorded APPEND/CREATE/certificate step in these controls.** `direct-ws` and `explicit-ws` both show CREATE4 → APPEND6 → ESTABLISHED9 → CLIENT_CLOSED20 → DESTROY21. `direct-wss` and `explicit-wss` both show CREATE4 → successful certificate6(depth1) and7(depth0) → APPEND8 → ESTABLISHED11 → CLIENT_CLOSED22 → DESTROY23. The explicit rows have exactly one A CONNECT/upstream/tunnel and one source Upgrade; direct rows have neither proxy CONNECT. All four echo. Trusted wss has hostname skipping false and every certificate preverify1/error0. Each source receives the synthetic Authorization marker; no CONNECT carries Authorization. Thus the initial APPEND/CREATE/cert sequence provides no evidence that successful proxying itself requires weakening the existing redirect guards. Production proxy success remains a separate B2 gate.
- **Denial and abrupt EOF are not distinguished by recorded public signals.** `deny-ws`, `deny-wss`, `close-ws`, and `close-wss` each have CREATE4, one error6 with input_present=false/len0/empty hex, no APPEND/certificate/target request, and matching destruction. DESTROY is7 except `close-ws`8, where the game-thread primary marker falls between error and destruction; that scheduling difference is not a protocol discriminator. The proxy journals supply the distinct ground truth: successful denied403-send versus closed_without_response. Null input does not supply HTTP403, a status line, or a reason string. A proposed contextual pre-upgrade empty-error grouping must use generic failure detail and preserve stronger pending failures; the controller's explicit B2 ruling is required.
- **Silence is an observed destruction-only outcome, not an empty error callback.** Both `silent-ws` and `silent-wss` show CREATE4 → DESTROY6 → primary-finished7 → shutdown-begin8 → shutdown-end10, with error callback count0. Primary durations are20.094499s and20.107336s under configured30s budgets; these durations are measurements, never a classifier. Each40s fixture records connect → silent → peer_eof → end, without deadline/stop/IO substitution. Root independently accepted this narrow native lifecycle proof for immutable silent rows; every distinct associated token still requires actual destruction. Context shutdown is not substituted for an absent callback. B2 must cover a pre-open DESTROY with no error and preserve VaRest's own deadline/cancellation precedence, rather than synthesizing null/0 input or a callback count.
- **The post-tunnel controls are counterexamples to coarse stage/count grouping.** `untrusted-wss` has one successful tunnel, CREATE4 → failed certificate6(depth1,preverify0,error19) → error7(present,len21,`client connect failed`) → DESTROY8; no Upgrade reaches the source. It shares no-APPEND with denial but has actual certificate evidence and nonempty error bytes. `upgrade403-ws` has APPEND6 → error8(present,len24,`HS: ws upgrade forbidden`) → DESTROY9; `upgrade403-wss` has successful cert6/7 → APPEND8 → error10 with the same24 bytes → DESTROY11. Both source journals record403 after a successful tunnel. These particular post-tunnel controls do not reproduce null/0, but cannot prove that no other post-tunnel failure ever can. Certificate counts alone do not identify connections, outcomes, or policy; preserve failed-verification evidence and stronger TLS failure precedence.
- **Environment precedence is observed, not promised as a bypass API.** `env-ws`/`env-wss` select B once and A never; `precedence-ws`/`precedence-wss` select explicit A once and B never. `env-noproxy-ws`/`env-noproxy-wss` also select B once despite the controlled `no_proxy=*`. Each row records engine-configured proxy absence and successful open/echo/source Upgrade. These Linux archive observations inform Q7's engine-following semantics; they do not establish platform-independent environment behavior or a new bypass API.
- **A second CREATE after APPEND remains the earliest recorded redirect signal here.** `redirect-ws` shows CREATE4 → APPEND6 → repeated CREATE8 on the same token3 → APPEND9 → ESTABLISHED12 → CLIENT_CLOSED23 → DESTROY24. Two A tunnels and separate source302/target101 journals each contain one request and the synthetic Authorization marker. The rig deliberately appends that marker at every APPEND, so this is not proof of automatic library credential replay. The independent RedirectReplay probe covers that distinct question. Repeated CREATE on token3 does not imply a second live token or require a second DESTROY.
- **The TLS redirect rows demonstrate target-zero is insufficient to prove a redirect policy.** `redirect-wss` has successful cert6/7 → source APPEND8 → repeated CREATE10 → failed certificate11(depth0,preverify0,error62) → error12(present,len21,`client connect failed`) → DESTROY13. `redirect-cross-ws` has APPEND6 → repeated CREATE8 → failed certificate9(depth0,preverify0,error62) → error10 with the same21 bytes → DESTROY11. Each has two successful A tunnels and source302, but target journal requests0 and no second APPEND/open/echo. The recorded initial `redirect_hostname_skip=true` does not establish effective skipping after the library reset; actual verification reports62 (hostname mismatch, pinned OpenSSL Unix `x509_vfy.h:166`). Chain verification was never disabled. These are accepted negative raw redirect outcomes, not successful redirected TLS. B2 must require explicit Redirect failure precedence and no target request/credential forwarding, not merely target0 caused by a TLS failure.

The supported inputs above favor retaining public callback ordering and stronger pending failures over a guessed CONNECT status or internal library state. No opaque offset, global-log parse, duration classifier, or invented callback enters the generator. Root policy remains separate from these measurements. Production ws/wss proxying, no-follow redirects, TLS/redirect/cancel/deadline precedence, Q7 semantics, and no-follow credential isolation remain B2 acceptance obligations. B1 is complete prerequisite evidence only after its independent task and whole-B1 reviews; it is not W1 completion, a native-platform result, or a cooked/shared-copy runtime result.

### Controller B2 planning ruling (2026-10-23)

After independently inspecting the full summary and current FConn/Errors implementation, root accepted a **generic ConnectFailed, HttpStatus0** grouping for an initial proxied pre-upgrade failure with no stronger pending reason and no APPEND/establishment, when CONNECTION_ERROR is empty. Null and present-empty input both qualify for that future policy while the raw evidence continues to distinguish them. Include an initial proxied destruction-only/no-error fallback with the same generic result when no explicit reason exists; do not invent an error callback. Detail must not claim denial or403. This intentionally cannot distinguish permanent proxy denial from abrupt EOF and may allow retries of a permanent denial.

Explicit stronger reasons (TLS verification/pins, Redirect, cancellation, shutdown, and deadlines) retain precedence; VaRest's own ConnectTimeout retains Timeout. No duration heuristic belongs to this proxy grouping. Unrelated historical direct classification remains unchanged unless separately ruled. Keep the existing APPEND/CREATE/certificate redirect veto counts because successful proxy controls match direct ordering. Preserve Q7: engine-configured proxy, then native lowercase environment fallback; explicit beats environment. The observed ignored `no_proxy=*` is not a promised bypass feature.

This is direction for the separately reviewed B2 plan after B1 final review. Task5 implements no production classifier, proxy setting, timeout policy, redirect veto, or TLS change.
