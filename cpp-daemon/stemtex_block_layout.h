#pragma once

#include <string>

namespace stemtex {
// Render-time source preparation. No changes to the worker template or profile.
std::string prepare_block_source(const std::string &source);
}
