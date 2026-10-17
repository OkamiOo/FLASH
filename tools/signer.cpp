// Clé et manifeste signé des mises à jour de FLASH (outil de publication, jamais livré).
//
//   signer cle <fichier.cle>                       crée la clé (une seule fois), affiche la clé publique
//   signer publique <fichier.cle>                  affiche la clé publique d'une clé existante
//   signer manifeste <dossier> <version> <fichier.cle> [notes]
//                                                  écrit manifeste.txt et manifeste.sig dans le dossier
//   signer essai                                   vérifie Ed25519 sur les vecteurs de la RFC 8032
//
// La clé privée ne s'affiche jamais et ne va jamais dans un dépôt. Si elle est perdue, les FLASH
// déjà distribués ne pourront plus recevoir de mise à jour.

#include <flash/update.h>

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace flash::update;

namespace {

bool read_file(const fs::path &p, std::vector<uint8_t> &out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), {});
	return true;
}

bool write_file(const fs::path &p, const std::string &s)
{
	std::ofstream f(p, std::ios::binary);
	f.write(s.data(), (std::streamsize)s.size());
	return (bool)f;
}

bool read_seed(const fs::path &p, uint8_t seed[32])
{
	std::vector<uint8_t> text, bytes;
	if (!read_file(p, text) || !from_hex(std::string(text.begin(), text.end()), bytes) || bytes.size() != 32) {
		std::fprintf(stderr, "signer : clé illisible\n");
		return false;
	}
	std::copy(bytes.begin(), bytes.end(), seed);
	SecureZeroMemory(bytes.data(), bytes.size());
	SecureZeroMemory(text.data(), text.size());
	return true;
}

void print_public(const uint8_t pk[32])
{
	std::printf("clé publique : %s\npour app/updater.cpp : const uint8_t kPublicKey[32] = {", hex(pk, 32).c_str());
	for (int i = 0; i < 32; i++)
		std::printf("%s0x%02x", i ? ", " : "", pk[i]);
	std::printf("};\n");
}

int make_key(const fs::path &p)
{
	if (fs::exists(p)) {
		std::fprintf(stderr, "signer : %s existe déjà, une clé ne se remplace pas\n", p.string().c_str());
		return 1;
	}
	uint8_t seed[32];
	if (BCryptGenRandom(nullptr, seed, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
		std::fprintf(stderr, "signer : générateur aléatoire indisponible\n");
		return 1;
	}
	if (p.has_parent_path())
		fs::create_directories(p.parent_path());
	if (!write_file(p, hex(seed, 32))) {
		std::fprintf(stderr, "signer : écriture de la clé impossible\n");
		return 1;
	}
	uint8_t pk[32];
	ed25519_public_key(pk, seed);
	SecureZeroMemory(seed, sizeof(seed));
	print_public(pk);
	return 0;
}

int show_public(const fs::path &p)
{
	uint8_t seed[32], pk[32];
	if (!read_seed(p, seed))
		return 1;
	ed25519_public_key(pk, seed);
	SecureZeroMemory(seed, sizeof(seed));
	print_public(pk);
	return 0;
}

int make_manifest(const fs::path &dir, const std::string &version, const fs::path &key, const std::string &notes)
{
	if (version.empty() || version.find_first_not_of("0123456789.") != std::string::npos) {
		std::fprintf(stderr, "signer : version invalide\n");
		return 1;
	}
	Manifest m;
	m.version = version;
	m.notes = notes;
	for (const auto &e : fs::recursive_directory_iterator(dir)) {
		if (!e.is_regular_file())
			continue;
		const std::u8string u = fs::relative(e.path(), dir).generic_u8string();
		const std::string rel(u.begin(), u.end());
		if (rel == "manifeste.txt" || rel == "manifeste.sig" || e.path().extension() == ".zip")
			continue;
		if (!safe_path(rel)) {
			std::fprintf(stderr, "signer : chemin refusé : %s\n", rel.c_str());
			return 1;
		}
		std::vector<uint8_t> data;
		if (!read_file(e.path(), data)) {
			std::fprintf(stderr, "signer : lecture impossible : %s\n", rel.c_str());
			return 1;
		}
		m.files.push_back({rel, data.size(), sha256(data.data(), data.size())});
	}
	std::sort(m.files.begin(), m.files.end(), [](const File &a, const File &b) { return a.path < b.path; });
	// Publiés à plat : deux chemins ne doivent pas donner le même nom
	for (size_t i = 1; i < m.files.size(); i++)
		for (size_t j = 0; j < i; j++)
			if (flat_name(m.files[i].path) == flat_name(m.files[j].path)) {
				std::fprintf(stderr, "signer : même nom publié pour %s et %s\n", m.files[i].path.c_str(), m.files[j].path.c_str());
				return 1;
			}
	if (std::none_of(m.files.begin(), m.files.end(), [](const File &f) { return f.path == "flash.exe"; })) {
		std::fprintf(stderr, "signer : flash.exe absent du dossier\n");
		return 1;
	}

	uint8_t seed[32], pk[32], sig[64];
	if (!read_seed(key, seed))
		return 1;
	const std::string text = write_manifest(m);
	ed25519_sign(sig, seed, text.data(), text.size());
	ed25519_public_key(pk, seed);
	SecureZeroMemory(seed, sizeof(seed));

	// Relecture immédiate, comme le fera FLASH
	Manifest check;
	std::string error;
	if (!parse_manifest(text, hex(sig, 64), pk, check, error)) {
		std::fprintf(stderr, "signer : manifeste incohérent (%s)\n", error.c_str());
		return 1;
	}
	if (!write_file(dir / "manifeste.txt", text) || !write_file(dir / "manifeste.sig", hex(sig, 64))) {
		std::fprintf(stderr, "signer : écriture du manifeste impossible\n");
		return 1;
	}
	std::printf("%s", text.c_str());
	return 0;
}

// Vecteurs 1 à 3 de la RFC 8032, section 7.1
int self_test()
{
	struct Vector {
		const char *seed, *pk, *msg, *sig;
	} vectors[] = {
		{"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
		 "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
		 "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
		{"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
		 "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
		 "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
		{"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
		 "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
		 "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
	};
	int failures = 0;
	for (const Vector &v : vectors) {
		std::vector<uint8_t> seed, pk, msg, sig;
		from_hex(v.seed, seed);
		from_hex(v.pk, pk);
		from_hex(v.msg, msg);
		from_hex(v.sig, sig);
		uint8_t got_pk[32], got_sig[64];
		ed25519_public_key(got_pk, seed.data());
		ed25519_sign(got_sig, seed.data(), msg.data(), msg.size());
		const bool ok_pk = std::equal(pk.begin(), pk.end(), got_pk);
		const bool ok_sig = std::equal(sig.begin(), sig.end(), got_sig);
		const bool ok_check = ed25519_check(sig.data(), pk.data(), msg.data(), msg.size());
		got_sig[10] ^= 1;
		const bool refuses_bad = !ed25519_check(got_sig, pk.data(), msg.data(), msg.size());
		std::printf("vecteur %s : clé %s, signature %s, vérification %s, signature altérée %s\n", v.msg[0] ? v.msg : "vide",
			    ok_pk ? "ok" : "FAUX", ok_sig ? "ok" : "FAUX", ok_check ? "ok" : "FAUX", refuses_bad ? "refusée" : "ACCEPTÉE");
		failures += !(ok_pk && ok_sig && ok_check && refuses_bad);
	}
	return failures ? 1 : 0;
}

// Arguments lus en Unicode : les notes et les chemins gardent leurs accents
std::string utf8(const wchar_t *w)
{
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	SetConsoleOutputCP(CP_UTF8);
	std::vector<std::string> a;
	for (int i = 1; i < argc; i++)
		a.push_back(utf8(argv[i]));
	if (a.size() == 2 && a[0] == "cle")
		return make_key(fs::path(argv[2]));
	if (a.size() == 2 && a[0] == "publique")
		return show_public(fs::path(argv[2]));
	if ((a.size() == 4 || a.size() == 5) && a[0] == "manifeste")
		return make_manifest(fs::path(argv[2]), a[2], fs::path(argv[4]), a.size() == 5 ? a[4] : "");
	if (a.size() == 1 && a[0] == "essai")
		return self_test();
	std::fprintf(stderr, "usage : signer cle <fichier> | publique <fichier> | manifeste <dossier> <version> <fichier.cle> [notes] | essai\n");
	return 2;
}
