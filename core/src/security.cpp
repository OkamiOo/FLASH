#include "flash/security.h"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <cctype>

namespace flash {

namespace {
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // base 32 de Crockford
}

void random_bytes(void *buf, size_t size)
{
	BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

std::string to_hex(const uint8_t *data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	out.reserve(size * 2);
	for (size_t i = 0; i < size; i++) {
		out += digits[data[i] >> 4];
		out += digits[data[i] & 0x0F];
	}
	return out;
}

bool from_hex(const std::string &hex, std::vector<uint8_t> &out)
{
	if (hex.size() % 2)
		return false;
	out.clear();
	auto nibble = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < hex.size(); i += 2) {
		const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		out.push_back((uint8_t)(hi << 4 | lo));
	}
	return true;
}

std::string new_device_id()
{
	uint8_t id[16];
	random_bytes(id, sizeof(id));
	return to_hex(id, sizeof(id));
}

std::string new_invitation_code()
{
	uint8_t r[8];
	random_bytes(r, sizeof(r));
	std::string code;
	for (int i = 0; i < 8; i++) {
		if (i == 4)
			code += '-';
		code += kAlphabet[r[i] & 31];
	}
	return code;
}

std::string normalize_code(const std::string &code)
{
	std::string out;
	for (char c : code) {
		if (c == '-' || c == ' ')
			continue;
		c = (char)std::toupper((unsigned char)c);
		if (c == 'O')
			c = '0';
		else if (c == 'I' || c == 'L')
			c = '1';
		out += c;
	}
	return out;
}

std::string protect_secret(const std::string &secret)
{
	DATA_BLOB in{(DWORD)secret.size(), (BYTE *)secret.data()};
	DATA_BLOB out{};
	if (!CryptProtectData(&in, L"FLASH", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return {};
	DWORD len = 0;
	CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
	std::string b64(len, '\0');
	CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len);
	b64.resize(len);
	LocalFree(out.pbData);
	return b64;
}

std::string unprotect_secret(const std::string &blob)
{
	DWORD len = 0;
	if (!CryptStringToBinaryA(blob.c_str(), (DWORD)blob.size(), CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr))
		return {};
	std::vector<BYTE> raw(len);
	CryptStringToBinaryA(blob.c_str(), (DWORD)blob.size(), CRYPT_STRING_BASE64, raw.data(), &len, nullptr, nullptr);
	DATA_BLOB in{len, raw.data()};
	DATA_BLOB out{};
	if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return {};
	std::string secret((const char *)out.pbData, out.cbData);
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return secret;
}

} // namespace flash
