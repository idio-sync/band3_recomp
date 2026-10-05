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

// A directory of game:\, made of the writes folder's directory at the same path
// (which needn't exist until something is written in it) and the game data's,
// if it has one. Its children are the writes folder's entries and, where the
// writes folder has none of the same name, the game data's. Those belong to a
// read-only device, so the game can read them but not change them.
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
                children_.emplace_back(fs::HostPathEntry::Create(data_device_, this, full_path, info));
            }
        }
    }

    X_STATUS Open(uint32_t desired_access, fs::File** out_file) override {
        *out_file = new DirFile(desired_access, this);
        return X_STATUS_SUCCESS;
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
};

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

    fs::Entry* ResolvePath(std::string_view path) override { return root_->ResolvePath(path); }

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
    const auto game_data = std::filesystem::absolute(runtime.game_data_root());
    const auto writes = std::filesystem::absolute(GameWritesFolder(runtime.user_data_root()));

    auto device = std::make_unique<GameDevice>(game_data, writes);
    if (!device->Initialize()) {
        REXLOG_ERROR("game:\\ stays read-only: can't create {}", rex::path_to_utf8(writes));
        return false;
    }
    g_game_device = device.get();
    vfs->RegisterDevice(std::move(device));
    for (const char* link : {"game:", "d:"}) {
        vfs->UnregisterSymbolicLink(link);
        vfs->RegisterSymbolicLink(link, kMountPath);
    }
    REXLOG_INFO("game:\\ writes go to {}", rex::path_to_utf8(writes));
    return true;
}

}
