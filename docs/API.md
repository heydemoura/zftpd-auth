# zhttpd REST API Documentation

## Endpoints

### GET /api/status

Identity and build switches of the running daemon. The web interface polls it
for the connection state and to know which features the payload was compiled
with, so views can hide what the build does not have.

```json
{"ok":true,"version":"1.6.0","instance_id":"15114b8a2c31d9f0",
 "start_monotonic_ns":463980095296000,"pid":74,"platform":"ps4",
 "features":{"pkg_install":false},
 "auth":{"enabled":true,"authenticated":false,"user":"","role":"none"}}
```

- `instance_id` changes when the payload is re-injected: clients treat it as a
  restart and drop their server-side handles.
- `features.pkg_install` mirrors the `ENABLE_PKG_INSTALL` build flag. When it is
  `false` the package installer endpoints answer `409 PKG installation is
  disabled for this build` and the UI hides the installer entirely.
- `auth` describes the login gate (see below). `/api/status` itself never
  requires a login so clients can always discover the gate.

## Login gate

The gate is off unless the daemon was started with `ZFTPD_ADMIN_PASSWORD`
(see the README). When it is on, every `/api/*` route except `/api/status`,
`/api/auth/login`, `/api/auth/logout` and `/api/auth/me` needs a session, sent
either as the `zftpd_session` cookie set by the login call or as
`Authorization: Bearer <token>`. Unauthenticated calls answer
`401 {"error":"Login required","auth":"required"}`.

Two roles exist. `admin` may call everything. `user` may call only the file
routes (`/api/list`, `/api/dirsize`, `/api/file/get`, `/api/download`,
`/api/create_file`, `/api/mkdir`, `/api/delete`, `/api/rename`, `/api/copy*`,
`/api/upload`, `/api/extract*`, `/api/archive/zip`, `/api/stats*`,
`/api/disk/info`, `/api/mounts`, `/api/game/meta`, `/api/game/icon`,
`/api/auth/password`) and every path those calls carry (`path`, `dst`, `paths`,
`dest`) must be inside one of the allowed folders, otherwise
`403 {"error":"...","auth":"folder"}`. Other routes answer
`403 {"auth":"admin"}` for users.

POST calls still need the `X-CSRF-Token` header taken from `index.html`.

### POST /api/auth/login

Body `{"login":"admin","password":"..."}`. Answers `200` with the session
cookie and `{"ok":true,"enabled":true,"user":"admin","role":"admin",
"token":"<64 hex>","expires_in":604800}`; a wrong password answers `401` after
a short delay. With the gate off it answers `{"ok":true,"enabled":false,...}`.

### POST /api/auth/logout

Ends the session and clears the cookie.

### GET /api/auth/me

`{"ok":true,"enabled":true,"authenticated":true,"user":"bob","role":"user",
"folders":["/data/shared"]}` — `folders` is the allow-list for the user role.

### POST /api/auth/password

Body `{"current":"...","password":"..."}`: changes the caller's own password
(6 to 128 characters).

### Users (administrators)

- `GET /api/auth/users` → `{"ok":true,"users":[{"login":"admin","role":"admin"}]}`
- `POST /api/auth/users` body `{"login","password","role"}` creates an account.
  Logins are 1–32 characters from `A-Z a-z 0-9 . _ - @`; role is `admin` or `user`.
- `POST /api/auth/users/update` body `{"login","password"?,"role"?}`. A new
  password signs that user out everywhere. The caller cannot change their own
  role; the last administrator cannot be demoted.
- `POST /api/auth/users/delete` body `{"login"}`. The caller cannot delete
  their own account; the last administrator cannot be removed.

Each call answers with the updated user list.

### Folder rules (administrators)

- `GET /api/auth/access` → `{"ok":true,"folders":[...]}`
- `POST /api/auth/access` body `{"folders":["/data/shared", ...]}` replaces
  the list (32 folders at most; each must exist inside the served root).

## Share links

A share makes one file or folder reachable **without a login** at
`/s/<id>` (`id` = 32 hex characters). Shares are persisted and may expire.

- `GET /s/<id>` — file share: direct download (`Content-Disposition:
  attachment`, `Range` supported). Folder share: HTML listing.
- `GET /s/<id>/<sub/path>` — a file (download) or sub-folder (listing) inside a
  shared folder. Nothing outside the shared folder is reachable.
- `GET /s/<id>/<sub>?zip=1` — the folder streamed as a ZIP archive.
- An unknown id answers `404`; an expired link answers `410 Gone`.

### GET /api/shares (administrators)

```json
{"ok":true,"now":1790910000,"shares":[
 {"id":"cbc3e1733df411af5870e654121f9498","path":"/data/pub","name":"pub",
  "type":"directory","url":"/s/cbc3e1733df411af5870e654121f9498",
  "created":1790910297,"expires":1790913897,"expired":false,"owner":"admin"}]}
```

`expires` is a Unix time, `0` meaning never.

### POST /api/shares/create (administrators)

Body `{"path":"/data/pub","ttl":3600}` or `{"path":"...","expires":<unix>}`
(omit both, or `"expires":0`, for a link that never expires). Answers
`{"ok":true,"share":{...}}` with the record shown above.

### POST /api/shares/delete (administrators)

Body `{"id":"..."}`. Removes the link immediately.

### POST /api/shares/purge (administrators)

Removes every expired link; answers `{"ok":true,"removed":N}`.

### GET /api/list

List directory contents.

**Query Parameters:**
- `path` (required): Directory path

**Response:**
```json
{
  "path": "/home",
  "entries": [
    {
      "name": "file.txt",
      "type": "file",
      "size": 1024
    },
    {
      "name": "subdir",
      "type": "directory",
      "size": 0
    }
  ]
}
```

**Errors:**
- `404`: Directory not found
- `500`: Internal error

### GET /api/download

Download file (TODO).

### GET /api/mounts

Lists the removable volumes that are **really** connected (`/mnt/usb0`…`/mnt/usb7`,
`/mnt/ext0`, `/mnt/ext1`). The console creates those directories even with
nothing plugged in, so zftpd checks whether a filesystem is mounted on them
(different `st_dev` from the parent directory) and reports size and free space.
The file manager's Places list is built from this, so empty slots no longer
appear as drives.

```json
{"ok":true,"mounts":[{"path":"/mnt/usb0","name":"usb0","kind":"usb","total":32007397376,"free":12561055744}]}
```

### GET /api/archive/zip

Streams a ZIP archive of the selection — the bulk download used by the web UI
for "Download as ZIP". Entries are produced while the response is sent (no
temporary archive on the console), stored uncompressed, and folders are walked
recursively. ZIP64 records are emitted automatically for entries, offsets or
counts past the 32-bit limits, so multi-gigabyte selections stay valid.

The response announces the exact `Content-Length` (no chunked encoding), so
console browsers show progress and complete the download reliably.

**Query Parameters:**
- `path` (required, repeatable): file or folder, confined to the HTTP root.
- `name` (optional): download name, default `zftpd-<timestamp>.zip`.

Up to 64 paths per request; unreadable or unconfined entries are skipped, and
when the selection exceeds the entry limit the response carries
`X-Zftpd-Truncated: entry-limit`.

```text
GET /api/archive/zip?path=/data/games&path=/data/saves/config.bin&name=backup.zip
```

### GET /api/file/get

Streams a file to the client. By default the raw on-disk bytes are sent
(zero-copy `sendfile`), exactly like every other transfer path.

**Query Parameters:**
- `path` (required): file to send, confined to the HTTP root.
- `decrypt` (optional): `1` asks for the **decrypted** content when the file is
  a SELF container (PS4/PS5). The kernel pager is swapped in for the transfer
  (MAP_SELF), so the client receives the plain executable instead of the
  encrypted container. Other files are always sent untouched, and the response
  carries `X-Zftpd-Decrypted: self` plus chunked encoding.

The web UI exposes this as *Settings → Downloads → Decrypt protected files*,
which appends the flag to every download and preview link.

```text
GET /api/file/get?path=/mnt/disc/CUSA00001/sce_sys/eboot.bin&decrypt=1
```

### Game dump

Decrypted dump of an installed title, read from the running title's sandbox
mount (`/mnt/sandbox/<TITLEID>-app0[-patch0-union]` on PS4, `_000/app0` and
friends under `/mnt/sandbox` on PS5), where the kernel exposes the application
in the clear; SELF/SPRX entries are decrypted through the self pager while they
are read. There is no fallback to the encrypted `/user/app` image, so a title
that is not running is launched first and the job waits for its sandbox to
mount. One dump runs at a time.

```text
POST /api/dump/start
{"title_id":"CUSA00001","target":"download","format":"zip","decrypt":1}
{"title_id":"CUSA00001","target":"local","dest":"/mnt/usb0/homebrew","format":"files"}

GET  /api/dump/status          # state, source, entries, size, bytes_done
GET  /api/dump?id=N            # streams the prepared archive (Content-Length)
POST /api/dump/cancel          # {"id":N} optional; the single slot is implied
```

`target` is `download` (default; a streamed ZIP the client then fetches with
`GET /api/dump?id=N`) or `local`. `format` is `zip` (default) or `files`
(console targets only — a streamed dump is always a ZIP). `decrypt` defaults
to `1`; `0` keeps the raw SELF containers.

`POST /api/dump/start` answers immediately with the slot id and the initial
state; `source`, `entries` and `size` become available through the status as
soon as the job worker has walked the source tree:

```json
{"ok":true,"id":1,"state":"launching","title_id":"CUSA00001","target":"download"}
```

`GET /api/dump/status` (the response the web UI polls):

```json
{"ok":true,"active":true,"id":1,"state":"running","title_id":"CUSA00001",
 "source":"/mnt/sandbox/CUSA00001_000/app0","entries":1234,"size":32212254720,
 "bytes_done":1073741824,"started":1790778983,"to_console":false,
 "cancelled":false,"message":""}
```

`state` is `launching`, `running`, `ready` (streamed dump prepared, waiting for
the download), `done` or `failed`; `message` carries the failure reason
(`Cancelled` after a cancel request). A slot that stops advancing is retired by
its deadline and reported as `failed`, so a broken dump never blocks the next
one.

`POST /api/dump/cancel` stops the job — the local writer gives up at the next
chunk, a prepared archive is dropped, a streamed download ends early:

```json
{"ok":true,"id":1,"state":"running","cancelled":true}
```

### POST /api/system/eject

Ejects the Blu-ray disc from the console's optical drive (PS4/PS5). The drive
is a FreeBSD `cd(4)` device: zftpd sends `CDIOCALLOW` + `CDIOCEJECT` to
`/dev/cd0` and shows a toast on success.

```json
{"ok":true,"status":"ok","message":"Disc ejected","code":0}
```

Failures answer HTTP 200 with `ok:false` and the driver reason, so a client can
tell a Digital Edition console (`ENOENT`) from a busy drive (`EBUSY`):

```json
{"ok":false,"status":"error","message":"Drive busy: a disc game is running","code":-16}
```

### GET /api/system/disc

Reports whether the console has an optical drive, so the UI can hide the action
on Digital Edition consoles.

```json
{"ok":true,"present":true}
```

### GET|POST /api/notify

Show a custom system notification on PS4/PS5 (toast). On non-console builds the
message is forwarded to syslog. Designed for local-network automation such as
Home Assistant.

**Query Parameters:**
- `text` (required): Notification message (URL-encoded). Max 1023 bytes after decode.
- `icon` (optional): PS notification texture suffix, default `icon_system`.
  Allowed characters: `[A-Za-z0-9_]`.

**Examples:**
```
GET /api/notify?text=Washing%20machine%20is%20done
GET /api/notify?text=Doorbell&icon=icon_system
```

**Home Assistant (`rest_command`):**
```yaml
rest_command:
  ps5_notify:
    url: "http://{{ states('sensor.ps5_ip') }}:2121/api/notify?text={{ text | urlencode }}"
    method: GET
```

**Response:**
```json
{
  "ok": true,
  "text": "Washing machine is done",
  "icon": "icon_system"
}
```

**Errors:**
- `400`: Missing/empty `text`, control characters, or invalid `icon`
- `405`: Method other than GET/POST

> Prefer **GET** for automation clients. POST is subject to CSRF validation when
> web upload support is enabled.

### Static Files

- `GET /` → `index.html`
- `GET /style.css` → CSS
- `GET /app.js` → JavaScript

## Adding Custom Endpoints

Edit `src/http/http_api.c`:

```c
http_response_t* http_api_handle(const http_request_t *request) {
    if (strncmp(request->uri, "/api/custom", 11) == 0) {
        return handle_custom(request);
    }
    // ... existing handlers
}
```

## Package installation

`GET|POST /api/admin/games/install`

| Parameter | Meaning |
| --------- | ------- |
| `path` | Package already on console storage. |
| `url` | Package the console downloads by itself (http/https). |
| `dest` | `internal` (default) or `extended`. |
| `overwrite` | `1` replaces an existing installation of the same title. |

Exactly one of `path` and `url` is required. When the package is already
installed the request answers `409` with
`{"error":"already_installed","title_id":…,"can_overwrite":true}` and nothing is
changed; the caller repeats it with `overwrite=1` to replace the installation.

`GET /api/admin/games/install_status` reports the running task:

```json
{"ok":true,"active":true,"task_id":-1,"progress":42,"error":0,
 "length":0,"transferred":0,"title_id":"","path":"https://…/package.pkg"}
```

On PS5 the installation is driven by a helper program that zftpd delivers to the
loader on loopback, because the system installer API is only usable from a
process the loader spawned. On PS4 the download service performs it directly.
