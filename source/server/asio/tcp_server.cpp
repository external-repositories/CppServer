/*!
    \file tcp_server.cpp
    \brief TCP server implementation
    \author Ivan Shynkarenka
    \date 14.12.2016
    \copyright MIT License
*/

#include "server/asio/tcp_server.h"

namespace CppServer {
namespace Asio {

TCPServer::TCPServer(std::shared_ptr<Service> service, InternetProtocol protocol, int port)
    : _service(service),
      _io_service(_service->GetAsioService()),
      _strand(*_io_service),
      _strand_required(_service->IsStrandRequired()),
      _acceptor(*_io_service),
      _started(false),
      _bytes_pending(0),
      _bytes_sent(0),
      _bytes_received(0),
      _option_no_delay(false),
      _option_reuse_address(false),
      _option_reuse_port(false)
{
    assert((service != nullptr) && "Asio service is invalid!");
    if (service == nullptr)
        throw CppCommon::ArgumentException("Asio service is invalid!");

    switch (protocol)
    {
        case InternetProtocol::IPv4:
            _endpoint = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), (unsigned short)port);
            break;
        case InternetProtocol::IPv6:
            _endpoint = asio::ip::tcp::endpoint(asio::ip::tcp::v6(), (unsigned short)port);
            break;
    }
}

TCPServer::TCPServer(std::shared_ptr<Service> service, const std::string& address, int port)
    : _service(service),
      _io_service(_service->GetAsioService()),
      _strand(*_io_service),
      _strand_required(_service->IsStrandRequired()),
      _acceptor(*_io_service),
      _started(false),
      _bytes_pending(0),
      _bytes_sent(0),
      _bytes_received(0),
      _option_no_delay(false),
      _option_reuse_address(false),
      _option_reuse_port(false)
{
    assert((service != nullptr) && "Asio service is invalid!");
    if (service == nullptr)
        throw CppCommon::ArgumentException("Asio service is invalid!");

    _endpoint = asio::ip::tcp::endpoint(asio::ip::address::from_string(address), (unsigned short)port);
}

TCPServer::TCPServer(std::shared_ptr<Service> service, const asio::ip::tcp::endpoint& endpoint)
    : _service(service),
      _io_service(_service->GetAsioService()),
      _strand(*_io_service),
      _strand_required(_service->IsStrandRequired()),
      _endpoint(endpoint),
      _acceptor(*_io_service),
      _started(false),
      _bytes_pending(0),
      _bytes_sent(0),
      _bytes_received(0),
      _option_no_delay(false),
      _option_reuse_address(false),
      _option_reuse_port(false)
{
    assert((service != nullptr) && "Asio service is invalid!");
    if (service == nullptr)
        throw CppCommon::ArgumentException("Asio service is invalid!");
}

bool TCPServer::Start()
{
    assert(!IsStarted() && "TCP server is already started!");
    if (IsStarted())
        return false;

    // Post the start handler
    auto self(this->shared_from_this());
    auto start_handler = [this, self]()
    {
        if (IsStarted())
            return;

        // Create a server acceptor
        _acceptor = asio::ip::tcp::acceptor(*_io_service);
        _acceptor.open(_endpoint.protocol());
        if (option_reuse_address())
            _acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
#if (defined(unix) || defined(__unix) || defined(__unix__) || defined(__APPLE__)) && !defined(__CYGWIN__)
        if (option_reuse_port())
        {
            typedef asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT> reuse_port;
            _acceptor.set_option(reuse_port(true));
        }
#endif
        _acceptor.bind(_endpoint);
        _acceptor.listen();

        // Reset statistic
        _bytes_pending = 0;
        _bytes_sent = 0;
        _bytes_received = 0;

        // Update the started flag
        _started = true;

        // Call the server started handler
        onStarted();

        // Perform the first server accept
        Accept();
    };
    if (_strand_required)
        _strand.post(start_handler);
    else
        _io_service->post(start_handler);

    return true;
}

bool TCPServer::Stop()
{
    assert(IsStarted() && "TCP server is not started!");
    if (!IsStarted())
        return false;

    // Post the stop handler
    auto self(this->shared_from_this());
    auto stop_handler = [this, self]()
    {
        if (!IsStarted())
            return;

        // Reset the session
        _session.reset();

        // Close the server acceptor
        _acceptor.close();

        // Disconnect all sessions
        DisconnectAll();

        // Update the started flag
        _started = false;

        // Clear multicast buffer
        ClearBuffers();

        // Call the server stopped handler
        onStopped();
    };
    if (_strand_required)
        _strand.post(stop_handler);
    else
        _io_service->post(stop_handler);

    return true;
}

bool TCPServer::Restart()
{
    if (!Stop())
        return false;

    while (IsStarted())
        CppCommon::Thread::Yield();

    return Start();
}

void TCPServer::Accept()
{
    if (!IsStarted())
        return;

    // Dispatch the accept handler
    auto self(this->shared_from_this());
    auto accept_handler = make_alloc_handler(_acceptor_storage, [this, self]()
    {
        if (!IsStarted())
            return;

        // Create a new session to accept
        _session = CreateSession(self);

        auto async_accept_handler = make_alloc_handler(_acceptor_storage, [this, self](std::error_code ec)
        {
            if (!ec)
                RegisterSession();
            else
                SendError(ec);

            // Perform the next server accept
            Accept();
        });
        if (_strand_required)
            _acceptor.async_accept(_session->socket(), bind_executor(_strand, async_accept_handler));
        else
            _acceptor.async_accept(_session->socket(), async_accept_handler);
    });
    if (_strand_required)
        _strand.dispatch(accept_handler);
    else
        _io_service->dispatch(accept_handler);
}

bool TCPServer::Multicast(const void* buffer, size_t size)
{
    assert((buffer != nullptr) && "Pointer to the buffer should not be equal to 'nullptr'!");
    assert((size > 0) && "Buffer size should be greater than zero!");
    if ((buffer == nullptr) || (size == 0))
        return false;

    if (!IsStarted())
        return false;

    {
        std::lock_guard<std::mutex> locker(_multicast_lock);

        // Detect multiple multicast handlers
        bool multicast_required = _multicast_buffer.empty();

        // Fill the multicast buffer
        const uint8_t* bytes = (const uint8_t*)buffer;
        _multicast_buffer.insert(_multicast_buffer.end(), bytes, bytes + size);

        // Update statistic
        _bytes_pending += size;

        // Avoid multiple multicast hanlders
        if (!multicast_required)
            return true;
    }

    // Dispatch the multicast handler
    auto self(this->shared_from_this());
    auto multicast_handler = make_alloc_handler(_multicast_storage, [this, self]()
    {
        if (!IsStarted())
            return;

        std::lock_guard<std::mutex> locker(_multicast_lock);

        // Check for empty multicast buffer
        if (_multicast_buffer.empty())
            return;

        // Multicast all sessions
        for (auto& session : _sessions)
            session.second->Send(_multicast_buffer.data(), _multicast_buffer.size());

        // Update statistic
        _bytes_pending -= _multicast_buffer.size();

        // Clear the multicast buffer
        _multicast_buffer.clear();
    });
    if (_strand_required)
        _strand.dispatch(multicast_handler);
    else
        _io_service->dispatch(multicast_handler);

    return true;
}

bool TCPServer::DisconnectAll()
{
    if (!IsStarted())
        return false;

    // Dispatch the disconnect all handler
    auto self(this->shared_from_this());
    auto disconnect_all_handler = [this, self]()
    {
        if (!IsStarted())
            return;

        // Disconnect all sessions
        for (auto& session : _sessions)
            session.second->Disconnect();
    };
    if (_strand_required)
        _strand.dispatch(disconnect_all_handler);
    else
        _io_service->dispatch(disconnect_all_handler);

    return true;
}

void TCPServer::RegisterSession()
{
    // Register a new session
    _sessions.emplace(_session->id(), _session);

    // Connect a new session
    _session->Connect();
}

void TCPServer::UnregisterSession(const CppCommon::UUID& id)
{
    // Try to find the unregistered session
    auto it = _sessions.find(id);
    if (it != _sessions.end())
    {
        // Erase the session
        _sessions.erase(it);
    }
}

void TCPServer::ClearBuffers()
{
    std::lock_guard<std::mutex> locker(_multicast_lock);

    _multicast_buffer.clear();

    // Update statistic
    _bytes_pending = 0;
}

void TCPServer::SendError(std::error_code ec)
{
    // Skip Asio disconnect errors
    if ((ec == asio::error::connection_aborted) ||
        (ec == asio::error::connection_refused) ||
        (ec == asio::error::connection_reset) ||
        (ec == asio::error::eof) ||
        (ec == asio::error::operation_aborted))
        return;

    // Skip Winsock error 995: The I/O operation has been aborted because of either a thread exit or an application request
    if (ec.value() == 995)
        return;

    onError(ec.value(), ec.category().name(), ec.message());
}

} // namespace Asio
} // namespace CppServer
