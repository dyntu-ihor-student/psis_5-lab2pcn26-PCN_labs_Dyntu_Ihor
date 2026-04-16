#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

const int PORT = 5000;
const int BUFFER_SIZE = 4096;

#pragma pack(push, 1)
struct FileHeader
{
    char fileName[260];
    uint64_t fileSize;
    // 1 - whole, 2 - framented
    uint32_t mode;
    uint32_t fragmentCount;
};
#pragma pack(pop)

bool recvAll(SOCKET s, char* data, int length)
{
    int totalReceived = 0;
    while (totalReceived < length)
    {
        int received = recv(s, data + totalReceived, length - totalReceived, 0);
        if (received == SOCKET_ERROR || received == 0)
        {
            return false;
        }
        totalReceived += received;
    }
    return true;
}

bool hasValidExtension(const string& fileName)
{
    size_t pos = fileName.find_last_of('.');
    if (pos == string::npos)
    {
        return false;
    }

    string ext = fileName.substr(pos);
    for (char& ch : ext)
    {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }

    return ext == ".c" || ext == ".cpp";
}

bool receiveFile(SOCKET clientSocket)
{
    FileHeader header{};
    if (!recvAll(clientSocket, reinterpret_cast<char*>(&header), sizeof(header)))
    {
        cerr << "Failed to receive file header." << endl;
        return false;
    }

    string fileName = header.fileName;
    if (!hasValidExtension(fileName))
    {
        cerr << "Error. Only .c and .cpp files are allowed." << endl;
        return false;
    }

    ofstream outFile(fileName, std::ios::binary);
    if (!outFile)
    {
        cerr << "Failed to create output file: " << fileName << endl;
        return false;
    }

    cout << "\nReceiving file: " << fileName << endl;
    cout << "File size: " << header.fileSize << " bytes" << endl;
    cout << "Mode: " << (header.mode == 1 ? "whole" : "fragmented") << endl;

    uint64_t remaining = header.fileSize;
    vector<char> buffer(BUFFER_SIZE);

    if (header.mode == 1)
    {
        while (remaining > 0)
        {
            int toRead = static_cast<int>((remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining);
            int received = recv(clientSocket, buffer.data(), toRead, 0);
            if (received == SOCKET_ERROR || received == 0)
            {
                cerr << "Error while receiving file data." << endl;
                return false;
            }

            outFile.write(buffer.data(), received);
            remaining -= static_cast<uint64_t>(received);
        }
    }
    else if (header.mode == 2)
    {
        cout << "Fragment count: " << header.fragmentCount << endl;

        for (uint32_t i = 0; i < header.fragmentCount && remaining > 0; ++i)
        {
            uint32_t fragmentSize = 0;
            if (!recvAll(clientSocket, reinterpret_cast<char*>(&fragmentSize), sizeof(fragmentSize)))
            {
                cerr << "Failed to receive fragment size." << endl;
                return false;
            }

            if (fragmentSize > remaining)
            {
                cerr << "Fragment size is larger than remaining file size." << endl;
                return false;
            }

            uint32_t fragmentRemaining = fragmentSize;
            while (fragmentRemaining > 0)
            {
                int toRead = (fragmentRemaining > static_cast<uint32_t>(BUFFER_SIZE))
                    ? BUFFER_SIZE
                    : static_cast<int>(fragmentRemaining);

                int received = recv(clientSocket, buffer.data(), toRead, 0);
                if (received == SOCKET_ERROR || received == 0)
                {
                    cerr << "Error while receiving fragment data." << endl;
                    return false;
                }

                outFile.write(buffer.data(), received);
                fragmentRemaining -= static_cast<uint32_t>(received);
                remaining -= static_cast<uint64_t>(received);
            }

            cout << "Fragment " << (i + 1) << " of " << header.fragmentCount << endl;
        }
    }
    else
    {
        cerr << "Unknown transfer mode." << endl;
        return false;
    }

    outFile.close();

    if (remaining != 0)
    {
        cerr << "Failed to receive complete file." << endl;
        return false;
    }

    cout << "File received successfully." << endl;
    return true;
}

int main()
{
    WSADATA wsaData{};
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0)
    {
        cerr << "WSAStartup failed: " << wsaResult << endl;
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
    {
        cerr << "socket creation failed: " << WSAGetLastError() << endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
    {
        cerr << "socket bind failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        cerr << "listen failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    cout << "Server started on port " << PORT << endl;
    cout << "Waiting for clients..." << endl;

    while (true)
    {
        sockaddr_in clientAddr{};
        int clientAddrSize = sizeof(clientAddr);

        cout << "\nCalling accept()..." << endl;
        SOCKET clientSocket = accept(
            listenSocket,
            reinterpret_cast<sockaddr*>(&clientAddr),
            &clientAddrSize
        );

        if (clientSocket == INVALID_SOCKET)
        {
            cerr << "accept failed: " << WSAGetLastError() << endl;
            continue;
        }

        char clientIp[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);

        bool ok = receiveFile(clientSocket);
        if (!ok)
        {
            cerr << "Failed to receive file from client." << endl;
        }

        closesocket(clientSocket);
        cout << "Client disconnected." << endl;
    }

    closesocket(listenSocket);
    WSACleanup();
    return 0;
}

