// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmresponselimits.h"

bool QLLMResponseLimits::valid() const
{
    return maxBytes > 0 && maxBytes <= 512 * 1024 * 1024 && maxEventBytes > 0
           && maxEventBytes <= maxBytes && maxArgumentBytes > 0 && maxArgumentBytes <= maxBytes
           && maxToolCalls > 0 && maxToolCalls <= 65536 && maxJsonDepth > 0 && maxJsonDepth <= 256;
}

bool qllmJsonDepthAllowed(std::string_view source, int limit)
{
    int  depth   = 0;
    bool quoted  = false;
    bool escaped = false;
    for (const char byte : source) {
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                quoted = false;
            }
        } else if (byte == '"') {
            quoted = true;
        } else if (byte == '{' || byte == '[') {
            if (++depth > limit) {
                return false;
            }
        } else if (byte == '}' || byte == ']') {
            --depth;
        }
    }
    return true;
}
