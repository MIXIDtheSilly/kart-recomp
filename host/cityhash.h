// SPDX-FileCopyrightText: 2011 Google, Inc.
// SPDX-FileContributor: Geoff Pike
// SPDX-FileContributor: Jyrki Alakuijala
// SPDX-License-Identifier: MIT

// CityHash64, by Geoff Pike and Jyrki Alakuijala. The renderer's cache keys (pipelines, textures) are made with it:
// the values must stay the same as before, or the caches on disk no longer match.
#pragma once

#include <cstddef>

#include "common.h"

// Hash function for a byte array.
[[nodiscard]] u64 CityHash64(const char* buf, size_t len);

// Hash function for a byte array.  For convenience, a 64-bit seed is also
// hashed into the result.
[[nodiscard]] u64 CityHash64WithSeed(const char* buf, size_t len, u64 seed);

// Hash function for a byte array.  For convenience, two seeds are also
// hashed into the result.
[[nodiscard]] u64 CityHash64WithSeeds(const char* buf, size_t len, u64 seed0, u64 seed1);
