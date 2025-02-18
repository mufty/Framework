/*
 * MafiaHub OSS license
 * Copyright (c) 2022, MafiaHub. All rights reserved.
 *
 * This file comes from MafiaHub, hosted at https://github.com/MafiaHub/Framework.
 * See LICENSE file in the source repository for information regarding licensing.
 */

#include "instance.h"

#include "../shared/modules/mod.hpp"
#include "../shared/types/environment.hpp"
#include "../shared/types/player.hpp"
#include "integrations/shared/messages/weather_update.h"
#include "world/server.h"

#include "networking/messages/client_connection_finalized.h"
#include "networking/messages/client_handshake.h"
#include "networking/messages/client_kick.h"
#include "networking/messages/messages.h"

#include "scripting/builtins/entity.h"

#include "scripting/builtins/player.h"

#include "utils/version.h"

#include "cxxopts.hpp"
#include "nlohmann/json.hpp"
#include "optick.h"

namespace Framework::Integrations::Server {
    Instance::Instance(): _alive(false) {
        OPTICK_START_CAPTURE();
        _networkingEngine = std::make_shared<Networking::Engine>();
        _webServer        = std::make_shared<HTTP::Webserver>();
        _masterlistSync   = std::make_unique<Masterlist>(this);
        _fileConfig       = std::make_unique<Utils::Config>();
        _firebaseWrapper  = std::make_unique<External::Firebase::Wrapper>();
        _worldEngine      = std::make_shared<World::ServerEngine>();
        _scriptingEngine  = std::make_shared<Scripting::ServerEngine>(_worldEngine);
        _playerFactory    = std::make_shared<Integrations::Shared::Archetypes::PlayerFactory>();
        _streamingFactory = std::make_shared<Integrations::Shared::Archetypes::StreamingFactory>();
    }

    Instance::~Instance() {
        sig_detach(this);
        OPTICK_STOP_CAPTURE();
    }

    ServerError Instance::Init(InstanceOptions &opts) {
        _opts = opts;

        // First level is argument parser, because we might want to overwrite stuffs
        cxxopts::Options options(_opts.modSlug, _opts.modHelpText);
        options.add_options("", {{"p,port", "Networking port to bind", cxxopts::value<int32_t>()->default_value(std::to_string(_opts.bindPort))}, {"h,host", "Networking host to bind", cxxopts::value<std::string>()->default_value(_opts.bindHost)},
                                    {"c,config", "JSON config file to read", cxxopts::value<std::string>()->default_value(_opts.modConfigFile)}});

        // Try to parse and return if anything wrong happened
        auto result = options.parse(_opts.argc, _opts.argv);

        // Allow mod to specify custom JSON config file name
        _opts.modConfigFile = result["config"].as<std::string>();

        // Load JSON config if present
        if (!LoadConfigFromJSON()) {
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->critical("Failed to parse JSON config file");
            return ServerError::SERVER_CONFIG_PARSE_ERROR;
        }

        // Finally apply back to the structure that is used everywhere the settings from the parser
        _opts.bindHost = result["host"].as<std::string>();
        _opts.bindPort = result["port"].as<int32_t>();

        // Initialize the logging instance with the mod slug name
        Logging::GetInstance()->SetLogName(_opts.modSlug);

        // Initialize the web server
        if (!_webServer->Init(_opts.bindHost, _opts.bindPort, _opts.httpServeDir)) {
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->critical("Failed to initialize the webserver engine");
            return ServerError::SERVER_WEBSERVER_INIT_FAILED;
        }

        // Initialize our networking engine
        if (!_networkingEngine->Init(_opts.bindPort, _opts.bindHost, _opts.maxPlayers, _opts.bindPassword)) {
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->critical("Failed to initialize the networking engine");
            return ServerError::SERVER_NETWORKING_INIT_FAILED;
        }

        // Initialize the world
        if (_worldEngine->Init(_networkingEngine->GetNetworkServer(), _opts.streamerTickInterval) != World::EngineError::ENGINE_NONE) {
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->critical("Failed to initialize the world engine");
            return ServerError::SERVER_WORLD_INIT_FAILED;
        }

        const auto sdkCallback = [this](Framework::Scripting::SDK *sdk) {
            this->RegisterScriptingBuiltins(sdk);
        };

        // Initialize the scripting engine
        if (_scriptingEngine->Init(sdkCallback) != Framework::Scripting::EngineError::ENGINE_NONE) {
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->critical("Failed to initialize the scripting engine");
            return ServerError::SERVER_SCRIPTING_INIT_FAILED;
        }

        if (_opts.firebaseEnabled && _firebaseWrapper->Init(_opts.firebaseProjectId, _opts.firebaseAppId, _opts.firebaseApiKey) != External::Firebase::FirebaseError::FIREBASE_NONE) {
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->critical("Failed to initialize the firebase wrapper");
            return ServerError::SERVER_FIREBASE_WRAPPER_INIT_FAILED;
        }

        // Register the default endpoints
        InitEndpoints();

        // Register built in modules
        InitModules();

        // Initialize default messages
        InitNetworkingMessages();

        // Initialize mod subsystems
        PostInit();

        // Init the signals handlers if enabled
        if (_opts.enableSignals) {
            sig_attach(SIGINT, sig_slot(this, &Instance::OnSignal), sig_ctx_sys());
            sig_attach(SIGTERM, sig_slot(this, &Instance::OnSignal), sig_ctx_sys());
        }

        _alive = true;
        Logging::GetLogger(FRAMEWORK_INNER_SERVER)->info("{} Server successfully started", _opts.modName);
        Logging::GetLogger(FRAMEWORK_INNER_SERVER)->info("Name:\t{}", _opts.bindName);
        Logging::GetLogger(FRAMEWORK_INNER_SERVER)->info("Host:\t{}", _opts.bindHost);
        Logging::GetLogger(FRAMEWORK_INNER_SERVER)->info("Port:\t{}", _opts.bindPort);
        Logging::GetLogger(FRAMEWORK_INNER_SERVER)->info("Max Players:\t{}", _opts.maxPlayers);
        return ServerError::SERVER_NONE;
    }

    void Instance::InitEndpoints() {
        _webServer->RegisterRequest("/networking/status", [this](struct mg_connection *c, void *ev_data, Framework::HTTP::ResponseCallback cb) {
            nlohmann::json root;
            root["mod_name"]          = _opts.modName;
            root["mod_slug"]          = _opts.modSlug;
            root["mod_version"]       = _opts.modVersion;
            root["host"]              = _opts.bindHost;
            root["port"]              = _opts.bindPort;
            root["password_required"] = !_opts.bindPassword.empty();
            root["max_players"]       = _opts.maxPlayers;
            cb(200, root.dump(4));
        });
    }

    void Instance::InitModules() {
        if (_worldEngine) {
            auto world = _worldEngine->GetWorld();

            world->import<Integrations::Shared::Modules::Mod>();
        }
    }

    bool Instance::LoadConfigFromJSON() {
        auto configHandle = cppfs::fs::open(_opts.modConfigFile);

        if (!configHandle.exists()) {
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->info("JSON config file is not present, skipping load...");
            return true;
        }

        auto configData = configHandle.readFile();

        try {
            // Parse our config data first
            _fileConfig->Parse(configData);

            if (!_fileConfig->IsParsed()) {
                Logging::GetLogger(FRAMEWORK_INNER_SERVER)->critical("JSON config load has failed: {}", _fileConfig->GetLastError());
                return false;
            }

            // Retrieve fields and overwrite InstanceOptions defaults
            _opts.bindHost      = _fileConfig->Get<std::string>("host");
            _opts.bindName      = _fileConfig->Get<std::string>("name");
            _opts.bindPort      = _fileConfig->Get<int>("port");
            _opts.bindMapName   = _fileConfig->Get<std::string>("map");
            _opts.maxPlayers    = _fileConfig->Get<int>("maxplayers");
            _opts.bindSecretKey = _fileConfig->Get<std::string>("server-token");
        }
        catch (const std::exception &ex) {
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->critical("JSON config has missing fields: {}", ex.what());
            return false;
        }
        return true;
    }

    void Instance::InitNetworkingMessages() {
        using namespace Framework::Networking::Messages;
        const auto net = _networkingEngine->GetNetworkServer();
        net->RegisterMessage<ClientHandshake>(Framework::Networking::Messages::GameMessages::GAME_CONNECTION_HANDSHAKE, [this, net](SLNet::RakNetGUID guid, ClientHandshake *msg) {
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->debug("Received handshake message for player {}", msg->GetPlayerName());

            // Make sure handshake payload was correctly formatted
            if (!msg->Valid()) {
                Logging::GetLogger(FRAMEWORK_INNER_SERVER)->error("Handshake payload was invalid, force-disconnecting peer");
                net->GetPeer()->CloseConnection(guid, true);
                return;
            }

            const auto clientVersion = msg->GetClientVersion();

            if (!Utils::Version::VersionSatisfies(clientVersion.c_str(), Utils::Version::rel)) {
                Logging::GetLogger(FRAMEWORK_INNER_SERVER)->error("Client has invalid version, force-disconnecting peer");
                Framework::Networking::Messages::ClientKick kick;
                kick.FromParameters(Framework::Networking::Messages::DisconnectionReason::WRONG_VERSION);
                net->Send(kick, guid);
                net->GetPeer()->CloseConnection(guid, true);
                return;
            }

            // Create player entity and add on world
            const auto newPlayer = _worldEngine->CreateEntity();
            _streamingFactory->SetupServer(newPlayer, guid.g);
            _playerFactory->SetupServer(newPlayer, guid.g);

            if (_onPlayerConnectedCallback)
                _onPlayerConnectedCallback(newPlayer, guid.g);

            // Send the connection finalized packet
            Framework::Networking::Messages::ClientConnectionFinalized answer;
            answer.FromParameters(_opts.tickInterval, newPlayer.id());
            net->Send(answer, guid);
        });

        net->SetOnPlayerDisconnectCallback([this, net](SLNet::Packet *packet, uint32_t reason) {
            const auto guid = packet->guid;
            Logging::GetLogger(FRAMEWORK_INNER_SERVER)->debug("Disconnecting peer {}, reason: {}", guid.g, reason);

            auto e = _worldEngine->GetEntityByGUID(guid.g);

            if (e.is_valid()) {
                if (_onPlayerDisconnectedCallback)
                    _onPlayerDisconnectedCallback(e, guid.g);

                _worldEngine->RemoveEntity(e);
            }

            net->GetPeer()->CloseConnection(guid, true);
        });

        // default entity events
        net->RegisterMessage<GameSyncEntityUpdate>(GameMessages::GAME_SYNC_ENTITY_UPDATE, [this](SLNet::RakNetGUID guid, GameSyncEntityUpdate *msg) {
            if (!msg->Valid()) {
                return;
            }

            const auto e = _worldEngine->WrapEntity(msg->GetServerID());

            if (!e.is_alive()) {
                return;
            }

            if (!GetWorldEngine()->IsEntityOwner(e, guid.g)) {
                return;
            }

            auto tr = e.get_mut<World::Modules::Base::Transform>();
            *tr     = msg->GetTransform();
        });
    }

    ServerError Instance::Shutdown() {
        if (_networkingEngine) {
            _networkingEngine->Shutdown();
        }

        if (_scriptingEngine) {
            _scriptingEngine->Shutdown();
        }

        if (_webServer) {
            _webServer->Shutdown();
        }

        if (_worldEngine) {
            _worldEngine->Shutdown();
        }

        // Detach signal handlers
        sig_detach(SIGINT, sig_slot(this, &Instance::OnSignal));
        sig_detach(SIGTERM, sig_slot(this, &Instance::OnSignal));

        _alive = false;
        return ServerError::SERVER_NONE;
    }

    void Instance::Update() {
        const auto start = std::chrono::high_resolution_clock::now();
        if (_nextTick <= start) {
            OPTICK_EVENT();
            if (_networkingEngine) {
                _networkingEngine->Update();
            }

            if (_scriptingEngine) {
                _scriptingEngine->Update();
            }

            if (_worldEngine) {
                _worldEngine->Update();
            }

            if (_firebaseWrapper && _opts.firebaseEnabled && _opts.bindPublicServer) {
                _masterlistSync->Update(_firebaseWrapper.get());
            }

            PostUpdate();

            _nextTick = std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(_opts.tickInterval * 1000.0f));
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(0));
        }
    }
    void Instance::Run() {
        while (_alive) { Update(); }
    }

    void Instance::OnSignal(const sig_signal_t signal) {
        if (signal.context != sig_ctx_sys()) {
            return;
        }

        Logging::GetLogger(FRAMEWORK_INNER_SERVER)->info("Received shutdown signal. In progress...");

        PreShutdown();
        Shutdown();
    }

    void Instance::RegisterScriptingBuiltins(Framework::Scripting::SDK *sdk) {
        using namespace Framework::Scripting;
        Builtins::EntityRegister(_worldEngine, sdk->GetRootModule());
        Builtins::PlayerRegister(sdk->GetRootModule());

        // mod-specific builtins
        if (_opts.sdkRegisterCallback) {
            _opts.sdkRegisterCallback(sdk);
        }
    }
} // namespace Framework::Integrations::Server
