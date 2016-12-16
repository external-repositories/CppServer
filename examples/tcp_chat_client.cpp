/*!
    \file tcp_chat_client.cpp
    \brief TCP chat client example
    \author Ivan Shynkarenka
    \date 14.12.2016
    \copyright MIT License
*/

#include "server/tcp/client.h"

#include <iostream>

class ChatClient : public CppServer::TCPClient
{
public:
    using CppServer::TCPClient::TCPClient;

protected:
    void onStarting() override
    {
        std::cout << "Chat client starting..." << std::endl;
    }
    void onStarted() override
    {
        std::cout << "Chat client started!" << std::endl;
    }
    void onStopping() override
    {
        std::cout << "Chat client stopping..." << std::endl;
    }
    void onStopped() override
    {
        std::cout << "Chat client stopped!" << std::endl;
    }

    void onConnected() override
    {
        std::cout << "Chat client connected a new session with Id " << id() << std::endl;
    }
    void onDisconnected() override
    {
        std::cout << "Chat client disconnected a session with Id " << id() << std::endl;

        // Try to wait for a while
        CppCommon::Thread::Sleep(1000);

        // Try to connect again
        Connect();
    }

    size_t onReceived(const void* buffer, size_t size) override
    {
        std::cout << "Incoming: " << std::string((const char*)buffer, size) << std::endl;
        return size;
    }

    void onError(int error, const std::string& category, const std::string& message) override
    {
        std::cout << "Chat client caught an error with code " << error << " and category '" << category << "': " << message << std::endl;
    }
};

int main(int argc, char** argv)
{
    // Server address
    std::string address = "127.0.0.1";
    if (argc > 1)
        address = argv[1];

    // Server port
    int port = 1234;
    if (argc > 2)
        port = std::atoi(argv[2]);

    std::cout << "Server address: " << address << std::endl;
    std::cout << "Server port: " << port << std::endl;
    std::cout << "Press Enter to stop..." << std::endl;

    // Create a new TCP chat client
    ChatClient client(address, port);

    // Start the client
    client.Start();

    // Connect the client
    client.Connect();

    // Perform text input
    std::string line;
    while (getline(std::cin, line))
    {
        if (line.empty())
            break;

        // Send the entered text to the chat server
        client.Send(line.data(), line.size());
    }

    // Disconnect the client
    client.Disconnect();

    // Stop the client
    client.Stop();

    return 0;
}
