/*
 * MafiaHub OSS license
 * Copyright (c) 2022, MafiaHub. All rights reserved.
 *
 * This file comes from MafiaHub, hosted at https://github.com/MafiaHub/Framework.
 * See LICENSE file in the source repository for information regarding licensing.
 */

#pragma once

#include "scripting/engine.h"

namespace Framework::Integrations::Scripting {
    class ClientEngine: public Framework::Scripting::Engine {
      public:
        ClientEngine() = default;
        ~ClientEngine() = default;
    };
} // namespace Framework::Integrations::Scripting
