# Security reporting

Voltura Markdown reads local documents, decodes images, can fetch HTTPS images, and opens supported links through Windows. It does not execute embedded scripts or CSS. The viewer is not a security sandbox.

## Report privately

Do not put exploit details, sensitive documents, or credentials in a public issue. Once the repository enables GitHub private vulnerability reporting, use **Security → Report a vulnerability**. If that option is unavailable, ask the maintainer for a private reporting channel without including exploit details. A private reporting channel must be configured before public release.

Include the app version/build, Windows version, minimal sanitized reproduction, impact, and steps. Do not upload unrelated private files. A response-time commitment and supported-version policy have not yet been established.

## Distribution

Use release artifacts published by the project's owner and compare their published SHA-256 values. Signing status must be stated per release; checksums establish file identity and do not replace publisher signatures. Security fixes and affected versions should be documented in the changelog and release notes when available.
