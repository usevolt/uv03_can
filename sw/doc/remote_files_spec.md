# Usevolt Remote Files — Technical Specification (v1)

Usevolt issues each customer a single account (a username and a server-issued
password); customers cannot self-register, change their username, or change their
password. The desktop tool **uvcan** uses those credentials to (1) list the
downloadable files available to that account and (2) download a selected file
(firmware images and packages, a few MB at most) from a Usevolt server. The design
is deliberately small: one service, SQLite, and files on disk, behind a
reverse proxy that terminates TLS. Credentials are stored client-side in plain
text; that is acceptable because every connection uses TLS with certificate
verification, passwords are high-entropy random strings issued by Usevolt, and
each account can only ever read files inside its own folder. Simplicity is a
feature — this spec is meant to be implementable by one engineer in a few days and
maintained without a dedicated ops team.

---

## Threat model & security principles

The system is not trying to defend against a compromised customer machine or a
determined insider. It is trying to make the ordinary risks — network sniffing,
credential brute-forcing, cross-account snooping, and accidental secret leakage —
structurally impossible or economically pointless.

- **All traffic over HTTPS/TLS**, with certificate verification **ON** in the
  client. Optionally pin Usevolt's CA or leaf certificate for defense in depth.
  Plaintext HTTP is never accepted (the proxy redirects to HTTPS and sets HSTS).
- **Passwords are server-issued and high-entropy** — 16+ characters of random
  data (roughly 96+ bits). The customer never chooses a password, so it can't be
  weak or reused. This single decision is what makes both plaintext client-side
  storage and network brute-force non-issues: there is nothing to guess, and a
  stolen file only exposes an account that Usevolt can disable centrally.
- **Token-based auth.** The password is sent exactly once, to `POST /login`,
  which returns a short-lived bearer token. Every later request carries the token,
  not the password. The token lives in client RAM only and is **never written to
  disk**. The password is stored client-side in a per-user file (plain text —
  acceptable given the points above).
- **Server hardening:**
  - Passwords hashed with **argon2id** (never stored or logged in the clear).
  - Session tokens stored only as their **SHA-256 hash**; a database leak cannot
    be replayed as a live token.
  - **Rate-limiting / lockout** on `POST /login` (per-username and per-IP) to blunt
    online guessing.
  - **Generic error messages** — always "invalid credentials," never revealing
    whether the username exists.
  - **Never log secrets** (no passwords, tokens, or `Authorization` headers in
    request logs).
  - **Opaque per-account file access** — the server only ever builds paths under
    the authenticated account's own directory, so one account can never read, list,
    or even confirm the existence of another account's files.

---

## Server architecture (recommended: Option A)

**Option A (recommended):** one small HTTP service + **SQLite** + files on local
disk, sitting behind **Caddy**, which provides automatic HTTPS via Let's Encrypt.
Everything runs on a single VM.

- **Primary implementation: Go.** Compiles to a single static binary with no
  runtime to install, has a solid standard-library HTTP server, `sendfile`-based
  file streaming, and good SQLite and argon2 libraries. Deployment is "copy one
  file and restart a service."
- **Equally valid alternative: Python / FastAPI.** Slightly more readable for some
  teams, at the cost of shipping a Python environment. Pick whichever the
  maintainers are most comfortable owning; the HTTP API in this document is
  identical either way.

**Options considered and dismissed at this scale:**

- **Option B — object storage (S3 or compatible) with signed URLs.** Great for
  large files, high traffic, or CDN fan-out. Overkill here: a few MB per download
  and a handful of customers don't justify the extra moving parts. Noted as a later
  optimization (see the download endpoint) that would *not* change the client flow.
- **Option C — serverless (Cloudflare Workers + R2, or similar).** Attractive for
  zero-maintenance edge hosting, but adds a platform dependency and makes the
  simple "one VM, one binary, one folder" mental model harder. Not warranted for a
  low-volume internal distribution tool.

**File storage is per-customer only.** Each account has exactly one root
directory:

```
/srv/uvfiles/<username>/
```

Within that root the customer may have arbitrary subdirectories. **Versions are
just multiple files**, optionally organized under a per-product subfolder — there
is no special "version" object on disk, only files listed in the manifest.

---

## Storage layout & the per-account manifest

Each customer folder contains a `manifest.json`. It is **hidden from listings**
(never itself downloadable) and serves two jobs at once:

1. It is the **source of truth** for what products/versions this account has.
2. It is the **download allowlist** — a file may only be downloaded if it appears
   in the manifest.

The manifest is a `products → versions` tree with metadata. Exact structure:

```json
{
  "products": [
    {
      "id": "uv07_controller",
      "name": "UV07 Controller Firmware",
      "versions": [
        { "version": "13", "path": "uv07_controller/firmware_v13.bin",
          "released": "2026-07-01", "notes": "Fixes CAN bring-up", "sha256": "a1b2…" },
        { "version": "12", "path": "uv07_controller/firmware_v12.bin",
          "released": "2026-06-01", "notes": "Initial release", "sha256": "c3d4…" }
      ]
    }
  ]
}
```

Field notes:

- `path` is **relative to the account root** (`/srv/uvfiles/<username>/`). It is
  never absolute and never escapes the root (see *Path safety*).
- `sha256` is the hex digest of the file contents, computed by the admin CLI at
  upload time and echoed to the client so downloads can be verified.
- `version`, `released`, and `notes` are display metadata for the client UI.

**Server behavior:**

- **On list** (`GET /api/v1/files`): read `manifest.json`; for each version, verify
  its `path` exists as a regular file under the account root; **enrich** each entry
  with `size` and `modified` (mtime) read from disk; **drop** any entry whose file
  is missing. Return the resulting products/versions tree.
- **On download** (`GET /api/v1/files/content`): the requested `path` must be
  **present in the manifest** (allowlist check) **and** pass the path-safety checks
  below. Only then is the file streamed.
- **Atomic writes:** the manifest is always written by the admin CLI to a temp file
  in the same directory and then `rename()`d over the old one, so readers never see
  a half-written file.

This on-disk manifest is the natural stopping point *before* introducing a real
database of files. If requirements later grow (search, auditing, shared
entitlements), graduating to `products` / `versions` tables **does not change the
client API** — the same JSON shapes are served from a different backing store.

---

## HTTP API (v1)

Base path: `/api/v1`. All responses are JSON unless streaming file content. All
requests other than `login` require `Authorization: Bearer <token>`.

### `POST /api/v1/login`

Exchange credentials for a bearer token.

```http
POST /api/v1/login HTTP/1.1
Host: files.usevolt.fi
Content-Type: application/json

{ "username": "acme", "password": "xЗ9…16+ random chars…" }
```

Success (`200`):

```json
{ "token": "b7f3…opaque…", "expires_at": "2026-07-10T08:00:00Z" }
```

Bad credentials (`401`) — deliberately generic:

```json
{ "error": "invalid credentials" }
```

### `GET /api/v1/files`

List the account's products/versions, enriched from disk.

```http
GET /api/v1/files HTTP/1.1
Host: files.usevolt.fi
Authorization: Bearer b7f3…
```

Success (`200`):

```json
{
  "products": [
    {
      "id": "uv07_controller",
      "name": "UV07 Controller Firmware",
      "versions": [
        {
          "version": "13",
          "path": "uv07_controller/firmware_v13.bin",
          "released": "2026-07-01",
          "notes": "Fixes CAN bring-up",
          "sha256": "a1b2c3d4e5f6…",
          "size": 262144,
          "modified": "2026-07-01T09:14:22Z"
        },
        {
          "version": "12",
          "path": "uv07_controller/firmware_v12.bin",
          "released": "2026-06-01",
          "notes": "Initial release",
          "sha256": "c3d4e5f6a1b2…",
          "size": 258048,
          "modified": "2026-06-01T11:02:05Z"
        }
      ]
    }
  ]
}
```

`size` (bytes) and `modified` (mtime, RFC 3339) are added by the server; `sha256`
is carried through from the manifest.

### `GET /api/v1/files/content?path=<relpath>`

Download one file. `path` must exactly match a `path` in the manifest.

```http
GET /api/v1/files/content?path=uv07_controller/firmware_v13.bin HTTP/1.1
Host: files.usevolt.fi
Authorization: Bearer b7f3…
```

Success (`200`) streams the bytes:

```http
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: 262144
Content-Disposition: attachment; filename="firmware_v13.bin"

<binary file bytes>
```

Errors: `401` (missing/expired token), `403` or `404` if the path is not in the
manifest or fails path safety (a single generic "not found" is fine — do not
distinguish "exists but forbidden" from "does not exist").

The server streams **directly from disk** — `sendfile`/chunked transfer, O(1)
memory, never buffering the whole file. At a few MB per file and low concurrency
this is entirely fine: there is no CDN to feed and no worry about holding
connections open for large transfers. Signed URLs / object storage (Option B) are
a later optimization that would move only the *bytes* off the box; the client flow
(list, pick, download, verify sha256) would be unchanged.

### `POST /api/v1/logout` (optional)

Invalidate the current token by deleting its session row.

```http
POST /api/v1/logout HTTP/1.1
Authorization: Bearer b7f3…
```

Success: `204 No Content`.

### Token lifecycle

Tokens expire in **12–24 hours**. Clients do not need to track expiry precisely:
on any request that returns `401`, the client **silently re-runs `login`** with the
stored credentials to get a fresh token and **retries the original request once**.
If the retry also fails, the error is surfaced to the user (likely a
disabled account or changed password).

---

## Path safety (critical)

The download endpoint receives a client-supplied relative `path`. Even though it is
also checked against the manifest allowlist, treat it as hostile and validate it
independently:

1. **Reject outright** if the path:
   - is empty,
   - is absolute or starts with `/`,
   - contains a `..` component,
   - (Windows-origin inputs) contains a drive letter (`C:`) or a backslash `\`.
2. **Join** the cleaned relative path to the authenticated account's root:
   `/srv/uvfiles/<username>/<path>`.
3. **Resolve** the result with `realpath()` (canonicalize, following symlinks).
4. **Verify** the resolved absolute path still starts with the account root
   (`/srv/uvfiles/<username>/`). This defeats symlink-escape attempts, where a file
   inside the folder points outside it.
5. **Serve only regular files** — reject directories, symlinks-to-directories,
   devices, FIFOs, etc.

Because the effective path is **always rebuilt from the authenticated account's own
folder**, and re-verified after canonicalization, cross-account access is
impossible even if the manifest or filesystem is misconfigured.

---

## Admin / provisioning CLI (`uvfilesctl`)

There is **no self-service**: no signup, no password reset, no web admin panel.
Usevolt staff manage everything with a small server-side CLI that runs on the VM
next to the data. Staff **never hand-edit `manifest.json`** — the CLI is the only
writer, guaranteeing valid JSON and correct sha256 values.

- **`uvfilesctl adduser <username>`**
  Generates a high-entropy random password, stores its **argon2id** hash in the
  `users` table, creates `/srv/uvfiles/<username>/`, and **prints the password
  once** (to be delivered to the customer through a secure channel). The plaintext
  is never stored or shown again.

- **`uvfilesctl addversion <user> --product <id> --name "<name>" --version <v> --released <YYYY-MM-DD> --notes "<text>" <file>`**
  Copies `<file>` into the account's product folder
  (`/srv/uvfiles/<user>/<id>/`), computes its **sha256**, and updates
  `manifest.json` **atomically** (temp file + rename). If the product `id` is new
  it is created; if the version already exists the command errors rather than
  silently overwriting.

  ```bash
  uvfilesctl addversion acme \
    --product uv07_controller \
    --name "UV07 Controller Firmware" \
    --version 13 \
    --released 2026-07-01 \
    --notes "Fixes CAN bring-up" \
    ./build/firmware_v13.bin
  ```

- **`uvfilesctl rmversion <user> <productId> <version>`**
  Removes the version entry from the manifest (and optionally its file), atomically.

- **`uvfilesctl ls <user>`**
  Prints the account's products/versions and whether each file is present on disk.

- **`uvfilesctl disable <user>`**
  Marks the account disabled (`users.disabled = 1`) so future logins fail; existing
  sessions can be cleared at the same time.

---

## Client integration (uvcan)

### Transport

uvcan **shells out to the `curl` CLI** rather than linking a new HTTP library.
`curl` is present on Linux and built into Windows 10+, so this adds **no new link
dependency** and lets curl handle TLS, certificate verification, redirects, and
retries. (If Usevolt later prefers linking a library, **libcurl** is the drop-in
alternative and the module boundary below stays the same.)

### `remotefiles` module

A new `remotefiles` module wraps the three operations. All network work runs
**off the UI thread** (as a task, consistent with uvcan's task model), so the
interface never blocks.

- **`remotefiles_login()`** — POST credentials to `/api/v1/login`, capture the
  bearer token into RAM (never written to disk).
- **`remotefiles_list()`** — GET `/api/v1/files` with the token, parse the JSON
  products/versions tree.
- **`remotefiles_download(path, dest)`** — GET `/api/v1/files/content?path=…` to
  `dest`, then **verify the downloaded file's sha256** against the value from the
  listing; a mismatch is a hard error and the file is discarded.

On any `401`, the module transparently re-logs-in and retries once (per the token
lifecycle above).

### Credentials & configuration

- Username and password come from **uvcan's shared account store** (the same
  credential storage used elsewhere in the tool), plus a new **server URL** field.
- If the username or password is unset, uvcan **prompts the user to set them**
  before the first request. There is no in-app way to create or change the account
  itself — those come from Usevolt out of band.

### UI

- Each device tab gains a **"Server files"** button.
- Clicking it opens a window that lists the account's files as a **tree
  (product → versions)**, showing each version's metadata: **version, release
  date, notes, and size**.
- Each version row has a **Download** action. After download, uvcan performs the
  **sha256 verification** and reports success or a checksum failure.

---

## Data model (SQLite) — reference

The database holds only accounts and sessions. Files are **not** in the database in
the simple design — the per-account `manifest.json` on disk is the source of truth.

```
users(id, username UNIQUE, pw_hash, disabled, created_at)
sessions(token_hash, user_id, expires_at)
```

- `users.pw_hash` is an argon2id hash. `users.disabled` gates login.
- `sessions.token_hash` is the SHA-256 of the bearer token (the token itself is
  never stored). Expired rows are pruned lazily on use and/or by a periodic sweep.

**Where a real file schema would slot in** if requirements grow: add
`products`, `versions`, and `assignments` (which account may see which
versions) tables. This would enable server-side **search**, **download
auditing**, and **shared entitlements** (one uploaded version visible to several
accounts). Crucially, the client-facing HTTP API stays byte-for-byte the same —
only the code that produces the `products` JSON changes.

---

## Deployment notes

- **One small VM** runs everything: the service binary (or FastAPI app) plus
  **Caddy** as the TLS-terminating reverse proxy. A minimal Caddyfile is about
  three lines and gives automatic HTTPS from Let's Encrypt:

  ```caddy
  files.usevolt.fi {
      reverse_proxy 127.0.0.1:8080
  }
  ```

  Caddy manages certificate issuance and renewal automatically.
- **HSTS on** — instruct browsers/clients to use HTTPS only.
- **Backups** are trivial: copy the **SQLite file** and the **`/srv/uvfiles`**
  tree. That is the entire durable state.
- **Keep everything on one box.** No CDN, no object store, no external services to
  coordinate. The whole system is a binary, a config file, a database file, and a
  directory of customer folders.
