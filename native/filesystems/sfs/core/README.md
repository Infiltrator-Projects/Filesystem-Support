# Canonical SFS core

This directory owns host-neutral **SFS (SFS\\0, structure version 3)**
semantics.

The canonical core now owns root-block identity, structure-version policy,
layout range validation and bitmap geometry arithmetic. Linux and Windows
adapters consume these same rules rather than maintaining independent copies.

Further allocation, object, extent and directory semantics move here as they
are separated from host APIs. SFS2 remains a distinct filesystem.
