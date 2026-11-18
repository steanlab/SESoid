#!/usr/bin/env python3
# SES V300 (EFR32FG22): check and remove the debug lock of a FACTORY tag over SWD with pyOCD, no Simplicity Commander needed.
#
#   python v300_unlock.py --pack ./SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack            (status only, changes nothing)
#   python v300_unlock.py --pack ./SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack --unlock   (ERASES the factory firmware, UserData survives)
#
# Repository: steanlab/SESoid (public), file tools/v300_unlock.py. Needs pyocd >= 0.44 (pip install pyocd) and the FG22 CMSIS pack.
# How it works: Series 2 chips have a Debug Challenge Interface (DCI) on access port 1 (ID 0x54770002). Commands go to the Secure Engine
# as 32-bit words through AP1 registers 0x04 (select), 0x0C (data); the SE answers in the same way. Command 0xFE010000 = read status,
# 0x430F0000 = device erase + remove the standard debug lock. The protocol is Silicon Labs' public DCI description (AN1303); the
# working pyOCD example that this follows is reflash.py of OpenEPaperLink/Tag_FW_EFR32xG22 (read for reference, code written anew).
# Wiring: SWCLK, SWDIO, GND, tag supply, and nRESET (used for the reset after the erase). The tag must be powered.
import argparse, os, sys, time
from pyocd.core.helpers import ConnectHelper
from pyocd.core.session import Session

AP_DCI = 0x01000000                  # access port 1
DCI_IDR = 0x54770002
CMD_STATUS = 0xFE010000
CMD_ERASE_UNLOCK = 0x430F0000


class Dci:
    def __init__(self, session):
        self.dp = session.board.target.dp
        idr = self.dp.read_ap(AP_DCI | 0xFC)
        if idr != DCI_IDR:
            raise SystemExit('access port 1 has ID 0x%08x, not a Series 2 DCI (0x%08x): wrong chip or no connection' % (idr, DCI_IDR))

    def _status(self):
        self.dp.write_ap(AP_DCI | 0x04, 0x1008)
        return self.dp.read_ap(AP_DCI | 0x0C)

    def _put(self, word):
        self.dp.write_ap(AP_DCI | 0x04, 0x1000)
        self.dp.write_ap(AP_DCI | 0x0C, word)

    def _get(self):
        self.dp.write_ap(AP_DCI | 0x04, 0x1004)
        return self.dp.read_ap(AP_DCI | 0x0C)

    def command(self, cmd, timeout_s=0.4):
        end = time.time() + timeout_s
        words = [8, cmd]                                  # first word = message length in bytes
        for w in words:
            while self._status() & 1:                      # bit 0: the write side is full
                if time.time() > end: raise TimeoutError('DCI write timeout')
                time.sleep(0.001)
            self._put(w)
        while not (self._status() & 0x100):                # bit 8: an answer word is waiting
            if time.time() > end: raise TimeoutError('DCI read timeout')
            time.sleep(0.001)
        head = self._get()
        code, extra = head >> 16, (head & 0xFFFF) // 4 - 1
        if code != 0: raise RuntimeError('the Secure Engine answered with error code %d' % code)
        out = []
        for _ in range(extra):
            while not (self._status() & 0x100):
                if time.time() > end: raise TimeoutError('DCI read timeout')
                time.sleep(0.001)
            out.append(self._get())
        return out


def open_session(a, cores):
    probes = ConnectHelper.get_all_connected_probes(blocking=False)
    if not probes: raise SystemExit('no debug probe found')
    probe = probes[0]
    if len(probes) > 1:
        probe = next((p for p in probes if p.unique_id == a.adapter), None)
        if probe is None: raise SystemExit('several probes found, choose one with --adapter ID: ' + ', '.join(p.unique_id for p in probes))
    opts = {'adi.v5.max_invalid_ap_count': 0, 'scan_all_aps': False, 'target_override': a.device, 'allow_no_cores': not cores,
            'pack': os.path.abspath(os.path.expanduser(a.pack)), 'frequency': a.freq}
    return Session(probe, options=opts)


def reset(session):
    session.probe.open(); time.sleep(0.2)
    session.probe.assert_reset(True); time.sleep(0.1)
    session.probe.assert_reset(False); time.sleep(0.2)
    session.probe.close()


def main():
    ap = argparse.ArgumentParser(description='status / debug unlock of a factory EFR32FG22 tag (SES V300)')
    ap.add_argument('--pack', required=True, help='CMSIS pack of the EFR32FG22 family')
    ap.add_argument('--device', default='efr32fg22c121f256gm40')
    ap.add_argument('--freq', type=int, default=100000, help='SWD clock in Hz (lower it if you see "No ACK")')
    ap.add_argument('--adapter', help='probe ID when several are connected')
    ap.add_argument('--unlock', action='store_true', help='erase the main flash and remove the debug lock (the factory firmware is lost)')
    a = ap.parse_args()

    session = open_session(a, cores=False)
    with session:
        s = Dci(session).command(CMD_STATUS)
        if len(s) > 5: s = s[4:]
        lock = s[3]
        print('device status 0x%x, family EFR32xG2%x, SE firmware 0x%x' % (s[0], (s[1] >> 24) + 1, s[1] & 0xFFFFFF))
        print('debug lock:', 'APPLIED' if lock & 1 else 'none', '| unlock by device erase:', 'allowed' if lock & 2 else 'NOT allowed',
              '| authenticated unlock:', 'allowed (needs a key, not supported here)' if lock & 4 else 'no')
        if a.unlock:
            if not lock & 1: print('the tag is not locked, nothing to do')
            elif not lock & 2: raise SystemExit('device erase is disabled on this chip, it cannot be unlocked this way')
            else:
                print('erasing the factory firmware and removing the lock ...')
                Dci(session).command(CMD_ERASE_UNLOCK, timeout_s=2.0)
                print('done')
    if a.unlock:
        reset(session)
        print('if the next command does not see the chip, power the tag off for 5 s and on again, then run this script without --unlock: it must say "debug lock: none"')


if __name__ == '__main__':
    main()
