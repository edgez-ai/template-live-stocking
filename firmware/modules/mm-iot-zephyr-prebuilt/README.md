# Binary Morse Micro Zephyr module

This module contains target-specific prebuilt `libmorse.a` archives for Zephyr
4.2.1 on the nRF54L15. The default MM6108 archive supports FGH100M/HT-HC01
boards with a controllable radio supply, the `mm6108-sense` archive supports
the always-powered XIAO nRF54L15 Sense shield, and the MM8108 archive supports
FGH200M. It includes the Morse Micro stack, HostAP, packet memory, regulatory
database, Zephyr radio driver objects, radio firmware, and board calibration
blobs.

The package contains headers needed by the firmware. It has no Morse Micro
implementation source or MM-IoT SDK source tree. The archive is tied to the
Kconfig, board, and toolchain profile recorded in `build-info.json`; rebuild it
from the private source checkout when those inputs change.
