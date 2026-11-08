#!/usr/bin/env python3
# SES V300, BLE stage 2: reads the diagnostic structure g_diag from the RAM of the running tag through SWD WITHOUT halting the CPU
# (halting would break a Bluetooth connection), optionally programs a new image first.
#
#   python ble_probe.py --pack ./SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack --flash v300_ble_probe.bin --watch
#   python ble_probe.py --pack ./SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack --watch           (only read, tag keeps running)
#
# Repository: steanlab/SESoid (SES V300), file tools/ble_probe.py. Writes nothing except, with --flash, the program image.
import argparse, struct, sys, time

MAGIC = 0x32454C42            # "BLE2"
END = 0x444E4532              # "2END"
TRACE_N = 32
# order = DIAG_FIELDS in ble_diag.h (firmware); keep both in step
FIELDS = [
    'magic', 'struct_size', 'nfields', 'heartbeat', 'stage', 'rail_version', 'last_status', 'status_where',
    'adv_rounds', 'adv_tx_sent', 'adv_tx_failed', 'scan_req', 'connect_ind', 'connect_ignored', 'conns', 'disconns',
    'disc_reason', 'ev_armed', 'ev_heard', 'ev_missed', 'rx_good', 'rx_bad', 'rx_dup', 'rx_overflow', 'rx_timeout',
    'tx_sent', 'tx_fail', 'a_count', 'nack_seen', 'gap_last', 'gap_min', 'gap_max', 'gap_n', 'rxtotx_cfg',
    'gap_sum_err', 'interval_us', 'timeout_ms', 'latency', 'hop', 'aa', 'crcinit', 'chm_lo', 'chm_hi', 'ev_cnt',
    'll_ctrl_rx', 'll_ctrl_op', 'll_rsp_sent', 'att_rx', 'att_op', 'att_err_sent', 'att_write_n', 'att_write_bytes',
    'notify_sent', 'notify_dropped', 'cccd', 'sig_rx', 'smp_rx', 'inbox_ovf', 'outbox_full', 'cal_runs', 'last_rssi',
    'arm_fail', 'upd_applied', 'chm_applied', 'last_ev_lo', 'last_ev_hi'
]
STAGE = {1: 'main reached', 2: 'RAIL ready', 3: 'advertising', 4: 'CONNECTED'}
WHERE = {0: '-', 1: 'RAIL_BLE_ConfigChannelRadioParams', 2: 'RAIL_WriteTxFifo', 3: 'RAIL_StartTx', 4: 'RAIL_ScheduleRx/StartRx',
         5: 'RAIL_Calibrate', 11: 'WriteTxFifo (connection)', 12: 'RAIL_SetStateTiming', 13: 'RAIL_Set*Transitions'}
LL_OPS = {0x00: 'CONNECTION_UPDATE_IND', 0x01: 'CHANNEL_MAP_IND', 0x02: 'TERMINATE_IND', 0x03: 'ENC_REQ', 0x07: 'UNKNOWN_RSP',
          0x08: 'FEATURE_REQ', 0x09: 'FEATURE_RSP', 0x0C: 'VERSION_IND', 0x0F: 'CONNECTION_PARAM_REQ', 0x12: 'PING_REQ',
          0x14: 'LENGTH_REQ', 0x16: 'PHY_REQ'}
ATT_OPS = {0x01: 'ERROR_RSP', 0x02: 'MTU_REQ', 0x03: 'MTU_RSP', 0x04: 'FIND_INFO_REQ', 0x05: 'FIND_INFO_RSP', 0x06: 'FIND_BY_TYPE_REQ',
           0x07: 'FIND_BY_TYPE_RSP', 0x08: 'READ_BY_TYPE_REQ', 0x09: 'READ_BY_TYPE_RSP', 0x0A: 'READ_REQ', 0x0B: 'READ_RSP',
           0x0C: 'READ_BLOB_REQ', 0x0D: 'READ_BLOB_RSP', 0x10: 'READ_BY_GROUP_REQ', 0x11: 'READ_BY_GROUP_RSP', 0x12: 'WRITE_REQ',
           0x13: 'WRITE_RSP', 0x1B: 'NOTIFY', 0x52: 'WRITE_CMD'}


def sgn(v):
    return v - (1 << 32) if v >= 1 << 31 else v


def find_struct(target):
    ram = bytes(target.read_memory_block8(0x20000000, 0x8000))
    pos = ram.find(struct.pack('<I', MAGIC))
    while pos >= 0:
        size = struct.unpack_from('<I', ram, pos + 4)[0]
        if 128 <= size <= 1024 and ram[pos + size - 4:pos + size] == b'2END':
            return 0x20000000 + pos, size
        pos = ram.find(struct.pack('<I', MAGIC), pos + 4)
    return None, None


def decode(raw):
    n = len(FIELDS)
    d = dict(zip(FIELDS, struct.unpack_from('<%dI' % n, raw, 0)))
    off = 4 * n
    d['trace'] = [raw[off + 4 * i:off + 4 * i + 4] for i in range(TRACE_N)]
    off += 4 * TRACE_N
    d['trace_pos'] = struct.unpack_from('<I', raw, off)[0]
    off += 4
    d['last_ll'] = raw[off:off + 16]; off += 16
    d['last_att'] = raw[off:off + 24]; off += 24
    d['last_att_tx'] = raw[off:off + 24]
    return d


def hx(b, n=None):
    b = bytes(b)
    return b[:n].hex(' ') if n else b.hex(' ')


def show(d, prev_hb=[None]):
    v = d['rail_version']
    alive = '' if prev_hb[0] is None else ('   firmware running' if d['heartbeat'] != prev_hb[0] else '   !! heartbeat not moving (CPU stopped?)')
    prev_hb[0] = d['heartbeat']
    print('RAIL %d.%d.%d   state: %s%s' % (v >> 16, (v >> 8) & 255, v & 255, STAGE.get(d['stage'], d['stage']), alive))
    if d['last_status']:
        print('last RAIL error: status %d in %s' % (d['last_status'], WHERE.get(d['status_where'], d['status_where'])))
    print('advertising: rounds %d, TX sent %d failed %d | SCAN_REQ %d | CONNECT_IND %d (ignored %d)' % (
        d['adv_rounds'], d['adv_tx_sent'], d['adv_tx_failed'], d['scan_req'], d['connect_ind'], d['connect_ignored']))
    print('connections: %d started, %d ended, last end reason 0x%x (0x1xx = master terminated with code xx, 0x200 = supervision timeout)' % (
        d['conns'], d['disconns'], d['disc_reason']))
    if d['conns']:
        print('parameters: interval %.2f ms, timeout %d ms, latency %d, hop %d, AA 0x%08x, CRCinit 0x%06x, channel map %02x%08x' % (
            d['interval_us'] / 1000.0, d['timeout_ms'], d['latency'], d['hop'], d['aa'], d['crcinit'], d['chm_hi'], d['chm_lo']))
        print('events: armed %d, master heard in %d, missed %d | event counter %d | updates applied %d, channel maps %d' % (
            d['ev_armed'], d['ev_heard'], d['ev_missed'], d['ev_cnt'], d['upd_applied'], d['chm_applied']))
        print('packets: received %d good, %d bad, %d repeated | our TX %d sent, %d failed | rx timeouts %d | FIFO overflow %d | our packets acked %d, not acked %d' % (
            d['rx_good'], d['rx_bad'], d['rx_dup'], d['tx_sent'], d['tx_fail'], d['rx_timeout'], d['rx_overflow'], d['a_count'], d['nack_seen']))
        print('turnaround RX end -> our TX start (target 150 us): last %d, min %d, max %d us over %d packets; RAIL rxToTx now %d us, mean error before last tuning %d us' % (
            sgn(d['gap_last']), sgn(d['gap_min']), sgn(d['gap_max']), d['gap_n'], d['rxtotx_cfg'], sgn(d['gap_sum_err'])))
        print('RSSI of the master: %d dBm | arm failures %d | calibrations %d | queues: inbox overflow %d, outbox full %d' % (
            sgn(d['last_rssi']), d['arm_fail'], d['cal_runs'], d['inbox_ovf'], d['outbox_full']))
        print('LL control received %d, answers %d, last: %s' % (d['ll_ctrl_rx'], d['ll_rsp_sent'],
              LL_OPS.get(d['ll_ctrl_op'], '0x%02x' % d['ll_ctrl_op']) + '  ' + hx(d['last_ll'], 12)))
        print('ATT received %d, errors sent %d, last: %s  [%s]' % (d['att_rx'], d['att_err_sent'],
              ATT_OPS.get(d['att_op'], '0x%02x' % d['att_op']), hx(d['last_att'], 14)))
        print('ATT sent last: %s' % hx(d['last_att_tx'], 14))
        print('RX characteristic: %d writes, %d bytes | notifications: %s, sent %d, dropped %d | SMP %d, L2CAP signalling %d' % (
            d['att_write_n'], d['att_write_bytes'], 'ON' if d['cccd'] else 'off', d['notify_sent'], d['notify_dropped'], d['smp_rx'], d['sig_rx']))
        tp = d['trace_pos']
        rows = []
        for i in range(min(tp, 12)):
            t = d['trace'][(tp - 1 - i) % TRACE_N]
            rows.append('%s ch%02d hdr %02x len %2d A=%d' % ('TX' if t[0] & 0x80 else 'RX', t[0] & 0x3F, t[1], t[2], t[3]))
        if rows:
            print('last exchanges (newest first): ' + ' | '.join(rows))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pack', required=True)
    ap.add_argument('--target', default='efr32fg22c121f256gm40')
    ap.add_argument('--freq', type=int, default=400000)
    ap.add_argument('--flash', help='program this .bin at flash address 0 first and reset the chip')
    ap.add_argument('--watch', action='store_true', help='repeat every 2 seconds until Ctrl-C')
    a = ap.parse_args()
    from pyocd.core.helpers import ConnectHelper
    opts = {'target_override': a.target, 'pack': a.pack, 'frequency': a.freq, 'connect_mode': 'halt' if a.flash else 'attach'}
    with ConnectHelper.session_with_chosen_probe(options=opts, blocking=False) as session:
        target = session.target
        if a.flash:
            from pyocd.flash.file_programmer import FileProgrammer
            FileProgrammer(session, chip_erase='auto').program(a.flash, file_format='bin', base_address=0)
            print('programmed', a.flash)
            target.reset_and_halt()
            target.resume()
            time.sleep(1.0)
        addr, size = find_struct(target)
        if addr is None:
            print('diagnostic structure not found in RAM (is the program running? flash it with --flash)')
            return 1
        while True:
            d = decode(bytes(target.read_memory_block8(addr, size)))
            show(d)
            print('-' * 100)
            if not a.watch:
                break
            time.sleep(2.0)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
