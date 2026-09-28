#!/usr/bin/env python3

import re
import sys
from pathlib import Path


def percentile(values, pct):
    values = sorted(values)
    if not values:
        return None

    index = (len(values) * pct + 99) // 100 - 1
    index = max(0, min(index, len(values) - 1))
    return values[index]


def longest_over(values, threshold):
    longest = 0
    current = 0

    for value in values:
        if value >= threshold:
            current += 1
            longest = max(longest, current)
        else:
            current = 0

    return longest


if len(sys.argv) != 2:
    print(f"usage: {sys.argv[0]} <ping-log>", file=sys.stderr)
    sys.exit(2)

text = Path(sys.argv[1]).read_text(errors="replace")

samples = [
    float(x)
    for x in re.findall(r"time[=<]([0-9.]+)\s*ms", text)
]

loss_match = re.search(
    r"([0-9.]+)% packet loss",
    text,
)

if not samples or not loss_match:
    print("PARSE_ERROR=1")
    sys.exit(2)

loss = float(loss_match.group(1))

print(f"SAMPLES={len(samples)}")
print(f"LOSS={loss:g}")
print(f"MIN={min(samples):.3f}")
print(f"AVG={sum(samples) / len(samples):.3f}")
print(f"MAX={max(samples):.3f}")
print(f"P95={percentile(samples, 95):.3f}")
print(f"P99={percentile(samples, 99):.3f}")

for threshold in (20, 50, 100, 200, 500, 1000):
    count = sum(v >= threshold for v in samples)
    longest = longest_over(samples, threshold)

    print(f"GE_{threshold}MS={count}")
    print(f"LONGEST_GE_{threshold}MS={longest}")
