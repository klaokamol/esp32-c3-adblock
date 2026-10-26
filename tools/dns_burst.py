import argparse
import random
import socket
import struct
import time


def query(txid, name):
    q = b"".join(bytes([len(p)]) + p.encode() for p in name.split(".")) + b"\0"
    return struct.pack(">HHHHHH", txid, 0x0100, 1, 0, 0, 0) + q + struct.pack(">HH", 1, 1)


def drain(s, sent, got):
    n = 0
    while True:
        try:
            d, _ = s.recvfrom(2048)
        except BlockingIOError:
            return n
        n += 1
        txid = struct.unpack(">H", d[:2])[0]
        if txid in sent and txid not in got:
            got[txid] = time.perf_counter() - sent[txid]


def trial(host, n, gap_ms, blocked):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setblocking(False)
    sent, got = {}, {}
    tag = random.randrange(1 << 30)
    for i in range(n):
        txid = random.randrange(1 << 16)
        while txid in sent:
            txid = random.randrange(1 << 16)
        name = f"b{i}-{tag}.doubleclick.net" if blocked else f"f{i}-{tag}.example.com"
        t = time.perf_counter()
        while True:
            try:
                s.sendto(query(txid, name), (host, 53))
                break
            except BlockingIOError:
                drain(s, sent, got)
        sent[txid] = t
        end = t + gap_ms / 1000
        while time.perf_counter() < end:
            drain(s, sent, got)
    deadline = time.perf_counter() + 3
    while time.perf_counter() < deadline and len(got) < n:
        if not drain(s, sent, got):
            time.sleep(0.0005)
    s.close()
    return len(got), max(got.values(), default=0) * 1000


def main():
    ap = argparse.ArgumentParser(description="Send bursts of DNS queries and count how many get answered.")
    ap.add_argument("host", help="resolver IP, e.g. the device or 9.9.9.9 as a control")
    ap.add_argument("-n", type=int, default=16, help="queries per burst")
    ap.add_argument("-t", "--trials", type=int, default=5)
    ap.add_argument("--gaps", default="0,2,5,10,20", help="ms between sends, comma-separated")
    ap.add_argument("--no-blocked", action="store_true", help="skip the blocked-name runs (use for a public resolver)")
    a = ap.parse_args()
    host = socket.gethostbyname(a.host)
    for blocked in (False,) if a.no_blocked else (False, True):
        for gap in (int(g) for g in a.gaps.split(",")):
            res = [trial(host, a.n, gap, blocked) for _ in range(a.trials)]
            time.sleep(0.5)
            print(f"{'blk' if blocked else 'fwd'} gap={gap:>2}ms answered={[r[0] for r in res]}/{a.n} slowest_ms={[round(r[1]) for r in res]}")


if __name__ == "__main__":
    main()
