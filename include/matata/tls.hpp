#pragma once
#include <cstdint>
#include <string>

// Winsock SOCKET type without pulling in <winsock2.h> from headers.
// A SOCKET is just a UINT_PTR on Windows; consumers include winsock2.h
// in .cpp files that actually call send/recv.
typedef unsigned long long MATATA_SOCKET;

namespace matata {

// Minimal SChannel TLS client over an already-connected TCP socket.
// The socket stays owned by the caller (TlsStream does not close it).
class TlsStream {
public:
    TlsStream();
    ~TlsStream();
    TlsStream(const TlsStream&)            = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    // Perform the client-side TLS handshake. The remote hostname is used
    // for SNI and certificate CN / SAN validation when enabled.
    // Pass the socket value from Winsock (converted via (unsigned long
    // long)sock). The socket must stay valid for the lifetime of this
    // stream.
    bool handshakeClient(MATATA_SOCKET sock,
                         const std::wstring& hostname,
                         std::wstring& err);

    // Reset an encrypted stream over an EXISTING TLS session from another
    // TlsStream (used for the FTPS data channel, which reuses the
    // control-channel's session cache). Pass the reference control stream.
    bool handshakeDataClient(MATATA_SOCKET sock,
                             const std::wstring& hostname,
                             const TlsStream& controlSession,
                             std::wstring& err);

    // Encrypt+send. Returns bytes sent (> 0) or 0 on error with `err` set.
    int sendAll(const void* buf, int len, std::wstring& err);

    // Recv+decrypt one or more bytes into `buf`. Returns bytes copied:
    //   > 0  : payload bytes copied
    //   = 0  : clean TLS close_notify (EOF)
    //   < 0  : error, with `err` populated
    int recvSome(void* buf, int len, std::wstring& err);

    // Send TLS close_notify. Does not close the underlying socket.
    void shutdown();

    // Give TLS a chance to decrypt + copy any already-buffered plaintext
    // without calling recv on the socket. Useful when the peer piggybacks
    // application data at the end of the handshake.
    int drainBuffered(void* buf, int len);

private:
    struct Impl;
    Impl* m_p;
};

}
