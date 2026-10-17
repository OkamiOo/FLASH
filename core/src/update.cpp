#include "flash/update.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <monocypher.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sstream>

namespace flash::update {

namespace {

struct Part {
	const void *data;
	size_t size;
};

void sha512(uint8_t out[64], std::initializer_list<Part> parts)
{
	BCRYPT_HASH_HANDLE h = nullptr;
	BCryptCreateHash(BCRYPT_SHA512_ALG_HANDLE, &h, nullptr, 0, nullptr, 0, 0);
	for (const Part &p : parts)
		if (p.size)
			BCryptHashData(h, (PUCHAR)p.data, (ULONG)p.size, 0);
	BCryptFinishHash(h, out, 64, 0);
	BCryptDestroyHash(h);
}

// SHA-512 des morceaux, réduit modulo l'ordre du groupe (RFC 8032)
void hash_reduce(uint8_t out[32], std::initializer_list<Part> parts)
{
	uint8_t h[64];
	sha512(h, parts);
	crypto_eddsa_reduce(out, h);
}

} // namespace

Sha256 sha256(const void *data, size_t size)
{
	Sha256 out{};
	BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data, (ULONG)size, out.data(), (ULONG)out.size());
	return out;
}

std::string hex(const uint8_t *data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string s(size * 2, '0');
	for (size_t i = 0; i < size; i++) {
		s[2 * i] = digits[data[i] >> 4];
		s[2 * i + 1] = digits[data[i] & 15];
	}
	return s;
}

bool from_hex(const std::string &text, std::vector<uint8_t> &out)
{
	std::string t;
	for (char c : text)
		if (!std::isspace((unsigned char)c))
			t.push_back(c);
	if (t.size() % 2)
		return false;
	auto value = [](char c) {
		return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
	};
	out.clear();
	for (size_t i = 0; i < t.size(); i += 2) {
		const int hi = value(t[i]), lo = value(t[i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		out.push_back((uint8_t)(hi * 16 + lo));
	}
	return true;
}

// ---------- Ed25519 (fonctions de bas niveau de Monocypher, SHA-512 de Windows) ----------

void ed25519_public_key(uint8_t public_key[32], const uint8_t seed[32])
{
	uint8_t a[64];
	sha512(a, {{seed, 32}});
	crypto_eddsa_trim_scalar(a, a);
	crypto_eddsa_scalarbase(public_key, a);
	crypto_wipe(a, sizeof(a));
}

void ed25519_sign(uint8_t signature[64], const uint8_t seed[32], const void *message, size_t size)
{
	uint8_t a[64], r[32], h[32], R[32], A[32];
	sha512(a, {{seed, 32}});
	crypto_eddsa_trim_scalar(a, a);
	crypto_eddsa_scalarbase(A, a);
	hash_reduce(r, {{a + 32, 32}, {message, size}});
	crypto_eddsa_scalarbase(R, r);
	hash_reduce(h, {{R, 32}, {A, 32}, {message, size}});
	std::memcpy(signature, R, 32);
	crypto_eddsa_mul_add(signature + 32, h, a, r);
	crypto_wipe(a, sizeof(a));
	crypto_wipe(r, sizeof(r));
}

bool ed25519_check(const uint8_t signature[64], const uint8_t public_key[32], const void *message, size_t size)
{
	uint8_t h[32];
	hash_reduce(h, {{signature, 32}, {public_key, 32}, {message, size}});
	return crypto_eddsa_check_equation(signature, public_key, h) == 0;
}

// ---------- Manifeste ----------

bool safe_path(const std::string &path)
{
	if (path.empty() || path.size() > 200 || path.front() == '/')
		return false;
	std::stringstream ss(path);
	std::string part;
	while (std::getline(ss, part, '/')) {
		if (part.empty() || part[0] == '.' || part.find_first_of("\\:*?\"<>|") != std::string::npos)
			return false;
		for (char c : part)
			if ((unsigned char)c < 32)
				return false;
	}
	return path.back() != '/';
}

std::string flat_name(const std::string &path)
{
	std::string s = path;
	for (char &c : s)
		if (c == '/')
			c = '.';
	return s;
}

bool parse_manifest(const std::string &text, const std::string &signature_hex, const uint8_t public_key[32],
		    Manifest &out, std::string &error)
{
	std::vector<uint8_t> sig;
	if (!from_hex(signature_hex, sig) || sig.size() != 64) {
		error = "signature illisible";
		return false;
	}
	if (!ed25519_check(sig.data(), public_key, text.data(), text.size())) {
		error = "signature invalide : mise à jour refusée";
		return false;
	}

	std::stringstream ss(text);
	std::string line;
	if (!std::getline(ss, line) || line != "FLASH-MAJ 1") {
		error = "format de manifeste inconnu";
		return false;
	}
	out = Manifest{};
	while (std::getline(ss, line)) {
		const size_t sp = line.find(' ');
		const std::string key = line.substr(0, sp);
		const std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
		if (key == "version") {
			out.version = rest;
		} else if (key == "notes") {
			out.notes = rest;
		} else if (key == "fichier") {
			// « taille empreinte chemin », le chemin pouvant contenir des espaces
			const size_t a = rest.find(' ');
			const size_t b = a == std::string::npos ? a : rest.find(' ', a + 1);
			if (b == std::string::npos) {
				error = "ligne de fichier illisible";
				return false;
			}
			File file;
			char *end = nullptr;
			file.size = std::strtoull(rest.c_str(), &end, 10);
			if (end != rest.c_str() + a) {
				error = "taille illisible";
				return false;
			}
			const std::string digest = rest.substr(a + 1, b - a - 1);
			file.path = rest.substr(b + 1);
			std::vector<uint8_t> d;
			if (!from_hex(digest, d) || d.size() != 32) {
				error = "empreinte illisible";
				return false;
			}
			if (!safe_path(file.path)) {
				error = "chemin refusé : " + file.path;
				return false;
			}
			std::memcpy(file.sha256.data(), d.data(), 32);
			out.files.push_back(file);
		}
	}
	if (out.version.empty()) {
		error = "version manquante";
		return false;
	}
	return true;
}

std::string write_manifest(const Manifest &m)
{
	std::string s = "FLASH-MAJ 1\nversion " + m.version + "\nnotes " + m.notes + "\n";
	for (const File &f : m.files)
		s += "fichier " + std::to_string(f.size) + " " + hex(f.sha256.data(), f.sha256.size()) + " " + f.path + "\n";
	return s;
}

bool newer(const std::string &a, const std::string &b)
{
	auto parts = [](const std::string &v) {
		std::vector<unsigned> n;
		std::stringstream ss(v);
		std::string p;
		while (std::getline(ss, p, '.'))
			n.push_back((unsigned)std::strtoul(p.c_str(), nullptr, 10));
		return n;
	};
	return parts(a) > parts(b);
}

// ---------- Réseau (WinHTTP) ----------

namespace {

struct Handle {
	HINTERNET h = nullptr;
	~Handle()
	{
		if (h)
			WinHttpCloseHandle(h);
	}
};

std::wstring widen(const std::string &s)
{
	std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
	return w;
}

} // namespace

bool download(const std::string &url, std::vector<uint8_t> &out, std::string &error, size_t max_size)
{
	bool secure;
	std::string rest;
	if (url.rfind("https://", 0) == 0) {
		secure = true;
		rest = url.substr(8);
	} else if (url.rfind("http://", 0) == 0) {
		secure = false;
		rest = url.substr(7);
	} else {
		error = "adresse invalide";
		return false;
	}
	const size_t slash = rest.find('/');
	std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
	const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
	INTERNET_PORT port = secure ? 443 : 80;
	if (const size_t colon = host_port.rfind(':'); colon != std::string::npos) {
		port = (INTERNET_PORT)std::strtoul(host_port.c_str() + colon + 1, nullptr, 10);
		host_port.resize(colon);
	}
	if (!secure && host_port != "127.0.0.1" && host_port != "localhost") {
		error = "adresse non chiffrée refusée";
		return false;
	}

	Handle session{WinHttpOpen(L"FLASH", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
	if (!session.h) {
		error = "réseau indisponible";
		return false;
	}
	WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 30000);
	Handle connection{WinHttpConnect(session.h, widen(host_port).c_str(), port, 0)};
	Handle request{connection.h ? WinHttpOpenRequest(connection.h, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
							 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
				    : nullptr};
	if (!request.h || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(request.h, nullptr)) {
		error = "téléchargement impossible (" + std::to_string(GetLastError()) + ")";
		return false;
	}
	DWORD status = 0, len = sizeof(status);
	WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			    &status, &len, WINHTTP_NO_HEADER_INDEX);
	if (status != 200) {
		error = "réponse " + std::to_string(status);
		return false;
	}
	out.clear();
	std::vector<uint8_t> buf(64 * 1024);
	for (;;) {
		DWORD got = 0;
		if (!WinHttpReadData(request.h, buf.data(), (DWORD)buf.size(), &got)) {
			error = "lecture interrompue";
			return false;
		}
		if (!got)
			break;
		out.insert(out.end(), buf.begin(), buf.begin() + got);
		if (out.size() > max_size) {
			error = "fichier trop gros";
			return false;
		}
	}
	return true;
}

} // namespace flash::update
