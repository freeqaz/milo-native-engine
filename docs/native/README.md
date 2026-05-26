# milo-native-engine — docs

The **canonical, durable plan lives in the rb3 decomp**, not here:

- Roadmap & phase tracker: [`rb3/docs/native/NATIVE_PORT_ROADMAP.md`](../../../rb3/docs/native/NATIVE_PORT_ROADMAP.md)
- Disposition catalog (what moves into the engine, in what shape):
  [`rb3/docs/native/NATIVE_PORT_INVENTORY.md`](../../../rb3/docs/native/NATIVE_PORT_INVENTORY.md)
- DC3 native port (the model being extracted): `dc3-decomp/native/`,
  `dc3-decomp/docs/native/NATIVE_PORT_STATUS.md`

This directory holds engine-specific notes that don't belong in a single
decomp. Per-session detail goes in [`../sessions/`](../sessions/), one file per
work session, mirroring `dc3-decomp/docs/sessions/`.

The engine repo is intentionally light on prose: it is the shared
implementation, and the cross-decomp narrative is owned by the roadmap so both
decomps read the same source of truth.
