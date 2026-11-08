#!/usr/bin/env python3
# SES V300, stage 1: reads the probe counters from the RAM of the running tag through SWD.
# Usage:   python ble_probe.py --pack ./SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack [--watch] [--flash v300_ble_probe.bin]
# Does not write anything except (with --flash) the program itself into the internal flash of the EFR32 at address 0.
import argparse, struct, sys, time

MAGIC = 0x31454C42
FIELDS = ['magic', 'struct_size', 'stage', 'supports_ble', 'ble_enabled', 'rail_version', 'last_status', 'status_where',
          'adv_rounds', 'tx_started', 'tx_sent', 'tx_failed', 'rx_packets', 'rx_bad', 'rx_overflow', 'cal_runs',
          'ev_lo', 'ev_hi', 'last_ev_lo', 'last_ev_hi', 'tx_power_ddbm', 'wr_len_last', 'scan_req', 'connect_ind',
          'last_rx_len', 'last_rx_rssi']
FMT = '<24IIi'               # 24 unsigned words, last_rx_len, last_rx_rssi
WHERE = ['-', 'RAIL_BLE_ConfigChannelRadioParams', 'RAIL_WriteTxFifo', 'RAIL_StartTx', 'RAIL_StartRx', 'RAIL_Calibrate']
STAGE = {1: 'main reached, RAIL not queried yet', 2: 'RAIL initialised', 3: 'advertising loop running'}


def read_probe(target):
    ram = bytes(target.read_memory_block8(0x20000000, 0x8000))
    pos = ram.find(struct.pack('<I', MAGIC))
    while pos >= 0:
        size = struct.unpack_from('<I', ram, pos + 4)[0]
        if 64 <= size <= 512 and ram[pos + size - 4:pos + size] == b'1END':
            return pos, ram[pos:pos + size]
        pos = ram.find(struct.pack('<I', MAGIC), pos + 4)
    return None, None


def decode(raw):
    base = struct.unpack_from(FMT, raw, 0)
    d = dict(zip(FIELDS, base))
    off = struct.calcsize(FMT)
    d['last_rx'] = raw[off:off + 48]
    d['connect_len'] = struct.unpack_from('<I', raw, off + 48)[0]
    d['connect_raw'] = raw[off + 52:off + 52 + 48]
    return d


def show(d):
    v = d['rail_version']
    print('RAIL %d.%d.%d   stage: %s' % (v >> 16, (v >> 8) & 255, v & 255, STAGE.get(d['stage'], d['stage'])))
    print('RAIL says it supports BLE: %s   BLE mode switched on: %s   TX power: %.1f dBm' % (
        'yes' if d['supports_ble'] else 'NO', 'yes' if d['ble_enabled'] else 'NO', d['tx_power_ddbm'] / 10.0 if d['tx_power_ddbm'] < 0x80000000 else (d['tx_power_ddbm'] - 2**32) / 10.0))
    print('advertising rounds: %d   TX started: %d   sent: %d   failed: %d' % (d['adv_rounds'], d['tx_started'], d['tx_sent'], d['tx_failed']))
    print('received: %d good, %d bad, overflow %d   SCAN_REQ: %d   CONNECT_IND: %d   calibrations: %d' % (
        d['rx_packets'], d['rx_bad'], d['rx_overflow'], d['scan_req'], d['connect_ind'], d['cal_runs']))
    if d['last_status']:
        w = WHERE[d['status_where']] if d['status_where'] < len(WHERE) else d['status_where']
        print('last RAIL error status: %d (in %s)' % (d['last_status'], w))
    print('events seen (OR): 0x%08x%08x   last event: 0x%08x%08x' % (d['ev_hi'], d['ev_lo'], d['last_ev_hi'], d['last_ev_lo']))
    if d['last_rx_len']:
        r = d['last_rx_rssi'] - (1 << 32) if d['last_rx_rssi'] >= 1 << 31 else d['last_rx_rssi']
        print('last received packet (%d bytes, RSSI %d dBm): %s' % (d['last_rx_len'], r, d['last_rx'][:min(48, d['last_rx_len'])].hex(' ')))
    if d['connect_ind']:
        print('last CONNECT_IND (%d bytes): %s' % (d['connect_len'], d['connect_raw'][:min(48, d['connect_len'])].hex(' ')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pack', required=True)
    ap.add_argument('--target', default='efr32fg22c121f256gm40')
    ap.add_argument('--freq', type=int, default=400000)
    ap.add_argument('--flash', help='program this .bin at flash address 0 first and reset the chip')
    ap.add_argument('--watch', action='store_true', help='repeat every 2 seconds until Ctrl-C')
    a = ap.parse_args()
    from pyocd.core.helpers import ConnectHelper
    opts = {'target_override': a.target, 'pack': a.pack, 'frequency': a.freq, 'connect_mode': 'halt'}
    with ConnectHelper.session_with_chosen_probe(options=opts, blocking=False) as session:
        target = session.target
        if a.flash:
            from pyocd.flash.file_programmer import FileProgrammer
            FileProgrammer(session, chip_erase='auto').program(a.flash, file_format='bin', base_address=0)
            print('programmed', a.flash)
        target.reset_and_halt()
        target.resume()
        time.sleep(1.0)
        while True:
            target.halt()
            pos, raw = read_probe(target)
            if raw is None:
                print('probe structure not found in RAM (the program is not running?)')
            else:
                show(decode(raw))
            target.resume()
            if not a.watch:
                break
            print('-' * 70)
            time.sleep(2.0)


if __name__ == '__main__':
    main()
