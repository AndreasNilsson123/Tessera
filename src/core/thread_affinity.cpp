// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "thread_affinity.hpp"

namespace tessera {
namespace {
struct ThreadHolder {
    std::shared_ptr<ThreadToken> token = std::make_shared<ThreadToken>();
    ~ThreadHolder() {
        token->alive.store(false);
    }
};
} // namespace

std::shared_ptr<ThreadToken> currentThreadToken() {
    thread_local ThreadHolder holder;
    return holder.token;
}

} // namespace tessera
