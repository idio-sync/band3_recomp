#include "game_writes.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <system_error>
#include <vector>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include "src/config.h"

namespace band3 {

namespace {

namespace fs = rex::filesystem;
using rex::X_STATUS;

// where game:\ and d:\ point once mounted; the SDK's own mount of the game data
// (\Device\Harddisk0\Partition1) stays registered for anything that names it
constexpr const char* kMountPath = "\\Device\\Band3Game";

// what opening a directory gives: the game enumerates its entry's children
class DirFile : public fs::File {
public:
    using File::File;
    void Destroy() override { delete this; }
    X_STATUS ReadSync(std::span<uint8_t>, size_t, size_t*) override {
        return X_STATUS_FILE_IS_A_DIRECTORY;
    }
    X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t*) override {
        return X_STATUS_FILE_IS_A_DIRECTORY;
    }
};

class GameDirEntry;

// what opening for any of these changes a file
constexpr uint32_t kWriteAccess = fs::FileAccess::kGenericWrite | fs::FileAccess::kGenericAll |
                                  fs::FileAccess::kFileWriteData |
                                  fs::FileAccess::kFileAppendData;

// A file of the game data, as game:\ shows it. The game data's own entry, on a
// read-only device, does the reading. The first time the game opens it to
// write, truncates it or maps it writable, it's copied to the writes folder
// (GameDirEntry::CopyUp), the copy takes its place in its directory, and this
// passes everything on to the copy. It belongs to game:\'s writable device
// itself: the SDK refuses a write to an entry whose device is read-only before
// the entry is asked.
class GameDataFileEntry : public fs::Entry {
public:
    GameDataFileEntry(fs::Device* device, GameDirEntry* parent, std::string_view path,
                      fs::HostPathEntry* data);

    const std::filesystem::path& data_path() const { return data_->host_path(); }

    X_STATUS Open(uint32_t desired_access, fs::File** out_file) override;
    bool Truncate() override;
    bool can_map() const override { return true; }
    std::unique_ptr<rex::memory::MappedMemory> OpenMapped(rex::memory::MappedMemory::Mode mode,
                                                          size_t offset, size_t length) override;
    void update() override;
    bool SetAttributes(uint64_t attributes) override {
        return copy_ && copy_->SetAttributes(attributes);
    }
    bool SetCreateTimestamp(uint64_t timestamp) override {
        return copy_ && copy_->SetCreateTimestamp(timestamp);
    }
    bool SetAccessTimestamp(uint64_t timestamp) override {
        return copy_ && copy_->SetAccessTimestamp(timestamp);
    }
    bool SetWriteTimestamp(uint64_t timestamp) override {
        return copy_ && copy_->SetWriteTimestamp(timestamp);
    }

private:
    friend class GameDirEntry;

    // the copy, made if it isn't yet; null if it can't be
    fs::Entry* Copy(bool keep_contents);
    // size and times as the file being read has them
    void TakeInfo(const fs::Entry& from);

    std::unique_ptr<fs::HostPathEntry> data_;
    // the copy in the writes folder, once there is one
    fs::Entry* copy_ = nullptr;
};

// A directory of game:\, made of the writes folder's directory at the same path
// (which needn't exist until something is written in it) and the game data's,
// if it has one. Its children are the writes folder's entries and, where the
// writes folder has none of the same name, the game data's, which are copied to
// the writes folder when the game first changes them (GameDataFileEntry).
//
// Not a HostPathEntry: the VFS drops a HostPathEntry's child when the file
// isn't in that entry's host directory, which the game data's files aren't.
class GameDirEntry : public fs::Entry {
public:
    GameDirEntry(fs::Device* device, fs::Entry* parent, std::string_view path,
                 const std::filesystem::path& writes_path, const std::filesystem::path& data_path,
                 fs::HostPathDevice* data_device)
        : Entry(device, parent, path),
          writes_path_(writes_path),
          data_path_(data_path),
          data_device_(data_device) {
        attributes_ = fs::kFileAttributeDirectory;
        fs::FileInfo info;
        if (fs::GetInfo(data_path_.empty() ? writes_path_ : data_path_, &info)) {
            create_timestamp_ = info.create_timestamp;
            access_timestamp_ = info.access_timestamp;
            write_timestamp_ = info.write_timestamp;
        }
    }

    void Populate() {
        std::error_code ec;
        std::vector<fs::FileInfo> data_infos;
        if (!data_path_.empty()) data_infos = fs::ListFiles(data_path_);

        if (std::filesystem::is_directory(writes_path_, ec)) {
            for (const auto& info : fs::ListFiles(writes_path_)) {
                const auto name = rex::path_to_utf8(info.name);
                const auto full_path = writes_path_ / info.name;
                if (info.type != fs::FileInfo::Type::kDirectory) {
                    children_.emplace_back(fs::HostPathEntry::Create(device_, this, full_path, info));
                    continue;
                }
                // the game data's directory of the same name, under the writes one
                std::filesystem::path data_dir;
                for (const auto& data_info : data_infos) {
                    if (data_info.type == fs::FileInfo::Type::kDirectory &&
                        rex::string::utf8_equal_case(rex::path_to_utf8(data_info.name), name)) {
                        data_dir = data_path_ / data_info.name;
                        break;
                    }
                }
                AddDirectory(name, full_path, data_dir);
            }
        }

        for (const auto& info : data_infos) {
            const auto name = rex::path_to_utf8(info.name);
            if (GetChild(name)) continue;
            const auto full_path = data_path_ / info.name;
            if (info.type == fs::FileInfo::Type::kDirectory) {
                AddDirectory(name, writes_path_ / info.name, full_path);
            } else {
                children_.push_back(std::make_unique<GameDataFileEntry>(
                    device_, this, rex::string::utf8_join_guest_paths(path(), name),
                    fs::HostPathEntry::Create(data_device_, this, full_path, info)));
            }
        }
    }

    X_STATUS Open(uint32_t desired_access, fs::File** out_file) override {
        *out_file = new DirFile(desired_access, this);
        return X_STATUS_SUCCESS;
    }

    // Copies `file` (one of this directory's) to the writes folder, with its
    // contents or empty, and puts the copy in its place. `file` lives on until
    // the device goes, as the SDK may still hold it while it opens it. Null if
    // the copy can't be made.
    fs::Entry* CopyUp(GameDataFileEntry* file, bool keep_contents) {
        auto global_lock = global_critical_region_.Acquire();
        auto it = std::find_if(children_.begin(), children_.end(),
                               [file](const auto& child) { return child.get() == file; });
        if (it == children_.end()) return nullptr;

        std::error_code ec;
        std::filesystem::create_directories(writes_path_, ec);
        const auto full_path = writes_path_ / rex::to_path(file->name());
        if (keep_contents) {
            std::filesystem::copy_file(file->data_path(), full_path,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        } else if (FILE* f = fs::OpenFile(full_path, "wb")) {
            fclose(f);
        } else {
            ec = std::make_error_code(std::errc::io_error);
        }
        fs::FileInfo info;
        if (ec || !fs::GetInfo(full_path, &info)) {
            REXLOG_ERROR("game:\\{} stays read-only: can't copy it to {} ({})", file->path(),
                         rex::path_to_utf8(full_path), ec.message());
            return nullptr;
        }

        fs::Entry* copy = fs::HostPathEntry::Create(device_, this, full_path, info);
        retired_.push_back(std::move(*it));
        it->reset(copy);
        REXLOG_INFO("game:\\{}: the game changes it, so it's copied from the game data to {}",
                    file->path(), rex::path_to_utf8(full_path));
        return copy;
    }

private:
    void AddDirectory(const std::string& name, const std::filesystem::path& writes_path,
                      const std::filesystem::path& data_path) {
        auto dir = std::make_unique<GameDirEntry>(
            device_, this, rex::string::utf8_join_guest_paths(path(), name), writes_path,
            data_path, data_device_);
        dir->Populate();
        children_.push_back(std::move(dir));
    }

    // new files and directories go in the writes folder, making this directory
    // there first if it isn't yet
    std::unique_ptr<fs::Entry> CreateEntryInternal(std::string_view name,
                                                   uint32_t attributes) override {
        std::error_code ec;
        std::filesystem::create_directories(writes_path_, ec);
        const auto full_path = writes_path_ / rex::to_path(name);
        if (attributes & fs::kFileAttributeDirectory) {
            std::filesystem::create_directory(full_path, ec);
            if (!std::filesystem::is_directory(full_path, ec)) return nullptr;
            return std::make_unique<GameDirEntry>(
                device_, this, rex::string::utf8_join_guest_paths(path(), name), full_path,
                std::filesystem::path(), data_device_);
        }
        auto file = fs::OpenFile(full_path, "wb");
        if (!file) return nullptr;
        fclose(file);
        fs::FileInfo info;
        if (!fs::GetInfo(full_path, &info)) return nullptr;
        return std::unique_ptr<fs::Entry>(fs::HostPathEntry::Create(device_, this, full_path, info));
    }

    // only what the writes folder holds alone can go: never the game data's
    // files, nor a directory that has some of them
    bool DeleteEntryInternal(fs::Entry* entry) override {
        if (entry->device() != device_) return false;
        // a game data file the game deletes (as when it replaces one) is gone
        // from game:\ until the next launch, and the game data keeps it; a file
        // of the same name the game then makes goes in the writes folder. Kept
        // alive, as the SDK may still hold it.
        if (dynamic_cast<GameDataFileEntry*>(entry)) {
            auto it = std::find_if(children_.begin(), children_.end(),
                                   [entry](const auto& child) { return child.get() == entry; });
            if (it == children_.end()) return false;
            retired_.push_back(std::move(*it));
            children_.erase(it);
            return true;
        }
        std::filesystem::path full_path;
        if (auto* dir = dynamic_cast<GameDirEntry*>(entry)) {
            if (!dir->data_path_.empty()) return false;
            full_path = dir->writes_path_;
        } else {
            full_path = static_cast<fs::HostPathEntry*>(entry)->host_path();
        }
        std::error_code ec;
        std::filesystem::remove_all(full_path, ec);
        return !ec && !std::filesystem::exists(full_path, ec);
    }

    std::filesystem::path writes_path_;
    std::filesystem::path data_path_;  // empty when the game data has no such directory
    fs::HostPathDevice* data_device_;
    // game data files copied up or deleted, which the SDK may still hold
    std::vector<std::unique_ptr<fs::Entry>> retired_;
};

GameDataFileEntry::GameDataFileEntry(fs::Device* device, GameDirEntry* parent,
                                     std::string_view path, fs::HostPathEntry* data)
    : Entry(device, parent, path), data_(data) {
    TakeInfo(*data_);
    // writable, as its copy will be
    attributes_ = data_->attributes() & ~fs::kFileAttributeReadOnly;
}

void GameDataFileEntry::TakeInfo(const fs::Entry& from) {
    size_ = from.size();
    allocation_size_ = from.allocation_size();
    create_timestamp_ = from.create_timestamp();
    access_timestamp_ = from.access_timestamp();
    write_timestamp_ = from.write_timestamp();
}

fs::Entry* GameDataFileEntry::Copy(bool keep_contents) {
    if (!copy_) copy_ = static_cast<GameDirEntry*>(parent_)->CopyUp(this, keep_contents);
    return copy_;
}

X_STATUS GameDataFileEntry::Open(uint32_t desired_access, fs::File** out_file) {
    if (!copy_ && !(desired_access & kWriteAccess)) return data_->Open(desired_access, out_file);
    fs::Entry* copy = Copy(true);
    return copy ? copy->Open(desired_access, out_file) : X_STATUS_ACCESS_DENIED;
}

bool GameDataFileEntry::Truncate() {
    fs::Entry* copy = Copy(false);
    return copy && copy->Truncate();
}

std::unique_ptr<rex::memory::MappedMemory> GameDataFileEntry::OpenMapped(
    rex::memory::MappedMemory::Mode mode, size_t offset, size_t length) {
    if (!copy_ && mode == rex::memory::MappedMemory::Mode::kRead) {
        return data_->OpenMapped(mode, offset, length);
    }
    fs::Entry* copy = Copy(true);
    return copy ? copy->OpenMapped(mode, offset, length) : nullptr;
}

void GameDataFileEntry::update() {
    fs::Entry& from = copy_ ? *copy_ : static_cast<fs::Entry&>(*data_);
    from.update();
    TakeInfo(from);
}

// The writes folder is this device's host path, so files the game creates open,
// truncate and rename within it as the SDK's own do. The game data has a
// read-only device of its own that owns its files' entries; the VFS never sees it.
class GameDevice : public fs::HostPathDevice {
public:
    GameDevice(const std::filesystem::path& game_data, const std::filesystem::path& writes)
        : HostPathDevice(kMountPath, writes, false, true),
          data_device_(kMountPath, game_data, true) {}

    bool Initialize() override {
        std::error_code ec;
        std::filesystem::create_directories(host_path(), ec);
        if (!std::filesystem::is_directory(host_path(), ec)) return false;
        root_ = std::make_unique<GameDirEntry>(this, nullptr, "", host_path(),
                                               data_device_.host_path(), &data_device_);
        root_->Populate();
        return true;
    }

    // without its "." folders: RB3 makes its file root "." a path as "game:.\",
    // as when Rock Band 3 Deluxe's file_list looks for its saved themes
    fs::Entry* ResolvePath(std::string_view path) override {
        std::string resolved;
        size_t start = 0;
        while (start <= path.size()) {
            const size_t end = std::min(path.find_first_of("\\/", start), path.size());
            const std::string_view part = path.substr(start, end - start);
            if (!part.empty() && part != ".") {
                if (!resolved.empty()) resolved += '\\';
                resolved += part;
            }
            start = end + 1;
        }
        return root_->ResolvePath(resolved);
    }

    void Dump(rex::string::StringBuffer* string_buffer) override {
        auto global_lock = global_critical_region_.Acquire();
        root_->Dump(string_buffer, 0);
    }

private:
    // before root_, which holds entries of it
    fs::HostPathDevice data_device_;
    std::unique_ptr<GameDirEntry> root_;
};

// game:\'s device once MountGameWrites has mounted it, before the game starts
GameDevice* g_game_device = nullptr;

}

bool GameFileExists(std::string_view path) {
    std::string native(path);
    std::replace(native.begin(), native.end(), '/', '\\');
    if (g_game_device) {
        const fs::Entry* entry = g_game_device->ResolvePath(native);
        return entry && !(entry->attributes() & fs::kFileAttributeDirectory);
    }
    const std::filesystem::path& root = GameDataRoot();
    if (root.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(root / rex::to_path(native), ec);
}

std::filesystem::path GameWritesFolder(const std::filesystem::path& user_data_root) {
    return user_data_root / "game";
}

bool MountGameWrites(rex::Runtime& runtime) {
    auto* vfs = runtime.file_system();
    if (!vfs || runtime.game_data_root().empty()) return false;
    return MountGameWrites(*vfs, runtime.game_data_root(),
                           GameWritesFolder(runtime.user_data_root()));
}

bool MountGameWrites(rex::filesystem::VirtualFileSystem& vfs,
                     const std::filesystem::path& game_data_root,
                     const std::filesystem::path& writes_folder) {
    const auto game_data = std::filesystem::absolute(game_data_root);
    const auto writes = std::filesystem::absolute(writes_folder);

    auto device = std::make_unique<GameDevice>(game_data, writes);
    if (!device->Initialize()) {
        REXLOG_ERROR("game:\\ stays read-only: can't create {}", rex::path_to_utf8(writes));
        return false;
    }
    g_game_device = device.get();
    vfs.RegisterDevice(std::move(device));
    for (const char* link : {"game:", "d:"}) {
        vfs.UnregisterSymbolicLink(link);
        vfs.RegisterSymbolicLink(link, kMountPath);
    }
    REXLOG_INFO("game:\\ writes go to {}", rex::path_to_utf8(writes));
    return true;
}

}
