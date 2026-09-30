# Game dumping (spec)

Reimplementation of the app-dumper idea ([EchoStretch/ps5-app-dumper](https://github.com/EchoStretch/ps5-app-dumper),
itself based on [logic-68/pfsmnt-dumper](https://github.com/logic-68/pfsmnt-dumper)) inside zftpd,
built out of the modules zftpd already has.

## Confirmed requirements

1. **Decrypted output**: the dump contains the game's files in the clear, taken
   from the running title's sandbox mount `/mnt/sandbox/<TITLEID>_000/app0`
   ("pfsmnt"), where the kernel already exposes the app decrypted. SELF/SPRX
   inside the tree are decrypted through the self pager as they are read.
2. **Two destinations, one job**:
   * **Browser**: streamed to the PC over HTTP (no temporary copy on the
     console, works for dumps far larger than the free space).
   * **Console**: written to any path the daemon can reach — USB (`/mnt/usbN`),
     extended storage (`/mnt/extN`), internal (`/data/...`).
3. Nothing in the dump is modified: no FSELF re-signing, no SDK backport
   (those change the payload; a transfer daemon hands out what the console
   produced).

## Reuse map

| Need | Existing piece |
| --- | --- |
| List titles + names/icons | `src/http/games/catalog.c`, `/api/admin/games/installed` |
| Start the title so its sandbox mounts | `src/http/games/psx_launch.c`, `POST /api/admin/launch` |
| Walk a tree, stream it, ZIP64 for multi-GB | `src/archive/zip_writer.c` |
| Decrypt SELF/SPRX while reading | `psx_vfs_try_open_self()` / `psx_vfs_read()` (MAP_SELF) |
| Job progress, cancel, single slot | `src/transfer/dump_job.c`, `src/http/http_api_dump.c` |
| Prepared-job + short URL pattern | `/api/archive/zip` POST → `?id=N` |
| Progress UI | Transfers view (`web/js/transfers.js`) |

## API

```text
POST /api/dump/start
{"title_id":"PPSA01234","target":"download","format":"zip","decrypt":1}
{"title_id":"PPSA01234","target":"local","dest":"/mnt/usb0/homebrew","format":"files"}
→ {"ok":true,"id":N,"title_id":"...","target":"download","state":"launching"}

GET  /api/dump?id=N               # streams the prepared archive (Content-Length)
GET  /api/dump/status             # state, source, entries, size, bytes_done
POST /api/dump/cancel {"id":N}    # id optional: one slot is implied
```

* The start reply is immediate: the title is launched (when needed) and the
  source tree is walked by the job worker, so `source`, `entries` and `size`
  arrive with the first `/api/dump/status` poll that finds the job ready.
  `target:"download"` then fetches `GET /api/dump?id=N`; `target:"local"` runs
  in the background and is observed through the status.
* `format:"zip"` — one archive (`<TITLEID>.zip`), streamed for downloads,
  written file-by-file for local targets (atomic: `<name>.zftpd.part` then
  rename, same convention as the downloader).
* `format:"files"` — directory tree, the reference layout: every file is copied
  to `dest/<TITLEID>/` with the same walk, so a dump can be inspected without
  extracting. Download targets are always ZIP.
* `decrypt` (default `1`) — SELF/SPRX entries are read through the self pager;
  `0` keeps the raw containers.
* `source` is always the running title's sandbox mount: there is no fallback to
  `/user/app/<TITLEID>`, because that path holds the *encrypted* image and the
  result would not be a usable dump.
* `/api/dump/cancel` stops a running dump (the writer gives up at the next
  chunk, a prepared archive is dropped, a streamed download ends early) and is
  what the Transfers view cancels with.

## UI

* **Games view**: `Dump to this PC` streams the decrypted archive straight to
  the browser; `Dump on the console…` picks a destination folder first and then
  offers the format (files or one ZIP) and the decrypt toggle (on by default).
* **Transfers view**: the job appears with live rate/ETA and cancel, so a
  50 GB dump is observable and interruptible.

## Auto-launch (confirmed)

The job launches the title through `POST /api/admin/launch` when `app0` is not
mounted yet and stays in state `waiting-for-title` until the sandbox appears
(bounded retry), then switches to `running`. Nothing else about the flow
changes: the dump itself starts only once the source path exists.

## SELF entries inside a ZIP (implementation note)

`zip_writer` emits each entry's size in its local header and in the central
directory, and the download mode announces the total length **before**
streaming (`zip_writer_total_size()`): the size of a SELF entry must therefore
be known at *walk* time, not at read time.

So `decrypt` support is split:

* during collection, files whose first bytes are a SELF magic are opened once
  through `psx_vfs_try_open_self()` and recorded with the **decrypted** size
  plus a `self` flag (a non-SELF or unreadable file keeps the raw size);
* while streaming, an entry flagged `self` is read with `psx_vfs_read()`
  (re-opened lazily, which is what the VFS does anyway), everything else is
  read with plain `pread()`.

The file-tree mode (`format:"files"`) needs none of this: it copies each file
as it goes, so it can decide per file at read time.
