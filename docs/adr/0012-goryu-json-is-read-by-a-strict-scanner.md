# goryu.json is read by a small strict scanner, and an unknown key is a warning

The config is a flat object of string and integer values, read in one pass by a scanner in `relay_config.c` instead of a JSON library, because the build depends on nothing beyond libsrt and OpenSSL. A duplicate key, a nested value, a missing or trailing comma, or text after the object is refused at startup, with the byte where it went wrong. A key Goryu does not know is ignored and named in the log.

The first version looked the text up by `"key":` and ignored everything around it. Testing it on 2026-10-30 showed what that accepted: a misspelled `output_passphrase` was dropped without a word, so the output fell back to the input's passphrase; a duplicate key (the first won); a key nested inside another object, read as if it were top level; a missing comma; text after the closing brace. A value equal to another key's name could also make a valid file fail.

## Considered options

- **A JSON library.** A new dependency for six keys; the one-line build is a recorded constraint.
- **Refusing an unknown key.** It would make a misspelling impossible to miss, but a config with an extra key would stop starting after an upgrade. Bunryu's generator writes only the known keys (its `normalizeRelayConfig`), but other deployments are not visible from here.

## Consequences

A file the old text search accepted in one of the malformed shapes above, or with `//` comments (documented as unsupported, but they happened to work), now fails at startup; note it in the release notes. A UTF-8 byte order mark at the start is still accepted, since Windows editors add one. An unknown key is only a log line, so a typo in an optional key still changes behaviour: it is reported, not prevented.
