// band3 takes over RB3's content calls (see content_hooks.cpp)
#pragma once

namespace band3::content {

// looks the SDK's content exports up once; false (after logging which) if one is missing
bool ResolveSdkContentExports();

}  // namespace band3::content
