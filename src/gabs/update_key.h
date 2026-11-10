/**
 * @file src/gabs/update_key.h
 * @brief Gabs Remote: public Ed25519 key that signs the Release manifest (decision 0006).
 *
 * The private key stays on Gab's PC (docs/segredos.md). Base64 of the 32 raw bytes.
 */
#pragma once

// standard includes
#include <string_view>

namespace gabs::update {

  inline constexpr std::string_view kPublicKeyBase64 = "vt/Fsd5CTf97gakAjSs4U3ugaDp5pM2hBkNKlA+RibA=";

}  // namespace gabs::update
