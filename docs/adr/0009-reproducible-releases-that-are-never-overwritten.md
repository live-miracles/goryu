# Releases are reproducible and a published release is never overwritten

The build pins the base image by digest and fails if the libsrt tag no longer resolves to the expected commit, so a moved tag breaks the build instead of changing what ships. A release runs CI first, attests the archive's build provenance, and publishes `checksums.sha256` next to it. The workflow refuses to replace an existing release; a mistake is fixed forward with a new version. Consumers such as Bunryu pin a release and download it by asset name, so a release that changed after publication would change what they run.

The cost is manual bumps of the digest and the libsrt commit (see [Development](../4-reference/3-development.md#pinned-build-inputs)).
