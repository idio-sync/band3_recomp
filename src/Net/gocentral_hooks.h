#pragma once

namespace band3::gocentral {

// Finds the SDK's XAM user exports that band3's sign-in overrides hand on to;
// false if any is missing. Call before the game runs.
bool ResolveSdkExports();

// With the gocentral setting on, sends RB3's Rock Central connections to
// gocentral_address, as RB3Enhanced does. Call before the game runs.
void Start();

}
