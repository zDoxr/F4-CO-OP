#pragma once

#include <string>
#include <functional>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <cstring>

// Steam Networking
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

// F4MP networking
#include "Networking.h"

namespace f4mp {
    namespace librg {

        class Librg;

        // =========================================================================
        //  EVENT BASE CLASS (used for connect/disconnect AND message payloads)
        // =========================================================================
        class Event1 : public networking::Event {
        public:
            using Type = networking::Event::Type;   // uint32_t

            Event(Librg* owner, Type type)
                : owner_(owner), type_(type) {
            }

            Type GetType() const override { return type_; }

        protected:
            networking::Networking& GetNetworking() override;

            // No payload for pure connection events
            void _Read(void*, size_t) override {}
            void _Write(const void*, size_t) override {}

        protected:
            Librg* owner_ = nullptr;
            Type   type_;
        };


        // =========================================================================
        //  SIMPLE MESSAGE BUFFER (binary read/write)
        // =========================================================================
        struct MessageBuffer {
            std::vector<std::uint8_t> data;
            std::size_t readPos = 0;

            void Write(const void* src, std::size_t size) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
                data.insert(data.end(), p, p + size);
            }

            void Read(void* dst, std::size_t size) {
                if (readPos + size > data.size())
                    size = (data.size() > readPos) ? (data.size() - readPos) : 0;

                if (size > 0) {
                    std::memcpy(dst, data.data() + readPos, size);
                    readPos += size;
                }
            }
        };


        // =========================================================================
        //  EVENT WITH PAYLOAD (Incoming and Outgoing variants use this)
        // =========================================================================
        class PayloadEvent : public Event {
        public:
            PayloadEvent(Librg* owner, Type t, MessageBuffer* buf)
                : Event(owner, t), buf_(buf) {
            }

            template<typename T>
            T Read() {
                T val{};
                _Read(&val, sizeof(T));
                return val;
            }

            template<typename T>
            void Write(const T& v) {
                _Write(&v, sizeof(T));
            }

        protected:
            void _Read(void* out, size_t s) override {
                if (buf_) buf_->Read(out, s);
            }

            void _Write(const void* in, size_t s) override {
                if (buf_) buf_->Write(in, s);
            }

        private:
            MessageBuffer* buf_ = nullptr;
        };


        // =========================================================================
        //  Message = received packet
        // =========================================================================
        class Message : public PayloadEvent {
        public:
            Message(Librg* owner, Type t, MessageBuffer* buf)
                : PayloadEvent(owner, t, buf) {
            }
        };

        // =========================================================================
        //  MessageData = outgoing payload builder
        // =========================================================================
        class MessageData : public PayloadEvent {
        public:
            MessageData(Librg* owner, Type t, MessageBuffer* buf)
                : PayloadEvent(owner, t, buf) {
            }
        };


        // =========================================================================
        //  ENTITY WRAPPER
        // =========================================================================
        class Entity : public networking::Entity {
        public:
            using ID = uint32_t;

            ~Entity() { delete _interface; }

            struct _Interface : public networking::Entity::_Interface {
                Librg& net;
                ID id;

                _Interface(Librg& backend, ID initial = networking::Entity::InvalidID)
                    : net(backend), id(initial) {
                }

                void SendMessage(networking::Event::Type type,
                    const networking::EventCallback& cb,
                    const networking::MessageOptions& opt) override;
            };

            _Interface* _interface = nullptr;
        };


        // =========================================================================
        //  LIBRG BACKEND (STEAM NETWORKING ONLY)
        // =========================================================================
        class Librg : public networking::Networking {
        public:
            explicit Librg(bool server);
            ~Librg() override;

            // ---- Networking implementation ----
            void Start(const std::string& address, int32_t port) override;
            void Stop() override;
            void Tick() override;
            bool Connected() const override;

            void RegisterMessage(networking::Event::Type) override {}
            void UnregisterMessage(networking::Event::Type) override {}

        protected:
            Entity::_Interface* GetEntityInterface() override;

        public:
            // Send raw byte packet ↓
            void SendRaw(const void* data, size_t size, bool reliable);

            static Librg& FromNetworking(networking::Networking& net) {
                return static_cast<Librg&>(net);
            }

            // F4MP callbacks
            std::function<void(Event&)> onConnectionRequest;
            std::function<void(Event&)> onConnectionAccept;
            std::function<void(Event&)> onConnectionRefuse;

            std::function<void(Event&)> onEntityCreate;
            std::function<void(Event&)> onEntityUpdate;
            std::function<void(Event&)> onEntityRemove;
            std::function<void(Event&)> onClientStreamerUpdate;

            // Exposed backend state for SteamConnectionHandler
            bool IsServer() const { return isServer_; }
            ISteamNetworkingSockets* Steam() { return steam_; }
            HSteamListenSocket ListenSock() const { return listenSocket_; }
            HSteamNetConnection ServerConn() const { return serverConn_; }
            std::vector<HSteamNetConnection>& Clients() { return clients_; }

        private:
            // ---- internal state ----
            bool isServer_;
            ISteamNetworkingSockets* steam_ = nullptr;

            HSteamListenSocket listenSocket_ = k_HSteamListenSocket_Invalid;
            HSteamNetConnection serverConn_ = k_HSteamNetConnection_Invalid;

            std::vector<HSteamNetConnection> clients_;

            // ---- helpers ----
            void HandleIncomingOnListenSocket();
            void HandleIncomingOnConnection();

            void DispatchRaw(const void* data, size_t size);
        };


        // =========================================================================
        // Event::GetNetworking()
        // =========================================================================
        inline networking::Networking& Event::GetNetworking() {
            return *owner_;
        }

    } // namespace librg
} // namespace f4mp
