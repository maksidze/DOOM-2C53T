#!/usr/bin/env python3
"""Export maksidze's SPI3_2C53T.sal (issue #18) to CSVs via Logic 2 automation.

Requires the Logic 2 app running with --automation --automationPort 10430.
Exports: raw digital CSV (all 8 channels) + analyzer data tables (SPI, 2x serial).
"""
import os
import sys

from saleae import automation

CAP = os.path.join(os.path.dirname(__file__), "SPI3_2C53T.sal")
OUT = os.path.join(os.path.dirname(__file__), "export")
os.makedirs(OUT, exist_ok=True)

with automation.Manager.connect(port=10430) as mgr:
    cap = mgr.load_capture(CAP)
    print("capture loaded")
    cap.export_raw_data_csv(directory=OUT, digital_channels=[0, 1, 2, 3, 4, 5, 6, 7])
    print("raw digital csv exported")
    cap.close()
print("done ->", OUT)
