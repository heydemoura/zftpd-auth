#include "transfer/backend_torrent.h"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace lt = libtorrent;

static void set_error(char *out, size_t size, const std::string &message) {
  if (out != nullptr && size != 0) {
    std::snprintf(out, size, "%s", message.c_str());
  }
}

static bool same_hash(lt::info_hash_t const &a, lt::info_hash_t const &b) {
  return (a.has_v1() && b.has_v1() && a.v1 == b.v1) ||
         (a.has_v2() && b.has_v2() && a.v2 == b.v2);
}

static void load_resume(const char *path, lt::add_torrent_params &params) {
  if (path == nullptr) return;
  std::FILE *file = std::fopen(path, "rb");
  if (file == nullptr) return;
  std::vector<char> data;
  char buffer[8192];
  size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), file)) != 0 &&
         data.size() + n <= 16U * 1024U * 1024U) {
    data.insert(data.end(), buffer, buffer + n);
  }
  const bool complete = std::feof(file) != 0;
  (void)std::fclose(file);
  if (!complete || data.empty()) return;

  lt::error_code ec;
  lt::add_torrent_params saved = lt::read_resume_data(
      lt::span<char const>(data.data(), data.size()), ec);
  if (ec || !same_hash(params.info_hashes, saved.info_hashes)) return;
  saved.save_path = params.save_path;
  saved.trackers = params.trackers;
  saved.tracker_tiers = params.tracker_tiers;
  saved.url_seeds = params.url_seeds;
  saved.flags = params.flags;
  params = std::move(saved);
}

static void save_resume(const char *path, lt::add_torrent_params const &params) {
  if (path == nullptr) return;
  const std::vector<char> data = lt::write_resume_data_buf(params);
  const std::string temporary = std::string(path) + ".tmp";
  (void)unlink(temporary.c_str());
  int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return;
  size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = write(fd, data.data() + offset, data.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    offset += static_cast<size_t>(n);
  }
  const bool written = offset == data.size() && fsync(fd) == 0;
  (void)close(fd);
  if (written) {
    (void)rename(temporary.c_str(), path);
  } else {
    (void)unlink(temporary.c_str());
  }
}

extern "C" int transfer_torrent_validate(const char *uri) {
  if (uri == nullptr) return 0;
  lt::error_code ec;
  lt::add_torrent_params params = lt::parse_magnet_uri(uri, ec);
  return !ec && (params.info_hashes.has_v1() || params.info_hashes.has_v2()) ? 1 : 0;
}

extern "C" int transfer_torrent_run(const char *uri, const char *save_dir,
                                      const char *resume_file,
                                      transfer_torrent_progress_cb progress,
                                      void *ctx, char *error,
                                      size_t error_size) {
  if (uri == nullptr || save_dir == nullptr || progress == nullptr) {
    set_error(error, error_size, "Invalid BitTorrent arguments");
    return -1;
  }

  try {
    lt::error_code ec;
    lt::add_torrent_params params = lt::parse_magnet_uri(uri, ec);
    if (ec || (!params.info_hashes.has_v1() && !params.info_hashes.has_v2())) {
      set_error(error, error_size, ec ? ec.message() : "Invalid magnet info hash");
      return -1;
    }
    params.save_path = save_dir;
    load_resume(resume_file, params);

    lt::settings_pack settings = lt::min_memory_usage();
    settings.set_int(lt::settings_pack::connections_limit, 80);
    settings.set_int(lt::settings_pack::active_downloads, 1);
    settings.set_int(lt::settings_pack::active_seeds, 0);
    settings.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:0");
    lt::session session(settings);
    lt::torrent_handle handle = session.add_torrent(params, ec);
    if (ec || !handle.is_valid()) {
      set_error(error, error_size, ec ? ec.message() : "Cannot start BitTorrent");
      return -1;
    }

    bool paused = false;
    auto next_save = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
      std::vector<lt::alert *> alerts;
      session.pop_alerts(&alerts);
      for (lt::alert *alert : alerts) {
        if (auto *saved = lt::alert_cast<lt::save_resume_data_alert>(alert))
          save_resume(resume_file, saved->params);
      }
      const lt::torrent_status status = handle.status();
      if (status.errc) {
        set_error(error, error_size, status.errc.message());
        return -1;
      }
      const uint64_t done = static_cast<uint64_t>(std::max<std::int64_t>(0, status.total_wanted_done));
      const uint64_t total = static_cast<uint64_t>(std::max<std::int64_t>(0, status.total_wanted));
      const uint64_t speed = static_cast<uint64_t>(std::max(0, status.download_rate));
      const int action = progress(ctx, done, total, speed);
      if (action < 0) return -1;
      if (action > 0 && !paused) {
        handle.pause();
        paused = true;
      } else if (action == 0 && paused) {
        handle.resume();
        paused = false;
      }
      if (status.has_metadata && status.is_seeding && action == 0) {
        handle.pause();
        if (resume_file != nullptr) (void)unlink(resume_file);
        return 0;
      }
      if (resume_file != nullptr && status.has_metadata &&
          std::chrono::steady_clock::now() >= next_save) {
        handle.save_resume_data();
        next_save = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  } catch (const std::exception &e) {
    set_error(error, error_size, e.what());
    return -1;
  } catch (...) {
    set_error(error, error_size, "Unexpected BitTorrent error");
    return -1;
  }
}
