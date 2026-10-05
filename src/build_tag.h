#pragma once

// Which band3 this is: `git describe` of the checkout it was built from (a
// tag or commit, "-dirty" with uncommitted changes), BAND3_BUILD_TAG when the
// build set one, "unknown" when neither was there. Written each build by
// cmake/build_tag.cmake; only build_tag.cpp sees the generated header, so a
// new tag recompiles that one file.

namespace band3 {

const char* BuildTag();

}
