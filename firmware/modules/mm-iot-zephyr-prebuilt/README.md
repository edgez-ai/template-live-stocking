# Binary Morse Micro Zephyr module

This module contains chip-specific prebuilt `libmorse.a` archives for Zephyr
4.2.1 on the nRF54L15 with MM6108 (FGH100M) or MM8108 (FGH200M). It includes
the Morse Micro stack, HostAP, packet memory, regulatory database, Zephyr radio
driver objects, radio firmware, and board calibration blobs.

The package contains headers needed by the firmware. It has no Morse Micro
implementation source or MM-IoT SDK source tree. The archive is tied to the
Kconfig, board, and toolchain profile recorded in `build-info.json`; rebuild it
from the private source checkout when those inputs change.
