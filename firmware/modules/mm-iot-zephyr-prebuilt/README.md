# Binary Morse Micro Zephyr module

This module contains a prebuilt `libmorse.a` for Zephyr 4.2.1 on the
KeepTeen nRF54L15 with an MM6108 radio. It includes the Morse Micro stack,
HostAP, packet memory, regulatory database, and Zephyr radio driver objects.
The radio firmware and board calibration blob remain separate binary files.

The package contains headers needed by the firmware. It has no Morse Micro
implementation source or MM-IoT SDK source tree. The archive is tied to the
Kconfig, board, and toolchain profile recorded in `build-info.json`; rebuild it
from the private source checkout when those inputs change.
