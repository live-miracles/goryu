# The destination has its own optional passphrase, and an empty one means unencrypted

The output used to reuse the input's passphrase, so a downstream had to use the same one or none, although a downstream on the same host or a trusted network often does not want one while the encoders cross the internet. `goryu.json` now takes an optional `output_passphrase`: left out, the output uses `passphrase` as before, so existing configs behave the same; `""` leaves the downstream unencrypted and does not fall back to the input's; a value encrypts the downstream with that passphrase only.

The empty string is deliberately not the same as the key being absent: it states that the downstream is unencrypted, so a downstream that still requires a passphrase refuses the connection and is retried like any rejecting downstream, instead of quietly being given the input's. The `output_passphrase` scenario tests that case.
