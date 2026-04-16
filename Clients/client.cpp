#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

const int PORT = 5000;
const int BUFFER_SIZE = 8192;
const char* SERVER_IP = "127.0.0.1";

#pragma pack(push, 1)
struct FileHeader
{
    char fileName[260];
    uint64_t fileSize;
    // 1 - whole, 2 - fragmented
    uint32_t mode;          
    uint32_t fragmentCount; 
};
#pragma pack(pop)

bool sendAll(SOCKET s, const char* data, int length)
{
    int totalSent = 0;
    while (totalSent < length)
    {
        int sent = send(s, data + totalSent, length - totalSent, 0);
        if (sent == SOCKET_ERROR || sent == 0)
        {
            return false;
        }
        totalSent += sent;
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

string extractFileName(const string& path)
{
    size_t pos = path.find_last_of("\\/");
    if (pos == string::npos)
    {
        return path;
    }
    return path.substr(pos + 1);
}

uint64_t getFileSize(ifstream& file)
{
    file.seekg(0, ios::end);
    uint64_t size = static_cast<uint64_t>(file.tellg());
    file.seekg(0, ios::beg);
    return size;
}

bool sendWholeFile(SOCKET sock, ifstream& file, uint64_t fileSize)
{
    vector<char> buffer(BUFFER_SIZE);
    uint64_t remaining = fileSize;

    while (remaining > 0)
    {
        streamsize toRead = static_cast<streamsize>(
            (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining
            );

        file.read(buffer.data(), toRead);
        streamsize actuallyRead = file.gcount();

        if (actuallyRead <= 0)
        {
            cerr << "Failed to read file data." << endl;
            return false;
        }

        if (!sendAll(sock, buffer.data(), static_cast<int>(actuallyRead)))
        {
            cerr << "Failed to send file data." << endl;
            return false;
        }

        remaining -= static_cast<uint64_t>(actuallyRead);
    }

    return true;
}

bool sendFragmentedFile(SOCKET sock, ifstream& file, uint64_t fileSize, uint32_t fragmentCount)
{
    if (fragmentCount == 0)
    {
        cerr << "Fragment count cannot be zero." << endl;
        return false;
    }

    uint64_t baseSize = fileSize / fragmentCount;
    uint64_t remainder = fileSize % fragmentCount;
    vector<char> buffer(BUFFER_SIZE);

    for (uint32_t i = 0; i < fragmentCount; ++i)
    {
        uint64_t currentFragmentSize = baseSize + (i < remainder ? 1 : 0);

        uint32_t sendFragmentSize = static_cast<uint32_t>(currentFragmentSize);
        if (!sendAll(sock, reinterpret_cast<const char*>(&sendFragmentSize), sizeof(sendFragmentSize)))
        {
            cerr << "Failed to send fragment size." << endl;
            return false;
        }

        uint64_t remaining = currentFragmentSize;
        while (remaining > 0)
        {
            streamsize toRead = static_cast<streamsize>(
                (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining
                );

            file.read(buffer.data(), toRead);
            streamsize actuallyRead = file.gcount();

            if (actuallyRead <= 0)
            {
                cerr << "Failed to read fragment data." << endl;
                return false;
            }

            if (!sendAll(sock, buffer.data(), static_cast<int>(actuallyRead)))
            {
                cerr << "Failed to send fragment data." << endl;
                return false;
            }

            remaining -= static_cast<uint64_t>(actuallyRead);
        }

        cout << "Sent fragment " << (i + 1) << " of " << fragmentCount << endl;
    }

    return true;
}

int main()
{
    string fileName;
    int mode = 0;
    uint32_t fragmentCount = 67;

    cout << "Enter name of .c or .cpp file: ";
    std::getline(cin, fileName);

    if (!hasValidExtension(fileName))
    {
        cerr << "Only .c and .cpp files are allowed." << endl;
        return 1;
    }

    ifstream file(fileName, ios::binary);
    if (!file)
    {
        cerr << "Cannot open file." << endl;
        return 1;
    }

    uint64_t fileSize = getFileSize(file);

    cout << "Choose mode:\n";
    cout << "1 - send one fragment\n";
    cout << "2 - send as multiple fragments\n";
    cin >> mode;

    WSADATA wsaData{};
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0)
    {
        cerr << "WSAStartup failed: " << wsaResult << endl;
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        cerr << "socket() failed: " << WSAGetLastError() << endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serverAddr.sin_addr) != 1)
    {
        cerr << "Invalid server IP." << endl;
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
    {
        cerr << "connect failed: " << WSAGetLastError() << endl;
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    cout << "Connected to server." << endl;

    FileHeader header{};
    std::memset(&header, 0, sizeof(header));
    strncpy_s(header.fileName, fileName.c_str(), sizeof(header.fileName) - 1);
    header.fileSize = fileSize;
    header.mode = static_cast<uint32_t>(mode);
    header.fragmentCount = (mode == 2) ? fragmentCount : 1;

    if (!sendAll(sock, reinterpret_cast<const char*>(&header), sizeof(header)))
    {
        cerr << "Failed to send file header." << endl;
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    bool ok = false;

    if (mode == 1)
    {
        ok = sendWholeFile(sock, file, fileSize);
    }
    else if (mode == 2)
    {
        ok = sendFragmentedFile(sock, file, fileSize, fragmentCount);
    }
    else
    {
        cerr << "Unknown mode." << endl;
    }

    if (ok)
    {
        cout << "File sent successfully." << endl;
    }
    else
    {
        cerr << "File transfer failed." << endl;
    }

    file.close();
    closesocket(sock);
    WSACleanup();
    return ok ? 0 : 1;
}


