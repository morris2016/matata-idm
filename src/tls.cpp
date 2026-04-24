#include "matata/tls.hpp"

#define SECURITY_WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sspi.h>
#include <schannel.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#pragma comment(lib, "secur32.lib")

namespace matata {

namespace {

constexpr size_t kIoBufSize = 16 * 1024 + 512;  // TLS record max + header

std::wstring schannelErr(const wchar_t* op, SECURITY_STATUS s) {
    std::wostringstream ss;
    ss << op << L" failed (SECURITY_STATUS=0x"
       << std::hex << (unsigned)s << L")";
    return ss.str();
}

int sockSendAll(SOCKET s, const void* buf, int len, std::wstring& err) {
    const char* p = (const char*)buf;
    int left = len;
    while (left > 0) {
        int n = send(s, p, left, 0);
        if (n == SOCKET_ERROR) {
            std::wostringstream ss; ss << L"send failed (WSA=" << WSAGetLastError() << L")";
            err = ss.str(); return -1;
        }
        if (n == 0) { err = L"send returned 0"; return -1; }
        p += n; left -= n;
    }
    return len;
}

int sockRecv(SOCKET s, void* buf, int len, std::wstring& err) {
    int n = recv(s, (char*)buf, len, 0);
    if (n == SOCKET_ERROR) {
        std::wostringstream ss; ss << L"recv failed (WSA=" << WSAGetLastError() << L")";
        err = ss.str(); return -1;
    }
    return n;
}

} // anon

// Process-wide client credential. Sharing this handle across every
// TlsStream enables Schannel's session cache, which is required for
// FTPS servers that enforce data-channel session resumption (rebex.net,
// vsftpd with require_ssl_reuse=YES, many strict compliance servers).
std::mutex g_credMu;
CredHandle g_cred{};
bool       g_haveCred = false;

static CredHandle* sharedCred(std::wstring& err);

struct TlsStream::Impl {
    SOCKET              sock        = INVALID_SOCKET;
    CredHandle*         cred        = nullptr;  // NOT owned; shared global
    CtxtHandle          ctx         {};
    bool                haveCtx     = false;
    SecPkgContext_StreamSizes sizes {};

    // Incoming ring buffer (raw TLS bytes from the socket, partially
    // decrypted). `inBuf` holds up to 2*kIoBufSize; `inLen` is the valid
    // portion starting at offset 0.
    std::vector<uint8_t> inBuf;
    size_t               inLen = 0;

    // Already-decrypted plaintext not yet copied out to the caller.
    std::vector<uint8_t> plain;
    size_t               plainRead = 0;
};

TlsStream::TlsStream() : m_p(new Impl()) {
    m_p->inBuf.resize(32 * 1024);
}

TlsStream::~TlsStream() {
    shutdown();
    if (m_p->haveCtx) DeleteSecurityContext(&m_p->ctx);
    // Shared credential is not freed here — it lives for the program's
    // lifetime so Schannel can cache TLS sessions across reconnects.
    delete m_p;
}

// Lazily acquire the shared client credential.
static CredHandle* sharedCred(std::wstring& err) {
    std::lock_guard<std::mutex> lk(g_credMu);
    if (g_haveCred) return &g_cred;

    SCHANNEL_CRED sc{};
    sc.dwVersion             = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2 | SP_PROT_TLS1_3;
    sc.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_DEFAULT_CREDS
               | SCH_CRED_AUTO_CRED_VALIDATION;

    TimeStamp ts;
    SECURITY_STATUS s = AcquireCredentialsHandleW(
        nullptr, (LPWSTR)UNISP_NAME_W, SECPKG_CRED_OUTBOUND,
        nullptr, &sc, nullptr, nullptr, &g_cred, &ts);
    if (s != SEC_E_OK) {
        err = schannelErr(L"AcquireCredentialsHandle(SCHANNEL_CRED)", s);
        return nullptr;
    }
    g_haveCred = true;
    return &g_cred;
}

bool TlsStream::handshakeClient(MATATA_SOCKET sock,
                                const std::wstring& hostname,
                                std::wstring& err) {
    m_p->sock = (SOCKET)sock;
    // Use the process-wide credential so Schannel can resume TLS
    // sessions (required by strict FTPS servers).
    m_p->cred = sharedCred(err);
    if (!m_p->cred) return false;

    DWORD reqFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT
                   | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY
                   | ISC_REQ_STREAM;
    DWORD outFlags = 0;

    // Output buffer: Schannel allocates this for us (we free with
    // FreeContextBuffer).
    SecBuffer outBuf{};
    outBuf.BufferType = SECBUFFER_TOKEN;
    SecBufferDesc outDesc{};
    outDesc.ulVersion = SECBUFFER_VERSION;
    outDesc.cBuffers  = 1;
    outDesc.pBuffers  = &outBuf;

    // First ISC call has no input — Schannel produces ClientHello.
    TimeStamp ts;
    SECURITY_STATUS s = InitializeSecurityContextW(
        m_p->cred,nullptr, (SEC_WCHAR*)hostname.c_str(),
        reqFlags, 0, 0, nullptr, 0,
        &m_p->ctx, &outDesc, &outFlags, &ts);
    if (s != SEC_I_CONTINUE_NEEDED && s != SEC_E_OK) {
        err = schannelErr(L"InitializeSecurityContext(initial)", s);
        return false;
    }
    m_p->haveCtx = true;
    if (outBuf.cbBuffer > 0 && outBuf.pvBuffer) {
        if (sockSendAll(m_p->sock, outBuf.pvBuffer, outBuf.cbBuffer, err) < 0) {
            FreeContextBuffer(outBuf.pvBuffer);
            return false;
        }
        FreeContextBuffer(outBuf.pvBuffer);
        outBuf.pvBuffer = nullptr;
        outBuf.cbBuffer = 0;
    }

    // Handshake loop: recv raw bytes, feed into ISC, send any output,
    // repeat until SEC_E_OK.
    while (s == SEC_I_CONTINUE_NEEDED || s == SEC_E_INCOMPLETE_MESSAGE) {
        int n = sockRecv(m_p->sock,
                         m_p->inBuf.data() + m_p->inLen,
                         (int)(m_p->inBuf.size() - m_p->inLen), err);
        if (n <= 0) {
            if (err.empty()) err = L"handshake: peer closed";
            return false;
        }
        m_p->inLen += (size_t)n;

        SecBuffer inBufs[2]{};
        inBufs[0].BufferType = SECBUFFER_TOKEN;
        inBufs[0].pvBuffer   = m_p->inBuf.data();
        inBufs[0].cbBuffer   = (DWORD)m_p->inLen;
        inBufs[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc inDesc{};
        inDesc.ulVersion = SECBUFFER_VERSION;
        inDesc.cBuffers  = 2;
        inDesc.pBuffers  = inBufs;

        outBuf = {};
        outBuf.BufferType = SECBUFFER_TOKEN;

        s = InitializeSecurityContextW(
            m_p->cred,&m_p->ctx, (SEC_WCHAR*)hostname.c_str(),
            reqFlags, 0, 0, &inDesc, 0,
            nullptr, &outDesc, &outFlags, &ts);

        if (s == SEC_E_INCOMPLETE_MESSAGE) continue;

        if (outBuf.cbBuffer > 0 && outBuf.pvBuffer) {
            if (sockSendAll(m_p->sock, outBuf.pvBuffer, outBuf.cbBuffer, err) < 0) {
                FreeContextBuffer(outBuf.pvBuffer);
                return false;
            }
            FreeContextBuffer(outBuf.pvBuffer);
        }

        // Schannel may have consumed only part of our inBuf; the
        // second SecBuffer (EXTRA) tells us what's left over.
        if (inBufs[1].BufferType == SECBUFFER_EXTRA) {
            size_t left = inBufs[1].cbBuffer;
            if (left > 0 && left <= m_p->inLen) {
                std::memmove(m_p->inBuf.data(),
                             m_p->inBuf.data() + (m_p->inLen - left), left);
            }
            m_p->inLen = left;
        } else {
            m_p->inLen = 0;
        }

        if (s == SEC_E_OK) break;
        if (s != SEC_I_CONTINUE_NEEDED && s != SEC_E_INCOMPLETE_MESSAGE) {
            err = schannelErr(L"InitializeSecurityContext(loop)", s);
            return false;
        }
    }

    // Cache record sizes for EncryptMessage / DecryptMessage.
    if (QueryContextAttributesW(&m_p->ctx, SECPKG_ATTR_STREAM_SIZES,
                                &m_p->sizes) != SEC_E_OK) {
        err = L"QueryContextAttributes(SECPKG_ATTR_STREAM_SIZES) failed";
        return false;
    }
    return true;
}

bool TlsStream::handshakeDataClient(MATATA_SOCKET sock,
                                    const std::wstring& hostname,
                                    const TlsStream& /*controlSession*/,
                                    std::wstring& err) {
    // Most FTPS servers support session reuse between control and data
    // channels, but requiring reuse creates portability headaches. We
    // just do a fresh handshake; performance is fine for our use case.
    return handshakeClient(sock, hostname, err);
}

int TlsStream::sendAll(const void* buf, int len, std::wstring& err) {
    if (!m_p->haveCtx) { err = L"no TLS context"; return -1; }
    const size_t hdr = m_p->sizes.cbHeader;
    const size_t trl = m_p->sizes.cbTrailer;
    const size_t maxRec = m_p->sizes.cbMaximumMessage;

    std::vector<uint8_t> record(hdr + maxRec + trl);

    const uint8_t* p = (const uint8_t*)buf;
    int left = len;
    while (left > 0) {
        size_t chunk = std::min<size_t>((size_t)left, maxRec);
        std::memcpy(record.data() + hdr, p, chunk);

        SecBuffer bufs[4]{};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer   = record.data();
        bufs[0].cbBuffer   = (DWORD)hdr;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer   = record.data() + hdr;
        bufs[1].cbBuffer   = (DWORD)chunk;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer   = record.data() + hdr + chunk;
        bufs[2].cbBuffer   = (DWORD)trl;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc d{};
        d.ulVersion = SECBUFFER_VERSION;
        d.cBuffers  = 4;
        d.pBuffers  = bufs;

        SECURITY_STATUS s = EncryptMessage(&m_p->ctx, 0, &d, 0);
        if (s != SEC_E_OK) { err = schannelErr(L"EncryptMessage", s); return -1; }

        size_t totalLen = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        if (sockSendAll(m_p->sock, record.data(), (int)totalLen, err) < 0)
            return -1;

        p += chunk; left -= (int)chunk;
    }
    return len;
}

int TlsStream::drainBuffered(void* buf, int len) {
    if (m_p->plainRead < m_p->plain.size()) {
        size_t avail = m_p->plain.size() - m_p->plainRead;
        size_t copy  = std::min<size_t>(avail, (size_t)len);
        std::memcpy(buf, m_p->plain.data() + m_p->plainRead, copy);
        m_p->plainRead += copy;
        if (m_p->plainRead == m_p->plain.size()) {
            m_p->plain.clear(); m_p->plainRead = 0;
        }
        return (int)copy;
    }
    return 0;
}

int TlsStream::recvSome(void* buf, int len, std::wstring& err) {
    // Serve any leftover plaintext first.
    int copied = drainBuffered(buf, len);
    if (copied > 0) return copied;

    if (!m_p->haveCtx) { err = L"no TLS context"; return -1; }

    for (;;) {
        // If we have ciphertext, try to decrypt.
        if (m_p->inLen > 0) {
            SecBuffer bufs[4]{};
            bufs[0].BufferType = SECBUFFER_DATA;
            bufs[0].pvBuffer   = m_p->inBuf.data();
            bufs[0].cbBuffer   = (DWORD)m_p->inLen;
            bufs[1].BufferType = SECBUFFER_EMPTY;
            bufs[2].BufferType = SECBUFFER_EMPTY;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc d{};
            d.ulVersion = SECBUFFER_VERSION;
            d.cBuffers  = 4;
            d.pBuffers  = bufs;

            SECURITY_STATUS s = DecryptMessage(&m_p->ctx, &d, 0, nullptr);
            if (s == SEC_I_CONTEXT_EXPIRED) return 0;             // close_notify
            if (s == SEC_I_RENEGOTIATE)     { err = L"renegotiation not supported"; return -1; }
            if (s == SEC_E_INCOMPLETE_MESSAGE) {
                // fall through to read more
            } else if (s != SEC_E_OK) {
                err = schannelErr(L"DecryptMessage", s);
                return -1;
            } else {
                SecBuffer* dataBuf  = nullptr;
                SecBuffer* extraBuf = nullptr;
                for (int i = 0; i < 4; ++i) {
                    if (bufs[i].BufferType == SECBUFFER_DATA)  dataBuf  = &bufs[i];
                    if (bufs[i].BufferType == SECBUFFER_EXTRA) extraBuf = &bufs[i];
                }
                size_t plainLen = dataBuf ? dataBuf->cbBuffer : 0;
                if (plainLen > 0) {
                    size_t copy = std::min<size_t>(plainLen, (size_t)len);
                    std::memcpy(buf, dataBuf->pvBuffer, copy);
                    if (plainLen > copy) {
                        // Stash the rest for a future recvSome.
                        m_p->plain.assign((uint8_t*)dataBuf->pvBuffer + copy,
                                          (uint8_t*)dataBuf->pvBuffer + plainLen);
                        m_p->plainRead = 0;
                    }
                    // Slide leftover ciphertext (EXTRA) to front.
                    if (extraBuf && extraBuf->cbBuffer > 0) {
                        size_t left = extraBuf->cbBuffer;
                        std::memmove(m_p->inBuf.data(),
                                     m_p->inBuf.data() + (m_p->inLen - left),
                                     left);
                        m_p->inLen = left;
                    } else {
                        m_p->inLen = 0;
                    }
                    return (int)copy;
                }
                // No plaintext emitted (e.g., handshake messages
                // interleaved). Loop to pull more bytes.
                if (extraBuf && extraBuf->cbBuffer > 0) {
                    size_t left = extraBuf->cbBuffer;
                    std::memmove(m_p->inBuf.data(),
                                 m_p->inBuf.data() + (m_p->inLen - left), left);
                    m_p->inLen = left;
                } else {
                    m_p->inLen = 0;
                }
            }
        }

        // Need more ciphertext from the wire.
        if (m_p->inLen >= m_p->inBuf.size()) {
            err = L"TLS record buffer overflow";
            return -1;
        }
        int n = sockRecv(m_p->sock,
                         m_p->inBuf.data() + m_p->inLen,
                         (int)(m_p->inBuf.size() - m_p->inLen), err);
        if (n == 0) return 0;  // clean TCP EOF
        if (n < 0)  return -1;
        m_p->inLen += (size_t)n;
    }
}

void TlsStream::shutdown() {
    if (!m_p->haveCtx) return;
    DWORD shutdownCode = SCHANNEL_SHUTDOWN;
    SecBuffer sb{};
    sb.BufferType = SECBUFFER_TOKEN;
    sb.pvBuffer   = &shutdownCode;
    sb.cbBuffer   = sizeof(shutdownCode);
    SecBufferDesc sbd{};
    sbd.ulVersion = SECBUFFER_VERSION;
    sbd.cBuffers  = 1;
    sbd.pBuffers  = &sb;
    ApplyControlToken(&m_p->ctx, &sbd);

    // Generate the close_notify alert.
    SecBuffer out{};
    out.BufferType = SECBUFFER_TOKEN;
    SecBufferDesc outDesc{};
    outDesc.ulVersion = SECBUFFER_VERSION;
    outDesc.cBuffers  = 1;
    outDesc.pBuffers  = &out;

    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT
                | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY
                | ISC_REQ_STREAM;
    DWORD outFlags = 0;
    TimeStamp ts;
    SECURITY_STATUS s = InitializeSecurityContextW(
        m_p->cred,&m_p->ctx, nullptr,
        flags, 0, 0, nullptr, 0,
        nullptr, &outDesc, &outFlags, &ts);
    if (s == SEC_E_OK && out.cbBuffer > 0 && out.pvBuffer) {
        std::wstring dummy;
        sockSendAll(m_p->sock, out.pvBuffer, out.cbBuffer, dummy);
        FreeContextBuffer(out.pvBuffer);
    }
}

}
