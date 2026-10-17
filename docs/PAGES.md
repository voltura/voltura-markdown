# Product site and GitHub Pages

The end-user page lives in [site/](../site/), separate from developer documentation. It uses local artwork and static HTML/CSS, with no external fonts, scripts, or tracking.

## Build and preview

Use the intended repository and release tag. These commands only generate local output; they do not create a repository, release, or website:

```powershell
.\scripts\build-pages.ps1 -Repository voltura/voltura-markdown -ReleaseTag v1.0.0-preview.2
python -m http.server 8080 --bind 127.0.0.1 --directory build/pages
```

Open `http://127.0.0.1:8080`; Ctrl+C stops the server. Output lives in ignored `build/pages`. Repository/release links become usable once those destinations are actually published.

`-ReleaseTag` points downloads directly at that tag, including a prerelease. Omit it to use the stable `v<version>` tag matching `CMakeLists.txt`. Pin both downloads to the page's release so cached pages never combine an older installer filename with a newer release. Ensure the chosen release contains both matching assets. Update the first-release availability message in `site/index.html` only after downloads exist.

## Publish explicitly

The repository must already exist. Install Git/GitHub CLI and configure authentication, a Git commit identity, and repository push/Pages administration permissions.

```powershell
# Build only; no remote changes:
.\scripts\publish-pages.ps1 -Repository voltura/voltura-markdown -ReleaseTag v1.0.0-preview.2

# Publish only after reviewing the local output and real release downloads:
.\scripts\publish-pages.ps1 -Repository voltura/voltura-markdown -ReleaseTag v1.0.0-preview.2 -Publish
```

`-Publish` sends the generated files through GitHub's Git database API, preserves existing `gh-pages` history and `CNAME`, updates the remote ref without force, then configures [branch-based Pages](https://docs.github.com/en/rest/pages/pages) at `gh-pages:/`. It never creates another local checkout or switches the primary source away from `main`. No project GitHub Actions workflow is required. A concurrent branch update is rejected normally; review and rerun rather than force-publishing.

If Pages settings fail after the branch push, inspect the error and **Settings → Pages**. Verify the public URL and both downloads once deployment completes.

## Custom domain

The public product address is [https://markdown.voltura.se/](https://markdown.voltura.se/). GitHub Pages serves the existing `gh-pages:/` site directly at this hostname; one.com manages its DNS.

| Setting | Value |
| --- | --- |
| GitHub Pages custom domain | `markdown.voltura.se` |
| Published branch root `CNAME` file | `markdown.voltura.se` |
| one.com DNS type | `CNAME` |
| one.com hostname | `markdown` |
| one.com target | `voltura.github.io` |
| TTL | Provider default |

The DNS target is a hostname without a scheme or repository path. Keep **Enforce HTTPS** enabled after GitHub provisions the custom-domain certificate. The previous `https://voltura.github.io/voltura-markdown/` address redirects to the custom domain.

The publication script preserves the published branch's `CNAME`, so subsequent releases retain the custom domain. After publication, verify that file, the Pages domain and HTTPS settings, styles/artwork, and both downloads. DNS propagation and certificate provisioning can take time; confirm successful HTTPS before declaring the domain ready.

For rollback, remove this hostname's DNS record before removing the GitHub Pages custom-domain binding. Preserve the apex, wildcard, email, and other product DNS records.

## Source map

| File | Purpose |
| --- | --- |
| [index.html](../site/index.html) | Product copy, benefits, FAQ, download links |
| [styles.css](../site/styles.css) | Responsive layout, light/dark themes, focus styles |
| [build-pages.ps1](../scripts/build-pages.ps1) | Resolve repository/tag/version and copy static assets |
| [publish-pages.ps1](../scripts/publish-pages.ps1) | Explicit local `gh-pages` publication and Pages settings |

Before publishing, check narrow/desktop layouts, both themes, keyboard access, FAQ expansion, assets, and real download links. Keep build/architecture details in developer documentation.
