# PKG installation

Install `.pkg` packages on PS4 and PS5 from zftpd itself: from the file explorer
when the selection is a package, and from the Games page with either a file on
the computer or one already on the console storage.

**Status:** the web installer accepts a PKG from the computer, console storage,
or an HTTP(S) link. Console files are served by zftpd with HTTP byte ranges.
The PS5 route launches an installer helper through elfldr and polls
`sceAppInstUtilGetInstallStatus`; PS4 uses BGFT. Host tests cover the HTTP
range path, but an actual PS5/PS4 installation still needs console validation.
The design notes below record the intended behavior and may include items that
are not yet implemented.

## Decisions

- **Destinations:** PS4 exposes internal and extended storage. The current PS5
  installer API does not accept an explicit destination, so its extended
  storage choice is disabled in the interface.
- **Enabled by default in ZHTTP builds**, like the web interface itself:
  `ENABLE_PKG_INSTALL` must default to on whenever `ENABLE_ZHTTPD=1`, and stay
  overridable with `ENABLE_PKG_INSTALL=0`. Verify a release build still links
  before committing this default.
- The package is **never read into memory**: it is served over HTTP on loopback
  and the system installer pulls it from that URL.

## Existing code to start from

| Path | Role |
| ---- | ---- |
| `src/http/games/psx_install.c` | the current install path — read this first |
| `src/http/games/admin_api.c`, `common.c`, `games_internal.h` | endpoints and helpers around it |
| `src/http/http_upload.c` | streaming upload, reusable for "install from the computer" |
| `src/http/http_api_files.c` | file explorer API, where the `.pkg` action is detected |
| `Makefile`, `.github/workflows/release.yml` | the `ENABLE_PKG_INSTALL` flag |

## Flow

1. The package exists either on console storage (any path the user can browse) or
   arrives by upload; uploads land in a temporary directory under
   `/data/zftpd/pkg/` and are removed when the install finishes or fails.
2. zftpd serves the package over its own HTTP server (streamed, range-capable),
   so the installer pulls the bytes instead of the payload buffering them.
3. The URL plus the chosen destination go to the platform installer:
   - **PS4** — the PKG installer service path.
   - **PS5** — the `libSceAppInstaller` API; check in the SDK which entry point
     accepts both the title and the destination, and follow Itemzflow for the
     exact call sequence.
4. Progress is polled until the installer reports success or failure; the result
   is also pushed to the console notification area.

## API surface

- `POST /api/pkg/install` — `{"path" | "url", "destination": "internal"|"extended"}`.
  Returns an install id.
- `POST /api/pkg/upload` — streaming upload followed by an install; the response
  returns the same install id.
- `GET /api/pkg/status?id=N` — state, percentage, bytes, error text.
- `POST /api/pkg/cancel?id=N` — best effort, reported honestly when the system
  installer cannot be interrupted.

## Interface

- **File explorer:** a single `.pkg` in the selection adds *Install package* to
  the context menu (and to the actions menu). The entry must not appear for
  other file types, and stays hidden for multi-selections that contain anything
  else.
- **Games page:** an *Install PKG* action offering two sources — *from the
  computer* (upload with progress) and *from console storage* (reuse the
  existing folder picker) — plus the destination selector and the free space of
  each destination next to it.
- Progress belongs in the **Transfers** view, next to downloads and dumps, not
  in a toast that disappears.

## Work breakdown

1. Read `psx_install.c` and state what it already does on PS4 and PS5; keep what
   works, delete what does not.
2. Platform layer: expose one `pal_pkg_install(path_or_url, destination)` plus a
   `pal_pkg_status()` for both consoles, with `-ENOSYS` stubs on host builds.
3. Serving path: stream a package from an arbitrary path over HTTP, with range
   support, reusing the existing response streaming.
4. Upload path: reuse `http_upload.c`, write into `/data/zftpd/pkg/`, clean up on
   every exit path.
5. Endpoints: implement the four routes above and register them in the games API
   dispatcher.
6. Destination handling: enumerate internal and extended storage, check free
   space with `statvfs`, refuse the install when it does not fit.
7. Interface: context-menu entry in the file explorer, the Games page action
   with both sources and the destination picker, progress row in Transfers.
8. `ENABLE_PKG_INSTALL` on by default for ZHTTP builds, then a full release build
   for PS4 and PS5 to confirm the link.
9. Tests and docs: host tests for destination parsing and the free-space check,
   plus updates to `docs/API.md` and the README feature list.

## PS5: how the installation has to run

What the platform requires:

- The install API is **`sceAppInstUtil*`** — `sceAppInstUtilInitialize`,
  `sceAppInstUtilInstallByPackage(pkg_metadata_t *, pkg_info_t *, playgo_info_t *)`,
  `sceAppInstUtilGetInstallStatus`, `sceAppInstUtilAppInstallAll`. The metadata
  carries `uri`, `ex_uri`, `content_id`, `content_name`, `playgo_scenario_id`,
  `icon_url`.
- **It cannot be called from a payload like zftpd.** The reference runs it in a
  **separate helper process**: it embeds a small ELF, delivers it to **elfldr on
  127.0.0.1:9021**, and then talks to that process over a loopback IPC socket
  (the helper connects back to a listener the payload opened). That is why the
  project carries `install_helper.c`, `install_helper_blob.S`,
  `install_process.c` and `install_ipc.c`.
- `sceBgftService*` (what zftpd uses today) is the PS4-shaped path; on PS5 the
  helper route is the one that installs.

### Consequence for zftpd

1. Keep the current BGFT path for PS4.
2. Add an embedded **installer helper ELF** for PS5, delivered to elfldr:9021
   exactly like a payload, with a small IPC protocol (install request →
   progress → result).
3. The helper pulls the package over HTTP from zftpd itself (we already serve
   files over loopback), so nothing is copied twice: no temporary package on
   console storage.
4. Progress: the helper reports through IPC, zftpd forwards it to
   `/api/admin/games/install_status`, which the Transfers and Games views
   already poll.

## Helper protocol (PS5)

zftpd delivers the embedded helper to the loader on `127.0.0.1:9021`, then talks
to it over a loopback socket. Requests are one line each:

| Request | Answer |
| ------- | ------ |
| `CHECK <dest> <path>` | `INSTALLED <title_id>` or `NEW` |
| `INSTALL <dest> <url> <overwrite>` | `STATE …`, `PROGRESS <pct> <bytes> <total>`, then `DONE 0` or `ERROR 0x…` |

The check exists because the title id and the installed state can only be read
from a process the loader spawned: the payload asking the same question leaves
the request hanging. When the helper answers `INSTALLED` and the caller did not
ask to overwrite, zftpd answers the interface with `needs_confirm`, which is
what produces the *Replace?* prompt in the Games and Transfers views.

## Interface

The Games action opens a dialog with a drop target (clickable as well), a
destination choice that defaults to the normal application install location, an
**Explorer** button to pick a package already on the console, and **Cancel** and
**Install** on the right.
