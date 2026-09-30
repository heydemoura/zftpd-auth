#include "transfer/backend_torrent.h"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace lt = libtorrent;

struct progress_state {
  std::chrono::steady_clock::time_point deadline;
  uint64_t done = 0;
  uint64_t total = 0;
};

static int on_progress(void *opaque, uint64_t done, uint64_t total,
                       uint64_t speed) {
  (void)speed;
  auto *state = static_cast<progress_state *>(opaque);
  state->done = done;
  state->total = total;
  return std::chrono::steady_clock::now() > state->deadline ? -1 : 0;
}

static int write_bytes(std::string const &path, size_t size, char value) {
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return -1;
  for (size_t i = 0; i < size; ++i) {
    if (std::fputc(value, file) == EOF) {
      (void)std::fclose(file);
      return -1;
    }
  }
  return std::fclose(file);
}

int main() {
  char root[] = "/tmp/zftpd-torrent-test-XXXXXX";
  if (mkdtemp(root) == nullptr) return 1;
  std::string const seed_dir = std::string(root) + "/seed";
  std::string const save_dir = std::string(root) + "/download.zftpd.part";
  std::string const bundle_dir = seed_dir + "/bundle";
  std::string const nested_dir = bundle_dir + "/nested";
  if (mkdir(seed_dir.c_str(), 0700) != 0 ||
      mkdir(bundle_dir.c_str(), 0700) != 0 ||
      mkdir(nested_dir.c_str(), 0700) != 0 ||
      mkdir(save_dir.c_str(), 0700) != 0 ||
      write_bytes(bundle_dir + "/payload.bin", 65536, 'A') != 0 ||
      write_bytes(nested_dir + "/notes.txt", 1024, 'B') != 0) return 1;

  std::vector<lt::create_file_entry> files;
  files.emplace_back("bundle/payload.bin", 65536);
  files.emplace_back("bundle/nested/notes.txt", 1024);
  lt::create_torrent maker(std::move(files), 0, lt::create_torrent::v1_only);
  lt::set_piece_hashes(maker, seed_dir);
  std::vector<char> const torrent_file = maker.generate_buf();
  lt::error_code ec;
  lt::add_torrent_params params = lt::load_torrent_buffer(
      lt::span<char const>(torrent_file.data(), torrent_file.size()), ec, {});
  if (ec || !params.ti) {
    std::fprintf(stderr, "cannot load generated torrent: %s\n",
                 ec.message().c_str());
    return 1;
  }

  lt::settings_pack settings;
  settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
  settings.set_bool(lt::settings_pack::enable_dht, false);
  lt::session seeder(settings);
  params.save_path = seed_dir;
  params.flags |= lt::torrent_flags::seed_mode;
  lt::torrent_handle handle = seeder.add_torrent(params, ec);
  if (ec || !handle.is_valid()) {
    std::fprintf(stderr, "cannot start local seeder: %s\n",
                 ec.message().c_str());
    return 1;
  }

  lt::add_torrent_params magnet_params;
  magnet_params.info_hashes = params.ti->info_hashes();
  magnet_params.name = "zftpd-local-test";
  std::string magnet = lt::make_magnet_uri(magnet_params);
  magnet += "&x.pe=127.0.0.1:" + std::to_string(seeder.listen_port());
  if (!transfer_torrent_validate(magnet.c_str())) {
    std::fprintf(stderr, "generated magnet rejected: %s\n", magnet.c_str());
    return 1;
  }

  progress_state progress{std::chrono::steady_clock::now() +
                          std::chrono::seconds(30)};
  char error[256] = {0};
  int result = transfer_torrent_run(magnet.c_str(), save_dir.c_str(), nullptr,
                                    on_progress, &progress, error,
                                    sizeof(error));
  if (result != 0) {
    std::fprintf(stderr, "torrent download failed: %s\n", error);
    return 1;
  }
  struct stat first;
  struct stat second;
  if (stat((save_dir + "/bundle/payload.bin").c_str(), &first) != 0 ||
      stat((save_dir + "/bundle/nested/notes.txt").c_str(), &second) != 0 ||
      first.st_size != 65536 || second.st_size != 1024 ||
      progress.done != 66560 || progress.total != 66560) {
    std::fprintf(stderr, "downloaded files or progress differ\n");
    return 1;
  }
  std::puts("test_transfer_torrent: ok");
  return 0;
}
