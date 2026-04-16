#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

const int PORT = 5000;
const int BUFFER_SIZE = 8192;

#define WM_SOCKET_EVENT (WM_USER + 1)

#pragma pack(push, 1)
struct FileHeader
{
    char fileName[260];
    uint64_t fileSize;
    uint32_t mode;
    uint32_t fragmentCount;
};
#pragma pack(pop)

struct ClientState
{
    SOCKET socket = INVALID_SOCKET;

    bool headerReceived = false;
    FileHeader header{};
    int headerBytesRead = 0;

    std::ofstream outFile;
    std::string outputFileName;

    uint64_t totalBytesReceived = 0;

    uint32_t currentFragmentSize = 0;
    uint32_t currentFragmentBytesRead = 0;
    int fragmentSizeBytesRead = 0;
    uint32_t fragmentsReceived = 0;
};

SOCKET g_listenSocket = INVALID_SOCKET;
ClientState g_client;

bool hasValidExtension(const std::string& fileName)
{
    size_t pos = fileName.find_last_of('.');
    if (pos == std::string::npos)
    {
        return false;
    }

    std::string ext = fileName.substr(pos);
    for (char& ch : ext)
    {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }

    return ext == ".c" || ext == ".cpp";
}

void openReceivedFile(const std::string& fileName)
{
    char fullPath[MAX_PATH]{};
    DWORD len = GetFullPathNameA(fileName.c_str(), MAX_PATH, fullPath, nullptr);
    std::cout << "Trying to open: " << fullPath << std::endl;

    HINSTANCE result = ShellExecuteA(
        nullptr,
        "open",
        "devenv.exe",
        fullPath,
        nullptr,
        SW_SHOWNORMAL
    );
}

void resetClientStateOnly()
{
    g_client = ClientState{};
}

void closeClientSocketOnly()
{
    if (g_client.socket != INVALID_SOCKET)
    {
        closesocket(g_client.socket);
        g_client.socket = INVALID_SOCKET;
    }
}

void closeClient()
{
    if (g_client.outFile.is_open())
    {
        g_client.outFile.close();
    }

    closeClientSocketOnly();
    resetClientStateOnly();
}

bool prepareOutputFile()
{
    std::string originalName = g_client.header.fileName;

    if (!hasValidExtension(originalName))
    {
        std::cerr << "Invalid file type. Only .c and .cpp are allowed." << std::endl;
        return false;
    }

    g_client.outputFileName = "received_async_" + originalName;
    g_client.outFile.open(g_client.outputFileName, std::ios::binary);

    if (!g_client.outFile)
    {
        std::cerr << "Cannot create output file: " << g_client.outputFileName << std::endl;
        return false;
    }

    std::cout << "File name: " << originalName << std::endl;
    std::cout << "Output file: " << g_client.outputFileName << std::endl;
    std::cout << "File size: " << g_client.header.fileSize << " bytes" << std::endl;
    std::cout << "Mode: " << (g_client.header.mode == 1 ? "whole" : "fragmented") << std::endl;

    if (g_client.header.mode == 2)
    {
        std::cout << "Fragment count: " << g_client.header.fragmentCount << std::endl;
    }

    return true;
}

bool finalizeTransferIfComplete()
{
    if (!g_client.headerReceived)
    {
        return false;
    }

    if (g_client.totalBytesReceived != g_client.header.fileSize)
    {
        return false;
    }

    if (g_client.outFile.is_open())
    {
        g_client.outFile.close();
    }

    std::cout << "File received successfully." << std::endl;

    std::string pathToOpen = g_client.outputFileName;
    closeClient();
    openReceivedFile(pathToOpen);
    return true;
}

bool processHeader()
{
    while (g_client.headerBytesRead < static_cast<int>(sizeof(FileHeader)))
    {
        char* headerPtr = reinterpret_cast<char*>(&g_client.header);
        int need = static_cast<int>(sizeof(FileHeader)) - g_client.headerBytesRead;

        int received = recv(g_client.socket, headerPtr + g_client.headerBytesRead, need, 0);

        if (received > 0)
        {
            g_client.headerBytesRead += received;
        }
        else if (received == 0)
        {
            std::cout << "Client disconnected before header was fully received." << std::endl;
            return false;
        }
        else
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
            {
                return true;
            }

            std::cerr << "recv() failed while reading header: " << err << std::endl;
            return false;
        }
    }

    g_client.headerReceived = true;

    if (g_client.header.mode != 1 && g_client.header.mode != 2)
    {
        std::cerr << "Unknown mode in header." << std::endl;
        return false;
    }

    if (g_client.header.mode == 2 && g_client.header.fragmentCount == 0)
    {
        std::cerr << "Fragment count cannot be zero." << std::endl;
        return false;
    }

    if (!prepareOutputFile())
    {
        return false;
    }

    return true;
}

bool processWholeMode()
{
    while (true)
    {
        if (g_client.totalBytesReceived >= g_client.header.fileSize)
        {
            finalizeTransferIfComplete();
            return true;
        }

        char buffer[BUFFER_SIZE];

        uint64_t remaining64 = g_client.header.fileSize - g_client.totalBytesReceived;
        int toRead = (remaining64 > static_cast<uint64_t>(BUFFER_SIZE))
            ? BUFFER_SIZE
            : static_cast<int>(remaining64);

        int received = recv(g_client.socket, buffer, toRead, 0);

        if (received > 0)
        {
            g_client.outFile.write(buffer, received);
            g_client.totalBytesReceived += static_cast<uint64_t>(received);

            std::cout << "Received " << received
                << " bytes, total = "
                << g_client.totalBytesReceived << "/"
                << g_client.header.fileSize << std::endl;

            if (finalizeTransferIfComplete())
            {
                return true;
            }
        }
        else if (received == 0)
        {
            std::cout << "Client disconnected." << std::endl;
            return false;
        }
        else
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
            {
                return true;
            }

            std::cerr << "recv() failed in whole mode: " << err << std::endl;
            return false;
        }
    }
}

bool processFragmentedMode()
{
    while (true)
    {
        if (g_client.totalBytesReceived >= g_client.header.fileSize)
        {
            finalizeTransferIfComplete();
            return true;
        }

        if (g_client.fragmentSizeBytesRead < static_cast<int>(sizeof(uint32_t)))
        {
            char* sizePtr = reinterpret_cast<char*>(&g_client.currentFragmentSize);
            int need = static_cast<int>(sizeof(uint32_t)) - g_client.fragmentSizeBytesRead;

            int received = recv(
                g_client.socket,
                sizePtr + g_client.fragmentSizeBytesRead,
                need,
                0
            );

            if (received > 0)
            {
                g_client.fragmentSizeBytesRead += received;

                if (g_client.fragmentSizeBytesRead < static_cast<int>(sizeof(uint32_t)))
                {
                    return true;
                }

                g_client.currentFragmentBytesRead = 0;

                if (g_client.currentFragmentSize == 0)
                {
                    std::cerr << "Invalid fragment size = 0" << std::endl;
                    return false;
                }

                std::cout << "Fragment "
                    << (g_client.fragmentsReceived + 1)
                    << " size = "
                    << g_client.currentFragmentSize
                    << " bytes" << std::endl;
            }
            else if (received == 0)
            {
                std::cout << "Client disconnected." << std::endl;
                return false;
            }
            else
            {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                {
                    return true;
                }

                std::cerr << "recv() failed while reading fragment size: " << err << std::endl;
                return false;
            }
        }

        while (g_client.currentFragmentBytesRead < g_client.currentFragmentSize)
        {
            char buffer[BUFFER_SIZE];

            uint32_t remainInFragment = g_client.currentFragmentSize - g_client.currentFragmentBytesRead;
            int toRead = (remainInFragment > static_cast<uint32_t>(BUFFER_SIZE))
                ? BUFFER_SIZE
                : static_cast<int>(remainInFragment);

            int received = recv(g_client.socket, buffer, toRead, 0);

            if (received > 0)
            {
                g_client.outFile.write(buffer, received);
                g_client.currentFragmentBytesRead += static_cast<uint32_t>(received);
                g_client.totalBytesReceived += static_cast<uint64_t>(received);

                std::cout << "Received " << received
                    << " bytes of current fragment, total file bytes = "
                    << g_client.totalBytesReceived << "/"
                    << g_client.header.fileSize << std::endl;

                if (finalizeTransferIfComplete())
                {
                    return true;
                }
            }
            else if (received == 0)
            {
                std::cout << "Client disconnected." << std::endl;
                return false;
            }
            else
            {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                {
                    return true;
                }

                std::cerr << "recv() failed while reading fragment data: " << err << std::endl;
                return false;
            }
        }

        g_client.fragmentsReceived++;
        std::cout << "Fragment " << g_client.fragmentsReceived << " completed." << std::endl;

        g_client.currentFragmentSize = 0;
        g_client.currentFragmentBytesRead = 0;
        g_client.fragmentSizeBytesRead = 0;

        if (g_client.fragmentsReceived >= g_client.header.fragmentCount &&
            g_client.totalBytesReceived == g_client.header.fileSize)
        {
            finalizeTransferIfComplete();
            return true;
        }
    }
}

void drainSocketAfterClose()
{
    if (g_client.socket == INVALID_SOCKET || !g_client.headerReceived)
    {
        return;
    }

    while (true)
    {
        if (g_client.header.mode == 1)
        {
            if (g_client.totalBytesReceived >= g_client.header.fileSize)
            {
                break;
            }

            char buffer[BUFFER_SIZE];
            uint64_t remaining64 = g_client.header.fileSize - g_client.totalBytesReceived;
            int toRead = (remaining64 > static_cast<uint64_t>(BUFFER_SIZE))
                ? BUFFER_SIZE
                : static_cast<int>(remaining64);

            int received = recv(g_client.socket, buffer, toRead, 0);

            if (received > 0)
            {
                g_client.outFile.write(buffer, received);
                g_client.totalBytesReceived += static_cast<uint64_t>(received);

                std::cout << "Drained " << received
                    << " bytes after FD_CLOSE, total = "
                    << g_client.totalBytesReceived << "/"
                    << g_client.header.fileSize << std::endl;
            }
            else
            {
                break;
            }
        }
        else
        {
            bool progressed = false;

            if (g_client.fragmentSizeBytesRead < static_cast<int>(sizeof(uint32_t)))
            {
                char* sizePtr = reinterpret_cast<char*>(&g_client.currentFragmentSize);
                int need = static_cast<int>(sizeof(uint32_t)) - g_client.fragmentSizeBytesRead;

                int received = recv(
                    g_client.socket,
                    sizePtr + g_client.fragmentSizeBytesRead,
                    need,
                    0
                );

                if (received > 0)
                {
                    progressed = true;
                    g_client.fragmentSizeBytesRead += received;

                    if (g_client.fragmentSizeBytesRead == static_cast<int>(sizeof(uint32_t)))
                    {
                        g_client.currentFragmentBytesRead = 0;
                        std::cout << "Fragment "
                            << (g_client.fragmentsReceived + 1)
                            << " size = "
                            << g_client.currentFragmentSize
                            << " bytes" << std::endl;
                    }
                }
                else
                {
                    break;
                }
            }

            if (g_client.fragmentSizeBytesRead == static_cast<int>(sizeof(uint32_t)) &&
                g_client.currentFragmentBytesRead < g_client.currentFragmentSize)
            {
                char buffer[BUFFER_SIZE];
                uint32_t remainInFragment = g_client.currentFragmentSize - g_client.currentFragmentBytesRead;
                int toRead = (remainInFragment > static_cast<uint32_t>(BUFFER_SIZE))
                    ? BUFFER_SIZE
                    : static_cast<int>(remainInFragment);

                int received = recv(g_client.socket, buffer, toRead, 0);
                if (received > 0)
                {
                    progressed = true;
                    g_client.outFile.write(buffer, received);
                    g_client.currentFragmentBytesRead += static_cast<uint32_t>(received);
                    g_client.totalBytesReceived += static_cast<uint64_t>(received);

                    std::cout << "Drained " << received
                        << " bytes after FD_CLOSE, total = "
                        << g_client.totalBytesReceived << "/"
                        << g_client.header.fileSize << std::endl;

                    if (g_client.currentFragmentBytesRead == g_client.currentFragmentSize)
                    {
                        g_client.fragmentsReceived++;
                        std::cout << "Fragment " << g_client.fragmentsReceived << " completed." << std::endl;

                        g_client.currentFragmentSize = 0;
                        g_client.currentFragmentBytesRead = 0;
                        g_client.fragmentSizeBytesRead = 0;
                    }
                }
                else
                {
                    break;
                }
            }

            if (!progressed)
            {
                break;
            }

            if (g_client.totalBytesReceived >= g_client.header.fileSize)
            {
                break;
            }
        }
    }
}

void handleAccept(HWND hWnd)
{
    sockaddr_in clientAddr{};
    int clientAddrLen = sizeof(clientAddr);

    SOCKET clientSocket = accept(
        g_listenSocket,
        reinterpret_cast<sockaddr*>(&clientAddr),
        &clientAddrLen
    );

    if (clientSocket == INVALID_SOCKET)
    {
        std::cerr << "accept() failed: " << WSAGetLastError() << std::endl;
        return;
    }

    if (g_client.socket != INVALID_SOCKET)
    {
        closesocket(clientSocket);
        return;
    }

    u_long nonBlocking = 1;
    ioctlsocket(clientSocket, FIONBIO, &nonBlocking);

    resetClientStateOnly();
    g_client.socket = clientSocket;

    if (WSAAsyncSelect(clientSocket, hWnd, WM_SOCKET_EVENT, FD_READ | FD_CLOSE) == SOCKET_ERROR)
    {
        std::cerr << "WSAAsyncSelect(client) failed: " << WSAGetLastError() << std::endl;
        closeClient();
        return;
    }

    char clientIp[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);

    std::cout << "\nClient connected: "
        << clientIp << ":"
        << ntohs(clientAddr.sin_port) << std::endl;
}

void handleRead()
{
    if (g_client.socket == INVALID_SOCKET)
    {
        return;
    }

    if (!g_client.headerReceived)
    {
        if (!processHeader())
        {
            closeClient();
            return;
        }

        if (!g_client.headerReceived)
        {
            return;
        }
    }

    bool ok = false;

    if (g_client.header.mode == 1)
    {
        ok = processWholeMode();
    }
    else
    {
        ok = processFragmentedMode();
    }

    if (!ok)
    {
        closeClient();
    }
}

void handleClose()
{
    if (g_client.socket == INVALID_SOCKET)
    {
        return;
    }

    drainSocketAfterClose();

    if (finalizeTransferIfComplete())
    {
        return;
    }

    std::cout << "Connection closed by client." << std::endl;
    closeClient();
}

LRESULT CALLBACK HiddenWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SOCKET_EVENT:
    {
        int eventCode = WSAGETSELECTEVENT(lParam);
        int errorCode = WSAGETSELECTERROR(lParam);

        if (errorCode != 0)
        {
            std::cerr << "Socket event error: " << errorCode << std::endl;

            if ((SOCKET)wParam == g_client.socket)
            {
                closeClient();
            }

            return 0;
        }

        if (eventCode == FD_ACCEPT)
        {
            handleAccept(hWnd);
        }
        else if (eventCode == FD_READ)
        {
            handleRead();
        }
        else if (eventCode == FD_CLOSE)
        {
            handleClose();
        }

        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int main()
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }

    HINSTANCE hInstance = GetModuleHandle(nullptr);
    const char* className = "HiddenAsyncSocketWindow";

    WNDCLASSA wc{};
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;

    if (!RegisterClassA(&wc))
    {
        std::cerr << "RegisterClass failed." << std::endl;
        WSACleanup();
        return 1;
    }

    HWND hWnd = CreateWindowA(
        className,
        "HiddenAsyncSocketWindow",
        0,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hWnd)
    {
        std::cerr << "CreateWindow failed." << std::endl;
        WSACleanup();
        return 1;
    }

    g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSocket == INVALID_SOCKET)
    {
        std::cerr << "socket() failed: " << WSAGetLastError() << std::endl;
        DestroyWindow(hWnd);
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
    {
        std::cerr << "bind() failed: " << WSAGetLastError() << std::endl;
        closesocket(g_listenSocket);
        DestroyWindow(hWnd);
        WSACleanup();
        return 1;
    }

    if (listen(g_listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "listen() failed: " << WSAGetLastError() << std::endl;
        closesocket(g_listenSocket);
        DestroyWindow(hWnd);
        WSACleanup();
        return 1;
    }

    if (WSAAsyncSelect(g_listenSocket, hWnd, WM_SOCKET_EVENT, FD_ACCEPT) == SOCKET_ERROR)
    {
        std::cerr << "WSAAsyncSelect(listen socket) failed: " << WSAGetLastError() << std::endl;
        closesocket(g_listenSocket);
        DestroyWindow(hWnd);
        WSACleanup();
        return 1;
    }

    std::cout << "Server started on port " << PORT << std::endl;
    std::cout << "Waiting for client..." << std::endl;

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    closeClient();

    if (g_listenSocket != INVALID_SOCKET)
    {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }

    WSACleanup();
    return 0;
}


