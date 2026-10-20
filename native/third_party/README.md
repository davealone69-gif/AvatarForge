# Native texture codec dependencies

- **Arm ASTC Encoder 5.4.0** (`astcenc/Source`), Apache-2.0. Source copied from the
  upstream `5.4.0` tag, commit `3ee54d103ca66a5b1f751ad49a4b8064235e2cf9`.
- **Google Etc2Comp** (`etc2comp/EtcLib`), Apache-2.0. Source copied from the archived upstream
  repository `google/etc2comp`; only the encoder library is included (not its CLI or PNG tools).

The upstream license texts are retained beside each source tree. These are CPU encoders used only
when the active GPU profile advertises a compatible ASTC LDR or ETC2/EAC sampled-image format.
Raw RGBA/R8 texture upload remains the fallback.
