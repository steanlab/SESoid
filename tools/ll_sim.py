#!/usr/bin/env python3
# SES V300, BLE stage 2: simulation of the SN/NESN/ack logic of ble_ll.c against a spec-conforming master with random packet loss.
# It is a line-by-line model of rx_isr / tx_isr / prepare_next (same variable names). Passing means: with losses in both directions
# every PDU in both directions arrives exactly once and in order, and the link never gets stuck.
#   python ll_sim.py [seed ...]
import random, sys


class Slave:
    def __init__(self):
        self.nesn_exp = 0; self.last_rx_sn = 1; self.last_tx_nesn = 0
        self.A = 0; self.assigned = 0; self.have_last = False; self.last_idx = 0
        self.slot = [dict(used=False, data=None), dict(used=False, data=None)]
        self.prep = None            # (idx, nesn, data)
        self.outbox = []            # PDUs queued by the application
        self.inbox = []             # PDUs accepted from the master

    def prepare_next(self):
        idx = self.A + (1 if (self.have_last and self.last_idx == self.A) else 0)
        s = self.slot[idx & 1]
        if idx >= self.assigned:
            s['data'] = self.outbox.pop(0) if self.outbox else None
            s['used'] = True
            self.assigned = idx + 1
        pred_sn = (self.last_rx_sn ^ 1) if (self.last_tx_nesn != self.last_rx_sn) else self.last_rx_sn
        nesn = (self.nesn_exp ^ 1) if (pred_sn == self.nesn_exp) else self.nesn_exp
        self.prep = (idx, nesn, s['data'])

    def rx(self, sn, nesn, payload):          # a good packet from the master
        if nesn != (self.A & 1):
            self.A += 1
            d = self.slot[(self.A - 1) & 1]
            d['used'] = False; d['data'] = None
        if sn == self.nesn_exp:
            self.nesn_exp ^= 1
            if payload is not None:
                self.inbox.append(payload)
        self.last_rx_sn = sn

    def tx_done(self):                         # our prepared packet has been sent
        idx, nesn, data = self.prep
        self.last_idx = idx; self.last_tx_nesn = nesn; self.have_last = True
        pkt = (idx & 1, nesn, data)
        self.prepare_next()
        return pkt


class Master:
    def __init__(self):
        self.sn = 0; self.nesn = 0
        self.queue = []            # PDUs to send
        self.cur = None            # PDU currently in flight (kept until acked)
        self.inflight = False      # a packet (possibly an empty one) has been sent and is not acknowledged yet
        self.inbox = []

    def packet(self):
        if not self.inflight:      # a packet that was not acknowledged is repeated unchanged (spec)
            self.cur = self.queue.pop(0) if self.queue else None
            self.inflight = True
        return (self.sn, self.nesn, self.cur)

    def rx(self, sn, nesn, payload):
        if nesn != self.sn:        # slave acknowledged our last packet
            self.sn ^= 1
            self.cur = None
            self.inflight = False
        if sn == self.nesn:
            self.nesn ^= 1
            if payload is not None:
                self.inbox.append(payload)


def run(seed, p_m_lost, p_s_lost, events=4000):
    rnd = random.Random(seed)
    s, m = Slave(), Master()
    s.prepare_next()
    sent_m = sent_s = 0
    for ev in range(events):
        if rnd.random() < 0.3 and sent_m < 3000:
            for _ in range(rnd.randint(1, 3)):
                m.queue.append(('M', sent_m)); sent_m += 1
        if rnd.random() < 0.3 and sent_s < 3000:
            for _ in range(rnd.randint(1, 3)):
                s.outbox.append(('S', sent_s)); sent_s += 1
        s.prepare_next()                       # arming of the event re-prepares (must be idempotent)
        npk = 1 + (rnd.random() < 0.5) * rnd.randint(0, 3)
        for k in range(npk):
            sn, nesn, pl = m.packet()
            if rnd.random() < p_m_lost:
                break                          # slave heard nothing, no answer, event over
            s.rx(sn, nesn, pl)
            sn2, nesn2, pl2 = s.tx_done()
            if rnd.random() < p_s_lost:
                break                          # master did not hear our answer
            m.rx(sn2, nesn2, pl2)
    # drain with a clean channel
    for ev in range(20000):
        if not m.queue and m.cur is None and not m.inflight and not s.outbox and not any(x["data"] for x in s.slot) and ev > 50:
            break
        s.prepare_next()
        sn, nesn, pl = m.packet()
        s.rx(sn, nesn, pl)
        sn2, nesn2, pl2 = s.tx_done()
        m.rx(sn2, nesn2, pl2)
    ok_m = s.inbox == [('M', i) for i in range(sent_m)]
    ok_s = m.inbox == [('S', i) for i in range(sent_s)]
    return ok_m, ok_s, sent_m, sent_s, len(s.inbox), len(m.inbox)


if __name__ == '__main__':
    seeds = [int(a) for a in sys.argv[1:]] or list(range(1, 21))
    bad = 0
    for p_m, p_s in [(0.0, 0.0), (0.05, 0.05), (0.2, 0.2), (0.4, 0.1), (0.1, 0.4), (0.6, 0.6)]:
        res = [run(sd, p_m, p_s) for sd in seeds]
        fails = [(sd, r) for sd, r in zip(seeds, res) if not (r[0] and r[1])]
        bad += len(fails)
        print('loss master->slave %.2f slave->master %.2f: %d/%d runs fully correct%s' % (
            p_m, p_s, len(res) - len(fails), len(res), '' if not fails else '   FIRST FAIL ' + str(fails[0])))
    print('ALL OK' if bad == 0 else '%d FAILED RUNS' % bad)
    sys.exit(1 if bad else 0)
