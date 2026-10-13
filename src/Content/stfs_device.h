// A package file mounted as a VFS device. STFS packages are read through
// band3's StfsReader (stfs_reader.h), which opens a big pack on a network
// share in a few reads where the SDK's StfsContainerDevice makes thousands;
// anything else (SVOD) goes to the SDK's device.
#pragma once
#include <filesystem>
#include <memory>
#include <string_view>
#include <rex/filesystem/device.h>

namespace band3::content {

// the package at path as a device at mount_path, initialized; nullptr if it
// can't be read
std::unique_ptr<rex::filesystem::Device> OpenPackageDevice(std::string_view mount_path,
                                                           const std::filesystem::path& path);

}
