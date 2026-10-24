# Static hosting

Build the engine if needed, then create a release in a new directory:

```sh
python3 package-site.py ../web-dist
```

Serve the contents of `web-dist` as the site's document root. The package
includes runtime assets and licenses. Packaging uses explicit file lists;
local notes, test results, screenshots, and source archives are not published.

Hosting requirements:

- HTTPS, except when testing on localhost.
- `.mjs` served as JavaScript and `.wasm` as `application/wasm`.
- Relative paths preserved, including the `assets/` directory.
- HTML and scripts revalidated on updates to avoid stale versions.

No backend, database, or cross-origin isolation headers are required.
`nginx.conf` is an optional example; set its document root and listening port
for the host. Replace releases together so the scripts and WASM stay in sync.
Retain the license files when publishing.

To create a separate development source archive, add
`--source-archive ../source.tar.gz` to the packaging command. Its destination
must be outside the public site directory.
