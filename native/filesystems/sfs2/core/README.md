# Canonical SFS2 core

This directory owns host-neutral **SFS2 (SFS\\2, structure version 4)**
semantics.

The first canonical layer now owns SFS2 identity and version validation,
root-layout range checks, the 27-byte fixed object layout, the 16-byte extent
node layout with its 32-bit block count, and SFS2's 48-bit file-size encoding.

These rules are intentionally separate from SFS (SFS\\0/version 3). Linux and
Windows adapters will consume this core when their SFS2 bridges are implemented.
