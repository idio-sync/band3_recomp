#include "stfs_device.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/stfs_xbox.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include "stfs_reader.h"

namespace band3::content {

namespace {

using rex::X_STATUS;
using rex::filesystem::Device;
using rex::filesystem::Entry;
using rex::filesystem::File;

class PackageEntry : public Entry {
public:
    // the root when node is null
    PackageEntry(Device* device, Entry* parent, std::string_view path, StfsReader* reader,
                 const StfsReader::Node* node)
        : Entry(device, parent, path), reader_(reader), node_(node) {
        // as the SDK's device has them: never less than a block, even for a
        // folder or a file whose entry says none
        if (!node || node->directory) {
            attributes_ = rex::filesystem::kFileAttributeDirectory;
            size_ = 0;
        } else {
            attributes_ = rex::filesystem::kFileAttributeNormal | rex::filesystem::kFileAttributeReadOnly;
            size_ = node->length;
        }
        allocation_size_ = size_t(std::max<uint32_t>(node ? node->allocated_blocks : 0, 1)) * StfsReader::kBlockSize;
        if (node) {
            create_timestamp_ = rex::filesystem::decode_fat_timestamp(node->create_date, node->create_time);
            write_timestamp_ =
                rex::filesystem::decode_fat_timestamp(node->modified_date, node->modified_time);
            access_timestamp_ = write_timestamp_;
        } else {
            create_timestamp_ = access_timestamp_ = write_timestamp_ = 0;
        }
    }

    X_STATUS Open(uint32_t desired_access, File** out_file) override;

    void Add(std::unique_ptr<Entry> child) { children_.push_back(std::move(child)); }

    // the file's blocks, found the first time they're asked for
    const StfsReader::Blocks* Blocks() {
        std::lock_guard lock(blocks_mutex_);
        if (!blocks_) {
            StfsReader::Blocks found;
            if (!node_ || !reader_->FileBlocks(*node_, &found)) return nullptr;
            blocks_ = std::move(found);
        }
        return &*blocks_;
    }

    StfsReader* reader() const { return reader_; }

private:
    StfsReader* reader_;
    const StfsReader::Node* node_;
    std::mutex blocks_mutex_;
    std::optional<StfsReader::Blocks> blocks_;
};

class PackageFile : public File {
public:
    PackageFile(uint32_t file_access, PackageEntry* entry) : File(file_access, entry), package_(entry) {}

    void Destroy() override { delete this; }

    X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override {
        *out_bytes_read = 0;
        if (byte_offset >= package_->size()) return X_STATUS_END_OF_FILE;
        const StfsReader::Blocks* blocks = package_->Blocks();
        if (!blocks) return X_STATUS_UNSUCCESSFUL;
        if (!package_->reader()->Read(*blocks, package_->size(), byte_offset, buffer, out_bytes_read)) {
            return X_STATUS_UNSUCCESSFUL;
        }
        return X_STATUS_SUCCESS;
    }
    X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t*) override { return X_STATUS_ACCESS_DENIED; }
    X_STATUS SetLength(size_t) override { return X_STATUS_ACCESS_DENIED; }

private:
    PackageEntry* package_;
};

X_STATUS PackageEntry::Open(uint32_t desired_access, File** out_file) {
    *out_file = new PackageFile(desired_access, this);
    return X_STATUS_SUCCESS;
}

class PackageDevice : public Device {
public:
    PackageDevice(std::string_view mount_path, std::unique_ptr<StfsReader> reader)
        : Device(mount_path), reader_(std::move(reader)) {}

    bool Initialize() override {
        auto root = std::make_unique<PackageEntry>(this, nullptr, "", reader_.get(), nullptr);
        // a folder comes before what's in it in the table, which names it by its place
        std::unordered_map<uint32_t, PackageEntry*> folders;
        for (const StfsReader::Node& node : reader_->nodes()) {
            PackageEntry* parent = root.get();
            if (node.parent != StfsReader::kNoParent) {
                const auto it = folders.find(node.parent);
                if (it == folders.end()) continue;  // in no folder the table has
                parent = it->second;
            }
            const std::string path = parent->path().empty() ? node.name : parent->path() + "\\" + node.name;
            auto entry = std::make_unique<PackageEntry>(this, parent, path, reader_.get(), &node);
            if (node.directory) folders[node.index] = entry.get();
            parent->Add(std::move(entry));
        }
        root_ = std::move(root);
        return true;
    }

    void Dump(rex::string::StringBuffer* string_buffer) override { root_->Dump(string_buffer, 0); }
    Entry* ResolvePath(const std::string_view path) override { return root_->ResolvePath(path); }

    const std::string& name() const override { return name_; }
    uint32_t attributes() const override { return 0; }
    uint32_t component_name_max_length() const override { return 40; }
    uint32_t total_allocation_units() const override { return reader_->total_blocks(); }
    uint32_t available_allocation_units() const override { return 0; }
    uint32_t sectors_per_allocation_unit() const override { return 8; }
    uint32_t bytes_per_sector() const override { return 0x200; }

private:
    std::string name_ = "STFS";
    std::unique_ptr<StfsReader> reader_;
    std::unique_ptr<PackageEntry> root_;
};

}

std::unique_ptr<Device> OpenPackageDevice(std::string_view mount_path, const std::filesystem::path& path) {
    std::string why;
    if (auto reader = StfsReader::Open(path, &why)) {
        auto device = std::make_unique<PackageDevice>(mount_path, std::move(reader));
        if (device->Initialize()) return device;
    }
    REXLOG_DEBUG("content: {}: {}, so the SDK reads it", rex::path_to_utf8(path), why);
    auto device = std::make_unique<rex::filesystem::StfsContainerDevice>(mount_path, path);
    if (!device->Initialize()) return nullptr;
    return device;
}

}
