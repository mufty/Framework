/*
 * MafiaHub OSS license
 * Copyright (c) 2022, MafiaHub. All rights reserved.
 *
 * This file comes from MafiaHub, hosted at https://github.com/MafiaHub/Framework.
 * See LICENSE file in the source repository for information regarding licensing.
 */

#pragma once

#include "engine.h"
#include "errors.h"

#include <flecs/flecs.h>

#include <functional>

namespace Framework::World {
    class ClientEngine: public Engine {
      public:
        using OnEntityDestroyCallback = std::function<bool(flecs::entity)>;
      protected:
        flecs::entity _streamEntities;
        flecs::query<Modules::Base::ServerID> _queryGetEntityByServerID;
        OnEntityDestroyCallback _onEntityDestroyCallback;
      public:
        EngineError Init();

        EngineError Shutdown() override;

        void OnConnect(Networking::NetworkPeer *peer, float tickInterval);
        void OnDisconnect();

        void Update() override;

        flecs::entity CreateEntity(flecs::entity_t serverID);
        flecs::entity GetEntityByServerID(flecs::entity_t id) const;
        flecs::entity_t GetServerID(flecs::entity entity) const;

        void SetOnEntityDestroyCallback(OnEntityDestroyCallback cb) {
            _onEntityDestroyCallback = cb;
        }
    };
} // namespace Framework::World
