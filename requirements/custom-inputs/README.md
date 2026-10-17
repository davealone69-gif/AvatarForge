# Custom input intake

User-supplied input lists are preserved in this directory without silently claiming engine support.

- `avatar-customization.user.json` — the 58-entry body, face, hair, material, cybernetic, armor, clothing, accessory, weapon, makeup, and marking list supplied on 2026-10-18.
- `adult-anatomy.user.json` — the 10-entry adult-anatomy list supplied on 2026-10-18.

Normalized offline runtime contracts live under `config/customization/` and are packaged into the SDK AAR as assets. Run:

```bash
python3 tools/normalize_custom_inputs.py \
  requirements/custom-inputs/avatar-customization.user.json \
  config/customization/avatar-customization.inputs.json
python3 tools/validate_custom_inputs.py config/customization/*.json
```

## No-fakes gate

A schema entry is not automatically a UI feature. The production data-driven UI may expose only an entry labelled `VERIFIED`. `PARTIAL` entries have a real engine binding but still require the Android rebuild and physical-device validation. `BLOCKED_*` entries remain hidden import vocabulary until their C ABI, native geometry/material/rig behavior, GLB export, tests, and device inspection exist.
