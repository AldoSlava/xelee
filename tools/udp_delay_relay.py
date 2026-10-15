#!/usr/bin/env python3
"""A UDP relay that adds latency, jitter and loss between two netplay games on
one machine, so rollback actually runs in local tests (on localhost packets
arrive at once and nothing ever rolls back).

Pairs with `slippi_mm_server -R BASE`, which tells each player the other lives
at 127.0.0.1:BASE (the decider, A) or BASE+1 (B), and sends
"MAP <A game port> <B game port>" to BASE+2 when it pairs them.

What A sends to BASE+1 leaves the relay from BASE to B's game port, and what B
sends to BASE leaves from BASE+1 to A's, so each game sees packets come from
the address it was given for the other.

  udp_delay_relay.py [--base 43200] [--delay 60] [--jitter 20] [--loss 1]

Delay and jitter are one-way milliseconds (jitter is uniform +-, so packets
can reorder); loss is a percentage per packet. --spike MS,EVERY_S adds a
latency spike of MS lasting a second every EVERY_S seconds.
"""
import argparse
import heapq
import random
import select
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", type=int, default=43200)
    ap.add_argument("--delay", type=float, default=60.0)
    ap.add_argument("--jitter", type=float, default=20.0)
    ap.add_argument("--loss", type=float, default=1.0)
    ap.add_argument("--spike", default="")
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    spike_ms, spike_every = (map(float, args.spike.split(","))) if args.spike else (0.0, 0.0)

    def bind(port):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", port))
        s.setblocking(False)
        return s

    ra, rb, ctl = bind(args.base), bind(args.base + 1), bind(args.base + 2)
    a_port = b_port = None
    queue = []  # (due, seq, out socket, dest port, data)
    seq = 0
    start = time.monotonic()
    counts = {"ab": 0, "ba": 0, "lost": 0}
    last_report = start
    print("relay on %d/%d (control %d): delay %.0f +-%.0f ms, loss %.1f%%%s" %
          (args.base, args.base + 1, args.base + 2, args.delay, args.jitter, args.loss,
           ", spike %.0f ms every %.0f s" % (spike_ms, spike_every) if spike_every else ""),
          flush=True)
    while True:
        now = time.monotonic()
        timeout = max(0.0, queue[0][0] - now) if queue else 0.5
        readable, _, _ = select.select([ra, rb, ctl], [], [], min(timeout, 0.5))
        now = time.monotonic()
        for s in readable:
            try:
                data, src = s.recvfrom(65536)
            except BlockingIOError:
                continue
            if s is ctl:
                parts = data.decode(errors="replace").split()
                if len(parts) == 3 and parts[0] == "MAP":
                    a_port, b_port = int(parts[1]), int(parts[2])
                    queue.clear()
                    print("map: A game port %d, B game port %d" % (a_port, b_port), flush=True)
                continue
            if a_port is None:
                continue
            if rng.random() * 100.0 < args.loss:
                counts["lost"] += 1
                continue
            d = args.delay + rng.uniform(-args.jitter, args.jitter)
            if spike_every and (now - start) % spike_every < 1.0:
                d += spike_ms
            if s is rb:  # A -> B
                out, dest = ra, b_port
                counts["ab"] += 1
            else:  # B -> A
                out, dest = rb, a_port
                counts["ba"] += 1
            seq += 1
            heapq.heappush(queue, (now + max(0.0, d) / 1000.0, seq, out, dest, data))
        while queue and queue[0][0] <= now:
            _, _, out, dest, data = heapq.heappop(queue)
            try:
                out.sendto(data, ("127.0.0.1", dest))
            except OSError:
                pass
        if now - last_report >= 10.0:
            last_report = now
            print("A->B %d, B->A %d, dropped %d" % (counts["ab"], counts["ba"], counts["lost"]),
                  flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
