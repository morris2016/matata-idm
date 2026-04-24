#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace matata {

struct TorrentFile {
    std::wstring path;      // relative, forward-slash separated
    int64_t      length = 0;
};

struct TorrentMeta {
    // 20-byte SHA-1 of the bencoded `info` dict.
    uint8_t                  infoHash[20]{};
    std::wstring             name;             // suggested display name
    int64_t                  pieceLength = 0;
    std::vector<uint8_t>     pieces;           // concatenated 20-byte piece hashes
    std::vector<std::wstring> trackers;        // announce + announce-list
    std::vector<TorrentFile> files;            // single-file torrents have one entry
    int64_t                  totalLength = 0;
    std::wstring             comment;
    std::wstring             createdBy;
};

// Parse a .torrent file's bytes. Populates infoHash via SHA-1 over the
// raw info-dict bytes.
bool torrentParseFile(const uint8_t* data, size_t len,
                      TorrentMeta& out, std::wstring& err);

// Parse a magnet:?xt=urn:btih:<hash>&dn=<name>&tr=<tracker>[&tr=...] URI.
// Populates infoHash, name, and trackers; piece info is unknown until
// fetched from a peer via the BitTorrent extension protocol (BEP 9),
// which is beyond what we support without a real peer stack.
bool torrentParseMagnet(const std::wstring& magnet,
                        TorrentMeta& out, std::wstring& err);

bool looksLikeMagnetUri(const std::wstring& s);

}
