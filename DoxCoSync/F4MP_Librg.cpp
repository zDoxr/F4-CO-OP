#include "F4MP_Librg.h"

#include <stdexcept>
#include <cstring>
#include <algorithm>

namespace f4mp {
    namespace librg {

        // ============================================================================
        // Entity
        // ============================================================================
        Entity::~Entity()
        {
            delete _interface;
            _interface = nullptr;
        }

        void Entity::_Interface::SendMessage(
            networking::Event::Type type,
            const networking::EventCallback& cb,
            const networking::MessageOptions& opt)
        {
            // Build payload into a MessageBuffer
            MessageBuffer buf;

            // Let user callback serialize into the buffer via Event API
            MessageData md(&net, type, &buf);
            cb(md); // user calls md.Write<T>(...) inside the callback

            // Wire format: [u32 type][payload...]
            const std::size_t payloadSize = buf.data.size();
            std::vector<std::uint8_t> wire;
            wire.reserve(sizeof(std::uint32_t) + payloadSize);

            std::uint32_t type32 = static_cast<std::uint32_t>(type);
            wire.insert(
                wire.end(),
                reinterpret_cast<std::uint8_t*>(&type32),
                reinterpret_cast<std::uint8_t*>(&type32) + sizeof(type32)
            );

            if (payloadSize > 0) {
                wire.insert(wire.end(), buf.data.begin(), buf.data.end());
            }

            net.SendRaw(wire.data(), wire.size(), opt.reliable);
        }

        // ============================================================================
        // Librg ctor / dtor
        // ============================================================================
        Librg::Librg(bool server)
            : isServer_(server)
        {
            SteamNetworkingErrMsg err;
            if (!GameNetworkingSockets_Init(nullptr, err)) {
                throw std::runtime_error(err);
            }

            steam_ = SteamNetworkingSockets();
            if (!steam_) {
                throw std::runtime_error("SteamNetworkingSockets() returned nullptr");
            }
        }

        Librg::~Librg()
        {
            Stop();
            // Usually you *don't* call GameNetworkingSockets_Kill here in an F4SE plugin
            // because Steam/host may manage lifetime. If you own everything, you can.
        }

        // ============================================================================
        // Start / Stop
        // ============================================================================
        void Librg::Start(const std::string& address, std::int32_t port)
        {
            Stop(); // make sure old state is cleared

            if (!steam_) {
                throw std::runtime_error("SteamNetworkingSockets not initialized");
            }

            SteamNetworkingIPAddr addr;
            addr.Clear();
            addr.m_port = static_cast<std::uint16_t>(port);

            if (isServer_) {
                // Listen on all interfaces
                listenSocket_ = steam_->CreateListenSocketIP(addr, 0, nullptr);
                if (listenSocket_ == k_HSteamListenSocket_Invalid) {
                    throw std::runtime_error("CreateListenSocketIP failed");
                }
            }
            else {
                if (!addr.ParseString(address.c_str())) {
                    throw std::runtime_error("Invalid address string");
                }

                serverConn_ = steam_->ConnectByIPAddress(addr, 0, nullptr);
                if (serverConn_ == k_HSteamNetConnection_Invalid) {
                    throw std::runtime_error("ConnectByIPAddress failed");
                }
            }
        }

        void Librg::Stop()
        {
            if (!steam_) return;

            if (isServer_) {
                for (auto c : clients_) {
                    steam_->CloseConnection(c, 0, nullptr, false);
                }
                clients_.clear();

                if (listenSocket_ != k_HSteamListenSocket_Invalid) {
                    steam_->CloseListenSocket(listenSocket_);
                    listenSocket_ = k_HSteamListenSocket_Invalid;
                }
            }
            else {
                if (serverConn_ != k_HSteamNetConnection_Invalid) {
                    steam_->CloseConnection(serverConn_, 0, nullptr, false);
                    serverConn_ = k_HSteamNetConnection_Invalid;
                }
            }
        }

        // ============================================================================
        // Tick
        // ============================================================================
        void Librg::Tick()
        {
            if (!steam_) return;

            steam_->RunCallbacks();

            if (isServer_) {
                HandleIncomingOnListenSocket();
            }
            else {
                HandleIncomingOnConnection();
            }
        }

        // SERVER: poll each client connection
        void Librg::HandleIncomingOnListenSocket()
        {
            if (clients_.empty())
                return;

            for (auto c : clients_) {
                while (true) {
                    SteamNetworkingMessage_t* msg = nullptr;
                    int got = steam_->ReceiveMessagesOnConnection(c, &msg, 1);
                    if (got <= 0 || !msg)
                        break;

                    DispatchRaw(msg->m_pData, msg->m_cbSize);
                    msg->Release();
                }
            }
        }

        // CLIENT: poll server connection
        void Librg::HandleIncomingOnConnection()
        {
            if (serverConn_ == k_HSteamNetConnection_Invalid)
                return;

            while (true) {
                SteamNetworkingMessage_t* msg = nullptr;
                int got = steam_->ReceiveMessagesOnConnection(serverConn_, &msg, 1);
                if (got <= 0 || !msg)
                    break;

                DispatchRaw(msg->m_pData, msg->m_cbSize);
                msg->Release();
            }
        }

        // ============================================================================
        // Connected?
        // ============================================================================
        bool Librg::Connected() const
        {
            return isServer_
                ? (listenSocket_ != k_HSteamListenSocket_Invalid)
                : (serverConn_ != k_HSteamNetConnection_Invalid);
        }

        // ============================================================================
        // Entity interface factory
        // ============================================================================
        Entity::_Interface* Librg::GetEntityInterface()
        {
            return new Entity::_Interface(*this);
        }

        // ============================================================================
        // SendRaw
        // ============================================================================
        void Librg::SendRaw(const void* data, std::size_t size, bool reliable)
        {
            if (!steam_ || size == 0) return;

            const int flags = reliable
                ? k_nSteamNetworkingSend_Reliable
                : k_nSteamNetworkingSend_Unreliable;

            if (isServer_) {
                for (auto c : clients_) {
                    steam_->SendMessageToConnection(
                        c, data, static_cast<std::uint32_t>(size), flags, nullptr);
                }
            }
            else {
                if (serverConn_ != k_HSteamNetConnection_Invalid) {
                    steam_->SendMessageToConnection(
                        serverConn_, data, static_cast<std::uint32_t>(size), flags, nullptr);
                }
            }
        }

        // ============================================================================
        // DispatchRaw – [u32 type][payload...]
        // ============================================================================
        void Librg::DispatchRaw(const void* data, std::size_t size)
        {
            if (size < sizeof(std::uint32_t)) return;

            const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);

            std::uint32_t typeVal = 0;
            std::memcpy(&typeVal, bytes, sizeof(typeVal));

            MessageBuffer buf;
            if (size > sizeof(typeVal)) {
                buf.data.insert(
                    buf.data.end(),
                    bytes + sizeof(typeVal),
                    bytes + size
                );
            }

            Message msg(
                this, // owner Librg*
                static_cast<networking::Event::Type>(typeVal),
                &buf
            );

            // For now route everything to onEntityUpdate.
            // Later you can switch(msg.GetType()) and call different callbacks.
            if (onEntityUpdate) {
                onEntityUpdate(msg);
            }
        }

    } // namespace librg
} // namespace f4mp
