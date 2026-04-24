#include "matata/dash.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <sstream>
#include <unordered_map>

namespace matata {

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const char* d = s.data();
    size_t len = s.size();
    if (len >= 3 && (unsigned char)d[0] == 0xEF
                 && (unsigned char)d[1] == 0xBB
                 && (unsigned char)d[2] == 0xBF) { d += 3; len -= 3; }
    int n = MultiByteToWideChar(CP_UTF8, 0, d, (int)len, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, d, (int)len, w.data(), n);
    return w;
}

std::wstring lowerAscii(std::wstring s) {
    for (auto& c : s) if (c < 128) c = (wchar_t)towlower(c);
    return s;
}

// -----------------------------------------------------------------------
// Minimal XML parser.
// Builds a tree of Elements with attributes and children. Enough for DASH;
// not a conformant parser (no DTD, no namespaces beyond `prefix:local`,
// no CDATA, no entity expansion beyond the five predefined ones).

struct Element {
    std::wstring                               name;   // local name (no prefix)
    std::unordered_map<std::wstring, std::wstring> attrs;
    std::vector<Element>                       children;
    std::wstring                               text;   // aggregated inner text
};

class XmlParser {
public:
    bool parse(const std::wstring& xml, Element& root, std::wstring& err) {
        m_s = &xml; m_i = 0;
        skipProlog();
        skipWs();
        if (m_i >= m_s->size() || (*m_s)[m_i] != L'<') {
            err = L"no root element"; return false;
        }
        if (!parseElement(root)) { err = m_err; return false; }
        return true;
    }

private:
    const std::wstring* m_s = nullptr;
    size_t              m_i = 0;
    std::wstring        m_err;

    wchar_t at(size_t i) const { return (i < m_s->size()) ? (*m_s)[i] : 0; }
    void skipWs() {
        while (m_i < m_s->size()) {
            wchar_t c = (*m_s)[m_i];
            if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r') ++m_i;
            else break;
        }
    }
    void skipProlog() {
        // Skip <?xml ... ?>, comments, doctype.
        while (m_i < m_s->size()) {
            skipWs();
            if (m_i + 1 < m_s->size() && (*m_s)[m_i] == L'<') {
                if (at(m_i + 1) == L'?') {
                    auto end = m_s->find(L"?>", m_i + 2);
                    if (end == std::wstring::npos) { m_i = m_s->size(); return; }
                    m_i = end + 2;
                    continue;
                }
                if (at(m_i + 1) == L'!') {
                    if (m_s->compare(m_i, 4, L"<!--") == 0) {
                        auto end = m_s->find(L"-->", m_i + 4);
                        if (end == std::wstring::npos) { m_i = m_s->size(); return; }
                        m_i = end + 3;
                        continue;
                    }
                    // DOCTYPE — skip to closing '>'
                    auto end = m_s->find(L'>', m_i);
                    if (end == std::wstring::npos) { m_i = m_s->size(); return; }
                    m_i = end + 1;
                    continue;
                }
            }
            break;
        }
    }

    std::wstring readName() {
        size_t start = m_i;
        while (m_i < m_s->size()) {
            wchar_t c = (*m_s)[m_i];
            if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' ||
                c == L'/' || c == L'>' || c == L'=') break;
            ++m_i;
        }
        return m_s->substr(start, m_i - start);
    }

    std::wstring localName(const std::wstring& qname) {
        auto c = qname.find(L':');
        return (c == std::wstring::npos) ? qname : qname.substr(c + 1);
    }

    std::wstring decodeEntities(std::wstring s) {
        std::wstring out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            if (s[i] == L'&') {
                auto end = s.find(L';', i);
                if (end != std::wstring::npos) {
                    std::wstring e = s.substr(i + 1, end - i - 1);
                    if (e == L"amp")  out.push_back(L'&');
                    else if (e == L"lt") out.push_back(L'<');
                    else if (e == L"gt") out.push_back(L'>');
                    else if (e == L"quot") out.push_back(L'"');
                    else if (e == L"apos") out.push_back(L'\'');
                    else if (!e.empty() && e[0] == L'#') {
                        unsigned v = 0;
                        if (e.size() > 1 && (e[1] == L'x' || e[1] == L'X')) {
                            for (size_t k = 2; k < e.size(); ++k) {
                                wchar_t c = e[k]; v <<= 4;
                                if      (c >= L'0' && c <= L'9') v |= c - L'0';
                                else if (c >= L'a' && c <= L'f') v |= 10 + (c - L'a');
                                else if (c >= L'A' && c <= L'F') v |= 10 + (c - L'A');
                            }
                        } else {
                            for (size_t k = 1; k < e.size(); ++k) {
                                wchar_t c = e[k];
                                if (c < L'0' || c > L'9') { v = 0; break; }
                                v = v * 10 + (c - L'0');
                            }
                        }
                        if (v) out.push_back((wchar_t)v);
                    } else {
                        out.append(s, i, end - i + 1); // unknown entity, pass through
                    }
                    i = end + 1;
                    continue;
                }
            }
            out.push_back(s[i++]);
        }
        return out;
    }

    bool parseAttrs(std::unordered_map<std::wstring, std::wstring>& out) {
        while (true) {
            skipWs();
            if (m_i >= m_s->size()) { m_err = L"unexpected EOF in tag"; return false; }
            wchar_t c = (*m_s)[m_i];
            if (c == L'/' || c == L'>') return true;
            std::wstring qn = readName();
            if (qn.empty()) { m_err = L"bad attr name"; return false; }
            skipWs();
            if (m_i >= m_s->size() || (*m_s)[m_i] != L'=') { m_err = L"attr missing ="; return false; }
            ++m_i;
            skipWs();
            if (m_i >= m_s->size()) { m_err = L"EOF in attr"; return false; }
            wchar_t q = (*m_s)[m_i];
            if (q != L'"' && q != L'\'') { m_err = L"unquoted attr value"; return false; }
            ++m_i;
            size_t start = m_i;
            while (m_i < m_s->size() && (*m_s)[m_i] != q) ++m_i;
            if (m_i >= m_s->size()) { m_err = L"EOF in attr value"; return false; }
            std::wstring val = decodeEntities(m_s->substr(start, m_i - start));
            ++m_i; // consume closing quote
            out[localName(qn)] = std::move(val);
        }
    }

    bool parseElement(Element& el) {
        if (m_i >= m_s->size() || (*m_s)[m_i] != L'<') { m_err = L"expected '<'"; return false; }
        ++m_i;
        std::wstring qn = readName();
        if (qn.empty()) { m_err = L"empty tag name"; return false; }
        el.name = localName(qn);
        if (!parseAttrs(el.attrs)) return false;
        skipWs();
        if (m_i < m_s->size() && (*m_s)[m_i] == L'/') {
            ++m_i;
            if (m_i >= m_s->size() || (*m_s)[m_i] != L'>') { m_err = L"bad self-close"; return false; }
            ++m_i;
            return true;
        }
        if (m_i >= m_s->size() || (*m_s)[m_i] != L'>') { m_err = L"expected '>'"; return false; }
        ++m_i;

        // Children and text until closing tag.
        while (m_i < m_s->size()) {
            if ((*m_s)[m_i] == L'<') {
                if (m_i + 1 < m_s->size() && (*m_s)[m_i + 1] == L'/') {
                    m_i += 2;
                    std::wstring endName = readName();
                    skipWs();
                    if (m_i >= m_s->size() || (*m_s)[m_i] != L'>') {
                        m_err = L"bad closing tag"; return false;
                    }
                    ++m_i;
                    (void)endName;
                    return true;
                }
                if (m_i + 3 < m_s->size() && m_s->compare(m_i, 4, L"<!--") == 0) {
                    auto end = m_s->find(L"-->", m_i + 4);
                    if (end == std::wstring::npos) { m_err = L"unterminated comment"; return false; }
                    m_i = end + 3;
                    continue;
                }
                Element child;
                if (!parseElement(child)) return false;
                el.children.push_back(std::move(child));
            } else {
                size_t s = m_i;
                while (m_i < m_s->size() && (*m_s)[m_i] != L'<') ++m_i;
                el.text += decodeEntities(m_s->substr(s, m_i - s));
            }
        }
        m_err = L"unexpected EOF";
        return false;
    }
};

// -----------------------------------------------------------------------

std::wstring attrOr(const Element& e, const wchar_t* key, const wchar_t* dflt) {
    auto it = e.attrs.find(key);
    return (it == e.attrs.end()) ? std::wstring(dflt) : it->second;
}

int64_t attrInt(const Element& e, const wchar_t* key, int64_t dflt) {
    auto it = e.attrs.find(key);
    if (it == e.attrs.end()) return dflt;
    try { return std::stoll(it->second); } catch (...) { return dflt; }
}

const Element* firstChild(const Element& e, const wchar_t* name) {
    for (auto& c : e.children) if (c.name == name) return &c;
    return nullptr;
}

// Apply $Number$, $Time$, $RepresentationID$, $Bandwidth$ substitution
// (plus formatted %0Nd spec: $Number%05d$ -> 00042).
std::wstring fillTemplate(const std::wstring& tpl,
                          const std::wstring& repId,
                          int64_t bandwidth,
                          int64_t number,
                          int64_t time) {
    std::wstring out;
    out.reserve(tpl.size() + 16);
    for (size_t i = 0; i < tpl.size(); ) {
        if (tpl[i] == L'$') {
            auto end = tpl.find(L'$', i + 1);
            if (end == std::wstring::npos) { out.push_back(tpl[i++]); continue; }
            std::wstring token = tpl.substr(i + 1, end - i - 1);
            if (token.empty()) { out.push_back(L'$'); i = end + 1; continue; }
            // Split at optional '%' format spec.
            std::wstring name = token, fmt;
            auto pct = token.find(L'%');
            if (pct != std::wstring::npos) {
                name = token.substr(0, pct);
                fmt  = token.substr(pct);
            }
            std::wstring replacement;
            auto applyIntFormat = [&](int64_t v) {
                if (fmt.empty()) {
                    replacement = std::to_wstring(v);
                } else {
                    // Accept classic printf like %05d.
                    wchar_t buf[64];
                    std::wstring fullFmt = L"%" + fmt.substr(1);
                    if (fullFmt.find(L"lld") == std::wstring::npos) {
                        // Replace trailing 'd' with 'lld' so long long prints right.
                        auto d = fullFmt.find_last_of(L'd');
                        if (d != std::wstring::npos) fullFmt.replace(d, 1, L"lld");
                    }
                    _snwprintf_s(buf, _TRUNCATE, fullFmt.c_str(), (long long)v);
                    replacement = buf;
                }
            };
            if      (name == L"RepresentationID") replacement = repId;
            else if (name == L"Bandwidth")        applyIntFormat(bandwidth);
            else if (name == L"Number")           applyIntFormat(number);
            else if (name == L"Time")             applyIntFormat(time);
            else                                   replacement = token; // unknown — pass through
            out += replacement;
            i = end + 1;
        } else {
            out.push_back(tpl[i++]);
        }
    }
    return out;
}

std::wstring joinBase(const Url& base,
                      const std::wstring& mpdBaseUrl,
                      const std::wstring& periodBaseUrl,
                      const std::wstring& asBaseUrl,
                      const std::wstring& repBaseUrl,
                      const std::wstring& tail) {
    // Resolve tail against a cascade of <BaseURL>s relative to `base`.
    std::wstring cur = base.toString();
    Url b = base;
    auto apply = [&](const std::wstring& maybeRel) {
        if (maybeRel.empty()) return;
        cur = resolveUrl(b, maybeRel);
        // update b to new cur so relatives chain correctly
        Url nb; std::wstring e;
        if (parseUrl(cur, nb, e)) b = nb;
    };
    apply(mpdBaseUrl);
    apply(periodBaseUrl);
    apply(asBaseUrl);
    apply(repBaseUrl);
    apply(tail);
    return cur;
}

// Build representation segments from a SegmentTemplate (optionally with
// a SegmentTimeline).
void fillFromSegmentTemplate(const Element& st,
                             const Element* timeline,
                             double periodDurationS,
                             DashRepresentation& rep,
                             const Url& base,
                             const std::wstring& mpdBase,
                             const std::wstring& periodBase,
                             const std::wstring& asBase,
                             const std::wstring& repBase) {
    std::wstring initTmpl  = attrOr(st, L"initialization", L"");
    std::wstring mediaTmpl = attrOr(st, L"media",          L"");
    int64_t timescale      = attrInt(st, L"timescale", 1);
    int64_t duration       = attrInt(st, L"duration",  0);
    int64_t startNumber    = attrInt(st, L"startNumber", 1);

    if (!initTmpl.empty()) {
        std::wstring initRel = fillTemplate(initTmpl, rep.id, rep.bandwidth, 0, 0);
        rep.initUrl = joinBase(base, mpdBase, periodBase, asBase, repBase, initRel);
    }

    if (timeline) {
        // <S t="..." d="..." r="..."/> entries.
        int64_t number = startNumber;
        int64_t t = 0;
        bool    haveT = false;
        for (auto& s : timeline->children) {
            if (s.name != L"S") continue;
            int64_t sT = attrInt(s, L"t", -1);
            int64_t sD = attrInt(s, L"d", 0);
            int64_t sR = attrInt(s, L"r", 0);
            if (sT >= 0) { t = sT; haveT = true; } else if (!haveT) { t = 0; haveT = true; }
            for (int64_t k = 0; k <= sR; ++k) {
                std::wstring rel = fillTemplate(mediaTmpl, rep.id, rep.bandwidth, number, t);
                rep.segmentUrls.push_back(
                    joinBase(base, mpdBase, periodBase, asBase, repBase, rel));
                t += sD;
                ++number;
            }
        }
        return;
    }

    // Template without timeline: use period duration / segment duration.
    if (duration <= 0 || periodDurationS <= 0) return;
    double segSec = (double)duration / (double)timescale;
    if (segSec <= 0) return;
    int64_t count = (int64_t)std::ceil(periodDurationS / segSec);
    for (int64_t k = 0; k < count; ++k) {
        std::wstring rel = fillTemplate(mediaTmpl, rep.id, rep.bandwidth,
                                        startNumber + k, k * duration);
        rep.segmentUrls.push_back(
            joinBase(base, mpdBase, periodBase, asBase, repBase, rel));
    }
}

// Parse an ISO-8601 duration like PT1H2M3.5S (only the common cases).
double parseIsoDurationSeconds(const std::wstring& s) {
    if (s.size() < 2 || s[0] != L'P') return 0.0;
    double total = 0.0;
    bool   inTime = false;
    std::wstring num;
    auto flush = [&](wchar_t unit) {
        if (num.empty()) return;
        double v = 0.0;
        try { v = std::stod(num); } catch (...) { v = 0; }
        num.clear();
        if (inTime) {
            if (unit == L'H') total += v * 3600.0;
            else if (unit == L'M') total += v * 60.0;
            else if (unit == L'S') total += v;
        } else {
            if (unit == L'D') total += v * 86400.0;
            // years / months skipped
        }
    };
    for (size_t i = 1; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'T') { inTime = true; continue; }
        if ((c >= L'0' && c <= L'9') || c == L'.') { num.push_back(c); continue; }
        flush(c);
    }
    return total;
}

} // anon

bool looksLikeDashUrl(const std::wstring& url) {
    std::wstring u = url;
    auto q = u.find(L'?');
    if (q != std::wstring::npos) u.resize(q);
    if (u.size() < 4) return false;
    std::wstring tail = lowerAscii(u.substr(u.size() - 4));
    return tail == L".mpd";
}

bool parseDash(const std::wstring& xmlBody, const Url& baseUrl,
               DashManifest& out, std::wstring& err) {
    out = DashManifest{};
    out.manifestUrl = baseUrl;

    XmlParser xp;
    Element mpd;
    if (!xp.parse(xmlBody, mpd, err)) return false;
    if (mpd.name != L"MPD") { err = L"root element is not MPD"; return false; }

    if (attrOr(mpd, L"type", L"static") == L"dynamic") {
        out.unsupported = true;
        out.warning = L"live DASH manifest (type=dynamic) not supported yet";
    }

    double presentationSec = parseIsoDurationSeconds(
        attrOr(mpd, L"mediaPresentationDuration", L""));

    std::wstring mpdBase;
    if (auto b = firstChild(mpd, L"BaseURL")) mpdBase = b->text;

    for (auto& period : mpd.children) {
        if (period.name != L"Period") continue;
        double periodSec = parseIsoDurationSeconds(attrOr(period, L"duration", L""));
        if (periodSec <= 0) periodSec = presentationSec;

        std::wstring periodBase;
        if (auto b = firstChild(period, L"BaseURL")) periodBase = b->text;

        for (auto& aset : period.children) {
            if (aset.name != L"AdaptationSet") continue;
            std::wstring asMime = attrOr(aset, L"mimeType", L"");
            std::wstring asBase;
            if (auto b = firstChild(aset, L"BaseURL")) asBase = b->text;
            const Element* asSegTemplate = firstChild(aset, L"SegmentTemplate");

            for (auto& rep : aset.children) {
                if (rep.name != L"Representation") continue;
                DashRepresentation R;
                R.id        = attrOr(rep, L"id", L"");
                R.bandwidth = attrInt(rep, L"bandwidth", 0);
                R.width     = (int)attrInt(rep, L"width", 0);
                R.height    = (int)attrInt(rep, L"height", 0);
                R.mimeType  = attrOr(rep, L"mimeType", asMime.c_str());
                R.codecs    = attrOr(rep, L"codecs",
                                     attrOr(aset, L"codecs", L"").c_str());

                std::wstring repBase;
                if (auto b = firstChild(rep, L"BaseURL")) repBase = b->text;

                const Element* repSegTemplate = firstChild(rep, L"SegmentTemplate");
                const Element* st = repSegTemplate ? repSegTemplate : asSegTemplate;
                const Element* repSegList = firstChild(rep, L"SegmentList");

                if (st) {
                    const Element* timeline = firstChild(*st, L"SegmentTimeline");
                    fillFromSegmentTemplate(*st, timeline, periodSec, R, baseUrl,
                                            mpdBase, periodBase, asBase, repBase);
                } else if (repSegList) {
                    const Element* init = firstChild(*repSegList, L"Initialization");
                    if (init) {
                        std::wstring sur = attrOr(*init, L"sourceURL", L"");
                        if (!sur.empty()) {
                            R.initUrl = joinBase(baseUrl, mpdBase, periodBase,
                                                 asBase, repBase, sur);
                        }
                    }
                    for (auto& su : repSegList->children) {
                        if (su.name != L"SegmentURL") continue;
                        std::wstring m = attrOr(su, L"media", L"");
                        if (m.empty()) continue;
                        R.segmentUrls.push_back(
                            joinBase(baseUrl, mpdBase, periodBase,
                                     asBase, repBase, m));
                    }
                } else {
                    // No template, no list: a single BaseURL for the whole rep.
                    std::wstring single = joinBase(baseUrl, mpdBase, periodBase,
                                                   asBase, repBase, L"");
                    if (!single.empty())
                        R.segmentUrls.push_back(single);
                }

                std::wstring mime = lowerAscii(R.mimeType);
                if (mime.find(L"video") != std::wstring::npos) {
                    out.videoReps.push_back(std::move(R));
                } else if (mime.find(L"audio") != std::wstring::npos) {
                    out.audioReps.push_back(std::move(R));
                } else if (R.width > 0 || R.height > 0) {
                    out.videoReps.push_back(std::move(R));
                } else {
                    out.audioReps.push_back(std::move(R));
                }
            }
        }
        break;  // only the first Period for v0.5.x
    }

    if (out.videoReps.empty() && out.audioReps.empty()) {
        err = L"MPD contained no representations";
        return false;
    }
    err.clear();
    return true;
}

bool fetchAndParseDash(const Url& url, const ExtraHeaders& headers,
                       DashManifest& out, std::wstring& err) {
    std::string body;
    int status = httpFetchBody(url, headers, body, err);
    if (status < 200 || status >= 300) {
        if (err.empty()) {
            std::wostringstream s; s << L"HTTP " << status;
            err = s.str();
        }
        return false;
    }
    std::wstring xml = utf8ToWide(body);
    return parseDash(xml, url, out, err);
}

const DashRepresentation* pickDashVariant(const std::vector<DashRepresentation>& reps,
                                          const std::wstring& quality) {
    if (reps.empty()) return nullptr;
    std::wstring q = lowerAscii(quality);
    if (q.empty() || q == L"best") {
        const DashRepresentation* best = &reps[0];
        for (auto& r : reps) if (r.bandwidth > best->bandwidth) best = &r;
        return best;
    }
    if (q == L"worst") {
        const DashRepresentation* worst = &reps[0];
        for (auto& r : reps) if (r.bandwidth < worst->bandwidth) worst = &r;
        return worst;
    }
    std::wstring digits;
    for (wchar_t c : q) if (c >= L'0' && c <= L'9') digits.push_back(c);
    if (!digits.empty()) {
        int target = 0;
        try { target = std::stoi(digits); } catch (...) { target = 0; }
        if (target > 0) {
            const DashRepresentation* pick = nullptr;
            for (auto& r : reps) {
                if (r.height == target) return &r;
                if (r.height > 0 && r.height <= target) {
                    if (!pick || r.height > pick->height) pick = &r;
                }
            }
            if (pick) return pick;
        }
    }
    const DashRepresentation* best = &reps[0];
    for (auto& r : reps) if (r.bandwidth > best->bandwidth) best = &r;
    return best;
}

}
