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

        // =========================================================
        // EVENT BASE CLASS
        // =========================================================
        class Event : public networking::Event {
        public:
            using Type = networking::Event::Type;

            Event(Librg* owner, Type type)
                : owner_(owner), type_(type) {
            }

            Type GetType() const override { return type_; }

        protected:
            networking::Networking& GetNetworking() override {
                return reinterpret_cast<networking::Networking&>(*owner_);
            }

            void _Read(void*, size_t) override {}
            void _Write(const void*, size_t) override {}

            Librg* owner_ = nullptr;
            Type   type_;
        };


        // =========================================================
        // MESSAGE BUFFER
        // =========================================================
        struct MessageBuffer {
            std::vector<uint8_t> data;
            size_t readPos = 0;

            void Write(const void* src, size_t size) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
                data.insert(data.end(), p, p + size);
            }

            void Read(void* dst, size_t size) {
                if (readPos + size > data.size())
                    size = data.size() - readPos;

                if (size > 0) {
                    std::memcpy(dst, data.data() + readPos, size);
                    readPos += size;
                }
            }
        };


        // =========================================================
        // PAYLOAD EVENT
        // =========================================================
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


        // =========================================================
        // MESSAGE (incoming)
        // =========================================================
        class Message : public PayloadEvent {
        public:
            Message(Librg* owner, Type t, MessageBuffer* buf)
                : PayloadEvent(owner, t, buf) {
            }
        };


        // =========================================================
        // MESSAGE DATA (outgoing)
        // =========================================================
        class MessageData : public PayloadEvent {
        public:
            MessageData(Librg* owner, Type t, MessageBuffer* buf)
                : PayloadEvent(owner, t, buf) {
            }
        };


        // =========================================================
        // ENTITY WRAPPER
        // =========================================================
        class Entity : public networking::Entity {
        public:
            using ID = uint32_t;

            ~Entity();

            struct _Interface : public networking::Entity::_Interface {
                Librg& net;
                ID     id;

                _Interface(Librg& backend, ID initial = networking::Entity::InvalidID)
                    : net(backend), id(initial) {
                }

                void SendMessage(networking::Event::Type type,
                    const networking::EventCallback& cb,
                    const networking::MessageOptions& opt) override;
            };

            _Interface* _interface = nullptr;
        };


        // =========================================================
        // LIBRG BACKEND
        // =========================================================
        class Librg : public networking::Networking {
        public:
            explicit Librg(bool server);
            ~Librg() override;

            void Start(const std::string& address, int32_t port) override;
            void Stop() override;
            void Tick() override;
            bool Connected() const override;

            void RegisterMessage(networking::Event::Type) override {}
            void UnregisterMessage(networking::Event::Type) override {}

            HSteamNetConnection ServerConn() const { return serverConn_; }
            void SetServerConn(HSteamNetConnection conn) { serverConn_ = conn; }

        protected:
            Entity::_Interface* GetEntityInterface() override;

        public:
            void SendRaw(const void* data, size_t size, bool reliable);

            static Librg& FromNetworking(networking::Networking& net) {
                return static_cast<Librg&>(net);
            }

            // Callbacks
            std::function<void(Event&)> onConnectionRequest;
            std::function<void(Event&)> onConnectionAccept;
            std::function<void(Event&)> onConnectionRefuse;

            std::function<void(Event&)> onEntityCreate;
            std::function<void(Event&)> onEntityUpdate;
            std::function<void(Event&)> onEntityRemove;
            std::function<void(Event&)> onClientStreamerUpdate;

            // State exposed to SteamConnectionHandler
            bool IsServer() const { return isServer_; }
            ISteamNetworkingSockets* Steam() { return steam_; }
            HSteamListenSocket ListenSock() const { return listenSocket_; }
            std::vector<HSteamNetConnection>& Clients() { return clients_; }

        private:
            // Internal state
            bool isServer_;
            ISteamNetworkingSockets* steam_ = nullptr;

            HSteamListenSocket listenSocket_ = k_HSteamListenSocket_Invalid;
            HSteamNetConnection serverConn_ = k_HSteamNetConnection_Invalid;

            std::vector<HSteamNetConnection> clients_;

            // Helpers
            void HandleIncomingOnListenSocket();
            void HandleIncomingOnConnection();

            void DispatchRaw(const void* data, size_t size);
        };

    } // namespace librg
} // namespace f4mp
